// PowerGrade — cross-platform OpenFX color grade plugin for DaVinci Resolve.
// SPDX-License-Identifier: BSD-3-Clause

#include "PowerGrade.h"
#include "PowerGradePipeline.h"
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

#define kPluginName        "PowerGrade"
#define kPluginGrouping    "Power Grade"
#define kPluginDescription "One-node camera CST + balance + density + exposure + output encode. " \
                           "Set Output Encode to Cineon Log to feed a film-look LUT node."
#define kPluginIdentifier  "com.mattgrdinic.PowerGrade"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

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
    fs::path contents = fs::path(bin).parent_path().parent_path();   // Contents/<arch>/PowerGrade.ofx -> Contents
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
    if (!builtin.empty()) s_LookGroups.emplace_back("PowerGrade (built-in)", builtin);

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

////////////////////////////////////////////////////////////////////////////////

class PowerGradeProcessor : public OFX::ImageProcessor
{
public:
    explicit PowerGradeProcessor(OFX::ImageEffect& p_Instance);

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

PowerGradeProcessor::PowerGradeProcessor(OFX::ImageEffect& p_Instance)
    : OFX::ImageProcessor(p_Instance)
{
    for (int i = 0; i < kParamCount; ++i) _params[i] = 0.0f;
}

#ifdef OFX_SUPPORTS_CUDARENDER
extern void RunCudaKernel(void* p_Stream, int p_Width, int p_Height, const float* p_Params, int p_Camera, int p_Encode, const float* p_Lut, int p_LutSize, float p_LutMix, const float* p_Input, float* p_Output);
#endif

void PowerGradeProcessor::processImagesCuda()
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

void PowerGradeProcessor::processImagesMetal()
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

void PowerGradeProcessor::processImagesOpenCL()
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

void PowerGradeProcessor::multiThreadProcessImages(OfxRectI p_ProcWindow)
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
                pg::process(_camera, _encode, _params, srcPix[0], srcPix[1], srcPix[2],
                            dstPix[0], dstPix[1], dstPix[2]);
                if (_lut && _lutSize >= 2 && _lutMix > 0.0f)
                    pg::apply_lut(_lut, _lutSize, _lutMix, dstPix[0], dstPix[1], dstPix[2]);
                pg::apply_trim(_params[8], _params[9], dstPix[0], dstPix[1], dstPix[2]);  // post-LUT trim
                if (_params[12] > 0.0f && (_encode <= 2 || (_lut && _lutSize >= 2 && _lutMix > 0.0f)))
                    for (int c = 0; c < 3; ++c) dstPix[c] = pg::softclip(dstPix[c], _params[12]);  // highlight roll-off (display-referred only)
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

void PowerGradeProcessor::setSrcImg(OFX::Image* p_SrcImg) { _srcImg = p_SrcImg; }

void PowerGradeProcessor::setParams(const float p_Params[kParamCount], int p_Camera, int p_Encode)
{
    std::memcpy(_params, p_Params, sizeof(float) * kParamCount);
    _camera = p_Camera;
    _encode = p_Encode;
}

void PowerGradeProcessor::setLut(const float* p_Lut, int p_LutSize, float p_LutMix)
{
    _lut = p_Lut;
    _lutSize = p_LutSize;
    _lutMix = p_LutMix;
}

////////////////////////////////////////////////////////////////////////////////

class PowerGrade : public OFX::ImageEffect
{
public:
    explicit PowerGrade(OfxImageEffectHandle p_Handle);
    virtual void render(const OFX::RenderArguments& p_Args);
    virtual void changedParam(const OFX::InstanceChangedArgs& p_Args, const std::string& p_ParamName);
    void setEnabledness();
    void populateLookLut();     // repopulate the Look LUT dropdown for the current group
    void applyPreset(int p);    // set the look params (density/LGG/LUT/trim) to a starting point
    void setupAndProcess(PowerGradeProcessor& p_Proc, const OFX::RenderArguments& p_Args);

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
    OFX::DoubleParam* m_PostExp;
    OFX::DoubleParam* m_PostCon;
    OFX::DoubleParam* m_Rolloff;

    OFX::ChoiceParam* m_LutMode;    // 0 none, 1 custom look, 2 film-look built-in
    OFX::ChoiceParam* m_FilmLut;
    OFX::ChoiceParam* m_LookGroup;
    OFX::ChoiceParam* m_LookLut;
    OFX::DoubleParam* m_LutMix;
    CubeLUT           m_Lut;        // cached loaded LUT
};

PowerGrade::PowerGrade(OfxImageEffectHandle p_Handle)
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
    m_PostExp = fetchDoubleParam("postExp");
    m_PostCon = fetchDoubleParam("postCon");
    m_Rolloff = fetchDoubleParam("rolloff");

    m_LutMode  = fetchChoiceParam("lutMode");
    m_FilmLut  = fetchChoiceParam("filmLut");
    m_LookGroup= fetchChoiceParam("lookGroup");
    m_LookLut  = fetchChoiceParam("lookLut");
    m_LutMix   = fetchDoubleParam("lutMix");

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
void PowerGrade::setEnabledness()
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

    // Input Transform pins the encode to DaVinci Intermediate — the hand-off to the
    // clip-level grade — so it isn't the user's to pick.
    m_Encode->setEnabled(look);

    m_FilmLut->setEnabled(look && mode == 2);
    m_LookGroup->setEnabled(look && mode == 1);
    m_LookLut->setEnabled(look && mode == 1);
    m_LutMix->setEnabled(look && mode != 0);
}

// Rebuild the Look LUT dropdown to list only the currently selected group's LUTs.
void PowerGrade::populateLookLut()
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
// LUTs (Cineon path, swap stocks in Film Look LUT); Custom LUT = PowerGrade's built-in
// looks (Rec.709 path, swap looks in Look LUT).
void PowerGrade::applyPreset(int p)
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
        const bool lut = findLookLut(p == 3 ? "powergrade cinematic landscape"
                                            : "powergrade teal orange", gi, li);
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

void PowerGrade::changedParam(const OFX::InstanceChangedArgs& p_Args, const std::string& p_ParamName)
{
    if (p_ParamName == "lutMode") setEnabledness();
    else if (p_ParamName == "lookGroup") { populateLookLut(); m_LookLut->setValue(0); }
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
    // Only on a real user edit — project load / plugin edits must not re-stamp the preset
    // over values the user has since tweaked.
    else if (p_ParamName == "preset" && p_Args.reason == OFX::eChangeUserEdit) {
        int p = 0; m_Preset->getValue(p);
        applyPreset(p);
        setEnabledness();   // the preset may have switched LUT Mode
    }
}

void PowerGrade::render(const OFX::RenderArguments& p_Args)
{
    if ((m_DstClip->getPixelDepth() == OFX::eBitDepthFloat) && (m_DstClip->getPixelComponents() == OFX::ePixelComponentRGBA))
    {
        PowerGradeProcessor proc(*this);
        setupAndProcess(proc, p_Args);
    }
    else
    {
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

void PowerGrade::setupAndProcess(PowerGradeProcessor& p_Proc, const OFX::RenderArguments& p_Args)
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

    // Couple the pre-LUT encoding to the LUT path so the two can't mismatch:
    //   Film Look LUTs require Cineon log input; Custom look LUTs use Rec.709.
    if (lutMode == 2)      encode = 3;   // Film Look  -> Cineon Log
    else if (lutMode == 1) encode = 0;   // Custom Look -> Rec.709 (Scene)
    // lutMode == 0 (None) -> user's Output Encode is used unchanged

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

    // Resolve the active LUT (path from mode) and load it (cached by path).
    std::string lutPath;
    if (lutMode == 1) {
        int gi = 0, li = 0;
        m_LookGroup->getValueAtTime(p_Args.time, gi);
        m_LookLut->getValueAtTime(p_Args.time, li);
        if (gi >= 0 && gi < (int)s_LookGroups.size()) {
            const LutList& luts = s_LookGroups[gi].second;
            if (li >= 0 && li < (int)luts.size()) lutPath = luts[li].second;
        }
    } else if (lutMode == 2) {
        int idx = 0; m_FilmLut->getValueAtTime(p_Args.time, idx);
        if (idx >= 0 && idx < (int)s_FilmLuts.size()) lutPath = s_FilmLuts[idx].second;
    }
    bool lutOk = (lutMode != 0) && !lutPath.empty() && m_Lut.load(lutPath);
    float lutMix = (float)m_LutMix->getValueAtTime(p_Args.time);

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

PowerGradeFactory::PowerGradeFactory()
    : OFX::PluginFactoryHelper<PowerGradeFactory>(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor)
{
}

void PowerGradeFactory::describe(OFX::ImageEffectDescriptor& p_Desc)
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

void PowerGradeFactory::describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum /*p_Context*/)
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
    // PowerGrade::setEnabledness / setupAndProcess — the role is enforced at render.
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
    preset->setHint("One-click starting points on the happy path: every preset sets Camera to Rec.2100 PQ (the smooth decode, also the default) plus Balance, Density, Lift/Gamma/Gain, LUT and Trim — every slider stays live to tweak per clip; RAW and Output Encode are never touched. Film Emulation presets drive Resolve's print-film stocks (swap in Film Look LUT); Custom LUT presets drive PowerGrade's built-in looks, shipped inside the plugin (swap in Look LUT; six looks available). Trim any LUT with LUT Mix. None / Reset Look returns the look params to neutral (Camera stays put).");
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
    cam->setHint("Source camera log/gamut, decoded to DaVinci Wide Gamut linear working space. The default, Rec.2100 PQ, is NOT a camera match: it's a deliberately compressive smooth decode that flatters log footage (near-perfect highlight rolloff, smooth color) — the happy path all presets build on. For a colorimetric starting point instead, pick the real camera: e.g. Blackmagic Gen 5 Film for Pocket/URSA/Pyxis clips, Blackmagic (DWG/DI) for clips already in DaVinci Wide Gamut / Intermediate.");
    cam->appendOption("Blackmagic Gen 5 Film");
    cam->appendOption("Blackmagic (DWG/DI)");
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
    enc->setHint("Your delivery curve — the transfer function baked into the render. Rec.709 (Gamma 2.2) is the default: it matches what web/streaming platforms like YouTube assume, where most exports end up. Pick Rec.709 (Gamma 2.4) for broadcast/reference delivery, or Rec.709 (Scene) for a scene-referred hand-off. This is NOT the same setting as the project's Timeline Color Space and should not be changed to match it — on macOS the timeline must be Rec.709 (Scene) so Resolve's viewer agrees with QuickTime/YouTube, whatever you deliver in (see Setup / Help). The Lift/Gamma/Gain wheels grade in whichever Rec.709 curve you pick, so a wheel move reads linearly in that curve. Applies when LUT Mode = None; a LUT auto-sets it (Film Look -> Cineon, Custom Look -> Rec.709 Scene).");
    enc->appendOption("Rec.709 (Scene)");
    enc->appendOption("Rec.709 (Gamma 2.2)");
    enc->appendOption("Rec.709 (Gamma 2.4)");
    enc->appendOption("Cineon Log (feed film LUT)");
    enc->appendOption("DaVinci Intermediate");
    enc->appendOption("Linear");
    enc->setDefault(1);   // Rec.709 (Gamma 2.2) — web/YouTube delivery, where most exports land
    enc->setParent(*gOut);
    page->addChild(*enc);

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
             "Leave clips at their camera raw/log defaults - no input CST or LUT before this node. PowerGrade does the camera transform itself.");
    helpLine("help6", "Camera control", "Default Rec.2100 PQ = smooth decode",
             "Default Rec.2100 PQ is the creative smooth decode the presets use, not a camera match. Pick your camera's real log for a colorimetric transform instead.");
    helpLine("help7", "Output Encode", "Delivery curve - NOT the Timeline setting",
             "Your DELIVERY curve, baked into the render. Independent of Timeline Color Space - do NOT change it to match. Rec.709 (Gamma 2.2) is the default (web/YouTube); Gamma 2.4 for broadcast; Rec.709 (Scene) for a scene-referred hand-off.");
    helpLine("help8", "Monitor", "Calibrate; check on a second screen",
             "Calibrate your monitor and have Resolve show your delivery space; check the grade on a second screen before committing.");
}

ImageEffect* PowerGradeFactory::createInstance(OfxImageEffectHandle p_Handle, ContextEnum /*p_Context*/)
{
    return new PowerGrade(p_Handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& p_FactoryArray)
{
    static PowerGradeFactory powerGradePlugin;
    p_FactoryArray.push_back(&powerGradePlugin);
}
