// OneGrade — cross-platform OpenFX color grade plugin for DaVinci Resolve.
// SPDX-License-Identifier: BSD-3-Clause

#include "OneGrade.h"
#include "OneGradePipeline.h"
#include "CubeLUT.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <map>
#include <filesystem>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX            // keep windows.h from defining min/max macros (breaks std::min/max in OFX headers)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"
#include "ofxsLog.h"

#define kPluginName        "OneGrade"
#define kPluginGrouping    "OneGrade"
#define kPluginDescription "One-node camera CST + balance + density + exposure + output encode. " \
                           "Set Output Encode to Cineon Log to feed a film-look LUT node."
#define kPluginIdentifier  "com.mattgrdinic.OneGrade"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 1

#define kSupportsTiles              false
#define kSupportsMultiResolution    false
#define kSupportsMultipleClipPARs   false

#define kParamCount 13 // temp,tint,density,lift,gamma,gain,offTemp,offTint,postExp,postCon,rawExp,rawTemp,rolloff

// Folder scanned for built-in / film-look LUTs (Resolve's default LUT install).
// Resolve puts this somewhere different on every platform, so it has to be resolved at
// runtime: a single hardcoded POSIX path just yields an empty Film Look list off-macOS,
// with no error — the Film Emulation presets then quietly render without their print LUT.
static std::string filmLutDir()
{
#ifdef _WIN32
    const char* pd = std::getenv("PROGRAMDATA");
    return std::string(pd ? pd : "C:\\ProgramData") + "\\Blackmagic Design\\DaVinci Resolve\\Support\\LUT";
#elif defined(__APPLE__)
    return "/Library/Application Support/Blackmagic Design/DaVinci Resolve/LUT";
#else
    return "/opt/resolve/LUT";
#endif
}

// LUT lists, built once at describe.
typedef std::vector<std::pair<std::string, std::string>> LutList;   // (label, absolute path)
typedef std::pair<std::string, LutList> LutGroup;                   // (group name, luts)

//   s_FilmLuts: Resolve's "Film Looks" folder (print-emulation, need Cineon input).
//   s_LookGroups: the whole master LUT folder, grouped by top-level subfolder (Group -> LUT cascade).
static LutList s_FilmLuts;
static std::vector<LutGroup> s_LookGroups;
static bool s_Scanned = false;

static void scanDir(const std::string& root, LutList& out)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec))
    {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const fs::path& p = it->path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".cube") { out.emplace_back(p.stem().string(), p.string()); if (out.size() >= 1000) break; }
    }
    std::sort(out.begin(), out.end());
}

// Index of a film print stock in s_FilmLuts by case-insensitive name fragment, preferring
// the Rec.709 variant (the film path outputs Rec.709). -1 if absent. Used by the Film
// Emulation presets; kodak2383Index() wraps it for the filmLut describe-time default.
static int filmLutIndex(const char* fragment)
{
    int idx = -1;
    for (size_t i = 0; i < s_FilmLuts.size(); ++i) {
        std::string n = s_FilmLuts[i].first;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        if (n.find(fragment) != std::string::npos) {
            idx = (int)i;
            if (n.find("rec709") != std::string::npos) break;   // prefer Rec.709 variant
        }
    }
    return idx;
}
static int kodak2383Index() { int i = filmLutIndex("kodak 2383 d60"); return i < 0 ? 0 : i; }

// Directory of the LUTs we ship inside the bundle (<bundle>/Contents/Resources/LUTs),
// resolved from the plugin binary's own path so it works wherever the bundle lives.
static std::string bundleLutDir()
{
    std::string bin;
#ifdef _WIN32
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&bundleLutDir, &hm)) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(hm, path, MAX_PATH)) bin = path;
    }
#else
    Dl_info info;
    if (dladdr((void*)&bundleLutDir, &info) && info.dli_fname) bin = info.dli_fname;
#endif
    if (bin.empty()) return {};
    namespace fs = std::filesystem;
    fs::path contents = fs::path(bin).parent_path().parent_path();   // Contents/<arch>/OneGrade.ofx -> Contents
    return (contents / "Resources" / "LUTs").string();
}

// Find a Look LUT by case-insensitive name fragment across all groups. Fills (group, lut)
// indices when found. Used by the Custom LUT presets to select the matching built-in look
// (shipped in the bundle, so this normally resolves on any machine).
static bool findLookLut(const char* fragment, int& groupIdx, int& lutIdx)
{
    for (size_t g = 0; g < s_LookGroups.size(); ++g)
        for (size_t l = 0; l < s_LookGroups[g].second.size(); ++l) {
            std::string n = s_LookGroups[g].second[l].first;
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            if (n.find(fragment) != std::string::npos) { groupIdx = (int)g; lutIdx = (int)l; return true; }
        }
    return false;
}

static void scanLuts()
{
    if (s_Scanned) return;
    s_Scanned = true;
    namespace fs = std::filesystem;
    std::error_code ec;

    const std::string lutRoot = filmLutDir();
    const std::string filmDir = (fs::path(lutRoot) / "Film Looks").string();
    scanDir(fs::exists(filmDir, ec) ? filmDir : lutRoot, s_FilmLuts);

    // LUTs shipped inside the bundle — surfaced as the first Look group so the
    // presets (and users) get them with zero external installs.
    LutList builtin;
    scanDir(bundleLutDir(), builtin);
    if (!builtin.empty()) s_LookGroups.emplace_back("OneGrade (built-in)", builtin);

    // Group the whole master folder by top-level subfolder (files in root -> "General").
    std::map<std::string, LutList> groups;
    if (fs::exists(lutRoot, ec)) {
        for (fs::recursive_directory_iterator it(lutRoot, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec))
        {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            const fs::path& p = it->path();
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".cube") continue;
            std::error_code ec2;
            fs::path rel = fs::relative(p, lutRoot, ec2);
            std::string group = rel.has_parent_path() ? rel.begin()->string() : "General";
            groups[group].emplace_back(p.stem().string(), p.string());
        }
    }
    for (auto& g : groups) { std::sort(g.second.begin(), g.second.end()); s_LookGroups.emplace_back(g.first, g.second); }
    // std::map already sorts group names alphabetically.
}

// Path of the LUT the given mode + dropdown indices select, "" if the selection resolves
// to nothing (mode None, or an empty/absent LUT folder). Shared by the panel and the
// render so the two can't disagree about whether a LUT is actually in play.
static std::string resolveLutPath(int p_Mode, int p_Group, int p_Look, int p_Film)
{
    if (p_Mode == 1) {          // Custom Look — group -> LUT cascade
        if (p_Group >= 0 && p_Group < (int)s_LookGroups.size()) {
            const LutList& luts = s_LookGroups[p_Group].second;
            if (p_Look >= 0 && p_Look < (int)luts.size()) return luts[p_Look].second;
        }
    } else if (p_Mode == 2) {   // Film Look — flat list
        if (p_Film >= 0 && p_Film < (int)s_FilmLuts.size()) return s_FilmLuts[p_Film].second;
    }
    return std::string();
}

////////////////////////////////////////////////////////////////////////////////

class OneGradeProcessor : public OFX::ImageProcessor
{
public:
    explicit OneGradeProcessor(OFX::ImageEffect& p_Instance);

    virtual void processImagesCuda();
    virtual void processImagesOpenCL();
    virtual void processImagesMetal();
    virtual void multiThreadProcessImages(OfxRectI p_ProcWindow);

    void setSrcImg(OFX::Image* p_SrcImg);
    void setParams(const float p_Params[kParamCount], int p_Camera, int p_Encode);
    void setLut(const float* p_Lut, int p_LutSize, float p_LutMix);

private:
    OFX::Image* _srcImg = nullptr;
    float _params[kParamCount];
    int   _camera = 0;
    int   _encode = 0;
    const float* _lut = nullptr;   // not owned
    int   _lutSize = 0;
    float _lutMix = 0.0f;
};

OneGradeProcessor::OneGradeProcessor(OFX::ImageEffect& p_Instance)
    : OFX::ImageProcessor(p_Instance)
{
    for (int i = 0; i < kParamCount; ++i) _params[i] = 0.0f;
}

#ifdef OFX_SUPPORTS_CUDARENDER
extern void RunCudaKernel(void* p_Stream, int p_Width, int p_Height, const float* p_Params, int p_Camera, int p_Encode, const float* p_Lut, int p_LutSize, float p_LutMix, const float* p_Input, float* p_Output);
#endif

void OneGradeProcessor::processImagesCuda()
{
#ifdef OFX_SUPPORTS_CUDARENDER
    const OfxRectI& bounds = _srcImg->getBounds();
    const int width = bounds.x2 - bounds.x1;
    const int height = bounds.y2 - bounds.y1;
    float* input = static_cast<float*>(_srcImg->getPixelData());
    float* output = static_cast<float*>(_dstImg->getPixelData());
    RunCudaKernel(_pCudaStream, width, height, _params, _camera, _encode, _lut, _lutSize, _lutMix, input, output);
#endif
}

#ifdef __APPLE__
extern void RunMetalKernel(void* p_CmdQ, int p_Width, int p_Height, const float* p_Params, int p_Camera, int p_Encode, const float* p_Lut, int p_LutSize, float p_LutMix, const float* p_Input, float* p_Output);
#endif

void OneGradeProcessor::processImagesMetal()
{
#ifdef __APPLE__
    const OfxRectI& bounds = _srcImg->getBounds();
    const int width = bounds.x2 - bounds.x1;
    const int height = bounds.y2 - bounds.y1;
    float* input = static_cast<float*>(_srcImg->getPixelData());
    float* output = static_cast<float*>(_dstImg->getPixelData());
    RunMetalKernel(_pMetalCmdQ, width, height, _params, _camera, _encode, _lut, _lutSize, _lutMix, input, output);
#endif
}

extern void RunOpenCLKernelBuffers(void* p_CmdQ, int p_Width, int p_Height, const float* p_Params, int p_Camera, int p_Encode, const float* p_Lut, int p_LutSize, float p_LutMix, const float* p_Input, float* p_Output);

void OneGradeProcessor::processImagesOpenCL()
{
#ifdef OFX_SUPPORTS_OPENCLRENDER
    const OfxRectI& bounds = _srcImg->getBounds();
    const int width = bounds.x2 - bounds.x1;
    const int height = bounds.y2 - bounds.y1;
    float* input = static_cast<float*>(_srcImg->getPixelData());
    float* output = static_cast<float*>(_dstImg->getPixelData());
    RunOpenCLKernelBuffers(_pOpenCLCmdQ, width, height, _params, _camera, _encode, _lut, _lutSize, _lutMix, input, output);
#endif
}

void OneGradeProcessor::multiThreadProcessImages(OfxRectI p_ProcWindow)
{
    for (int y = p_ProcWindow.y1; y < p_ProcWindow.y2; ++y)
    {
        if (_effect.abort()) break;
        float* dstPix = static_cast<float*>(_dstImg->getPixelAddress(p_ProcWindow.x1, y));
        for (int x = p_ProcWindow.x1; x < p_ProcWindow.x2; ++x)
        {
            float* srcPix = static_cast<float*>(_srcImg ? _srcImg->getPixelAddress(x, y) : nullptr);
            if (srcPix)
            {
                og::process(_camera, _encode, _params, srcPix[0], srcPix[1], srcPix[2],
                            dstPix[0], dstPix[1], dstPix[2]);
                if (_lut && _lutSize >= 2 && _lutMix > 0.0f)
                    og::apply_lut(_lut, _lutSize, _lutMix, dstPix[0], dstPix[1], dstPix[2]);
                og::apply_trim(_params[8], _params[9], dstPix[0], dstPix[1], dstPix[2]);  // post-LUT trim
                if (_params[12] > 0.0f && (_encode <= 2 || (_lut && _lutSize >= 2 && _lutMix > 0.0f)))
                    for (int c = 0; c < 3; ++c) dstPix[c] = og::softclip(dstPix[c], _params[12]);  // highlight roll-off (display-referred only)
                dstPix[3] = srcPix[3];
            }
            else
            {
                for (int c = 0; c < 4; ++c) dstPix[c] = 0;
            }
            dstPix += 4;
        }
    }
}

void OneGradeProcessor::setSrcImg(OFX::Image* p_SrcImg) { _srcImg = p_SrcImg; }

void OneGradeProcessor::setParams(const float p_Params[kParamCount], int p_Camera, int p_Encode)
{
    std::memcpy(_params, p_Params, sizeof(float) * kParamCount);
    _camera = p_Camera;
    _encode = p_Encode;
}

void OneGradeProcessor::setLut(const float* p_Lut, int p_LutSize, float p_LutMix)
{
    _lut = p_Lut;
    _lutSize = p_LutSize;
    _lutMix = p_LutMix;
}

////////////////////////////////////////////////////////////////////////////////

class OneGrade : public OFX::ImageEffect
{
public:
    explicit OneGrade(OfxImageEffectHandle p_Handle);
    virtual void render(const OFX::RenderArguments& p_Args);
    virtual void changedParam(const OFX::InstanceChangedArgs& p_Args, const std::string& p_ParamName);
    void setEnabledness();
    bool lutSelected();         // does a LUT resolve behind the current LUT Mode? (Mix-independent)
    void probeAnalyze(double p_Time);   // measure the frame and report (writes m_LastKey)
    void applyAutoGrade(double p_Time); // measure, then set the film look + Gain from key
    double m_LastKey = 0.0;             // scene key in stops from the last successful analyse
    double m_LastPin = 0.0;             // % of frame clipped at the source ceiling
    bool   m_HaveKey = false;
    void populateLookLut();     // repopulate the Look LUT dropdown for the current group
    void applyPreset(int p);    // set the look params (density/LGG/LUT/trim) to a starting point
    void setupAndProcess(OneGradeProcessor& p_Proc, const OFX::RenderArguments& p_Args);

private:
    OFX::Clip* m_DstClip;
    OFX::Clip* m_SrcClip;

    OFX::ChoiceParam* m_NodeRole;   // 0 full grade, 1 input transform, 2 output transform
    OFX::ChoiceParam* m_Preset;
    OFX::ChoiceParam* m_Camera;
    OFX::DoubleParam* m_RawExp;
    OFX::DoubleParam* m_RawTemp;
    OFX::DoubleParam* m_Temp;
    OFX::DoubleParam* m_Tint;
    OFX::DoubleParam* m_OffTemp;
    OFX::DoubleParam* m_OffTint;
    OFX::DoubleParam* m_Density;
    OFX::DoubleParam* m_Lift;
    OFX::DoubleParam* m_Gamma;
    OFX::DoubleParam* m_Gain;
    OFX::ChoiceParam* m_Encode;
    OFX::StringParam* m_EncodeNote;   // says what the encode actually is when it's overridden
    OFX::DoubleParam* m_PostExp;
    OFX::DoubleParam* m_PostCon;
    OFX::DoubleParam* m_Rolloff;

    OFX::ChoiceParam* m_LutMode;    // 0 none, 1 custom look, 2 film-look built-in
    OFX::ChoiceParam* m_FilmLut;
    OFX::ChoiceParam* m_LookGroup;
    OFX::ChoiceParam* m_LookLut;
    OFX::DoubleParam* m_LutMix;
    CubeLUT           m_Lut;        // cached loaded LUT

    // Auto Grade probe (experimental) — see probeAnalyze().
    OFX::StringParam* m_ProbeStatus;
    OFX::StringParam* m_ProbeScene;
    OFX::StringParam* m_ProbeDisplay;
    OFX::StringParam* m_ProbeShape;
    OFX::StringParam* m_ProbeSubject;
    OFX::DoubleParam* m_AutoBias;
    OFX::StringParam* m_ProbePeak;
    OFX::StringParam* m_ProbeApplied;
};

OneGrade::OneGrade(OfxImageEffectHandle p_Handle)
    : ImageEffect(p_Handle)
{
    m_DstClip = fetchClip(kOfxImageEffectOutputClipName);
    m_SrcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);

    m_NodeRole= fetchChoiceParam("nodeRole");
    m_Preset  = fetchChoiceParam("preset");
    m_Camera  = fetchChoiceParam("camera");
    m_RawExp  = fetchDoubleParam("rawExp");
    m_RawTemp = fetchDoubleParam("rawTemp");
    m_Temp    = fetchDoubleParam("temp");
    m_Tint    = fetchDoubleParam("tint");
    m_OffTemp = fetchDoubleParam("offTemp");
    m_OffTint = fetchDoubleParam("offTint");
    m_Density = fetchDoubleParam("density");
    m_Lift    = fetchDoubleParam("lift");
    m_Gamma   = fetchDoubleParam("gamma");
    m_Gain    = fetchDoubleParam("gain");
    m_Encode  = fetchChoiceParam("outEncode");
    m_EncodeNote = fetchStringParam("encodeNote");
    m_PostExp = fetchDoubleParam("postExp");
    m_PostCon = fetchDoubleParam("postCon");
    m_Rolloff = fetchDoubleParam("rolloff");

    m_LutMode  = fetchChoiceParam("lutMode");
    m_FilmLut  = fetchChoiceParam("filmLut");
    m_LookGroup= fetchChoiceParam("lookGroup");
    m_LookLut  = fetchChoiceParam("lookLut");
    m_LutMix   = fetchDoubleParam("lutMix");
    m_ProbeStatus  = fetchStringParam("probeStatus");
    m_ProbeScene   = fetchStringParam("probeScene");
    m_ProbeDisplay = fetchStringParam("probeDisplay");
    m_ProbeShape   = fetchStringParam("probeShape");
    m_ProbeSubject = fetchStringParam("probeSubject");
    m_AutoBias     = fetchDoubleParam("autoBias");
    m_ProbePeak    = fetchStringParam("probePeak");
    m_ProbeApplied = fetchStringParam("probeApplied");

    populateLookLut();
    setEnabledness();
}

// Grey out whatever the current Node Role doesn't own, then apply the LUT-mode rule
// (Film Look and Custom Look are mutually exclusive) on top.
//
// Role splits the pipeline across Resolve's group grading levels:
//   0 Full Grade      — one node does everything (the original, and the default).
//   1 Input Transform — camera decode only, out to DaVinci Intermediate. Group Pre-Clip.
//   2 Output Transform— takes DWG/DI in, applies look + delivery encode. Group Post-Clip.
// Roles 1+2 chained reproduce role 0 to well under an 8-bit code value (test/pipeline_test).
// Panel-side mirror of the render's `lutOk`: does a LUT actually resolve behind the
// current mode? Mix is deliberately not consulted — Mix blends within the LUT's encode,
// it doesn't decide which encode is used, so a LUT at Mix 0 still owns Output Encode.
// This path-resolves rather than parsing the .cube (no file I/O from a param callback),
// so a corrupt LUT greys a control the render then honours — an error state where the
// LUT dropdown is the louder tell anyway.
bool OneGrade::lutSelected()
{
    int mode = 0, gi = 0, li = 0, fi = 0;
    m_LutMode->getValue(mode);
    m_LookGroup->getValue(gi);
    m_LookLut->getValue(li);
    m_FilmLut->getValue(fi);
    return !resolveLutPath(mode, gi, li, fi).empty();
}

// AUTO GRADE ANALYSIS — step 2 of the "magic button" (2026-08-02).
//
// Step 1 answered the only question that could have killed the idea: Resolve DOES hand
// over pixels from outside a render call (validated in Resolve, 4K frame, 230400 samples).
// So a button can measure the frame and write slider values, and the whole feature stays
// in the param layer — no kernel work, no golden-rule mirror.
//
// This step measures and REPORTS ONLY. Nothing is written to a slider yet: the numbers
// have to be shown to describe real shots correctly before any of them is allowed to move
// the picture. Wiring comes in step 3.
//
// The measurement runs the samples through the REAL pipeline, not a parallel copy of it:
//   - scene luminance is XYZ Y straight out of `to_XYZ`, which is exact and gamut-agnostic
//     (Rec.709 luma weights would be wrong against DWG primaries)
//   - display values come from `og::process()` itself at neutral params, so what's measured
//     is what the node would render with the grade zeroed
// Percentiles come from `nth_element` over the kept samples rather than a histogram: at
// ~200k samples the memory is under a megabyte and it removes binning error entirely.
//
// Everything stays wrapped: fetchImage outside render may throw, return null, or hand back
// zeros, and all three are answers as long as we survive them. `anyNonZero` is tracked
// separately because an empty buffer and a black shot both read as p1 = p50 = p99 = 0.
void OneGrade::probeAnalyze(double p_Time)
{
    m_ProbeScene->setValue("");
    m_ProbeDisplay->setValue("");
    m_ProbeShape->setValue("");
    m_ProbeSubject->setValue("");
    m_ProbePeak->setValue("");
    if (!m_SrcClip || !m_SrcClip->isConnected()) { m_ProbeStatus->setValue("No source clip connected"); return; }

    try {
        std::unique_ptr<OFX::Image> src(m_SrcClip->fetchImage(p_Time));
        if (!src.get()) { m_ProbeStatus->setValue("fetchImage returned null"); return; }

        const OfxRectI b = src->getBounds();
        const int w = b.x2 - b.x1, h = b.y2 - b.y1;
        if (w <= 0 || h <= 0) { m_ProbeStatus->setValue("Empty bounds"); return; }
        if (src->getPixelDepth() != OFX::eBitDepthFloat ||
            src->getPixelComponents() != OFX::ePixelComponentRGBA) {
            char m2[64]; snprintf(m2, sizeof m2, "%dx%d but not float RGBA", w, h);
            m_ProbeStatus->setValue(m2); return;
        }

        // Measure the NEUTRAL node: the analysis has to describe the footage, not the grade
        // already on it, or clicking twice would chase its own tail. Camera and Output
        // Encode are the user's, since they decide what space the numbers even mean.
        int camera = 0, encode = 0, lutMode = 0;
        m_Camera->getValue(camera);
        m_Encode->getValue(encode);
        m_LutMode->getValue(lutMode);
        // Start from the EFFECTIVE encode, the same override the render applies: with a LUT
        // selected the Output Encode param is not what gets rendered, so measuring against
        // it would report a curve the user isn't looking at.
        if (lutSelected()) encode = (lutMode == 2) ? 3 : 0;
        // ...but the analysis must land in a DISPLAY-REFERRED space, so fall back to Gamma
        // 2.2 when the effective encode isn't one. A Film Look forces Cineon, and Cineon is
        // a log encode: it clamps to [0,1] (so 'hot' reads a flat 0% on a genuinely blown
        // frame) and it compresses chroma (so the skin mask's saturation window, tuned for
        // display RGB, stops matching faces). Both were observed on the interview shot -
        // hot fell 22.9% -> 0.0% and skin coverage collapsed to 1.6% - purely from the
        // encode underneath, with no change to the picture. Percentile and hue thresholds
        // are only meaningful in the space they were chosen for.
        const int dispEnc = (encode <= 2) ? encode : 1;
        const char* encName = (dispEnc == 0) ? "Scene" : (dispEnc == 1) ? "2.2" : "2.4";
        float neutral[kParamCount] = {0.f,0.f,0.f, 0.f,1.f,1.f, 0.f,0.f, 0.f,1.f, 0.f,6500.f, 0.f};

        // Coarse grid, ~200k samples: percentiles don't need every pixel, and a button that
        // stalls the UI on an 8K frame is its own kind of failure.
        const int step = std::max(1, (int)(std::sqrt((double)(w * h) / 200000.0) + 0.5));
        std::vector<float> sceneY, dispL, skinY;
        double skinR = 0.0, skinG = 0.0, skinB = 0.0;   // skin chromaticity, for a warmth read
        sceneY.reserve(220000); dispL.reserve(220000);
        long long hot = 0;
        std::vector<float> srcTop;   // per-sample max input channel, for ceiling detection
        srcTop.reserve(220000);
        double satSum = 0.0; long long satN = 0;
        bool anyNonZero = false;

        for (int y = b.y1; y < b.y2; y += step) {
            const float* row = static_cast<const float*>(src->getPixelAddress(b.x1, y));
            if (!row) continue;
            for (int x = 0; x < w; x += step) {
                const float* p = row + (size_t)x * 4;
                if (p[0] != 0.f || p[1] != 0.f || p[2] != 0.f) anyNonZero = true;
                srcTop.push_back(std::max(p[0], std::max(p[1], p[2])));

                // Scene luminance: decode to camera-linear, then XYZ Y. Neutral RAW, so no
                // exposure gain and white_balance() at 6500 is identity — skipped, not
                // approximated.
                float lin[3] = { og::decode_log(camera, p[0]), og::decode_log(camera, p[1]), og::decode_log(camera, p[2]) };
                float xyz[3]; og::to_XYZ(camera, lin, xyz);
                sceneY.push_back(xyz[1]);

                // Display: the actual render path at neutral grade.
                float dr, dg, db;
                og::process(camera, dispEnc, neutral, p[0], p[1], p[2], dr, dg, db);
                const float L = 0.2126f*dr + 0.7152f*dg + 0.0722f*db;
                dispL.push_back(L);
                if (L > 1.0f) ++hot;   // above display white: lost on export unless rolled off

                float hh, ss, vv; og::rgb2hsv(dr, dg, db, hh, ss, vv);
                // Saturation on mid-tones only: shadows and blown highlights both report
                // meaningless saturation, and it's the mids that a Density move addresses.
                if (L > 0.15f && L < 0.85f) { satSum += ss; ++satN; }

                // Crude skin mask: warm hue, plausible saturation. Exists because
                // frame-median exposure is subject-blind — a dark interior drags the median
                // down and asks for a push that would blow the windows and overexpose a
                // face that was already fine. Reported with its own coverage %, because
                // this mask cannot tell skin from sand: on a desert shot it matches ~40% of
                // the frame, and that is the tell.
                //
                // Selection is on CHROMATICITY ONLY. A luminance window here (the first
                // version used display 0.15-0.95) is self-fulfilling: it picks mid-tone
                // pixels by construction, so their median lands near mid-gray and the key
                // reads ~0 on every shot. Caught on the desert frame, where masked Y came
                // back 0.21 against a frame median of 0.62 — a filter artefact, not a
                // measurement. The only luma guard left excludes pixels too dark or too
                // blown for their hue to mean anything.
                if (hh >= 0.01f && hh <= 0.11f && ss >= 0.10f && ss <= 0.65f &&
                    vv >= 0.03f && vv <= 1.05f) {
                    skinY.push_back(xyz[1]);
                    skinR += dr; skinG += dg; skinB += db;
                }
            }
        }
        const size_t n = dispL.size();
        if (n == 0) { m_ProbeStatus->setValue("Bounds ok but no rows readable"); return; }

        auto pct = [](std::vector<float>& v, double frac) {
            size_t k = (size_t)(frac * (v.size() - 1));
            std::nth_element(v.begin(), v.begin() + k, v.end());
            return (double)v[k];
        };
        // Each nth_element is O(n) and correct whatever state the vector is left in by the
        // previous call, so three ranks cost three linear passes — still far cheaper than
        // a full sort, and exact where a histogram would only be as good as its bin width.
        const double d1 = pct(dispL, 0.01), d50 = pct(dispL, 0.50), d99 = pct(dispL, 0.99);
        const double d999 = pct(dispL, 0.999);
        const double y1 = pct(sceneY, 0.01), y50 = pct(sceneY, 0.50), y99 = pct(sceneY, 0.99);

        // The two numbers a heuristic would actually act on. Key: how far the median sits
        // from 18% mid-gray, in stops — that IS the exposure correction, since RAW Exposure
        // is a linear gain in stops. DR: the scene's usable range, p1 to p99, which says
        // whether there is room to lift blacks and roll highlights or the shot is already flat.
        const double key = (y50 > 1e-6) ? std::log2(0.18 / y50) : 0.0;
        m_LastKey = key; m_HaveKey = true;
        const double dr_stops = (y1 > 1e-6 && y99 > y1) ? std::log2(y99 / y1) : 0.0;

        char m2[128];
        snprintf(m2, sizeof m2, "%s %dx%d step %d n=%zu", anyNonZero ? "OK" : "ALL ZERO", w, h, step, n);
        m_ProbeStatus->setValue(m2);
        snprintf(m2, sizeof m2, "Y50 %.4f  key %+.2f EV  DR %.1f st", y50, key, dr_stops);
        m_ProbeScene->setValue(m2);
        snprintf(m2, sizeof m2, "p1 %.3f  p50 %.3f  p99 %.3f  @%s", d1, d50, d99, encName);
        m_ProbeDisplay->setValue(m2);
        // Source clipping, measured against the CLIP'S OWN ceiling rather than an assumed
        // 1.0. Blackmagic log peaks around 0.75 of the code range (confirmed on a waveform
        // with the node disabled), so a fixed "> 0.995" test reports 0% on every Blackmagic
        // shot — including genuinely blown ones. What actually identifies clipping is a
        // PILE-UP at whatever the top of this clip's distribution happens to be: a real
        // highlight rolls off with falling density, a clipped one stacks samples on the
        // ceiling. So: find the max, then count how much of the frame is sitting on it.
        float srcMax = 0.f;
        for (float v : srcTop) if (v > srcMax) srcMax = v;
        const float eps = std::max(0.002f, srcMax * 0.004f);
        long long pinned = 0;
        for (float v : srcTop) if (v >= srcMax - eps) ++pinned;

        // Highlight SHAPE, not size. A big bright landscape and a small blown window both
        // raise 'hot', but only the second needs a rolloff: the user gave a 36%-hot cactus
        // rolloff 0 and a 22.9%-hot interview rolloff 0.557. What separates them is how far
        // the very top runs past the bulk - a compact specular core sits far above p99,
        // a broad bright field sits just above it. peak = p99.9 / p99.
        const double peak = (d99 > 1e-6) ? d999 / d99 : 1.0;
        snprintf(m2, sizeof m2, "p99.9 %.3f  peak x%.2f", d999, peak);
        m_ProbePeak->setValue(m2);

        m_LastPin = 100.0 * (double)pinned / (double)n;
        snprintf(m2, sizeof m2, "hot %.1f%%  pin %.2f%%@%.3f  sat %.3f",
                 100.0 * (double)hot / (double)n, 100.0 * (double)pinned / (double)n,
                 srcMax, satN ? satSum / (double)satN : 0.0);
        m_ProbeShape->setValue(m2);

        // Subject key: the same exposure question asked of skin-toned pixels only. Where
        // the two keys disagree, the frame median is the one that's wrong.
        const double skinFrac = 100.0 * (double)skinY.size() / (double)n;
        if (skinY.size() >= 200) {
            const double sy = pct(skinY, 0.50);
            const double skey = (sy > 1e-6) ? std::log2(0.18 / sy) : 0.0;
            // Warmth as the skin's own chromaticity: R/G and B/G of the masked pixels.
            // The user's fix for "too cool" on this footage was RAW Temperature 6500 ->
            // 9242, so what has to be measurable is how far skin sits from where skin
            // should sit - not a global grey-world guess, which a teal shirt would skew.
            const double g = (skinG > 1e-6) ? skinG : 1.0;
            // Short enough that B/G isn't truncated in the panel — the first version cut it
            // off, and B/G is the half that might carry a cool cast.
            snprintf(m2, sizeof m2, "%.0f%% k%+.2f RG%.2f BG%.2f",
                     skinFrac, skey, skinR / g, skinB / g);
        } else {
            snprintf(m2, sizeof m2, "skin %.1f%% - too few to trust", skinFrac);
        }
        m_ProbeSubject->setValue(m2);
    }
    catch (std::exception& e) {
        char m2[96]; snprintf(m2, sizeof m2, "threw: %.60s", e.what());
        m_ProbeStatus->setValue(m2);
    }
    catch (...) { m_ProbeStatus->setValue("fetchImage threw (unknown)"); }
}

// AUTO GRADE — step 3, fitted to the user's own grades rather than to a convention.
//
// Four hand-graded shots (2026-08-02) turned out to be the Cinematic Film Emulation preset
// with exactly ONE slider moved per shot: Gain. Everything else — lift, gamma, density,
// trim, and the Gain Temp -0.220 / Gain Tint 0.090 tint that gives the look its character —
// was identical across all four. The car-interior grade IS the untouched preset.
//
// And Gain tracks the measured key:
//     shot        key      gain
//     car       +2.60      0.800   (= preset, untouched)
//     desert    -0.79      0.642
//     interview -1.04      0.655
//     cactus    -1.96      0.407
// which a line fits to within 0.02 on three of the four:  gain = 0.80 + 0.19*key.
// (The interview is the outlier and explains itself: the only shot on a different camera,
// with RAW Exposure already at -0.50, so part of its correction happened upstream of Gain.)
//
// The clamp at the preset value for key >= 0 is the important half. It means a dark shot is
// never pushed up — the earlier finding that `key` is descriptive rather than prescriptive
// (a low-key interior is *supposed* to sit low, and chasing 18% grey would flatten it) is
// handled by refusing to act in that direction at all, rather than by a special case.
//
// Writes ordinary slider values the user can then drag. That is the whole design: a
// starting point that shows its work, so a bad analysis costs one undo, not trust.
void OneGrade::applyAutoGrade(double p_Time)
{
    probeAnalyze(p_Time);              // fills m_LastKey, and reports what it saw
    if (!m_HaveKey) return;            // analysis failed; probeAnalyze has already said why

    applyPreset(1);                    // Cinematic Film Emulation (Kodak 2383 D60)

    // Fitted from the user's grades. Floor exists because the fit is only evidenced out to
    // about -2 EV; beyond that it extrapolates, and an unclamped line reaches 0 near -4 EV.
    const double gain = std::min(0.80, std::max(0.30, 0.80 + 0.19 * m_LastKey));
    m_Gain->setValue(gain);

    // Highlight Rolloff from SOURCE CLIPPING, which is the only measurement that separated
    // the user's rolloff choices:
    //     cactus      pin 0.00%   rolloff 0        (33.7% hot, but nothing clipped)
    //     car         pin 0.00%   rolloff 0
    //     desert      pin 0.00%   rolloff 0
    //     interview   pin 6.18%   rolloff 0.557    (blown windows)
    // 0.557/6.18 = 0.090 per percent. Physically right, too: rolloff exists to soften flat
    // detail-free patches, and clipped-at-source IS flat and detail-free. A merely bright
    // frame keeps its texture and wants nothing. Two earlier candidates are ruled out by
    // this table - `hot` runs the wrong way (33.7% -> 0, 17.8% -> 0.557), and so does
    // p99.9/p99, because a big blown window makes p99 and p99.9 land on the same plateau.
    // Evidenced by ONE non-zero point, so it is a line through the origin; three controls
    // sit correctly at zero. Cap short of 1.0 - beyond ~0.8 the shoulder starts eating
    // diffuse white, and no measured shot came near it.
    // Bias: one slider trading highlight restraint against shadow openness, because the
    // measurement can only get the shot into the right neighbourhood — which end of that
    // neighbourhood you want is taste, and taste needs a knob rather than a constant.
    // Negative tames the top (more rolloff, shadows sit down); positive opens the bottom
    // (lift up, rolloff backed off). Zero is the fitted result.
    //
    // It moves Rolloff and Lift specifically because those are the two the user reached for
    // in exactly this situation: "add a touch of highlight rolloff until we bring the
    // highlights below 1023", and "lift darker images a bit". Gain deliberately stays on
    // its measurement — it's the one parameter with a hard physical anchor (distance from
    // mid-gray), and letting a taste control drag it would undo the part that works.
    double bias = 0.0; m_AutoBias->getValue(bias);
    const double rolloff = std::min(0.80, std::max(0.0, 0.090 * m_LastPin - bias * 0.35));
    const double lift    = std::min(0.50, std::max(-0.50, 0.11 + bias * 0.06));
    m_Rolloff->setValue(rolloff);
    m_Lift->setValue(lift);

    char msg[96];
    if (bias != 0.0)
        snprintf(msg, sizeof msg, "Gain %.3f  Roll %.3f  Lift %.3f  bias %+.2f",
                 gain, rolloff, lift, bias);
    else
        snprintf(msg, sizeof msg, "Gain %.3f (key %+.2f)  Roll %.3f (pin %.1f%%)",
                 gain, m_LastKey, rolloff, m_LastPin);
    m_ProbeApplied->setValue(msg);
    setEnabledness();                  // the preset switches LUT Mode
}

void OneGrade::setEnabledness()
{
    int role = 0, mode = 0;
    m_NodeRole->getValue(role);
    m_LutMode->getValue(mode);

    const bool input  = (role == 1);
    const bool output = (role == 2);
    const bool look   = !input;    // look/grade layer belongs to Full + Output Transform
    const bool src    = !output;   // camera + RAW belong to Full + Input Transform

    m_Camera->setEnabled(src);
    m_RawExp->setEnabled(src);
    m_RawTemp->setEnabled(src);

    m_Preset->setEnabled(look);
    m_Temp->setEnabled(look);
    m_Tint->setEnabled(look);
    m_OffTemp->setEnabled(look);
    m_OffTint->setEnabled(look);
    m_Density->setEnabled(look);
    m_Lift->setEnabled(look);
    m_Gamma->setEnabled(look);
    m_Gain->setEnabled(look);
    m_PostExp->setEnabled(look);
    m_PostCon->setEnabled(look);
    m_Rolloff->setEnabled(look);
    m_LutMode->setEnabled(look);

    // Input Transform pins the encode to DaVinci Wide Gamut / Intermediate — the hand-off
    // to the clip-level grade — so it isn't the user's to pick. An active LUT pins it too
    // (Film Look needs Cineon in, Custom Look needs Rec.709 Scene in) — and because what
    // comes OUT is then the LUT's own output convention, not the user's pick: a print
    // stock emits display-referred Rec.709 with its tone curve baked in, our built-in
    // looks emit the Rec.709 (Scene) they were authored in. The param genuinely has no
    // effect on the rendered curve while a LUT is on. Both overrides were
    // invisible until 2026-08-02: the dropdown stayed enabled showing a value the render
    // wasn't using, so picking a LUT read as the node silently blowing the contrast out
    // (github issue). Grey it AND spell out what is actually being rendered — a greyed
    // control still showing the old value is only half the truth.
    const bool lutOn = lutSelected();
    m_Encode->setEnabled(look && !lutOn);
    if (!look)
        m_EncodeNote->setValue("Pinned by Node Role: DaVinci Intermediate");
    else if (lutOn)
        m_EncodeNote->setValue(mode == 2 ? "Film LUT owns this: Cineon in, 709 out"
                                         : "Look LUT owns this: Rec.709 (Scene)");
    else if (mode != 0)
        m_EncodeNote->setValue("No LUT found - the encode above is used");
    else
        m_EncodeNote->setValue("");

    m_FilmLut->setEnabled(look && mode == 2);
    m_LookGroup->setEnabled(look && mode == 1);
    m_LookLut->setEnabled(look && mode == 1);
    m_LutMix->setEnabled(look && mode != 0);
}

// Rebuild the Look LUT dropdown to list only the currently selected group's LUTs.
void OneGrade::populateLookLut()
{
    int gi = 0;
    m_LookGroup->getValue(gi);
    m_LookLut->resetOptions();
    if (gi >= 0 && gi < (int)s_LookGroups.size() && !s_LookGroups[gi].second.empty())
        for (const auto& f : s_LookGroups[gi].second) m_LookLut->appendOption(f.first);
    else
        m_LookLut->appendOption("(none)");
}

// Presets are one-shot starting points down the "happy path": EVERY preset sets Camera
// to Rec.2100 PQ — the deliberately compressive smooth decode the plugin now defaults
// to (near-perfect highlight rolloff, smooth color, rich texture on log footage) — plus
// Balance, Density, Lift/Gamma/Gain, LUT and Trim. RAW and Output Encode are never
// touched. "None / Reset Look" returns the look params to neutral (Camera stays put).
// Names call out which LUT path a preset drives: Film Emulation = Resolve's print-film
// LUTs (Cineon path, swap stocks in Film Look LUT); Custom LUT = OneGrade's built-in
// looks (Rec.709 path, swap looks in Look LUT).
void OneGrade::applyPreset(int p)
{
    if (p == 1 || p == 2) { // Cinematic Film Emulation — 1: Kodak 2383 D60, 2: Fujifilm
                            // 3513DI D60. Cool highlights vs warm practicals, lift shadows
                            // off video-black, pull gain hard so highlights roll off into
                            // the print curve, bring brightness back post-LUT (values tuned
                            // on footage). Rolloff stays 0 — PQ is already the shoulder.
        // Fuji 3513DI ships only as DCI-P3 variants in Resolve's Film Looks (the file is
        // "DCI-P3 Fujifilm 3513DI D60.cube") — match the D60 name exactly so the
        // prefer-Rec709 tie-break can't drift to D55/D65.
        int film = filmLutIndex(p == 2 ? "fujifilm 3513di d60" : "kodak 2383 d60");
        if (film < 0) film = kodak2383Index();          // stock absent -> Kodak default
        m_Camera->setValue(11);     // Rec.2100 PQ / ST.2084
        m_OffTemp->setValue(-0.02);
        m_OffTint->setValue(0.01);
        m_Temp->setValue(-0.22);
        m_Tint->setValue(0.09);
        m_Density->setValue(0.10);
        m_Lift->setValue(0.11);
        m_Gamma->setValue(1.0);
        m_Gain->setValue(0.80);
        m_LutMode->setValue(2);
        m_FilmLut->setValue(film);
        m_LutMix->setValue(1.0);
        m_PostExp->setValue(0.55);
        m_PostCon->setValue(1.0);
        m_Rolloff->setValue(0.0);
    } else if (p == 3 || p == 4) {  // Custom LUT — 3: built-in Cinematic Landscape through
                            // the PQ decode with the user-validated cool offset (-0.112)
                            // plus a light trim (post-exp +0.023, contrast 0.965), tuned
                            // on footage 2026-07-21; 4: built-in Teal Orange with its own
                            // on-footage recipe (tuned 2026-07-16): softer cool offset,
                            // density backed off so the split-tone doesn't oversaturate,
                            // grade lifted and brightened into the look. Swap looks in
                            // the Look LUT dropdown below.
        int gi = 0, li = 0;
        const bool lut = findLookLut(p == 3 ? "onegrade cinematic landscape"
                                            : "onegrade teal orange", gi, li);
        const bool teal = (p == 4);
        m_Camera->setValue(11);     // Rec.2100 PQ / ST.2084
        m_OffTemp->setValue(teal ? -0.073 : -0.112);
        m_OffTint->setValue(0.0);
        m_Temp->setValue(0.0);
        m_Tint->setValue(0.0);
        m_Density->setValue(teal ? -0.15 : 0.0);
        m_Lift->setValue(teal ? 0.0 : 0.0);
        m_Gamma->setValue(teal ? 1.0 : 1.0);
        m_Gain->setValue(teal ? 1.0 : 1.0);
        if (lut) {
            m_LookGroup->setValue(gi);
            populateLookLut();
            m_LookLut->setValue(li);
            m_LutMode->setValue(1);
            m_LutMix->setValue(1.0);
        } else {                    // bundled LUT missing (shouldn't happen) -> no LUT
            m_LutMode->setValue(0);
            m_LutMix->setValue(1.0);
        }
        m_PostExp->setValue(teal ? 0.0 : 0.023);
        m_PostCon->setValue(teal ? 1.0 : 0.965);
        m_Rolloff->setValue(0.0);
    } else {                // None / Reset Look
        m_OffTemp->setValue(0.0);
        m_OffTint->setValue(0.0);
        m_Temp->setValue(0.0);
        m_Tint->setValue(0.0);
        m_Density->setValue(0.0);
        m_Lift->setValue(0.0);
        m_Gamma->setValue(1.0);
        m_Gain->setValue(1.0);
        m_LutMode->setValue(0);
        m_LutMix->setValue(1.0);
        m_PostExp->setValue(0.0);
        m_PostCon->setValue(1.0);
        m_Rolloff->setValue(0.0);
    }
}

void OneGrade::changedParam(const OFX::InstanceChangedArgs& p_Args, const std::string& p_ParamName)
{
    // Anything that can flip "is a LUT selected" has to re-run setEnabledness, since that
    // decides whether Output Encode is the user's to pick and what the note under it says.
    // LUT Mix is NOT in that set: it blends within the LUT's encode, it doesn't hand the
    // encode back at 0.
    if (p_ParamName == "lutMode" || p_ParamName == "lookLut" ||
        p_ParamName == "filmLut") setEnabledness();
    else if (p_ParamName == "lookGroup") { populateLookLut(); m_LookLut->setValue(0); setEnabledness(); }
    // Node Role: stamp the transform the role implies so the panel shows the truth.
    // Guarded like the preset — a project load must not overwrite stored values. Render
    // enforces the same rules regardless (see setupAndProcess), so a stale stored value
    // can't produce a wrong picture, only a stale-looking dropdown.
    else if (p_ParamName == "nodeRole") {
        if (p_Args.reason == OFX::eChangeUserEdit) {
            int role = 0; m_NodeRole->getValue(role);
            if (role == 1) {            // Input Transform -> hand off in DaVinci Intermediate
                m_Encode->setValue(4);
                m_LutMode->setValue(0);
                applyPreset(0);         // neutral look; leaves Camera and RAW alone
            } else if (role == 2) {     // Output Transform -> takes the pre-clip's DWG/DI
                m_Camera->setValue(1);
                m_RawExp->setValue(0.0);
                m_RawTemp->setValue(6500.0);
            }
        }
        setEnabledness();
    }
    // Experimental Auto Grade probe. Guarded on eChangeUserEdit like the preset: a project
    // load must never trigger a frame fetch.
    else if (p_ParamName == "probeAnalyze" && p_Args.reason == OFX::eChangeUserEdit) {
        probeAnalyze(p_Args.time);
    }
    else if (p_ParamName == "probeApply" && p_Args.reason == OFX::eChangeUserEdit) {
        applyAutoGrade(p_Args.time);
    }
    // Only on a real user edit — project load / plugin edits must not re-stamp the preset
    // over values the user has since tweaked.
    else if (p_ParamName == "preset" && p_Args.reason == OFX::eChangeUserEdit) {
        int p = 0; m_Preset->getValue(p);
        applyPreset(p);
        setEnabledness();   // the preset may have switched LUT Mode
    }
}

void OneGrade::render(const OFX::RenderArguments& p_Args)
{
    if ((m_DstClip->getPixelDepth() == OFX::eBitDepthFloat) && (m_DstClip->getPixelComponents() == OFX::ePixelComponentRGBA))
    {
        OneGradeProcessor proc(*this);
        setupAndProcess(proc, p_Args);
    }
    else
    {
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

void OneGrade::setupAndProcess(OneGradeProcessor& p_Proc, const OFX::RenderArguments& p_Args)
{
    std::unique_ptr<OFX::Image> dst(m_DstClip->fetchImage(p_Args.time));
    std::unique_ptr<OFX::Image> src(m_SrcClip->fetchImage(p_Args.time));

    if ((src->getPixelDepth() != dst->getPixelDepth()) || (src->getPixelComponents() != dst->getPixelComponents()))
        OFX::throwSuiteStatusException(kOfxStatErrValue);

    int role = 0, camera = 0, encode = 0, lutMode = 0;
    m_NodeRole->getValueAtTime(p_Args.time, role);
    m_Camera->getValueAtTime(p_Args.time, camera);
    m_Encode->getValueAtTime(p_Args.time, encode);
    m_LutMode->getValueAtTime(p_Args.time, lutMode);

    // Node Role is authoritative at render time, not just in the UI: a grade saved in one
    // role and switched to another (or loaded from an older project) must still render the
    // role it is set to, never a half-applied mix of the two.
    if (role == 1) {            // Input Transform: camera decode only, out to DWG/DI
        encode  = 4;
        lutMode = 0;
    } else if (role == 2) {     // Output Transform: input is the pre-clip node's DWG/DI
        camera = 1;
    }

    // Resolve the active LUT (path from mode) and load it (cached by path). This has to
    // happen BEFORE the encode coupling below, which is only legitimate when a LUT is
    // really going to be applied.
    int lookGroup = 0, lookLut = 0, filmLut = 0;
    m_LookGroup->getValueAtTime(p_Args.time, lookGroup);
    m_LookLut->getValueAtTime(p_Args.time, lookLut);
    m_FilmLut->getValueAtTime(p_Args.time, filmLut);
    const std::string lutPath = resolveLutPath(lutMode, lookGroup, lookLut, filmLut);
    const float lutMix = (float)m_LutMix->getValueAtTime(p_Args.time);
    const bool lutOk   = !lutPath.empty() && m_Lut.load(lutPath);

    // Couple the pre-LUT encoding to the LUT path so the two can't mismatch:
    //   Film Look LUTs require Cineon log input; Custom look LUTs use Rec.709.
    //
    // Keyed on the LUT RESOLVING, deliberately NOT on LUT Mix. Mix blends the un-LUTted
    // and LUTted picture *within the LUT's encode*; the encode is the domain the blend
    // happens in, not part of the blend. Tying it to Mix was tried (2026-08-02) and is
    // wrong: it puts a cliff at the first nudge off zero — 0.000 renders your delivery
    // curve, 0.001 snaps to the LUT's. Mix 0 with a LUT selected shows the LUT's working
    // curve, which is the honest preview of what the slider blends between.
    //
    // The `lutOk` gate (rather than lutMode alone) is the one real change: a .cube that
    // failed to load or a Film list that came up empty used to re-encode the picture
    // anyway, so a missing print stock rendered flat Cineon with no LUT and no error.
    if (lutOk) encode = (lutMode == 2) ? 3    // Film Look  -> Cineon Log
                                       : 0;   // Custom Look -> Rec.709 (Scene)
    // no LUT -> user's Output Encode is used unchanged

    float params[kParamCount];
    params[0] = (float)m_Temp->getValueAtTime(p_Args.time);
    params[1] = (float)m_Tint->getValueAtTime(p_Args.time);
    params[2] = (float)m_Density->getValueAtTime(p_Args.time);
    params[3] = (float)m_Lift->getValueAtTime(p_Args.time);
    params[4] = (float)m_Gamma->getValueAtTime(p_Args.time);
    params[5] = (float)m_Gain->getValueAtTime(p_Args.time);
    params[6] = (float)m_OffTemp->getValueAtTime(p_Args.time);
    params[7] = (float)m_OffTint->getValueAtTime(p_Args.time);
    params[8] = (float)m_PostExp->getValueAtTime(p_Args.time);
    params[9] = (float)m_PostCon->getValueAtTime(p_Args.time);
    params[10] = (float)m_RawExp->getValueAtTime(p_Args.time);
    params[11] = (float)m_RawTemp->getValueAtTime(p_Args.time);
    params[12] = (float)m_Rolloff->getValueAtTime(p_Args.time);

    // Force the params the role doesn't own to neutral, so the two nodes chain cleanly:
    // the look must be applied once (on the output node), the RAW/WB stage once (input).
    if (role == 1) {            // Input Transform: no look at all
        params[0]=0.f; params[1]=0.f; params[2]=0.f;              // temp, tint, density
        params[3]=0.f; params[4]=1.f; params[5]=1.f;              // lift, gamma, gain
        params[6]=0.f; params[7]=0.f;                             // offset temp/tint
        params[8]=0.f; params[9]=1.f; params[12]=0.f;             // trim exp/contrast/rolloff
    } else if (role == 2) {     // Output Transform: RAW stage already happened upstream
        params[10]=0.f; params[11]=6500.f;                        // rawExp, rawTemp
    }

    p_Proc.setDstImg(dst.get());
    p_Proc.setSrcImg(src.get());
    p_Proc.setGPURenderArgs(p_Args);
    p_Proc.setRenderWindow(p_Args.renderWindow);
    p_Proc.setParams(params, camera, encode);
    p_Proc.setLut(lutOk ? m_Lut.data.data() : nullptr, lutOk ? m_Lut.size : 0, lutOk ? lutMix : 0.0f);
    p_Proc.process();
}

////////////////////////////////////////////////////////////////////////////////

using namespace OFX;

OneGradeFactory::OneGradeFactory()
    : OFX::PluginFactoryHelper<OneGradeFactory>(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor)
{
}

void OneGradeFactory::describe(OFX::ImageEffectDescriptor& p_Desc)
{
    p_Desc.setLabels(kPluginName, kPluginName, kPluginName);
    p_Desc.setPluginGrouping(kPluginGrouping);
    p_Desc.setPluginDescription(kPluginDescription);

    p_Desc.addSupportedContext(eContextFilter);
    p_Desc.addSupportedContext(eContextGeneral);
    p_Desc.addSupportedBitDepth(eBitDepthFloat);

    p_Desc.setSingleInstance(false);
    p_Desc.setHostFrameThreading(false);
    p_Desc.setSupportsMultiResolution(kSupportsMultiResolution);
    p_Desc.setSupportsTiles(kSupportsTiles);
    p_Desc.setTemporalClipAccess(false);
    p_Desc.setRenderTwiceAlways(false);
    p_Desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);

    // Every backend advertised here must be one we actually compiled: the host picks a
    // GPU path from these flags and there is no CPU fallback once it does (see
    // ofxsProcessing.h process()). Claiming OpenCL while processImagesOpenCL() was
    // #ifdef'd out to an empty function meant AMD/Intel GPUs got an unwritten
    // destination buffer — a black frame, not a slow one.
#ifdef OFX_SUPPORTS_OPENCLRENDER
    p_Desc.setSupportsOpenCLBuffersRender(true);
#endif
#ifdef OFX_SUPPORTS_CUDARENDER
    p_Desc.setSupportsCudaRender(true);
    p_Desc.setSupportsCudaStream(true);
#endif
#ifdef __APPLE__
    p_Desc.setSupportsMetalRender(true);
#endif
}

static DoubleParamDescriptor* defineSlider(OFX::ImageEffectDescriptor& p_Desc, const char* name, const char* label,
                                           const char* hint, double def, double lo, double hi, double inc,
                                           GroupParamDescriptor* parent)
{
    DoubleParamDescriptor* param = p_Desc.defineDoubleParam(name);
    param->setLabels(label, label, label);
    param->setHint(hint);
    param->setDefault(def);
    param->setRange(lo, hi);
    param->setDisplayRange(lo, hi);
    param->setIncrement(inc);
    if (parent) param->setParent(*parent);
    return param;
}

void OneGradeFactory::describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum /*p_Context*/)
{
    ClipDescriptor* srcClip = p_Desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    ClipDescriptor* dstClip = p_Desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->setSupportsTiles(kSupportsTiles);

    PageParamDescriptor* page = p_Desc.definePageParam("Controls");

    // ---- 0. Role + Preset ----
    GroupParamDescriptor* gPreset = p_Desc.defineGroupParam("gPreset");
    gPreset->setLabels("0  Role / Preset", "0  Role / Preset", "0  Role / Preset");

    // Node Role splits the pipeline across Resolve's group grading levels. See
    // OneGrade::setEnabledness / setupAndProcess — the role is enforced at render.
    ChoiceParamDescriptor* role = p_Desc.defineChoiceParam("nodeRole");
    role->setLabels("Node Role", "Node Role", "Node Role");
    role->setHint("Which part of the pipeline this node does. Full Grade (the default) does everything in one node. The other two split it across Resolve's group grading levels so a whole group shares one setup: put an Input Transform node in the Group Pre-Clip graph (camera decode only, handed off in DaVinci Intermediate), grade your shots normally at the Clip level, then put an Output Transform node in the Group Post-Clip graph (look, LUT, trim and the delivery encode). Chained, the two match a single Full Grade node. Controls the role doesn't own are greyed out and forced neutral at render, so the look is never applied twice.");
    role->appendOption("Full Grade (single node)");
    role->appendOption("Input Transform (Group Pre-Clip)");
    role->appendOption("Output Transform (Group Post-Clip)");
    role->setDefault(0);
    role->setParent(*gPreset);
    page->addChild(*role);

    ChoiceParamDescriptor* preset = p_Desc.defineChoiceParam("preset");
    preset->setLabels("Preset", "Preset", "Preset");
    preset->setHint("One-click starting points on the happy path: every preset sets Camera to Rec.2100 PQ (the smooth decode, also the default) plus Balance, Density, Lift/Gamma/Gain, LUT and Trim — every slider stays live to tweak per clip; RAW and Output Encode are never touched. Film Emulation presets drive Resolve's print-film stocks (swap in Film Look LUT); Custom LUT presets drive OneGrade's built-in looks, shipped inside the plugin (swap in Look LUT; six looks available). Trim any LUT with LUT Mix. None / Reset Look returns the look params to neutral (Camera stays put).");
    preset->appendOption("None / Reset Look");
    preset->appendOption("Cinematic Film Emulation (Kodak 2383 D60)");
    preset->appendOption("Cinematic Film Emulation (Fujifilm 3513DI D60)");
    preset->appendOption("Custom LUT - Cinematic Landscape");
    preset->appendOption("Custom LUT - Teal Orange");
    preset->setDefault(0);
    preset->setParent(*gPreset);
    page->addChild(*preset);

    // ---- 1. Input Transform (CST) ----
    GroupParamDescriptor* gInput = p_Desc.defineGroupParam("gInput");
    gInput->setLabels("1  Input Transform", "1  Input Transform", "1  Input Transform");
    ChoiceParamDescriptor* cam = p_Desc.defineChoiceParam("camera");
    cam->setLabels("Camera", "Camera", "Camera");
    cam->setHint("Source camera log/gamut, decoded to DaVinci Wide Gamut linear working space. The default, Rec.2100 PQ, is NOT a camera match: it's a deliberately compressive smooth decode that flatters log footage (near-perfect highlight rolloff, smooth color) — the happy path all presets build on. For a colorimetric starting point instead, pick the real camera: e.g. Blackmagic Gen 5 Film for Pocket/URSA/Pyxis clips, DaVinci Wide Gamut / Intermediate for clips already in that space.");
    cam->appendOption("Blackmagic Gen 5 Film");
    // Same space, same words as Output Encode's option 4 — this pair used to read
    // "Blackmagic (DWG/DI)" here and "DaVinci Intermediate" there, which looked like two
    // different things (github issue).
    cam->appendOption("DaVinci Wide Gamut / Intermediate");
    cam->appendOption("Sony S-Log3");
    cam->appendOption("ARRI LogC3");
    cam->appendOption("ARRI LogC4");
    cam->appendOption("Canon Log3");
    cam->appendOption("RED Log3G10");
    cam->appendOption("DJI D-Log");
    cam->appendOption("Fuji F-Log2");
    cam->appendOption("Panasonic V-Log");
    cam->appendOption("Rec.2100 HLG (HDR)");
    cam->appendOption("Rec.2100 PQ / ST.2084 (HDR)");
    cam->setDefault(11);    // Rec.2100 PQ — the creative "smooth decode" default (see hint)
    cam->setParent(*gInput);
    page->addChild(*cam);

    // RAW-tab analogs: exposure + white balance applied on scene-linear before the CST,
    // so the Camera RAW tab can be left at its defaults (simplifies the round-trip).
    page->addChild(*defineSlider(p_Desc, "rawExp", "RAW Exposure", "Exposure in stops on scene light, before the CST. Matches the Camera RAW tab's Exposure control.", 0.0, -5.0, 5.0, 0.01, gInput));
    page->addChild(*defineSlider(p_Desc, "rawTemp", "RAW Temperature", "White-balance color temperature in Kelvin (chromatic adaptation). Raise = warmer, lower = cooler. 6500 = neutral. Approximates the Camera RAW tab's Temp (not byte-exact: no sensor metadata reaches the plugin).", 6500.0, 2000.0, 15000.0, 10.0, gInput));

    // ---- 2. Balance ----  (white balance in linear; watch the vectorscope while adjusting)
    GroupParamDescriptor* gBal = p_Desc.defineGroupParam("gBalance");
    gBal->setLabels("2  Balance", "2  Balance", "2  Balance");
    {
        StringParamDescriptor* tip = p_Desc.defineStringParam("balanceTip");
        tip->setLabels("Tip", "Tip", "Tip");
        tip->setStringType(eStringTypeLabel);
        tip->setDefault("Open the Vectorscope while adjusting. Offset = even balance across all tones; Gain = neutral highlights.");
        tip->setEnabled(false);
        tip->setParent(*gBal);
        page->addChild(*tip);
    }
    // Offset balance (additive) — shifts every tone's chroma evenly; best for stubborn casts.
    page->addChild(*defineSlider(p_Desc, "offTemp", "Offset Temp", "Warm (+) / cool (-) balance, additive (Offset wheel). Even across all tones.", 0.0, -1.0, 1.0, 0.001, gBal));
    page->addChild(*defineSlider(p_Desc, "offTint", "Offset Tint", "Green (+) / magenta (-) balance, additive (Offset wheel). Even across all tones.", 0.0, -1.0, 1.0, 0.001, gBal));
    // Gain balance (multiplicative) — keeps highlights neutral.
    page->addChild(*defineSlider(p_Desc, "temp", "Gain Temp", "Warm (+) / cool (-) balance, multiplicative (Gain wheel). Neutral highlights.", 0.0, -1.0, 1.0, 0.001, gBal));
    page->addChild(*defineSlider(p_Desc, "tint", "Gain Tint", "Green (+) / magenta (-) balance, multiplicative (Gain wheel). Neutral highlights.", 0.0, -1.0, 1.0, 0.001, gBal));

    // ---- 3. Density ----  (HSV saturation gain — the green-of-Gain-in-HSV trick)
    GroupParamDescriptor* gDen = p_Desc.defineGroupParam("gDensity");
    gDen->setLabels("3  Density", "3  Density", "3  Density");
    page->addChild(*defineSlider(p_Desc, "density", "Density", "Color density: saturation gain in HSV (the green-channel-of-Gain-in-HSV trick). -1 = grayscale, +1 = double saturation.", 0.0, -1.0, 1.0, 0.001, gDen));

    // ---- 4. Exposure (Lift / Gamma / Gain) ----
    GroupParamDescriptor* gExp = p_Desc.defineGroupParam("gExposure");
    gExp->setLabels("4  Exposure (Lift / Gamma / Gain)", "4  Exposure", "4  Exposure");
    page->addChild(*defineSlider(p_Desc, "lift",  "Lift",  "Raise/lower shadows (offset)", 0.0, -0.5, 0.5, 0.001, gExp));
    page->addChild(*defineSlider(p_Desc, "gamma", "Gamma", "Midtone brightness (power)",    1.0,  0.2, 3.0, 0.001, gExp));
    page->addChild(*defineSlider(p_Desc, "gain",  "Gain",  "Highlights / overall (multiply)", 1.0, 0.0, 3.0, 0.001, gExp));

    // ---- 5. Output ----
    GroupParamDescriptor* gOut = p_Desc.defineGroupParam("gOutput");
    gOut->setLabels("5  Output", "5  Output", "5  Output");
    ChoiceParamDescriptor* enc = p_Desc.defineChoiceParam("outEncode");
    enc->setLabels("Output Encode", "Output Encode", "Output Encode");
    enc->setHint("Your delivery curve — the transfer function baked into the render. Rec.709 (Gamma 2.2) is the default: it matches what web/streaming platforms like YouTube assume, where most exports end up. Pick Rec.709 (Gamma 2.4) for broadcast/reference delivery, or Rec.709 (Scene) for a scene-referred hand-off. This is NOT the same setting as the project's Timeline Color Space and should not be changed to match it — on macOS the timeline must be Rec.709 (Scene) so Resolve's viewer agrees with QuickTime/YouTube, whatever you deliver in (see Setup / Help). The Lift/Gamma/Gain wheels grade in whichever Rec.709 curve you pick, so a wheel move reads linearly in that curve. An active LUT takes this over and greys it out, because the LUT can only be fed the curve it was authored for (Film Look -> Cineon, Custom Look -> Rec.709 Scene) — the 'In effect' line below always names what is actually being rendered. LUT Mix does not hand it back: Mix blends the LUT in and out within that curve, so Mix 0 still previews the curve the blend happens in. Set LUT Mode to None to get the choice back.");
    enc->appendOption("Rec.709 (Scene)");
    enc->appendOption("Rec.709 (Gamma 2.2)");
    enc->appendOption("Rec.709 (Gamma 2.4)");
    enc->appendOption("Cineon Log (feed film LUT)");
    enc->appendOption("DaVinci Wide Gamut / Intermediate");   // same wording as Camera option 1
    enc->appendOption("Linear");
    enc->setDefault(1);   // Rec.709 (Gamma 2.2) — web/YouTube delivery, where most exports land
    enc->setParent(*gOut);
    page->addChild(*enc);

    // Both Node Role and an active LUT override the encode above. Greying the dropdown
    // isn't enough on its own — a greyed control still shows the *old* value, so the panel
    // keeps reading "Rec.709 (Gamma 2.2)" while the render uses Rec.709 (Scene). This line
    // states what is actually being rendered, and is empty when nothing is overridden.
    // Kept short and ASCII: the panel truncates labels around ~45 characters.
    {
        StringParamDescriptor* note = p_Desc.defineStringParam("encodeNote");
        note->setLabels("In effect", "In effect", "In effect");
        note->setStringType(eStringTypeLabel);
        note->setDefault("");
        note->setHint("What the node is actually encoding to. Blank when the Output Encode above is what's used; otherwise it names the override (an active LUT, or the Node Role) and why.");
        note->setEnabled(false);
        note->setParent(*gOut);
        page->addChild(*note);
    }

    // ---- 6. Look / Film LUT ----
    scanLuts();
    GroupParamDescriptor* gLut = p_Desc.defineGroupParam("gLut");
    gLut->setLabels("6  Look / Film LUT", "6  Look / Film LUT", "6  Look / Film LUT");

    ChoiceParamDescriptor* lutMode = p_Desc.defineChoiceParam("lutMode");
    lutMode->setLabels("LUT Mode", "LUT Mode", "LUT Mode");
    lutMode->setHint("None; a Custom Look LUT (Rec.709 path); or a built-in Film Look (Cineon path). Film and Look are mutually exclusive.");
    lutMode->appendOption("None");
    lutMode->appendOption("Custom Look LUT");
    lutMode->appendOption("Film Look (built-in)");
    lutMode->setDefault(0);
    lutMode->setParent(*gLut);
    page->addChild(*lutMode);

    ChoiceParamDescriptor* filmLut = p_Desc.defineChoiceParam("filmLut");
    filmLut->setLabels("Film Look LUT", "Film Look LUT", "Film Look LUT");
    filmLut->setHint("Built-in film-look LUT (Resolve's Film Looks). Active when LUT Mode = Film Look; encodes to Cineon automatically.");
    if (s_FilmLuts.empty()) filmLut->appendOption("(no .cube LUTs found)");
    else for (const auto& fl : s_FilmLuts) filmLut->appendOption(fl.first);
    filmLut->setDefault(kodak2383Index());   // Kodak 2383 D60, Rec.709 variant preferred
    filmLut->setParent(*gLut);
    page->addChild(*filmLut);

    // Look LUT: two-level cascade (Group -> LUT) to tame the big master-folder list.
    ChoiceParamDescriptor* lookGroup = p_Desc.defineChoiceParam("lookGroup");
    lookGroup->setLabels("Look LUT Group", "Look LUT Group", "Look LUT Group");
    lookGroup->setHint("LUT category (top-level folder in Resolve's LUT directory). Active when LUT Mode = Custom Look.");
    if (s_LookGroups.empty()) lookGroup->appendOption("(no .cube LUTs found)");
    else for (const auto& g : s_LookGroups) lookGroup->appendOption(g.first);
    lookGroup->setDefault(0);
    lookGroup->setParent(*gLut);
    page->addChild(*lookGroup);

    ChoiceParamDescriptor* lookLut = p_Desc.defineChoiceParam("lookLut");
    lookLut->setLabels("Look LUT", "Look LUT", "Look LUT");
    lookLut->setHint("LUT within the selected group. Active when LUT Mode = Custom Look; applied on the Rec.709 path.");
    if (!s_LookGroups.empty() && !s_LookGroups[0].second.empty())
        for (const auto& f : s_LookGroups[0].second) lookLut->appendOption(f.first);   // first group; repopulated per instance
    else
        lookLut->appendOption("(none)");
    lookLut->setDefault(0);
    lookLut->setParent(*gLut);
    page->addChild(*lookLut);

    page->addChild(*defineSlider(p_Desc, "lutMix", "LUT Mix", "LUT output level / strength (like Key Output). 0 = off, 1 = full.", 1.0, 0.0, 1.0, 0.001, gLut));

    // ---- 7. Trim (after LUT) ----  final display-space trims on top of the look/LUT
    GroupParamDescriptor* gTrim = p_Desc.defineGroupParam("gTrim");
    gTrim->setLabels("7  Trim (after LUT)", "7  Trim (after LUT)", "7  Trim (after LUT)");
    page->addChild(*defineSlider(p_Desc, "postExp", "Exposure", "Post-LUT exposure trim in stops. Bring brightness back after a film-emulation LUT.", 0.0, -3.0, 3.0, 0.01, gTrim));
    page->addChild(*defineSlider(p_Desc, "postCon", "Contrast", "Post-LUT contrast trim about mid (0.5), applied after the LUT.", 1.0, 0.0, 2.0, 0.001, gTrim));
    page->addChild(*defineSlider(p_Desc, "rolloff", "Highlight Rolloff", "Soft-clips bright highlights per channel so lamps/speculars roll off to white instead of clipping to a flat neon patch. Higher = earlier, stronger shoulder. Only active on display-referred output (Rec.709 encodes or any LUT path).", 0.0, 0.0, 1.0, 0.001, gTrim));

    // ---- 8. Setup / Help ----
    GroupParamDescriptor* gHelp = p_Desc.defineGroupParam("gHelp");
    gHelp->setLabels("8  Setup / Help", "8  Setup / Help", "8  Setup / Help");
    gHelp->setOpen(false);
    // The panel truncates these strings, so `text` must stay short enough to read at the
    // default OpenFX panel width (~45 chars) and carry the instruction on its own; the
    // full explanation goes in the hint, which the host shows on hover.
    auto helpLine = [&](const char* name, const char* label, const char* text,
                        const char* hint = nullptr) {
        StringParamDescriptor* s = p_Desc.defineStringParam(name);
        s->setLabels(label, label, label);
        s->setStringType(eStringTypeLabel);
        s->setDefault(text);
        s->setHint(hint ? hint : text);
        s->setEnabled(false);
        s->setParent(*gHelp);
        page->addChild(*s);
    };
    helpLine("help0", "Requires", "Project > Color Management, NOT color managed:");
    helpLine("help1", "Color Science", "DaVinci YRGB");
    helpLine("help2", "Timeline Color Space", "Rec.709 (Scene) - required on macOS",
             "Rec.709 (Scene). On macOS this is REQUIRED for Resolve's viewer to match QuickTime and YouTube: macOS reads a Rec.709 tag via the scene OETF, and this is the only timeline setting under which the viewer agrees. It is NOT tied to Output Encode - leave that on your delivery curve. Windows/Linux: unverified, start by matching Output Encode.");
    helpLine("help3", "Output Color Space", "Same as Timeline");
    helpLine("help4", "macOS Preference", "'Use Mac display color profiles' = ON",
             "Preferences > General > 'Use Mac display color profiles for viewers' ON. That enables its sub-option 'Viewers match QuickTime player when using Rec.709 Scene', which only engages on a Rec.709 Scene timeline - see Timeline Color Space above.");
    helpLine("help5", "Clips", "Camera raw/log defaults - no CST or LUT first",
             "Leave clips at their camera raw/log defaults - no input CST or LUT before this node. OneGrade does the camera transform itself.");
    helpLine("help6", "Camera control", "Default Rec.2100 PQ = smooth decode",
             "Default Rec.2100 PQ is the creative smooth decode the presets use, not a camera match. Pick your camera's real log for a colorimetric transform instead.");
    helpLine("help7", "Output Encode", "Delivery curve - NOT the Timeline setting",
             "Your DELIVERY curve, baked into the render. Independent of Timeline Color Space - do NOT change it to match. Rec.709 (Gamma 2.2) is the default (web/YouTube); Gamma 2.4 for broadcast; Rec.709 (Scene) for a scene-referred hand-off.");
    helpLine("help8", "Monitor", "Calibrate; check on a second screen",
             "Calibrate your monitor and have Resolve show your delivery space; check the grade on a second screen before committing.");

    // ---- 9. Auto Grade (experimental probe) ----
    // Step 1 of the Auto Grade design: this group exists only to answer "can a button read
    // the frame?". It changes nothing about the picture — it reads pixels and prints what
    // it found. Collapsed by default; remove the whole group if the answer turns out to be
    // no, or grow it into the real analysis if yes. See probeAnalyze().
    GroupParamDescriptor* gAuto = p_Desc.defineGroupParam("gAuto");
    gAuto->setLabels("9  Auto Grade (experimental)", "9  Auto Grade", "9  Auto Grade");
    gAuto->setOpen(false);
    {
        PushButtonParamDescriptor* btn = p_Desc.definePushButtonParam("probeAnalyze");
        btn->setLabels("Analyze Frame", "Analyze Frame", "Analyze Frame");
        btn->setHint("Experimental. Reads the current frame at the node's input and reports what it found below. Does not change the picture or any slider — this exists to prove the plugin can sample the image before any auto-grade feature is built on top of it.");
        btn->setParent(*gAuto);
        page->addChild(*btn);

        auto probeLine = [&](const char* name, const char* label, const char* hint) {
            StringParamDescriptor* s = p_Desc.defineStringParam(name);
            s->setLabels(label, label, label);
            s->setStringType(eStringTypeLabel);
            s->setDefault("(not run)");
            s->setHint(hint);
            s->setEnabled(false);
            s->setParent(*gAuto);
            page->addChild(*s);
        };
        probeLine("probeStatus", "Result",
                  "Whether pixels came back, the frame size, the sampling step and how many samples were read. 'ALL ZERO' means the host handed over a buffer but it was empty - a different answer from a black shot.");
        probeLine("probeScene", "Scene",
                  "Measured on scene light (XYZ luminance after the camera decode, before any grade). Y50 is the median. 'key' is how far that median sits from 18% mid-gray in stops - the exposure correction the shot is asking for, since RAW Exposure is a linear gain in stops. 'DR' is p1 to p99 in stops: how much usable range the shot actually has.");
        probeLine("probeDisplay", "Display",
                  "Luma percentiles after the full pipeline at NEUTRAL grade, in your current Output Encode: 1st, 50th, 99th. This is the space Lift/Gamma/Gain work in, so these are the numbers a black-point or highlight target would be set against. No LUT is applied.");
        probeLine("probeShape", "Shape",
                  "'hot' is the share above 1.0 in display - bright, but it pulls back fine if the range was captured. 'pin' is the share of the frame sitting ON the source ceiling, with that ceiling's code value after the @ - this is clipping at the sensor, where no exposure move brings anything back. Measured against the clip's own maximum rather than 1.0, because log formats don't all reach the top of the code range (Blackmagic peaks near 0.75). A low pin % means the highlights roll off and the range was captured; a high one means they are stacked on the ceiling and gone. 'sat' is mean HSV saturation over mid-tones only, which is what a Density move would act on.");
        PushButtonParamDescriptor* apply = p_Desc.definePushButtonParam("probeApply");
        apply->setLabels("Auto Grade", "Auto Grade", "Auto Grade");
        apply->setHint("Experimental. Analyses the frame, applies the Cinematic Film Emulation look, and sets Gain from the measured key. Fitted to hand-graded shots rather than to a textbook target: a bright shot gets Gain pulled down, a dark one is left at the preset - deliberately, since a low-key shot is meant to sit low. Everything it writes is an ordinary slider value you can drag afterwards.");
        apply->setParent(*gAuto);
        page->addChild(*apply);

        page->addChild(*defineSlider(p_Desc, "autoBias", "Bias",
            "Which way Auto Grade leans when you press it. 0 uses the measured result as-is. Negative tames the highlights - more Highlight Rolloff, shadows sitting lower - for a shot with blown windows or hot speculars. Positive opens the image up - more Lift, less rolloff - for something dark you want to breathe. Gain is never touched by this: it is set from the measured exposure and that part is not a matter of taste. Set this BEFORE pressing Auto Grade; it is an input to the button, not a live control.",
            0.0, -1.0, 1.0, 0.01, gAuto));

        probeLine("probePeak", "Peak",
                  "p99.9 in display, and how far it runs past p99. A compact blown specular - a window, a lamp - sits far above the bulk of the highlights and gives a high multiplier; a broad bright field like sunlit sand sits just above it. This is the shape of the top end rather than its size, which is what decides whether a shot wants Highlight Rolloff.");
        probeLine("probeSubject", "Subject",
                  "The same exposure question asked of skin-toned pixels only, plus what share of the frame matched. Frame-median exposure is subject-blind: a dark interior drags the median down and asks for a push that would blow the windows. Where the two keys disagree, the frame median is the wrong one. Note the mask cannot tell skin from sand - a high coverage % on a landscape means it matched the scene, not a face.");
        probeLine("probeApplied", "Applied",
                  "What the Auto Grade button last wrote, and the measurement it came from. Blank until you press it. Analyze Frame never changes anything; only Auto Grade does.");
    }
}

ImageEffect* OneGradeFactory::createInstance(OfxImageEffectHandle p_Handle, ContextEnum /*p_Context*/)
{
    return new OneGrade(p_Handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& p_FactoryArray)
{
    static OneGradeFactory oneGradePlugin;
    p_FactoryArray.push_back(&oneGradePlugin);
}
