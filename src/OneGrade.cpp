// OneGrade — cross-platform OpenFX color grade plugin for DaVinci Resolve.
// Copyright (C) 2026 Matthew Grdinic
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OneGrade.h"
#include "ofxColour.h"   // OFX 1.5 colour management properties (read-only probe)
#include "OneGradePipeline.h"
#include "OneGradeAnalysis.h"   // CPU-only scene descriptors + control Jacobian (NOT mirrored)
#include "OneGradeSegment.h"    // ncnn semantic segmentation for Magic Grade's regions
#include "OneGradeCreative.h"   // the grade solve, shared with the offline bench
#include "CubeLUT.h"
#ifdef __APPLE__
#include <OpenGL/gl.h>
#elif defined(_WIN64)
#include <Windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <map>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cmath>
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
#define kPluginVersionMinor 4

#define kSupportsTiles              false
#define kSupportsMultiResolution    false
#define kSupportsMultipleClipPARs   false

// Master switch for the Auto Grade analysis UI. While false the whole debug surface is
// hidden — the "Show analysis" checkbox, the Analyze Frame button, the nine measurement rows
// and the Applied readout — leaving just Auto Grade and Bias, which is all a colorist needs.
// The params still exist and still work; only their visibility is off, so nothing about
// saved projects or the measurement itself depends on this.
//
// *** CURRENTLY TRUE, AND MUST GO BACK TO FALSE BEFORE THIS SHIPS. ***
// Flipped on the scene-descriptor branch because that work exists to be READ on footage: the
// Colour / Regions / Response rows are how a colour rule for Magic Grade gets fitted, the same
// way the Gain and Rolloff rows produced their fits, and a measurement nobody can see is worth
// nothing. Revert to false when merging into the release branch. One character, no other
// consequence — the params are unaffected either way. See docs/AUTO-GRADE.md.
static const bool kAnalysisDebugUI = false;

// MUST equal og::analysis::kParamN and the P[] the kernels index. Three separate places size
// buffers off this, and a mismatch is silent on GPU: wrong values, no error, a different picture.
#define kParamCount 34 // temp,tint,density,lift,gamma,gain,offTemp,offTint,postExp,postCon,rawExp,rawTemp,rolloff,
                       // rbLatch,rbSoft,rbHigh,rbLift,rbGamma

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
static std::string bundleResourceDir()
{
    std::string bin;
#ifdef _WIN32
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&bundleResourceDir, &hm)) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(hm, path, MAX_PATH)) bin = path;
    }
#else
    Dl_info info;
    if (dladdr((void*)&bundleResourceDir, &info) && info.dli_fname) bin = info.dli_fname;
#endif
    if (bin.empty()) return {};
    namespace fs = std::filesystem;
    fs::path contents = fs::path(bin).parent_path().parent_path();   // Contents/<arch>/OneGrade.ofx -> Contents
    return (contents / "Resources").string();
}

// Both resources resolve from the plugin binary's own path, so they work wherever the bundle
// is installed and on a render node that has never seen this machine. Named functions rather
// than "the LUT dir plus /../Model": that relative hop was written once and was already wrong,
// pointing at Contents/Model while the files were in Contents/Resources/Model, and nothing
// would have reported it -- the model simply would not have loaded and Magic Grade would have
// quietly run on the heuristic stub forever.
static std::string bundleLutDir()
{
    const std::string r = bundleResourceDir();
    return r.empty() ? r : (std::filesystem::path(r) / "LUTs").string();
}

static std::string bundleModelDir()
{
    const std::string r = bundleResourceDir();
    return r.empty() ? r : (std::filesystem::path(r) / "Model").string();
}

// The segmenter, loaded once. Shared by the white-balance pass and the region pass, which both
// need it and would otherwise each carry their own copy of a 12 MB model.
static og::seg::Segmenter& s_segmenter()
{
    static og::seg::Segmenter seg;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const std::string mdir = bundleModelDir();
        if (!mdir.empty()) seg.load(mdir + "/ade20k.param", mdir + "/ade20k.bin");
    }
    return seg;
}
static bool s_seg_ready() { return s_segmenter().ready(); }

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

// The COMPLETE per-pixel chain as rendered: pipeline -> LUT -> trim -> highlight rolloff.
// og::process() stops at the output encode; steps 8-9 have always lived in the processor,
// which was fine while the CPU render loop was the only caller. The .cube exporter is a
// second caller, and an exported LUT that disagrees with the node it was exported from is
// worse than no exporter at all — so both now go through this one function.
//
// This is the CPU/GPU golden rule applied one level up: the kernels mirror og::process(),
// and everything that reads og::process() mirrors this.
static inline void og_full_chain(int camera, int encode, const float* P,
                                 const float* lut, int lutSize, float lutMix,
                                 float ri, float gi, float bi,
                                 float& ro, float& go, float& bo,
                                 float shapeM = 1.0f)
{
    og::process(camera, encode, P, ri, gi, bi, ro, go, bo, shapeM);
    // The matte is a measurement, not a picture: process() has already returned it raw and
    // unencoded, so a LUT or a trim on top would be a picture OF the mask rather than the mask.
    if (P[18] > 0.5f && P[13] > 0.f) return;
    const bool lutOn = (lut && lutSize >= 2 && lutMix > 0.0f);
    if (lutOn) og::apply_lut(lut, lutSize, lutMix, ro, go, bo);
    og::apply_trim(P[8], P[9], ro, go, bo);                 // post-LUT trim
    // Rolloff is display-referred only — never distort a Cineon/DI/Linear feed.
    if (P[12] > 0.0f && (encode <= 2 || lutOn)) {
        ro = og::softclip(ro, P[12]);
        go = og::softclip(go, P[12]);
        bo = og::softclip(bo, P[12]);
    }
}

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
    // The shape mask is the one thing here that depends on WHERE a pixel is, so the frame's own
    // extent has to come from the image rather than from a parameter. Taken off the destination
    // bounds, which is the region actually being written -- and the same rectangle the three GPU
    // paths pass as W/H, so all four agree on what "centre" means.
    const OfxRectI b = _dstImg->getBounds();
    const float shW = (float)(b.x2 - b.x1), shH = (float)(b.y2 - b.y1);
    const float shHalfH = (shH > 1.f) ? 0.5f*shH : 1.f;
    const int   shType  = (int)(_params[24] + 0.5f);

    for (int y = p_ProcWindow.y1; y < p_ProcWindow.y2; ++y)
    {
        if (_effect.abort()) break;
        float* dstPix = static_cast<float*>(_dstImg->getPixelAddress(p_ProcWindow.x1, y));
        const float shV = ((float)(y - b.y1) - 0.5f*shH) / shHalfH;
        for (int x = p_ProcWindow.x1; x < p_ProcWindow.x2; ++x)
        {
            float* srcPix = static_cast<float*>(_srcImg ? _srcImg->getPixelAddress(x, y) : nullptr);
            if (srcPix)
            {
                const float shU = ((float)(x - b.x1) - 0.5f*shW) / shHalfH;
                const float shM = shType <= 0 ? 1.0f
                    : og::shape_mask(shU, shV, shType, _params[25], _params[26], _params[27],
                                     _params[28], _params[29], _params[30], _params[31] > 0.5f);
                og_full_chain(_camera, _encode, _params, _lut, _lutSize, _lutMix,
                              srcPix[0], srcPix[1], srcPix[2],
                              dstPix[0], dstPix[1], dstPix[2], shM);
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

// Everything the node does at one point in time, after every override has been applied.
// One definition, two consumers: the renderer and the .cube exporter. See resolveConfig().
struct RenderConfig
{
    float params[kParamCount] = {0};
    int   camera = 0;
    int   encode = 0;
    bool  lutOk  = false;   // a LUT resolved AND loaded (and isn't bypassed)
    float lutMix = 0.f;
};

class OneGrade : public OFX::ImageEffect
{
public:
    explicit OneGrade(OfxImageEffectHandle p_Handle);
    virtual void render(const OFX::RenderArguments& p_Args);
    virtual void changedParam(const OFX::InstanceChangedArgs& p_Args, const std::string& p_ParamName);
    void setEnabledness();
    bool lutSelected();         // does a LUT resolve behind the current LUT Mode? (Mix-independent)
    bool ensureLutLoaded();     // ...and is it actually in memory? Solves need the pixels.
    // forCreative: measure in the configuration Creative Grade is about to CREATE, not the
    // one the node happens to be in. See the call in applyAutoGrade().
    void probeAnalyze(double p_Time, bool forCreative = false);   // writes m_LastKey
    void probeSetup(double p_Time);     // is the input actually camera log? + what the host says
    void applyAutoGrade(double p_Time);      // measure, then set the film look + Gain from key
    void applyAutoGradeClean(double p_Time); // measure, then contain the range with no LUT
    void applyBias();                   // offset the grade by Bias, relative to the anchor
    void applyMagicGrade(double p_Time); // Creative, then one classifier-chosen colour move
    void applySeparation();             // rescale the stored magic move without re-deciding
    void armBias(bool reset = false);   // store the current grade as Bias's zero point
    void armToneTargets();              // ...and the conditions it currently meets
    // Measure Range Balance's latch off the current frame. The button re-samples and switches the
    // matte on; refreshRangeLatch() does neither, because it fires behind a grade the user asked
    // for and must not fetch a frame again or take over the viewer.
    void setRangeLatch(double p_Time, bool p_Reanalyse = true, bool p_ShowMatte = true);
    void refreshRangeLatch();                // keep an existing latch current after a new grade
    void fitRangeShape(double p_Time);       // measure the held region and put the shape round it
    void fitToneMap(double p_Time);          // measure the frame and shape the shoulder to it
    void populateMagicSubject();        // rebuild the Subject list from the cached segmentation
    void applyMagicSubject();           // re-grade around the subject the user picked
    void applyMagicResult(const og::grade::MagicResult& R, const char* src, bool repopulate);
    double m_LastKey = 0.0;             // scene key in stops from the last successful analyse
    double m_LastPin = 0.0;             // % of frame clipped at the source ceiling
    double m_LastGain = 0.80;           // Gain the measurement asked for (bias moves off this)
    double m_LastHot = 0.0;             // % of frame above display white — headroom for brightening
    // Display-space percentiles from the last analyse, at NEUTRAL params. Cached because the
    // Clean auto-grade solves against them directly: the grade curve is monotonic, so it maps
    // percentiles exactly, and three numbers stand in for the whole frame (see solveClean()).
    double m_LastD01 = 0.0;   // p0.1 — the BLACK POINT the solve places (not p1, see below)
    // The same p0.1 measured in the RENDER encode rather than the display-referred fallback.
    // Every solve that runs the grade curve must use this one; see the rendC declaration.
    double m_LastR01 = 0.0;
    double m_LastD1  = 0.0;
    double m_LastD50 = 0.0;
    double m_LastD99 = 0.0;
    bool   m_HaveKey = false;
    // Scene descriptors and the control Jacobian from the last analyse. Cached for the same
    // reason the percentiles are: a colour heuristic has to ask "how much Offset Temp buys me
    // 3 units of b* ON THIS SHOT", and re-measuring per question would make it unusable.
    // Instance state, so it goes inert after a project reload exactly like the Bias anchor did
    // before it was made persistent — deliberately inert rather than acting on a stale frame.
    oga::Desc   m_LastDesc{};
    oga::Jac    m_LastJac{};
    oga::Extras m_LastExtras{};
    bool        m_HaveJac = false;
    // The sampled frame itself, kept so a solve can re-use it without going back to
    // fetchImage. ~40k samples is about half a megabyte, the same order as the percentile
    // buffers probeAnalyze already allocates and throws away.
    oga::SampleSet m_LastSamples;
    std::vector<float> m_LastThumbSrc;   // 512x512 RGB, SOURCE values, top-down
    // Render the stored source thumbnail through a parameter set. The model wants a
    // normally exposed picture, so callers pass the grade that is actually in effect.
    // Solve Scene White Balance so the frame's neutral-expectation regions read neutral.
    std::vector<uint8_t> m_LastRegions;   // 512x512 region map from the last segmentation
    int         m_LastCam = 0;      // the camera/encode the samples were classified under:
    int         m_LastEnc = 1;      // re-solving in a different space would be meaningless
    void populateLookLut();     // repopulate the Look LUT dropdown for the current group
    void applyPreset(int p);    // set the look params (density/LGG/LUT/trim) to a starting point
    void setupAndProcess(OneGradeProcessor& p_Proc, const OFX::RenderArguments& p_Args);
    RenderConfig resolveConfig(double p_Time);   // the single definition of what this node does
    void exportCube(double p_Time);              // bake that definition into a .cube file

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
    OFX::StringParam* m_BiasNote;     // ...and which of its two control laws Bias is running
    OFX::ChoiceParam* m_MagicSubject; // which of the frame's subjects the grade is built around
    // Creative's grade, before a subject was chosen -- what switching subjects re-runs from.
    // Instance state on purpose: it is only meaningful while the segmentation behind it is live,
    // which is this session. A reloaded project shows "press Magic Grade" instead of a stale list.
    // SIZED FROM kParamN, NEVER TYPED. This was a literal 13 and stayed 13 while the param count
    // grew to 21, so the copy below it wrote 32 bytes past the end of the array -- straight over
    // m_PostExp, m_PostCon and m_Rolloff, which applyMagicResult() then calls setValue() through.
    // Range Balance made it a hard crash every time because it puts large non-zero floats in
    // P[13..20], turning three null pointers into three wild ones.
    float m_MagicBaseP[oga::kParamN] =
        {0.f,0.f,0.f, 0.f,1.f,1.f, 0.f,0.f, 0.f,1.f, 0.f,6500.f, 0.f,
         0.f,2.6f,1.f, 0.f,1.f,0.f, 1.f,1.f};
    bool  m_HaveMagicBase = false;
    bool  m_MagicLutOk    = false;
    OFX::DoubleParam* m_PostExp;
    OFX::DoubleParam* m_PostCon;
    OFX::DoubleParam* m_Rolloff;

    OFX::BooleanParam* m_ToneMap;      // the display-range shoulder (experimental)
    OFX::DoubleParam*  m_ToneMapKnee;
    OFX::DoubleParam*  m_ToneMapWhite;
    OFX::PushButtonParam* m_ToneMapFit;
    OFX::StringParam*  m_ToneMapNote;
    OFX::ChoiceParam* m_LutMode;    // 0 none, 1 custom look, 2 film-look built-in
    OFX::ChoiceParam* m_FilmLut;
    OFX::ChoiceParam* m_LookGroup;
    OFX::ChoiceParam* m_LookLut;
    OFX::DoubleParam* m_LutMix;
    CubeLUT           m_Lut;        // cached loaded LUT

    // Per-stage bypass — one checkbox per pipeline group, so a stage can be auditioned in
    // and out without zeroing its sliders and putting them back. Enforced at RENDER by
    // forcing the stage's params neutral (same mechanism as Node Role), never by a separate
    // code path, so a bypassed stage is exactly a neutral stage and nothing can drift.
    // The Input Transform has no bypass: the camera decode is structural, not an effect —
    // "bypassing" it would emit raw log, which is never what the checkbox would mean.
    OFX::BooleanParam* m_BypBalance;
    OFX::BooleanParam* m_BypDensity;
    OFX::BooleanParam* m_BypExposure;
    OFX::BooleanParam* m_BypRange;
    OFX::BooleanParam* m_BypLut;
    OFX::BooleanParam* m_BypTrim;

    // Auto Grade probe (experimental) — see probeAnalyze().
    OFX::StringParam* m_ProbeStatus;
    OFX::StringParam* m_ProbeScene;
    OFX::StringParam* m_ProbeDisplay;
    OFX::StringParam* m_ProbeShape;
    OFX::StringParam* m_ProbeSubject;
    OFX::DoubleParam* m_AutoBias;
    // Bias anchor — the grade Bias offsets FROM. Saved with the project (hidden), so the
    // slider still works after a reload and needs no instance state at all.
    OFX::BooleanParam* m_BiasArmed;
    OFX::DoubleParam*  m_BiasGain;
    OFX::DoubleParam*  m_BiasLift;
    OFX::DoubleParam*  m_BiasGamma;
    OFX::DoubleParam*  m_BiasRoll;
    OFX::DoubleParam*  m_BiasHot;
    OFX::DoubleParam*  m_BiasAt;      // slider position when the anchor was captured
    // Magic Grade: the chosen move, saved with the project so the Separation slider keeps
    // scaling it after a reload without needing the frame back.
    OFX::PushButtonParam* m_MagicBtn;
    OFX::BooleanParam* m_WbFirst;
    OFX::DoubleParam*  m_Separation;
    OFX::IntParam*     m_MagicCycle;   // which option the next press should offer
    OFX::IntParam*     m_MagicParam;   // index into P[]: 6 Offset Temp, 0 Gain Temp, -1 none
    OFX::DoubleParam*  m_MagicBase;    // the move at Separation 1.0
    OFX::DoubleParam*  m_MagicAnchor;  // the control's value before the move
    OFX::DoubleParam*  m_MagicSepAt;   // Separation position when that anchor was captured
    OFX::DoubleParam*  m_BiasMirror;   // second face of autoBias, shown in the Magic section
    // TONE SEPARATION: how far the subject is pushed from its surround in lightness, and which
    // way "further" points on this frame. The direction is a MEASUREMENT (the sign of the region
    // separation triple's dL*), stored because it must not be recomputed mid-drag -- a subject
    // that crosses its surround mid-slider would flip the control under the user's hand.
    OFX::DoubleParam*  m_RangeLatch;
    OFX::DoubleParam*  m_RangeSoft;
    OFX::DoubleParam*  m_RangeHigh;
    OFX::DoubleParam*  m_RangeShadow;
    OFX::DoubleParam*  m_RangeMid;
    OFX::BooleanParam* m_RangeShow;
    OFX::ChoiceParam*  m_RangeShape;     // 0 none, 1 ellipse, 2 rectangle
    OFX::DoubleParam*  m_RangeShapeX;
    OFX::DoubleParam*  m_RangeShapeY;
    OFX::DoubleParam*  m_RangeShapeW;
    OFX::DoubleParam*  m_RangeShapeH;
    OFX::DoubleParam*  m_RangeShapeR;
    OFX::DoubleParam*  m_RangeShapeS;
    OFX::BooleanParam* m_RangeShapeInv;
    OFX::StringParam*  m_RangeShapeNote;
    OFX::PushButtonParam* m_RangeShapeFit;
    OFX::BooleanParam* m_RangeLock;      // freeze the mask against the grade under it
    OFX::DoubleParam*  m_RangeRefLift;   // ...the grade it was frozen at (hidden, saved)
    OFX::DoubleParam*  m_RangeRefGamma;
    OFX::DoubleParam*  m_RangeRefGain;
    OFX::DoubleParam*  m_RangeHiMid;
    OFX::DoubleParam*  m_RangeLoGain;
    OFX::StringParam*  m_RangeNote;
    OFX::StringParam*  m_SepNote;      // why Face Tone Separation is or is not available
    OFX::DoubleParam*  m_RawExpMirror; // second face of rawExp, shown in the Magic section
    OFX::DoubleParam*  m_ToneSep;
    OFX::DoubleParam*  m_ToneSepDir;
    OFX::StringParam*  m_MagicNote;
    OFX::StringParam*  m_MagicWhy;    // the reasoning, in a sentence
    OFX::BooleanParam* m_ShowAnalysis;
    OFX::PushButtonParam* m_ProbeBtn;
    OFX::PushButtonParam* m_CleanBtn;
    OFX::DoubleParam* m_CleanHigh;     // where p99 should land  (containment target)
    OFX::DoubleParam* m_CleanLow;      // where p1  should land
    OFX::DoubleParam* m_CleanMid;      // where p50 should land
    OFX::DoubleParam* m_CleanMidStr;   // how much of the midtone solve to apply
    OFX::DoubleParam* m_CleanMaxGain;  // ceiling on Gain: 1.0 = never brighten
    OFX::DoubleParam* m_CleanShoulder; // rolloff per unit of highlight overshoot
    OFX::DoubleParam* m_CleanMaxExp;   // ceiling on how far Base may brighten, in stops
    OFX::DoubleParam* m_ToneLo;        // Magic Tone's cached neutral percentiles; -1 = not solved
    OFX::DoubleParam* m_ToneMid;
    OFX::DoubleParam* m_ToneShi;
    OFX::DoubleParam* m_ToneFLo;
    // The three conditions the CURRENT grade meets. Bias leans away from these rather
    // than from the fitted constants, which is what lets a hand edit survive it. -1 = unset.
    OFX::DoubleParam* m_ToneTFloor;
    OFX::DoubleParam* m_ToneTMid;
    OFX::DoubleParam* m_ToneTCeil;
    OFX::DoubleParam* m_ToneTFMax;
    OFX::DoubleParam* m_ToneHi;
    OFX::DoubleParam* m_CreativeLow;   // where Creative places its black point (pre-LUT)
    OFX::StringParam* m_ProbePeak;
    OFX::StringParam* m_ProbeApplied;
    OFX::StringParam* m_ProbeColour;    // a*/b*/chroma/separation, at NEUTRAL
    OFX::StringParam* m_ProbeGraded;    // the same, for the grade actually on the node
    OFX::StringParam* m_ProbeRegions;   // the two colour populations + the vertical split
    OFX::StringParam* m_ProbeResponse;  // what the controls DO on this shot (Jacobian rows)
    OFX::StringParam* m_ProbeDriveB;    // which controls drove the warm/cool change
    OFX::StringParam* m_ProbeDriveC;    // ...and the colourfulness change
    OFX::StringParam* m_ProbeDriveS;    // ...and the warm/cool hue separation
    OFX::StringParam* m_ProbeSepTriple; // the separation triple, neutral -> graded
    OFX::StringParam* m_ProbeTone;      // black / mid / white / overshoot, neutral -> graded

    // Setup check — see probeSetup().
    OFX::PushButtonParam* m_SetupBtn;
    OFX::StringParam*     m_SetupStatus;
    OFX::StringParam*     m_SetupStats;
    OFX::StringParam*     m_SetupHost;

    // LUT export — see exportCube().
    OFX::StringParam*     m_LutExportPath;
    OFX::ChoiceParam*     m_LutExportSize;
    OFX::PushButtonParam* m_LutExportBtn;
    OFX::StringParam*     m_LutExportStatus;
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
    m_BiasNote   = fetchStringParam("biasNote");
    m_MagicSubject = fetchChoiceParam("magicSubject");
    m_PostExp = fetchDoubleParam("postExp");
    m_PostCon = fetchDoubleParam("postCon");
    m_Rolloff = fetchDoubleParam("rolloff");

    m_LutMode  = fetchChoiceParam("lutMode");
    m_FilmLut  = fetchChoiceParam("filmLut");
    m_LookGroup= fetchChoiceParam("lookGroup");
    m_LookLut  = fetchChoiceParam("lookLut");
    m_LutMix   = fetchDoubleParam("lutMix");
    m_BypBalance  = fetchBooleanParam("bypassBalance");
    m_BypDensity  = fetchBooleanParam("bypassDensity");
    m_BypExposure = fetchBooleanParam("bypassExposure");
    m_BypRange    = fetchBooleanParam("bypassRange");
    m_BypLut      = fetchBooleanParam("bypassLut");
    m_BypTrim     = fetchBooleanParam("bypassTrim");
    m_ProbeStatus  = fetchStringParam("probeStatus");
    m_ProbeScene   = fetchStringParam("probeScene");
    m_ProbeDisplay = fetchStringParam("probeDisplay");
    m_ProbeShape   = fetchStringParam("probeShape");
    m_ProbeSubject = fetchStringParam("probeSubject");
    m_AutoBias     = fetchDoubleParam("autoBias");
    m_MagicBtn     = fetchPushButtonParam("magicGrade");
    m_WbFirst      = fetchBooleanParam("wbFirst");
    m_Separation   = fetchDoubleParam("separation");
    m_MagicCycle   = fetchIntParam("magicCycle");
    m_MagicParam   = fetchIntParam("magicParam");
    m_MagicBase    = fetchDoubleParam("magicBase");
    m_MagicAnchor  = fetchDoubleParam("magicAnchor");
    m_MagicSepAt   = fetchDoubleParam("magicSepAt");
    m_BiasMirror   = fetchDoubleParam("autoBiasMirror");
    m_RangeLatch   = fetchDoubleParam("rangeLatch");
    m_RangeSoft    = fetchDoubleParam("rangeSoft");
    m_RangeHigh    = fetchDoubleParam("rangeHigh");
    m_RangeShadow  = fetchDoubleParam("rangeShadow");
    m_RangeMid     = fetchDoubleParam("rangeMid");
    m_RangeShow    = fetchBooleanParam("rangeShow");
    m_RangeShape    = fetchChoiceParam("rangeShape");
    m_RangeShapeX   = fetchDoubleParam("rangeShapeX");
    m_RangeShapeY   = fetchDoubleParam("rangeShapeY");
    m_RangeShapeW   = fetchDoubleParam("rangeShapeW");
    m_RangeShapeH   = fetchDoubleParam("rangeShapeH");
    m_RangeShapeR   = fetchDoubleParam("rangeShapeR");
    m_RangeShapeS   = fetchDoubleParam("rangeShapeS");
    m_RangeShapeInv = fetchBooleanParam("rangeShapeInv");
    m_RangeShapeNote = fetchStringParam("rangeShapeNote");
    m_RangeShapeFit  = fetchPushButtonParam("rangeShapeFit");
    m_ToneMap      = fetchBooleanParam("toneMap");
    m_ToneMapKnee  = fetchDoubleParam("toneMapKnee");
    m_ToneMapWhite = fetchDoubleParam("toneMapWhite");
    m_ToneMapFit   = fetchPushButtonParam("toneMapFit");
    m_ToneMapNote  = fetchStringParam("toneMapNote");
    m_RangeLock    = fetchBooleanParam("rangeLock");
    m_RangeRefLift  = fetchDoubleParam("rangeRefLift");
    m_RangeRefGamma = fetchDoubleParam("rangeRefGamma");
    m_RangeRefGain  = fetchDoubleParam("rangeRefGain");
    m_RangeHiMid   = fetchDoubleParam("rangeHiMid");
    m_RangeLoGain  = fetchDoubleParam("rangeLoGain");
    m_RangeNote    = fetchStringParam("rangeNote");
    m_SepNote      = fetchStringParam("sepNote");
    m_RawExpMirror = fetchDoubleParam("rawExpMirror");
    m_ToneSep      = fetchDoubleParam("toneSep");
    m_ToneSepDir   = fetchDoubleParam("toneSepDir");
    m_MagicNote    = fetchStringParam("magicNote");
    m_MagicWhy     = fetchStringParam("magicWhy");
    m_BiasArmed    = fetchBooleanParam("biasArmed");
    m_ToneLo       = fetchDoubleParam("toneLo");
    m_ToneMid      = fetchDoubleParam("toneMid");
    m_ToneShi      = fetchDoubleParam("toneShi");
    m_ToneFLo      = fetchDoubleParam("toneFLo");
    m_ToneTFloor   = fetchDoubleParam("toneTFloor");
    m_ToneTMid     = fetchDoubleParam("toneTMid");
    m_ToneTCeil    = fetchDoubleParam("toneTCeil");
    m_ToneTFMax    = fetchDoubleParam("toneTFMax");
    m_ToneHi       = fetchDoubleParam("toneHi");
    m_BiasGain     = fetchDoubleParam("biasGain");
    m_BiasLift     = fetchDoubleParam("biasLift");
    m_BiasGamma    = fetchDoubleParam("biasGamma");
    m_BiasRoll     = fetchDoubleParam("biasRoll");
    m_BiasHot      = fetchDoubleParam("biasHot");
    m_BiasAt       = fetchDoubleParam("biasAt");
    m_ShowAnalysis = fetchBooleanParam("showAnalysis");
    m_ProbeBtn     = fetchPushButtonParam("probeAnalyze");
    m_CleanBtn      = fetchPushButtonParam("autoGradeClean");
    m_CleanHigh     = fetchDoubleParam("cleanHigh");
    m_CleanLow      = fetchDoubleParam("cleanLow");
    m_CleanMid      = fetchDoubleParam("cleanMid");
    m_CleanMidStr   = fetchDoubleParam("cleanMidStr");
    m_CleanMaxGain  = fetchDoubleParam("cleanMaxGain");
    m_CleanShoulder = fetchDoubleParam("cleanShoulder");
    m_CleanMaxExp   = fetchDoubleParam("cleanMaxExp");
    m_CreativeLow   = fetchDoubleParam("creativeLow");
    m_ProbePeak    = fetchStringParam("probePeak");
    m_ProbeApplied = fetchStringParam("probeApplied");
    m_ProbeColour   = fetchStringParam("probeColour");
    m_ProbeGraded   = fetchStringParam("probeGraded");
    m_ProbeRegions  = fetchStringParam("probeRegions");
    m_ProbeResponse = fetchStringParam("probeResponse");
    m_ProbeDriveB   = fetchStringParam("probeDriveB");
    m_ProbeDriveC   = fetchStringParam("probeDriveC");
    m_ProbeDriveS   = fetchStringParam("probeDriveS");
    m_ProbeSepTriple = fetchStringParam("probeSepTriple");
    m_ProbeTone      = fetchStringParam("probeTone");
    m_SetupBtn    = fetchPushButtonParam("setupCheck");
    m_SetupStatus = fetchStringParam("setupStatus");
    m_SetupStats  = fetchStringParam("setupStats");
    m_SetupHost   = fetchStringParam("setupHost");
    m_LutExportPath   = fetchStringParam("lutExportPath");
    m_LutExportSize   = fetchChoiceParam("lutExportSize");
    m_LutExportBtn    = fetchPushButtonParam("lutExportBtn");
    m_LutExportStatus = fetchStringParam("lutExportStatus");

    populateLookLut();
    setEnabledness();
    // NOTHING THAT TOUCHES PIXELS MAY GO HERE. See autoInitOnce()'s removal note below.
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
    // A bypassed LUT stage owns nothing, so it must hand Output Encode back to the user —
    // otherwise bypassing the look would leave the encode pinned to the LUT's working curve
    // and the "bypass" would still be changing the picture. Mirrors the render, which
    // clears lutOk on the same condition before the encode coupling runs.
    bool byp = false;
    m_BypLut->getValue(byp);
    if (byp) return false;

    int mode = 0, gi = 0, li = 0, fi = 0;
    m_LutMode->getValue(mode);
    m_LookGroup->getValue(gi);
    m_LookLut->getValue(li);
    m_FilmLut->getValue(fi);
    return !resolveLutPath(mode, gi, li, fi).empty();
}

// LOAD the LUT, not just resolve it. lutSelected() answers "is one chosen"; this answers "is one
// IN MEMORY", which is what a solve needs.
//
// m_Lut is otherwise only filled by resolveConfig() on the render path, so a button pressed on a
// node that has not yet rendered with its LUT selected solved against no LUT at all -- placing
// every target pre-LUT and letting the print stock's toe crush them afterwards. It showed up as
// Magic Grade producing a dark, crushed picture that snapped correct the moment Bias was touched,
// because by then a render had loaded the LUT and Bias re-solves.
//
// This is deliberate file I/O from a param callback, against the rule lutSelected() follows. That
// rule exists because setEnabledness() runs constantly and must stay cheap; a button press is
// once, and it is already spending ~100 ms on inference. Reading a .cube is free beside that, and
// the alternative is a confident wrong answer -- the fourth time in this project that a missing
// resource has silently changed behaviour rather than failing.
bool OneGrade::ensureLutLoaded()
{
    bool byp = false;
    m_BypLut->getValue(byp);
    if (byp) return false;
    int mode = 0, gi = 0, li = 0, fi = 0;
    m_LutMode->getValue(mode);
    m_LookGroup->getValue(gi);
    m_LookLut->getValue(li);
    m_FilmLut->getValue(fi);
    const std::string path = resolveLutPath(mode, gi, li, fi);
    return !path.empty() && m_Lut.load(path) && m_Lut.size >= 2;
}

// SETUP CHECK — "is this node being fed what it expects?" (user's idea, 2026-08-03).
//
// The obvious version of this request is "read the Timeline Color Space and warn if it's
// wrong". That one is genuinely impossible, and it's worth writing down why so nobody tries
// again: Timeline Color Space is a MONITORING setting applied downstream of the node graph.
// It changes how Resolve interprets our OUTPUT for the viewer. Nothing about it is visible
// from inside the effect, and no OFX property carries it.
//
// But the thing that actually breaks users is visible, because it changes our INPUT. Every
// real failure — a color-managed timeline, a CST node in front of us, an input LUT on the
// clip — hands this node something that is no longer camera log. That IS measurable, from
// the pixels, with no API support at all:
//
//   Camera log has a narrow, lifted code-value footprint. Blacks sit well off zero and the
//   top rolls off far below 1.0 — Blackmagic log peaks around 0.75 on real footage (measured
//   during the Auto Grade work). Display-referred material does the opposite: it uses the
//   full range, crushing to 0 and clipping at 1.
//
// The check is deliberately CONSERVATIVE and always prints its numbers. A blown practical
// can push a genuinely log frame to 1.0, and a flat display-referred shot can look lifted,
// so anything ambiguous is reported as inconclusive rather than guessed at. Crying wolf on
// a correct setup would be worse than staying quiet — the same reasoning that stopped Auto
// Grade from guessing at white balance.
//
// It also reports what the host says via the OFX 1.5 colour management API (ofxColour.h,
// vendored). These properties are read WITHOUT declaring a colour management style: hosts
// that populate them anyway cost us nothing, and declaring support is what could invite
// Resolve to start converting our input — the one thing that would break the CST. If these
// come back "(absent)", the next experiment is to declare Basic and retest; that is a
// separate, deliberate step, not something to switch on speculatively.
void OneGrade::probeSetup(double p_Time)
{
    m_SetupHost->setValue("");
    m_SetupStats->setValue("");

    // --- what the host volunteers, if anything ---
    {
        std::string style = getPropertySet().propGetString(kOfxImageEffectPropColourManagementStyle, 0, false);
        std::string cs    = m_SrcClip ? m_SrcClip->getPropertySet().propGetString(kOfxImageClipPropColourspace, 0, false)
                                      : std::string();
        if (style.empty()) style = "(absent)";
        if (cs.empty())    cs    = "(absent)";
        char m[160];
        snprintf(m, sizeof m, "CM style %s / clip %s", style.c_str(), cs.c_str());
        m_SetupHost->setValue(m);
    }

    // --- what the pixels say ---
    if (!m_SrcClip || !m_SrcClip->isConnected()) { m_SetupStatus->setValue("No source clip connected"); return; }

    try {
        std::unique_ptr<OFX::Image> src(m_SrcClip->fetchImage(p_Time));
        if (!src.get()) { m_SetupStatus->setValue("Could not read the frame"); return; }
        const OfxRectI b = src->getBounds();
        const int w = b.x2 - b.x1, h = b.y2 - b.y1;
        if (w <= 0 || h <= 0 || src->getPixelDepth() != OFX::eBitDepthFloat ||
            src->getPixelComponents() != OFX::ePixelComponentRGBA) {
            m_SetupStatus->setValue("Frame not float RGBA - cannot check"); return;
        }

        std::vector<float> v;
        v.reserve(200000);
        const int step = std::max(1, (int)std::sqrt((double)w * h / 60000.0));
        for (int y = b.y1; y < b.y2; y += step)
            for (int x = b.x1; x < b.x2; x += step) {
                const float* p = (const float*)src->getPixelAddress(x, y);
                if (!p) continue;
                v.push_back(p[0]); v.push_back(p[1]); v.push_back(p[2]);
            }
        if (v.size() < 300) { m_SetupStatus->setValue("Too few samples to judge"); return; }

        auto pct = [&](double q) {
            size_t k = (size_t)(q * (v.size() - 1));
            std::nth_element(v.begin(), v.begin() + k, v.end());
            return (double)v[k];
        };
        const double p1 = pct(0.01), p50 = pct(0.50), p99 = pct(0.99);

        char m[160];
        snprintf(m, sizeof m, "p1 %.3f  p50 %.3f  p99 %.3f", p1, p50, p99);
        m_SetupStats->setValue(m);

        // Two confident verdicts and an honest shrug. Thresholds are deliberately far apart
        // so ordinary footage lands in neither trap.
        const bool looksDisplay = (p1 < 0.015 && p99 > 0.990);   // both ends pinned = full-range
        const bool looksLog     = (p1 > 0.030 && p99 < 0.950);   // lifted floor, rolled-off top
        if (looksDisplay)
            m_SetupStatus->setValue("WARNING: input looks display-referred, not log");
        else if (looksLog)
            m_SetupStatus->setValue("OK - input looks like camera log");
        else
            m_SetupStatus->setValue("Inconclusive - see the numbers below");
    }
    catch (const std::exception& e) { m_SetupStatus->setValue(std::string("threw: ") + e.what()); }
    catch (...)                     { m_SetupStatus->setValue("threw (unknown)"); }
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
void OneGrade::probeAnalyze(double p_Time, bool forCreative)
{
    m_ProbeScene->setValue("");
    m_ProbeDisplay->setValue("");
    m_ProbeShape->setValue("");
    m_ProbeSubject->setValue("");
    m_ProbePeak->setValue("");
    m_ProbeColour->setValue("");
    m_ProbeGraded->setValue("");
    m_ProbeRegions->setValue("");
    m_ProbeResponse->setValue("");
    m_ProbeDriveB->setValue("");
    m_ProbeDriveC->setValue("");
    m_ProbeDriveS->setValue("");
    m_ProbeSepTriple->setValue("");
    m_ProbeTone->setValue("");
    m_HaveJac = false;
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

        // MEASURE THE CONFIGURATION THE GRADE IS ABOUT TO CREATE, not the one the node is in.
        //
        // Creative Grade begins with applyPreset(1), which sets Camera to Rec.2100 PQ and selects
        // the film LUT, and a film LUT forces Cineon. On a node that does not have those yet --
        // the FIRST press, or any fresh node -- the lines above read the pre-preset state, so the
        // black point was solved against a Gamma 2.2 measurement and then rendered in Cineon.
        // That is the crush the encode fix was supposed to have removed, and it survived in
        // exactly one place: the first press. The second press worked because by then the node
        // was already configured, which is what made it look intermittent rather than wrong.
        //
        // Same cause for White Balance misfiring on a first press and settling by the third: it
        // segments and solves on a render built from these values, so it too was describing a
        // picture the node was about to stop being. A button that converges over repeated presses
        // is reading its own output; the tell is that pressing it again changes the answer when
        // nothing about the footage changed.
        //
        // Reading current state is right for the standalone Analyze button and wrong for a button
        // whose whole job is to put the node somewhere else. The target is known in advance -- it
        // is two constants in OneGradeCreative.h -- so it is asserted rather than discovered.
        if (forCreative) {
            camera = og::grade::kCreativeCamera;
            encode = og::grade::kCreativeEncode;
        }
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
        float neutral[kParamCount]; oga::neutral_params(neutral);

        // Coarse grid, ~200k samples: percentiles don't need every pixel, and a button that
        // stalls the UI on an 8K frame is its own kind of failure.
        const int step = std::max(1, (int)(std::sqrt((double)(w * h) / 200000.0) + 0.5));
        std::vector<float> sceneY, dispL, skinY;
        // Per-CHANNEL display values, kept separately from luma. The Base solve places its
        // percentiles on these, because a channel is what clips: a waveform shows R, G and B
        // independently, and on a saturated highlight they spread far apart. Containing luma
        // at 0.95 put blue over 1023 on a real interview frame while the luma number said the
        // target had been hit exactly. Measuring a summary statistic instead of the quantity
        // that actually fails is the same mistake as measuring in the wrong encode.
        std::vector<float> dispC;
        // ...and the same per-channel values in the encode the RENDER actually uses, which is
        // not always the one above.
        //
        // THE BLACK POINT IS SOLVED, NOT MEASURED, AND A SOLVE HAS A SPACE. Lift/Gamma/Gain run
        // in whatever curve the output encode selects, so solving "place p0.1 at 0.050" means
        // pushing p0.1 through og_lgg IN THAT CURVE. Feed it a percentile measured somewhere
        // else and the solve is exact about the wrong question.
        //
        // That is what shipped. Creative Grade always selects the film LUT, which forces Cineon,
        // while the display fallback above put the measurement in Gamma 2.2 -- so the solve
        // aimed at 0.050 and landed at 0.000, Lift going to -0.025 where the render's own space
        // wanted +0.034. The blacks crushed on every Creative and Magic grade, and the panel
        // reported "(blk 0.050)" while doing it, because the solve had hit the target it was
        // given. Found by the bench disagreeing with Resolve on one frame.
        //
        // The 2.2 fallback stays correct for what it was written for -- `hot`, saturation, the
        // skin mask -- because those are THRESHOLDS chosen in display space. The distinction is
        // not display-vs-log, it is "a number compared against a constant" (needs the space the
        // constant was chosen in) versus "a number pushed through the pipeline" (needs the
        // space the pipeline runs in). Same frame, same percentile, two legitimate answers.
        std::vector<float> rendC;
        const bool sameEnc = (encode == dispEnc);
        double skinR = 0.0, skinG = 0.0, skinB = 0.0;   // skin chromaticity, for a warmth read
        sceneY.reserve(220000); dispL.reserve(220000); dispC.reserve(660000);
        if (!sameEnc) rendC.reserve(660000);
        long long hot = 0;
        std::vector<float> srcTop;   // per-sample max input channel, for ceiling detection
        srcTop.reserve(220000);
        double satSum = 0.0; long long satN = 0;
        bool anyNonZero = false;

        // Scene-descriptor set (OneGradeAnalysis.h). Thinner than the percentile pass because
        // describe() gets run 27 times to build the Jacobian and the colour statistics are
        // means and cluster centroids, which converge far faster than a 0.1st percentile does.
        // Source values only — describe() re-renders them itself for whatever parameters it is
        // asked about, which is exactly what makes it a function of P rather than a snapshot.
        // ONE SAMPLING RULE, because the bench has to be able to predict this.
        //
        // This kept only every descStride'th grid sample (~40k of the ~200k walked) while the
        // bench kept all of them, and that was a PARAPHRASE of the same kind the arithmetic was
        // extracted to OneGradeCreative.h to prevent: the numbers were shared, the sampling was
        // written twice, and it drifted.
        //
        // It is not a rounding difference. Region coverage moves ~0.5 percentage points between
        // the two counts, and magic_decide() ranks subjects by cover * salience -- on a dark
        // interview that margin was 1.5%, so the plugin picked BUILT 78% where the bench picked
        // SKIN 16%. BUILT declines the tone solve as "not a face", which drops the RAW Exposure
        // rescue and lands the frame at midtone 0.339 against the bench's 0.632. A visibly
        // darker picture out of a sample count.
        //
        // The descriptors were what the decimation was for, and they still get it where it
        // actually costs: jacobian() runs describe() 27 times and decimates to 12k itself, just
        // below. classify() and describe() run ONCE each, so a full-grid pass is two linear
        // walks against ~100 ms of inference -- free, beside a wrong answer.
        oga::SampleSet SS;
        SS.rgb.reserve(660000); SS.band.reserve(220000);

        for (int y = b.y1; y < b.y2; y += step) {
            const float* row = static_cast<const float*>(src->getPixelAddress(b.x1, y));
            if (!row) continue;
            for (int x = 0; x < w; x += step) {
                const float* p = row + (size_t)x * 4;
                if (p[0] != 0.f || p[1] != 0.f || p[2] != 0.f) anyNonZero = true;
                srcTop.push_back(std::max(p[0], std::max(p[1], p[2])));

                // Vertical band, the one piece of geometry the descriptors need. OFX hands over
                // bottom-up rows, so the HIGHEST y is the top of the frame and band 2 is sky.
                {
                    SS.rgb.push_back(p[0]); SS.rgb.push_back(p[1]); SS.rgb.push_back(p[2]);
                    const int bd = (int)(((long long)(y - b.y1) * 3) / h);
                    SS.band.push_back((uint8_t)std::min(2, std::max(0, bd)));
                    // Normalised position, OFX origin (bottom-left), so a segmentation mask can
                    // be read at this sample's location later.
                    SS.u.push_back((float)x / (float)w);
                    SS.v.push_back((float)(y - b.y1) / (float)h);
                }

                // Scene luminance: decode to camera-linear, then XYZ Y. Neutral scene stage, so no
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
                dispC.push_back(dr); dispC.push_back(dg); dispC.push_back(db);
                if (!sameEnc) {
                    float rr, rg, rb;
                    og::process(camera, encode, neutral, p[0], p[1], p[2], rr, rg, rb);
                    rendC.push_back(rr); rendC.push_back(rg); rendC.push_back(rb);
                }
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
        // MODEL THUMBNAIL, built here because this is the only place the image exists. Magic
        // Grade needs a display-referred 8-bit picture at the segmentation model's input size,
        // and re-fetching later would mean a second fetchImage on a different code path for no
        // gain. ~100 ms on top of the pass already being made.
        //
        // DISPLAY-REFERRED, NOT CAMERA LOG. The model was trained on ordinary photographs, and
        // log footage is far outside that distribution -- the same trap as measuring
        // percentiles in Cineon, which produced a flat 0% `hot` on a blown frame.
        //
        // Rows are written TOP-DOWN because that is what a picture is; OFX hands them over
        // bottom-up, so the source row is mirrored.
        //
        // STORED AS SOURCE, RENDERED LATER. The first version baked a NEUTRAL render in here,
        // and that is the wrong picture to hand a segmentation model: a flat PQ-decoded log
        // frame looks nothing like the photographs it was trained on. Magic Grade applies
        // Creative Grade before it segments, so the model should see THAT -- a normally exposed
        // image -- which is also what every frame in the offline validation set was.
        //
        // Keeping the source and rendering on demand is what makes both possible: this row
        // stays grade-independent like every other measurement, and the caller renders it
        // through whatever parameters are actually in effect when it needs a picture.
        {
            const int T = 512;
            m_LastThumbSrc.assign((size_t)T * T * 3, 0.f);
            for (int ty = 0; ty < T; ++ty) {
                const int sy = b.y2 - 1 - (int)(((long long)ty * h) / T);   // flip to top-down
                const float* row = static_cast<const float*>(src->getPixelAddress(b.x1, sy));
                if (!row) continue;
                for (int tx = 0; tx < T; ++tx) {
                    const float* q = row + (size_t)(((long long)tx * w) / T) * 4;
                    float* o = &m_LastThumbSrc[((size_t)ty * T + tx) * 3];
                    o[0] = q[0]; o[1] = q[1]; o[2] = q[2];
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
        // Base-solve percentiles: per channel, not luma. See the dispC declaration.
        const double c01 = pct(dispC, 0.001), c50 = pct(dispC, 0.50), c99 = pct(dispC, 0.99);
        const double c999 = pct(dispC, 0.999);   // per-channel extreme, for the Base white point
        const double d999 = pct(dispL, 0.999);
        const double y1 = pct(sceneY, 0.01), y50 = pct(sceneY, 0.50), y99 = pct(sceneY, 0.99);

        // The two numbers a heuristic would actually act on. Key: how far the median sits
        // from 18% mid-gray, in stops — that IS the exposure correction, since Scene Exposure
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
        // Luma percentiles on the left (comparable with every earlier reading), and the two
        // per-channel numbers the Base solve actually places on the right.
        snprintf(m2, sizeof m2, "p1 %.3f p50 %.3f p99 %.3f @%s | ch %.3f-%.3f", d1, d50, d99, encName, c01, c99);
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
        // ch99.9 vs ch99 says how compact the very top is. A big gap means a small specular
        // region is dragging the whole picture down when p99 is forced to the target; a small
        // gap means the highlights are a broad field and containing them is honest.
        snprintf(m2, sizeof m2, "p99.9 %.3f peak x%.2f | ch99.9 %.3f x%.2f",
                 d999, peak, c999, (c99 > 1e-6) ? c999 / c99 : 1.0);
        m_ProbePeak->setValue(m2);

        m_LastD01 = c01; m_LastD50 = c50; m_LastD99 = c99;   // per-channel, for the Base solve
        m_LastR01 = sameEnc ? c01 : pct(rendC, 0.001);       // ...and in the render's own curve
        m_LastD1 = d1;                                       // luma p1, reported only
        m_LastPin = 100.0 * (double)pinned / (double)n;
        m_LastHot = 100.0 * (double)hot / (double)n;
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
            // The user's fix for "too cool" on this footage was Scene White Balance 6500 ->
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

        // ---- SCENE DESCRIPTORS + CONTROL JACOBIAN ----------------------------------------
        // Everything above answers "how is this frame exposed". This answers "what colour is
        // it, and what would each control do about that" — the half that was missing when the
        // user's own sunset grade reached for Offset Temp and no measurement could have asked
        // for it. Costs 27 more passes over a 40k set on top of the 200k already walked, all
        // arithmetic, no I/O.
        if (SS.size() >= 512) {
            m_LastExtras = oga::classify(SS, camera, dispEnc);
            float Pn[oga::kParamN]; oga::neutral_params(Pn);
            m_LastDesc = oga::describe(SS, camera, dispEnc, Pn);
            // The Jacobian runs on a thinned copy that KEEPS the memberships classify() just
            // assigned — a derivative has to be taken around the same masks the operating
            // point was measured with.
            oga::SampleSet J = oga::decimate(SS, 12000);
            m_LastJac = oga::jacobian(J, camera, dispEnc, Pn);
            m_HaveJac = true;
            m_LastCam = camera; m_LastEnc = dispEnc;

            snprintf(m2, sizeof m2, "a*%+.1f b*%+.1f C%.1f sep%.1f",
                     m_LastDesc.v[oga::D_A], m_LastDesc.v[oga::D_B],
                     m_LastDesc.v[oga::D_CHROMA], m_LastDesc.v[oga::D_SEP]);
            m_ProbeColour->setValue(m2);

            // THE SAME SAMPLES, THE SAME SPACE, THE GRADE THAT IS ACTUALLY ON THE NODE.
            //
            // Every other row here measures the neutral node on purpose, so that clicking
            // twice cannot chase its own tail — which also means they are identical whatever
            // the grade is, and therefore useless for the one question worth asking of two
            // grades: did this move do what I think it did? This row answers that, and it is
            // cheap because describe() is a pure function of the parameter vector.
            //
            // Camera and encode are held at the neutral row's, so the ONLY difference between
            // the two lines is the parameters. Comparing across two spaces would be measuring
            // the encode, not the grade.
            //
            // Read from the sliders rather than resolveConfig() for two reasons: no file I/O
            // in a param callback (resolveConfig loads the LUT), and for a diagnostic the
            // honest thing to show is what the panel says. It is PRE-LUT — with a film stock
            // selected the picture on screen is not this — so the row says so.
            // Neutral first, so the parameters this does NOT read off the panel are defined.
            // Declared bare, it left P[13..20] as stack garbage -- and P[18] is Show Mask, so a
            // stray bit there hands og::process a matte to describe instead of a picture.
            float Pg[oga::kParamN]; oga::neutral_params(Pg);
            Pg[0]  = (float)m_Temp->getValueAtTime(p_Time);
            Pg[1]  = (float)m_Tint->getValueAtTime(p_Time);
            Pg[2]  = (float)m_Density->getValueAtTime(p_Time);
            Pg[3]  = (float)m_Lift->getValueAtTime(p_Time);
            Pg[4]  = (float)m_Gamma->getValueAtTime(p_Time);
            Pg[5]  = (float)m_Gain->getValueAtTime(p_Time);
            Pg[6]  = (float)m_OffTemp->getValueAtTime(p_Time);
            Pg[7]  = (float)m_OffTint->getValueAtTime(p_Time);
            Pg[8]  = (float)m_PostExp->getValueAtTime(p_Time);
            Pg[9]  = (float)m_PostCon->getValueAtTime(p_Time);
            Pg[10] = (float)m_RawExp->getValueAtTime(p_Time);
            Pg[11] = (float)m_RawTemp->getValueAtTime(p_Time);
            Pg[12] = (float)m_Rolloff->getValueAtTime(p_Time);
            const oga::Desc dg = oga::describe(SS, camera, dispEnc, Pg);
            snprintf(m2, sizeof m2, "a*%+.1f b*%+.1f C%.1f sep%.1f%s",
                     dg.v[oga::D_A], dg.v[oga::D_B], dg.v[oga::D_CHROMA], dg.v[oga::D_SEP],
                     lutSelected() ? " pre-LUT" : "");
            m_ProbeGraded->setValue(m2);

            // TONE, neutral -> graded. The colour rows above cover half a grade; the city and
            // car shots were graded almost ENTIRELY on this half -- lift, gamma, gain and
            // contrast, with b* landing within 0.1 of where Creative had it -- and there was no
            // readout for any of it. The numbers were measured all along and simply never shown,
            // which is its own kind of blind spot: the panel decides what gets noticed.
            snprintf(m2, sizeof m2, "blk %.2f>%.2f mid %.2f>%.2f wht %.2f>%.2f ovr %.2f>%.2f",
                     m_LastDesc.v[oga::D_BLACK], dg.v[oga::D_BLACK],
                     m_LastDesc.v[oga::D_MID],   dg.v[oga::D_MID],
                     m_LastDesc.v[oga::D_WHITE], dg.v[oga::D_WHITE],
                     m_LastDesc.v[oga::D_OVER],  dg.v[oga::D_OVER]);
            m_ProbeTone->setValue(m2);

            snprintf(m2, sizeof m2, "cool %.0f%% h%.0f | warm %.0f%% h%.0f | db*%+.0f",
                     m_LastExtras.share[0], m_LastExtras.hue[0],
                     m_LastExtras.share[1], m_LastExtras.hue[1], m_LastDesc.v[oga::D_DB]);
            m_ProbeRegions->setValue(m2);

            // WHICH CONTROL DID IT. Decompose the neutral -> current-grade move through the
            // Jacobian, per descriptor, per control. This row exists because reading a
            // descriptor and naming the obvious control is wrong: on this very footage chroma
            // rose 1.2 between Creative and the hand grade and the honest answer was that
            // Density had gone DOWN while Lift, Gain and Offset Temp pushed it up. The
            // controls overlap far too much to attribute a change by eye.
            //
            // `act` is measured, `lin` is what the linear model expected. The GAP between them
            // is itself the signal — it says how far outside the linear regime the grade sits,
            // which is exactly when a single-shot solve would undershoot.
            // Linearised at the MIDPOINT of the move, not at either end. Decomposing a finite
            // move with the Jacobian taken at its start is a one-sided estimate and drifts
            // exactly as far as the move is long — which on a real grade is several natural
            // steps, the regime where the responses are already measurably saturating. The
            // midpoint is the mean-value point per component, so `lin` tracks `act` closely
            // enough that the gap means something. m_LastJac stays at neutral, because the
            // Response row is about the FOOTAGE's response, not this grade's.
            float Pmid[oga::kParamN];
            for (int i = 0; i < oga::kParamN; ++i) Pmid[i] = 0.5f * (Pn[i] + Pg[i]);
            const oga::Jac Jmid = oga::jacobian(oga::decimate(SS, 12000), camera, dispEnc, Pmid);
            const oga::Attribution A = oga::attribute(SS, camera, dispEnc, Jmid, Pn, Pg);
            auto driverRow = [&](int d, OFX::StringParam* out) {
                int drv[oga::kParamN];
                const int k = oga::top_drivers(A, d, 3, drv);
                char row[128];
                int off = snprintf(row, sizeof row, "%+.1f act %+.1f lin", A.actual[d], A.linear[d]);
                if (k == 0) snprintf(row + off, sizeof row - off, "  (nothing moved)");
                for (int i = 0; i < k && off < (int)sizeof row - 1; ++i)
                    off += snprintf(row + off, sizeof row - off, " %s%+.1f",
                                    oga::param_name(drv[i]), A.at(d, drv[i]));
                out->setValue(row);
            };
            snprintf(m2, sizeof m2, "dL* %.1f>%.1f  da* %.1f>%.1f  db* %.1f>%.1f",
                     m_LastDesc.v[oga::D_DL], dg.v[oga::D_DL],
                     m_LastDesc.v[oga::D_DA], dg.v[oga::D_DA],
                     m_LastDesc.v[oga::D_DB], dg.v[oga::D_DB]);
            m_ProbeSepTriple->setValue(m2);

            driverRow(oga::D_B, m_ProbeDriveB);
            driverRow(oga::D_DL,  m_ProbeDriveC);   // tone separation
            driverRow(oga::D_DB,  m_ProbeDriveS);   // hue separation, warm/cool

            // The row that shows its work: per one natural nudge of each control, how far the
            // warm/cool axis actually moves ON THIS SHOT. Reading it is how the fit for a
            // colour rule gets found, the same way the gain/rolloff rows produced theirs.
            snprintf(m2, sizeof m2, "b*/step oTmp%+.2f tmp%+.2f raw%+.2f C/dens%+.2f",
                     m_LastJac.at(oga::D_B, 6), m_LastJac.at(oga::D_B, 0),
                     m_LastJac.at(oga::D_B, 11), m_LastJac.at(oga::D_CHROMA, 2));
            m_ProbeResponse->setValue(m2);
            // Kept so a later solve can re-use the measured frame without going back to
            // fetchImage. Moved rather than copied — SS is dead after this point.
            m_LastSamples = std::move(SS);
        } else {
            m_ProbeColour->setValue("too few samples for colour analysis");
        }
    }
    catch (std::exception& e) {
        char m2[96]; snprintf(m2, sizeof m2, "threw: %.60s", e.what());
        m_ProbeStatus->setValue(m2);
    }
    catch (...) { m_ProbeStatus->setValue("fetchImage threw (unknown)"); }
}

// The grade curve, evaluated on a DISPLAY value. This is the exact arithmetic of step 6 in
// og::process(), and for a display-referred output encode it needs no round trip: the grade
// happens in `r709_g_enc(x, dg)` and the output encode is that same function, so the
// encode/decode pair on either side cancels. What is left acts straight on the numbers the
// analysis measured.
//
// The property that makes this useful: it is MONOTONIC. A monotonic map commutes with
// percentiles, so pushing the measured p1 / p50 / p99 through it gives exactly the graded
// p1 / p50 / p99 — three scalars stand in for the whole frame, and the solve below costs
// nothing instead of re-measuring 200k samples per iteration.
static double og_grade_display(double d, double lift, double gamma, double gain)
{
    return (double)og::lgg_core((float)d, (float)lift, (float)gamma, (float)gain);
}

// Monotonic 1-D bisection: find the parameter value that lands `probe` on `target`.
// Bisection rather than a closed-form inverse because the three controls interact and the
// closed form would have to be re-derived every time step 6 changes — this cannot drift.
static double og_solve(double lo, double hi, double target,
                       const std::function<double(double)>& probe)
{
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (probe(mid) < target) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
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
// with Scene Exposure already at -0.50, so part of its correction happened upstream of Gain.)
//
// The clamp at the preset value for key >= 0 is the important half. It means a dark shot is
// never pushed up — the earlier finding that `key` is descriptive rather than prescriptive
// (a low-key interior is *supposed* to sit low, and chasing 18% grey would flatten it) is
// handled by refusing to act in that direction at all, rather than by a special case.
//
// Writes ordinary slider values the user can then drag. That is the whole design: a
// starting point that shows its work, so a bad analysis costs one undo, not trust.
static inline int kCreativeCam() { return og::grade::kCreativeCamera; }
static inline int kCreativeEnc() { return og::grade::kCreativeEncode; }

void OneGrade::applyAutoGrade(double p_Time)
{
    // Measured in the configuration applyPreset(1) is ABOUT to create, not the current one --
    // otherwise a first press solves against the node's old camera and encode. The preset still
    // comes second, so a failed analysis leaves the node untouched.
    probeAnalyze(p_Time, /*forCreative=*/true);
    if (!m_HaveKey) return;            // analysis failed; probeAnalyze has already said why

    applyPreset(1);                    // Cinematic Film Emulation (Kodak 2383 D60)

    // Fitted from the user's grades. Floor exists because the fit is only evidenced out to
    // about -2 EV; beyond that it extrapolates, and an unclamped line reaches 0 near -4 EV.
    // THE SOLVE LIVES IN OneGradeCreative.h so the offline bench runs the same code rather
    // than a reimplementation. Every constant this feature has got wrong so far was a
    // paraphrase of something that already existed.
    og::grade::Measurements meas;
    meas.key = m_LastKey; meas.pin = m_LastPin; meas.hot = m_LastHot;
    // RENDER-ENCODE p0.1, not the display-referred one: solve_creative pushes this through
    // og_lgg, and Creative always forces Cineon via the film LUT. See the rendC declaration.
    meas.d01 = m_LastR01; meas.d50 = m_LastD50; meas.d99 = m_LastD99;
    meas.valid = true;
    og::grade::Tunables tun;
    m_CreativeLow->getValue(tun.blackTarget);
    float Pc[og::analysis::kParamN];
    // The LUT goes in, because the black point is judged on the picture the stock produces and
    // not on the one feeding it. Without this the solve hit 0.050 while the screen showed 0.000.
    const bool lutOkC = ensureLutLoaded();
    if (m_LastSamples.size() >= 512)
        og::grade::solve_creative_px(m_LastSamples, kCreativeCam(), kCreativeEnc(), meas, tun, Pc,
                                     nullptr, 0);   // pre-LUT on purpose -- see solve_black_px
    else
        og::grade::solve_creative(meas, tun, Pc, nullptr, 0);
    const double gain = Pc[5];

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
    // Rolloff from source clipping, the one fit evidenced across shots. Set here rather than
    // inside applyBias(), which is now a relative offset and no longer owns any absolute value.
    const double rolloff = Pc[12];
    m_Gain->setValue(gain);
    m_Rolloff->setValue(rolloff);
    m_LastGain = gain;

    // BLACK POINT: SOLVED, NOT STAMPED. The preset writes Lift 0.11 on every shot, and a fixed
    // lift lands a DIFFERENT black point depending on where the footage's own floor already
    // sits — which is why the same manual correction kept being needed:
    //
    //     shot    Creative lift    user's lift    Creative black point (measured)
    //     beach       0.050          -0.011                ~0.15
    //     city        0.110           0.066                 0.161
    //     car           —        "lifts shadows a bit too much"
    //
    // Three shots, all corrected downward, and on the city the user's own words were exactly
    // that. Base has always solved its floor to a target (`Target Low`); Creative stamped a
    // constant, and that difference is the whole defect. Same 1-D bisection Base uses, on the
    // same monotonic curve — test 14 proves lgg_core on a measured percentile predicts the
    // render, so three scalars stand in for the frame.
    //
    // MEASURED PRE-LUT, like every other number here. Creative renders through a print stock,
    // so this places the black point going INTO the stock rather than coming out of it. That
    // is the right place for it: it is the space the user's own corrections were made in, and
    // the stock's own toe is part of the look rather than something to solve around.
    m_Lift->setValue(Pc[3]);

    armBias(true);                     // fresh grade: Bias returns to neutral
    applyBias();

    char msg[128];
    snprintf(msg, sizeof msg, "Creative G %.3f L %+.3f (blk %.3f)  Roll %.3f",
             gain, Pc[3], tun.blackTarget, rolloff);
    m_ProbeApplied->setValue(msg);
    refreshRangeLatch();               // the grade under the mask just moved
    setEnabledness();                  // the preset switches LUT Mode
}

// CLEAN AUTO GRADE — "drop the node and the picture is already a sane starting point".
//
// Different question from the film Auto Grade, and a much better posed one. That button
// fits the USER'S TASTE (what gain did they choose on this shot?), which took four hand
// graded shots to pin down and is only evidenced over the range those shots covered. This
// one enforces a CONTAINMENT property — nothing crushed at 0, nothing clipped at 1023 —
// which is objective. There is no preference to fit: a frame either fits inside the range
// or it doesn't, and that is measurable on any footage from anyone.
//
// So the solve is direct rather than fitted:
//   gain  places the top    (p99 -> targetHigh)   gain pivots black, so it moves the top most
//   lift  places the bottom (p1  -> targetLow)    lift pivots white, so it moves the floor
//   gamma places the middle (p50 -> targetMid)    gamma pivots both ends, so it moves only mids
// Each control owns the end it pivots away from, which is why three 1-D solves converge in a
// couple of rounds instead of needing a real optimiser — they are nearly orthogonal.
//
// GAMMA IS DELIBERATELY ONLY PART-APPLIED (`midStrength`). Driving the median to a fixed
// target on every shot is the exact mistake the film Auto Grade had to unlearn: a moody
// low-key interior is SUPPOSED to sit low, and "move the median to mid-gray" flattens every
// deliberately dark shot into mush. Containment at the two ends is safe to enforce because
// clipping is a defect; the midtone is intent, so it only gets pulled part of the way.
void OneGrade::applyAutoGradeClean(double p_Time)
{
    probeAnalyze(p_Time);
    if (!m_HaveKey) return;             // probeAnalyze has already said why

    // No LUT, no film tint, no density: the smooth decode is meant to do the work, and
    // everything this writes is a range correction rather than a look. Density was tried as a
    // baseline (0.284, fitted on one shot) and taken back out at the user's call -- it is a
    // look decision, and Base exists to hand you a neutral picture to grade FROM.
    m_Camera->setValue(11);             // Rec.2100 PQ - Smooth Decode
    m_LutMode->setValue(0);
    m_LutMix->setValue(1.0);
    m_Temp->setValue(0.0);   m_Tint->setValue(0.0);
    m_OffTemp->setValue(0.0); m_OffTint->setValue(0.0);
    m_Density->setValue(0.0);
    m_PostCon->setValue(1.0);

    double tHigh = 0.94, tLow = 0.05, tMid = 0.70, midStr = 0.838, maxGain = 2.0, maxExp = 0.85;
    m_CleanHigh->getValue(tHigh);
    m_CleanLow->getValue(tLow);
    m_CleanMid->getValue(tMid);
    m_CleanMidStr->getValue(midStr);
    m_CleanMaxGain->getValue(maxGain);
    m_CleanMaxExp->getValue(maxExp);

    // Rolloff: the only shoulder Base has, since Lift/Gamma/Gain cannot make an S-curve on
    // its own. Two drivers, whichever asks for more --
    //   pin       0.090 x pin%              floor; softens source-clipped, detail-free patches
    //   overshoot k x max(0, ch99 - 1.0)    how far the channels run past display white
    // `hot` is ruled out twice over: it runs backwards, wanting more shoulder on the 35.6%-hot
    // beach than the 17.8%-hot office, which is already comfortable.
    double shoulder = 0.216;
    m_CleanShoulder->getValue(shoulder);
    const double rolloff = std::min(0.80, std::max(0.00,
        std::max(0.090 * m_LastPin, shoulder * std::max(0.0, m_LastD99 - 1.0))));

    // The whole chain in closed form. With no LUT and Contrast at 1.0, trim is just a multiply
    // by 2^postExp, so the rendered value is exactly:
    //     softclip( lgg_core(d, lift, gamma, gain) * 2^postExp , rolloff )
    // Every stage Base touches is in there, which is why no iteration is needed: test 14
    // proves lgg_core predicts the render, and the two stages after it are this simple.
    const double d01 = m_LastR01, d50 = m_LastD50, d99 = m_LastD99;   // render encode: it goes through og_lgg
    double gain = 1.0, lift = 0.0, postExp = 0.0;
    const double gamma = 1.0;           // the midtone rides on exposure, not gamma
    auto chain = [&](double d, double lf, double gn, double pe) {
        const double v = og_grade_display(d, lf, gamma, gn) * std::exp2(pe);
        return rolloff > 0.0 ? (double)og::softclip((float)v, (float)rolloff) : v;
    };

    // Mid Strength blends the shot's OWN midtone toward the target rather than damping the
    // correction: damping only changes how fast you arrive at the same place. 0 keeps the shot
    // as exposed, 1 forces the target.
    double tMidEff = tMid;

    // Three coordinate passes. Each control owns the end it pivots away from -- gain the top
    // (pivots black), lift the bottom (pivots white), exposure the middle -- so they are
    // nearly orthogonal and settle immediately.
    for (int pass = 0; pass < 3; ++pass) {
        gain = og_solve(0.05, 3.0, tHigh, [&](double g) { return chain(d99, lift, g, postExp); });
        if (gain > maxGain) gain = maxGain;

        const double natural = chain(d50, lift, gain, 0.0);
        if (natural > 1e-4 && tMid > 1e-4) tMidEff = natural * std::pow(tMid / natural, midStr);

        // Brightening is capped, darkening is not: pulling a blown frame down is always safe,
        // pushing a dark one up destroys a deliberately low-key shot. A car interior at
        // key +2.90 asked for +1.74 stops without this; the same shot graded by hand used
        // +0.55. Same asymmetry as the Gain ceiling.
        postExp = og_solve(-3.0, 3.0, tMidEff, [&](double pe) { return chain(d50, lift, gain, pe); });
        postExp = std::min(maxExp, std::max(-3.0, postExp));

        lift = og_solve(-0.5, 0.5, tLow, [&](double l) { return chain(d01, l, gain, postExp); });
    }

    gain  = std::min(3.00, std::max(0.05, gain));
    lift  = std::min(0.50, std::max(-0.50, lift));

    m_Gain->setValue(gain);
    m_Lift->setValue(lift);
    m_Gamma->setValue(1.0);
    m_Rolloff->setValue(rolloff);
    m_PostExp->setValue(postExp);

    // Bias offsets from whatever this solve just wrote — see armBias(). It used to be unsafe
    // to let Bias touch a Base grade at all, because applyBias() re-derived Lift and Gamma
    // from the FILM recipe's constants and would have undone the containment on the first
    // drag. Now that it is a relative offset from a saved anchor, both modes are fine.
    m_LastGain = gain;

    // Reports what the solve ACHIEVED, not what it aimed at, so an unreachable target is
    // visible rather than a quietly pinned slider.
    char msg[160];
    snprintf(msg, sizeof msg, "Base G %.2f E%+.2f L %+.2f R %.2f -> %.2f/%.2f/%.2f",
             gain, postExp, lift, rolloff,
             chain(d01, lift, gain, postExp),
             chain(d50, lift, gain, postExp),
             chain(d99, lift, gain, postExp));
    m_ProbeApplied->setValue(msg);

    armBias(true);                     // fresh grade: Bias returns to neutral
    applyBias();
    refreshRangeLatch();               // the grade under the mask just moved
    setEnabledness();
}

// GRADE ON DROP -- TRIED, AND IT CRASHES RESOLVE. DO NOT RETRY THIS SHAPE.
//
// The idea was to run Base Grade once at instance creation so a freshly dropped node was
// already in a gradable place. It was built, guarded by a saved `autoInitDone` param so a
// reload could not re-stamp a user's grade, and wrapped in try/catch on the assumption that
// the worst case was "no image this early, keep the defaults".
//
// Every part of that assumption was wrong. Calling fetchImage() from the instance
// CONSTRUCTOR trips an assertion inside Resolve, which calls abort(). Confirmed from a crash
// report, 2026-08-03 -- the stack is unambiguous:
//
//     __assert_rtn -> abort
//     ...
//     OFX::Clip::fetchImage(double)
//     OneGrade::probeAnalyze(double)
//     OneGrade::applyAutoGradeClean(double)
//     OneGrade::autoInitOnce()
//     OneGrade::OneGrade(OfxImageEffectStruct*)
//     OneGradeFactory::createInstance(...)
//
// TRY/CATCH CANNOT HELP: abort() is a process abort, not a C++ exception, so there is
// nothing to catch. Defensive wrapping gives no protection at all against a host assert,
// which is worth remembering the next time "wrap it and see" looks like a safe experiment.
//
// It was also far worse than "affects new nodes only". `autoInitDone` does not exist in
// projects saved before that build, so it defaulted to false and fired on EVERY node in
// every existing project -- the user's project died at 94% on load.
//
// The lesson generalises past this feature. fetchImage is safe from changedParam (validated
// during Auto Grade step 1) and from render. It is NOT safe from a lifecycle hook the host
// calls during project load. Anything that reads pixels must hang off a user action or a
// render, never off instance construction.
//
// If a pleasing default on drop is still wanted, the only route that carries no risk is
// better STATIC defaults -- footage-blind, but it cannot crash anything. See
// docs/ROADMAP.md.

// BIAS — one slider trading highlight restraint against shadow openness, because a
// measurement can only get a shot into the right neighbourhood; which end of that
// neighbourhood you want is taste, and taste needs a knob rather than a constant.
//
//   negative -> protect: shoulder the top, deepen the floor, darken mids, pull gain
//   positive -> open:    drop the shoulder, raise the floor, brighten mids and gain
//
// IT IS A RELATIVE OFFSET FROM AN ANCHOR, not a recomputation. The first version wrote
// ABSOLUTE values -- `lift = 0.11 + bias*0.06`, where 0.11 is the film preset's lift -- so it
// stamped the film recipe over whatever grade was actually there. Harmless while Creative was
// the only caller; wrong the moment Base existed, since Base solves its own lift. It also
// hung off instance state, which made it silently
// inert after a project reload.
//
// The anchor is five hidden params SAVED WITH THE PROJECT, so bias survives a restart and
// works for both grade modes without either needing to know the other exists. Creative's
// response is unchanged: at bias 0 the anchor holds exactly what Creative wrote, and the
// deltas are the same constants as before.
//
// If bias is moved on a node that was never auto-graded, the CURRENT parameter values become
// the anchor. That makes the slider work on a hand-built grade too, and it is safe because
// changedParam fires before anything else has been touched.
// Capture the current grade as Bias's zero point.
//
// `reset` is what a fresh button press does: Base, Creative and Magic all hand back a complete
// grade, and leaving a stale Bias on top of it pins every future press to whatever lean was left
// behind. Pressing the button should give you the grade the button computed, not that grade
// plus yesterday's taste.
//
// A manual edit re-arms WITHOUT resetting, recording the bias value the anchor was taken at so
// the offset is measured from there. That is what makes a hand tweak survive: the slider does
// not jump, the picture does not move, and the next drag continues from where the user left it
// rather than from where the button did.
// RE-DERIVE WHAT THE GRADE CURRENTLY MEETS, so Bias leans away from that rather than from the
// constants Magic Tone was solved to.
//
// Without this a hand edit to Lift, Gamma or Gain is discarded the next time Bias is touched:
// armBias() re-anchors, which preserves the edit exactly on the offset path, but the solving
// path drives back to targets stored when the button was pressed and the anchor is only the
// seed for a bracketed solve. So the fix is not to remember the parameters harder -- it is to
// move the CONDITIONS, because the conditions are what that path is actually about.
//
// Measured through tone_render(), the same function the solve places its targets with, so "where
// is the subject now" and "where should the subject go" can never be answered differently.
//
// No-op unless Magic Tone armed this node: the offset path already preserves edits.
// MEASURE THE LATCH, DO NOT MAKE THE USER FIND IT.
//
// The threshold cannot be derived at render: a percentile needs a reduction over the whole image
// and the kernels are per-pixel, which is the same reason Auto Grade is a button. So it is a saved
// param with a button beside it -- and that is better than automatic anyway, because a latch that
// re-measured every frame would move under the grade while you worked.
//
// READ THROUGH THE GRADE CURVE, mirroring what the mask itself reads. Measuring the flat
// pre-grade image is what made the latch unmatchable against a Resolve qualifier.
//
// SPLIT THE FRAME, DO NOT TAKE A PERCENTILE. The first version used p98, which assumes the
// highlight is a fixed share of the frame -- and it is not. On the user's bedroom p98 put the
// edge above the window entirely; on a landscape whose top half is cloud it selected 1.97%.
// og::grade::range_latch() reads the SHAPE of the histogram instead, and on those same two
// frames returns 58.2 (the window, 7.5%) and 46.9 (the sky, 52.5%) -- one rule, two shot
// shapes, both matting the thing the user pointed at. Still a starting point the slider owns.
void OneGrade::setRangeLatch(double p_Time, bool p_Reanalyse, bool p_ShowMatte)
{
    // Re-sampling means fetchImage plus the whole descriptor pass. The refresh path runs straight
    // after a grade that has just done it, on the same frame, and the samples are SOURCE pixels --
    // unchanged by anything the grade did. So it reuses them, which also keeps probeAnalyze from
    // overwriting state the caller is still using.
    if (p_Reanalyse || m_LastSamples.size() < 512) probeAnalyze(p_Time);
    const size_t n = m_LastSamples.size();
    if (n < 512) return;

    float Pn[oga::kParamN]; oga::neutral_params(Pn);
    Pn[2]=(float)m_Density->getValue();
    Pn[3]=(float)m_Lift->getValue(); Pn[4]=(float)m_Gamma->getValue(); Pn[5]=(float)m_Gain->getValue();
    Pn[10]=(float)m_RawExp->getValue(); Pn[11]=(float)m_RawTemp->getValue();
    // neutral_params() leaves Range Balance OFF, which is what this measurement needs: it is the
    // stage's INPUT, and a latch read through the mask it is about to set would chase its own
    // output. It also guarantees P[18] is clear -- Show Mask on here would hand og::process the
    // matte to measure instead of the picture.

    int cam = 0, enc = 0;
    m_Camera->getValue(cam); m_Encode->getValue(enc);
    const int dispEnc = (enc <= 2) ? enc : 1;

    std::vector<float> y; y.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        float r, g, b;
        og::process(cam, dispEnc, Pn, m_LastSamples.rgb[i*3], m_LastSamples.rgb[i*3+1],
                    m_LastSamples.rgb[i*3+2], r, g, b);
        y.push_back(0.2126f*r + 0.7152f*g + 0.0722f*b);
    }
    const og::grade::RangeLatch RL = og::grade::range_latch(y);
    // No gap in the histogram means no two populations to hold apart. Leave the latch where the
    // user had it and say so, rather than stamping an edge that would matte most of the picture:
    // Otsu answers on any frame, so declining is the caller's job, not the split's.
    if (!RL.ok) {
        char why[64];
        snprintf(why, sizeof why, "No bright region here (gap %.0f, needs %.0f)",
                 RL.gap, og::grade::kRangeGapMin);
        m_RangeNote->setValue(why);
        return;
    }
    m_RangeLatch->setValue(RL.latch);
    // CAPTURE THE GRADE THE LATCH WAS MEASURED AGAINST, always -- whether the Lock is on or not.
    // The measurement and the reference have to be the same picture or Lock would freeze the mask
    // somewhere the latch was never chosen for, and the anchor is meaningless until a latch exists
    // anyway. Ticking Lock afterwards then costs nothing and needs no second button.
    m_RangeRefLift ->setValue(m_Lift->getValue());
    m_RangeRefGamma->setValue(m_Gamma->getValue());
    m_RangeRefGain ->setValue(m_Gain->getValue());
    // Show the matte straight away -- for the BUTTON. Dialling a latch you cannot see is
    // guesswork, and the measured value is a starting point rather than an answer, so the point
    // of pressing it is to put you somewhere close enough to judge. The automatic refresh passes
    // false: it fires behind a grade the user asked for, and taking over the viewer with a matte
    // nobody asked to see is not a refresh, it is an interruption.
    if (p_ShowMatte) m_RangeShow->setValue(true);

    // Coverage is the number that tells you whether the split found what you meant. 7% on an
    // interior reads as "the window"; 52% on a landscape reads as "the sky"; 0.2% reads as
    // "it latched onto a practical" before you have looked at the matte at all.
    //
    // The refresh says so. A number that moved without being touched has to name what moved it,
    // or it reads as the panel losing the value the user set.
    char note[64];
    snprintf(note, sizeof note, "%s %.1f, holds %.1f%% of frame",
             p_ShowMatte ? "Latch" : "Re-latched", RL.latch, RL.cover);
    m_RangeNote->setValue(note);
}

// KEEP AN EXISTING LATCH CURRENT AFTER THE GRADE UNDER IT CHANGES.
//
// The mask reads the picture AFTER the grade curve, so anything that rewrites Lift/Gamma/Gain
// moves the luminance the latch was measured against -- and a latch set before Magic Grade
// describes a picture that no longer exists. The manual answer was "press Set From Frame again",
// which is a step nobody should have to know about.
//
// TWO DELIBERATE LIMITS:
//
// It only fires when a latch is ALREADY SET. Range Balance is off at latch 0, and a button the
// user has never pressed must not start stamping values into a stage they are not using.
//
// It fires on the BUTTONS that replace the whole grade, not on slider edits. Magic Grade, Auto
// Grade, the Subject dropdown and presets hand you a grade you did not dial, so the latch under
// it is stale through no act of yours. Dragging Gain yourself is different: re-measuring under
// the hand would make the latch move while you worked, which is exactly the behaviour the
// button-not-automatic design was chosen to avoid. Bias is excluded for the same reason plus a
// harder one -- it re-solves live during a drag, and a frame measurement per tick is not real
// time.
// No time argument, because it never fetches: it re-reads the samples the grade it follows has
// just taken. If there are none it does nothing rather than going to the host for a frame -- a
// silent refresh is not worth an image fetch, and pressing the button is right there.
void OneGrade::refreshRangeLatch()
{
    double latch = 0.0;
    m_RangeLatch->getValue(latch);
    if (latch <= 0.0 || m_LastSamples.size() < 512) return;
    // A LOCKED MASK IS NOT REFRESHED. Lock means the selection holds while the grade under it
    // moves, and Magic Grade moving the grade is the largest instance of exactly that -- a button
    // silently re-measuring a mask the user locked would break the one promise the control makes.
    // Re-measuring would also not preserve it: Otsu re-run on the new grade can land on a
    // different population, so this is not "the same mask, restated".
    bool lock = false;
    m_RangeLock->getValue(lock);
    if (lock) return;
    setRangeLatch(0.0, /*reanalyse=*/false, /*showMatte=*/false);
}

// FIT THE SHAPE TO WHAT THE LATCH ALREADY FOUND.
//
// The on-screen handle turned out to be unavailable: Resolve advertises OFX overlay support, our
// interact registers, and draw() is then never called on the Color page. Dragging four sliders to
// aim a rectangle is worse than Resolve's own power window, so competing on that was never the
// point -- what this plugin has that a power window does not is a MEASUREMENT.
//
// So: take the samples the latch is already holding and put the shape around them. On the bedroom
// frame that is the window, and the answer arrives without anyone aiming anything.
//
// PERCENTILES, NOT MIN/MAX. A bounding box is the textbook move and it is exactly wrong here: one
// stray specular on the far side of the room stretches the box across the whole frame, which is
// the failure this feature exists to prevent. p2/p98 of the held positions ignores the strays and
// lands on the population -- the same reasoning as percentiles over means everywhere else here.
void OneGrade::fitRangeShape(double p_Time)
{
    double latch = 0.0;
    m_RangeLatch->getValue(latch);
    if (latch <= 0.0) { m_RangeShapeNote->setValue("Set the latch first"); return; }
    if (m_LastSamples.size() < 512) probeAnalyze(p_Time);
    const size_t n = m_LastSamples.size();
    if (n < 512 || m_LastSamples.u.size() != n) {
        m_RangeShapeNote->setValue("Could not read this frame");
        return;
    }

    // Measured through the SAME picture the mask reads -- the grade curve applied, Range Balance
    // itself off. A shape fitted to a different render than the mask uses would sit beside it.
    float Pn[oga::kParamN]; oga::neutral_params(Pn);
    Pn[2]=(float)m_Density->getValue();
    Pn[3]=(float)m_Lift->getValue(); Pn[4]=(float)m_Gamma->getValue(); Pn[5]=(float)m_Gain->getValue();
    Pn[10]=(float)m_RawExp->getValue(); Pn[11]=(float)m_RawTemp->getValue();
    int cam = 0, enc = 0;
    m_Camera->getValue(cam); m_Encode->getValue(enc);
    const int dispEnc = (enc <= 2) ? enc : 1;

    std::vector<float> hu, hv;
    for (size_t i = 0; i < n; ++i) {
        float r, g, b;
        og::process(cam, dispEnc, Pn, m_LastSamples.rgb[i*3], m_LastSamples.rgb[i*3+1],
                    m_LastSamples.rgb[i*3+2], r, g, b);
        if (100.f*(0.2126f*r + 0.7152f*g + 0.0722f*b) >= (float)latch) {
            hu.push_back(m_LastSamples.u[i]);
            hv.push_back(m_LastSamples.v[i]);
        }
    }
    if (hu.size() < 64) { m_RangeShapeNote->setValue("Too little held to fit a shape"); return; }

    auto pct = [](std::vector<float>& v, double q) {
        const size_t k = (size_t)(q * (v.size() - 1));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return (double)v[k];
    };
    const double u0 = pct(hu, 0.02), u1 = pct(hu, 0.98);
    const double v0 = pct(hv, 0.02), v1 = pct(hv, 0.98);

    // 0..1 with a bottom-left origin, into the shape's centre-origin half-height units. The x
    // scale carries the aspect ratio because both axes are normalised by HALF-HEIGHT -- the same
    // convention shape_mask() uses, and the reason a circle stays round.
    double asp = 16.0/9.0;
    if (m_SrcClip) {
        const OfxRectD rod = m_SrcClip->getRegionOfDefinition(p_Time);
        const double w = rod.x2 - rod.x1, h = rod.y2 - rod.y1;
        if (w > 1.0 && h > 1.0) asp = w/h;
    }
    const double cx = ((u0 + u1)*0.5 - 0.5) * 2.0 * asp;
    const double cy = ((v0 + v1)*0.5 - 0.5) * 2.0;
    // Pad by a tenth. The fit is to where the held pixels ARE, and a shape that grazes them clips
    // the edge of the very thing it was fitted to as soon as the softness feathers inward.
    const double sx = std::max(0.02, (u1 - u0) * asp * 1.10);
    const double sy = std::max(0.02, (v1 - v0) * 1.10);

    int shp = 0; m_RangeShape->getValue(shp);
    if (shp <= 0) m_RangeShape->setValue(2);      // a rectangle: windows and skies are rectangular
    m_RangeShapeX->setValue(cx); m_RangeShapeY->setValue(cy);
    m_RangeShapeW->setValue(sx); m_RangeShapeH->setValue(sy);

    char note[64];
    snprintf(note, sizeof note, "Fitted to %.1f%% of frame", 100.0*(double)hu.size()/(double)n);
    m_RangeShapeNote->setValue(note);
}

// SHAPE THE SHOULDER TO THIS FRAME, rather than using one curve for every shot.
//
// The whole argument for doing this in a plugin instead of a fixed transform is that we have the
// pixels. A constant knee/white is a compromise struck once against a corpus -- too gentle for a
// frame that peaks at 3.3, needless compression on one that peaks at 1.1. The measurement lives in
// og::grade::fit_tone_map() so the bench calls the same code.
void OneGrade::fitToneMap(double p_Time)
{
    probeAnalyze(p_Time);
    if (m_LastSamples.size() < 512) { m_ToneMapNote->setValue("Could not read this frame"); return; }

    float P[oga::kParamN]; oga::neutral_params(P);
    P[2]=(float)m_Density->getValue();
    P[3]=(float)m_Lift->getValue(); P[4]=(float)m_Gamma->getValue(); P[5]=(float)m_Gain->getValue();
    P[10]=(float)m_RawExp->getValue(); P[11]=(float)m_RawTemp->getValue();
    int cam = 0, enc = 0;
    m_Camera->getValue(cam); m_Encode->getValue(enc);
    const int dispEnc = (enc <= 2) ? enc : 1;

    const og::grade::ToneMapFit f = og::grade::fit_tone_map(m_LastSamples, cam, dispEnc, P);
    if (!f.ok) { m_ToneMapNote->setValue(f.why); return; }

    if (f.white <= 0.0) {
        // Nothing exceeds white, so the honest fit is no shoulder at all. Switching one on anyway
        // would compress a picture that already fits -- a tone map is a remedy, not a house style.
        m_ToneMap->setValue(false);
        m_ToneMapNote->setValue(f.why);
        return;
    }
    m_ToneMapKnee->setValue(f.knee);
    m_ToneMapWhite->setValue(f.white);
    m_ToneMap->setValue(true);
    m_ToneMapNote->setValue(f.why);
}

void OneGrade::armToneTargets()
{
    double tLo = -1.0, tMid = -1.0, tShi = -1.0, tHi = -1.0, tFLo = -1.0;
    m_ToneLo->getValue(tLo);  m_ToneMid->getValue(tMid);
    m_ToneShi->getValue(tShi); m_ToneHi->getValue(tHi); m_ToneFLo->getValue(tFLo);
    if (!(tLo >= 0.0 && tMid >= 0.0 && tShi >= 0.0 && tHi >= 0.0 && tFLo >= 0.0)) {
        m_ToneTFloor->setValue(-1.0); m_ToneTMid->setValue(-1.0);
        m_ToneTCeil->setValue(-1.0);  m_ToneTFMax->setValue(-1.0);
        return;
    }
    float P[og::analysis::kParamN]; oga::neutral_params(P);
    P[0]=(float)m_Temp->getValue();    P[1]=(float)m_Tint->getValue();
    P[2]=(float)m_Density->getValue(); P[3]=(float)m_Lift->getValue();
    P[4]=(float)m_Gamma->getValue();   P[5]=(float)m_Gain->getValue();
    P[6]=(float)m_OffTemp->getValue(); P[7]=(float)m_OffTint->getValue();
    P[8]=(float)m_PostExp->getValue(); P[9]=(float)m_PostCon->getValue();
    P[10]=(float)m_RawExp->getValue(); P[11]=(float)m_RawTemp->getValue();
    P[12]=(float)m_Rolloff->getValue();
    const bool lutOk = ensureLutLoaded();
    const og::grade::ToneTargets t = og::grade::tone_targets_of(
        tLo, tMid, tHi, tFLo, P, lutOk ? m_Lut.data.data() : nullptr, lutOk ? m_Lut.size : 0);
    m_ToneTFloor->setValue(t.floor);
    m_ToneTMid->setValue(t.mid);
    m_ToneTCeil->setValue(t.ceil);
    m_ToneTFMax->setValue(t.floorMax);
}

void OneGrade::armBias(bool reset)
{
    if (reset) { m_AutoBias->setValue(0.0); m_BiasMirror->setValue(0.0); }
    m_BiasAt->setValue(reset ? 0.0 : m_AutoBias->getValue());
    m_BiasGain->setValue(m_Gain->getValue());
    m_BiasLift->setValue(m_Lift->getValue());
    m_BiasGamma->setValue(m_Gamma->getValue());
    m_BiasRoll->setValue(m_Rolloff->getValue());
    m_BiasHot->setValue(m_LastHot);
    m_BiasArmed->setValue(true);
}

void OneGrade::applyBias()
{
    bool armed = false;
    m_BiasArmed->getValue(armed);
    if (!armed) armBias(false);     // adopt whatever is on the node right now

    double bias = 0.0; m_AutoBias->getValue(bias);
    // Offset from where the anchor was taken, not from zero. After a manual Lift/Gamma/Gain
    // edit the anchor is re-armed at the current values with biasAt set to the slider's current
    // position, so this delta is zero at that instant and the hand tweak is preserved exactly.
    double biasAt = 0.0; m_BiasAt->getValue(biasAt);
    bias -= biasAt;

    // WHEN MAGIC TONE OWNS THE GRADE, BIAS MOVES THE TARGETS AND RE-SOLVES.
    //
    // The path below offsets Lift, Gamma and Gain by fixed coefficients. That is fine against a
    // preset, and destroys a solved grade: Magic Tone chose those three to satisfy three
    // conditions at once, so nudging any of them breaks all three and the picture falls apart on
    // the first slider move. Reported on footage as "if I touch the bias slider we kill the
    // grade", which is exactly what it was doing.
    //
    // Shifting the TARGETS instead is both correct and what the slider was asked to be: a
    // neutral starting point that contrast can be added to or taken from. Opening up raises the
    // subject's floor and lowers the frame's ceiling (less range, flatter); closing down does the
    // reverse. The subject's midtone never moves, because that is the legibility anchor -- Bias
    // changes how much contrast surrounds the subject, never how bright the subject is.
    //
    // It also makes crushing STRUCTURALLY IMPOSSIBLE rather than guarded against. The floor is a
    // target the solve hits, not a value the slider drifts toward, so there is nothing for the
    // anti-crush guard below to catch. Bias at -1 places the floor lower and the solve puts it
    // exactly there.
    //
    // Cheap enough to drag: the three percentiles are neutral measurements and do not depend on
    // Lift, Gamma or Gain, so a re-solve is three scalar bisections over cached scalars -- no
    // re-measuring, and no re-segmenting, which is what made this button read its own output.
    double tLo = -1.0, tMid = -1.0, tShi = -1.0, tHi = -1.0, tFLo = -1.0;
    m_ToneLo->getValue(tLo); m_ToneMid->getValue(tMid);
    m_ToneShi->getValue(tShi); m_ToneHi->getValue(tHi); m_ToneFLo->getValue(tFLo);
    if (tLo >= 0.0 && tMid >= 0.0 && tShi >= 0.0 && tHi >= 0.0 && tFLo >= 0.0) {
        // Neutral first -- see Pg above. RANGE BALANCE STAYS NEUTRAL HERE deliberately, not just
        // defined: it is a masked local adjustment applied after the grade curve, so letting it
        // into the solve's measurement would make the answer depend on the mask the solve is not
        // solving for. Same reason setRangeLatch() measures with it switched off.
        float Pc[oga::kParamN]; oga::neutral_params(Pc);
        Pc[0]=(float)m_Temp->getValue();    Pc[1]=(float)m_Tint->getValue();
        Pc[2]=(float)m_Density->getValue();
        // THE SOLVE STARTS FROM THE ARMED GRADE, NEVER FROM THE LIVE SLIDERS.
        //
        // These three used to be read straight off the node -- which are the PREVIOUS bias
        // solve's own output, so every drag event started where the last one finished and the
        // slider was reading itself. solve_magic_tone_from is path-dependent by design (its
        // ceiling fallback deliberately restores Creative's gamma from P0[4], and the coordinate
        // passes start where they are told), so feeding results forward turned the drag into a
        // 2-CYCLE: on one frame Lift alternated 0.087 / 0.000 and Gain 0.29 / 0.55 on every
        // other 0.002 of slider, from about +0.9 upward. On screen that is the picture flipping
        // between lifted and normal over and over as the slider moves -- reported as an
        // inversion, and the second thing this slider has done that looked like one.
        //
        // The anchor armBias() stores is exactly the stable reference this needs, and it was
        // already sitting there being used only by the coefficient path below. With it, the same
        // slider position always produces the same grade no matter how it was reached.
        //
        // Third time in this project a control has read its own output: Auto Grade's first press
        // measured the node it was about to change, White Balance settled over three presses,
        // and now this. The tell is the same every time -- the answer depends on the history
        // rather than on the footage.
        double aL = 0.0, aG = 1.0, aN = 1.0;
        m_BiasLift->getValue(aL); m_BiasGamma->getValue(aG); m_BiasGain->getValue(aN);
        Pc[3]=(float)aL; Pc[4]=(float)aG; Pc[5]=(float)aN;
        Pc[6]=(float)m_OffTemp->getValue(); Pc[7]=(float)m_OffTint->getValue();
        Pc[8]=(float)m_PostExp->getValue(); Pc[9]=(float)m_PostCon->getValue();
        Pc[10]=(float)m_RawExp->getValue(); Pc[11]=(float)m_RawTemp->getValue();
        Pc[12]=(float)m_Rolloff->getValue();
        const bool lutOk = ensureLutLoaded();
        og::grade::Tunables tn;
        // THE WHOLE SLIDER LAW IS IN THE HEADER, so the bench walks the same curve this does.
        // It holds at the last feasible bias rather than declining, which is what stops the
        // picture jumping when the targets stop being reachable -- see solve_magic_tone_bias.
        // The conditions the grade currently meets. Unset (-1) falls back to the fitted
        // constants inside the solve, which is what a fresh Magic grade wants.
        og::grade::ToneTargets base;
        m_ToneTFloor->getValue(base.floor);
        m_ToneTMid->getValue(base.mid);
        m_ToneTCeil->getValue(base.ceil);
        m_ToneTFMax->getValue(base.floorMax);
        // ONE SOLVE FOR BOTH SLIDERS. Bias and Tone Separation move different targets -- the
        // subject's floor and the frame's ceiling against the subject's midtone -- but they move
        // them in the same picture, so solving them separately would let each undo the other.
        // Both travel one line from the armed anchor, so the result depends on where they are
        // left rather than on which was touched last.
        double sep = 0.0, sepDir = 0.0;
        m_ToneSep->getValue(sep);
        m_ToneSepDir->getValue(sepDir);
        const og::grade::MagicTone mt = og::grade::solve_magic_tone_bias(
            tLo, tMid, tShi, tHi, Pc, lutOk ? m_Lut.data.data() : nullptr, lutOk ? m_Lut.size : 0,
            tn, tFLo, bias, base, sep, sepDir);
        if (mt.ok) {
            m_Lift->setValue(mt.lift);
            m_Gamma->setValue(mt.gamma);
            m_Gain->setValue(mt.gain);
            return;
        }
    }
    double aGain = 1.0, aLift = 0.0, aGamma = 1.0, aRoll = 0.0, aHot = 0.0;
    m_BiasGain->getValue(aGain);
    m_BiasLift->getValue(aLift);
    m_BiasGamma->getValue(aGamma);
    m_BiasRoll->getValue(aRoll);
    m_BiasHot->getValue(aHot);

    // Gain's response is measurement-modulated in one direction only. Brightening a frame
    // that already has a third of itself above display white just pushes more past clipping,
    // so the positive direction fades out with remaining headroom and is gone by ~40% hot.
    // The negative direction is never scaled: pulling gain down is always safe.
    const double headroom  = std::max(0.0, 1.0 - aHot / 40.0);
    const double gainDelta = (bias >= 0.0) ? bias * 0.08 * headroom : bias * 0.08;

    const double rolloff = std::min(0.80, std::max(0.00, aRoll  - bias * 0.35));
    double       lift    = std::min(0.50, std::max(-0.50, aLift  + bias * 0.06));
    const double gamma   = std::min(3.00, std::max(0.20, aGamma + bias * 0.12));
    const double gain    = std::min(3.00, std::max(0.20, aGain  + gainDelta));

    // ANTI-CRUSH FLOOR — needed the moment Creative stopped stamping a fixed Lift.
    //
    // Bias's -0.06 per unit was calibrated against the preset's constant 0.11, which left
    // plenty of room underneath. Now that Creative SOLVES its lift, the anchor can legitimately
    // land near zero, and a full negative bias on top drives the black point through it.
    // Measured on the beach: solved lift -0.019, bias -1 takes it to -0.079, and the black
    // point lands at -0.03. Crushed, from two changes that are each individually correct.
    //
    // Enforced as a floor rather than by re-tuning the coefficient, because the coefficient is
    // taste and the floor is a fact: "higher contrast, but not to the point of crushing black
    // areas" was the user's constraint and it should hold whatever the anchor happens to be.
    // Bias keeps its full range and simply stops taking shadows away once there are none left.
    if (m_HaveKey) {
        const double pe = m_PostExp->getValue();
        auto blackAt = [&](double lf) {
            const double v = og_grade_display(m_LastR01, lf, gamma, gain) * std::exp2(pe);
            return rolloff > 0.0 ? (double)og::softclip((float)v, (float)rolloff) : v;
        };
        const double blackFloor = 0.006;          // ~1.5 code values at 8 bit
        if (blackAt(lift) < blackFloor) lift = og_solve(lift, 0.50, blackFloor, blackAt);
    }
    m_Rolloff->setValue(rolloff);
    m_Lift->setValue(lift);
    m_Gamma->setValue(gamma);
    m_Gain->setValue(gain);
}

// MAGIC GRADE — Creative Grade, then one colour move chosen from what is in the frame.
//
// The chain is the user's: apply Creative, run the classifier, pick a subject, pick a slider
// and a direction, render it. The Separation slider then scales that decision. Press the button
// again and a DIFFERENT subject is chosen, and the same process runs.
//
// NOT EVERY SHOT HAS A MOVE AND THAT IS FINE. A downward city view comes back as one
// undifferentiated region and correctly produces nothing; Magic Grade is then just Creative
// Grade, and says so in the readout. The user's own grade of that shot was purely tonal, so
// human and machine agree. Silence is the failure mode to avoid, not the absence of a move.
//
// THE REGION MASKS ARE A STAND-IN (see stub_regions). Everything here consumes SampleSet::region
// and nothing else, so a real segmentation model swaps into that one function. This exists to
// prove the chain end to end in Resolve BEFORE the model goes in, so the model lands in a slot
// already known to work.
// Rebuild the Subject list from the segmentation the last press produced. Mirrors
// populateLookLut(): resetOptions() + appendOption() at runtime is a pattern this plugin already
// relies on, so Resolve is known to handle it.
//
// Labels carry the coverage because coverage is the tell. A face at 13% is a face; "skin" at 46%
// is sand, and the number is the only thing that says so -- the same reason the analysis panel
// has always printed it next to the subject key.
void OneGrade::populateMagicSubject()
{
    m_MagicSubject->resetOptions();
    if (!m_HaveMagicBase || m_LastSamples.size() < 512) {
        m_MagicSubject->appendOption("- press Magic Grade -");
        return;
    }
    const int n = og::grade::magic_option_count(m_LastSamples, og::grade::kCreativeCamera,
                                                og::grade::kCreativeEncode, m_MagicBaseP);
    if (n <= 0) {
        m_MagicSubject->appendOption("- nothing to separate -");
        return;
    }
    for (int k = 0; k < n; ++k) {
        const oga::MagicChoice c = og::grade::magic_option_at(
            m_LastSamples, og::grade::kCreativeCamera, og::grade::kCreativeEncode, m_MagicBaseP, k);
        char lab[64];
        snprintf(lab, sizeof lab, "%d - %s %.0f%%", k + 1,
                 c.ok ? oga::region_name(c.subject) : "?", c.ok ? c.cover : 0.f);
        m_MagicSubject->appendOption(lab);
    }
}

// Re-grade around the subject the user picked. No frame fetch, no measurement, no inference --
// steps 4-7 over a SampleSet whose regions are already assigned, which is why this can sit on a
// dropdown instead of behind a button press.
void OneGrade::applyMagicSubject()
{
    if (!m_HaveMagicBase || m_LastSamples.size() < 512) return;
    int idx = 0; m_MagicSubject->getValue(idx);

    og::grade::Tunables tun;
    m_CreativeLow->getValue(tun.blackTarget);
    // Re-checked rather than trusted from the press: the user can change LUT Mode in between, and
    // a solve placed against the wrong LUT is the bug that made the first press come out crushed.
    const bool lutOk = ensureLutLoaded();

    og::grade::Measurements meas;
    meas.key = m_LastKey; meas.pin = m_LastPin; meas.hot = m_LastHot;
    meas.d01 = m_LastR01; meas.d50 = m_LastD50; meas.d99 = m_LastD99;
    meas.valid = true;

    const og::grade::MagicResult R = og::grade::solve_magic_from_regions(
        m_LastSamples, m_MagicBaseP, meas, og::grade::kCreativeCamera, og::grade::kCreativeEncode,
        lutOk ? m_Lut.data.data() : nullptr, lutOk ? m_Lut.size : 0, tun, idx, 1.0);

    applyMagicResult(R, s_seg_ready() ? "model" : "heuristic", /*repopulate=*/false);
}

void OneGrade::applyMagicGrade(double p_Time)
{
    // THE ORDER LIVES IN ONE PLACE NOW. Everything from here to the panel notes used to be
    // spelled out twice -- once here and once in the bench -- and it drifted: a re-solve after
    // the colour move landed in one and not the other, and the two produced different pictures
    // from the same still. og::grade::solve_magic owns the sequence; this owns the panel.
    //
    // Writing it down exposed a second live divergence: the plugin balanced BEFORE Creative and
    // the bench balanced after, so Creative solved its black point at 6500 K in one and at the
    // corrected temperature in the other. Before is right, and now there is only one answer.
    probeAnalyze(p_Time, /*forCreative=*/true);
    if (!m_HaveKey || m_LastSamples.size() < 512) {
        m_MagicNote->setValue("No frame to analyse");
        m_MagicWhy->setValue("");
        m_MagicParam->setValue(-1);
        m_HaveMagicBase = false;
        populateMagicSubject();
        setEnabledness();
        return;
    }

    // Camera, the film LUT and the film tint -- the parts of Creative that are not in P[] and so
    // cannot come back from the solve. It must run before the solve, because the LUT it selects
    // is the one the solve places its targets against.
    applyPreset(1);

    bool wbFirst = false; m_WbFirst->getValue(wbFirst);
    // ONE PRESS, THEN PICK. The button used to cycle: press again for a different subject, with
    // no way to see what the alternatives were or to go back to one. It always solves the FIRST
    // option now and hands the rest to the Subject dropdown, which re-runs from the cached
    // segmentation. Everything expensive about this button -- the frame fetch, the measurement
    // and ~100 ms of inference -- happens once, and choosing between its answers is arithmetic.
    const int cycle = 0;
    og::grade::Tunables tun;
    m_CreativeLow->getValue(tun.blackTarget);
    // LOADED, not merely selected. applyPreset(1) has just chosen the film LUT, and on a node
    // that has not rendered since, m_Lut is still empty -- so the whole solve would place its
    // targets pre-LUT and let the print stock's toe crush them afterwards.
    const bool lutOk = ensureLutLoaded();

    // REGION SOURCE. The model is allowed to be absent -- Magic Grade is explicitly not fail-safe,
    // and a plugin that refuses to work because a resource is missing is worse than one that does
    // less. What it must not do is degrade SILENTLY, so the panel reports which source was used.
    const char* src = s_seg_ready() ? "model" : "heuristic";
    og::grade::SegmentFn segfn = [](const unsigned char* rgb, int w, int h,
                                    std::vector<unsigned char>& regions) {
        if (!s_seg_ready()) return false;
        std::vector<unsigned char> mk; int mw = 0, mh = 0;
        if (!s_segmenter().run(rgb, w, h, mk, mw, mh)) return false;
        regions.assign((size_t)512 * 512, (unsigned char)oga::R_OTHER);
        for (int y = 0; y < 512; ++y)
            for (int x = 0; x < 512; ++x)
                regions[(size_t)y * 512 + x] = mk[(size_t)(y * mh / 512) * mw + (x * mw / 512)];
        return true;
    };

    og::grade::Measurements meas;
    meas.key = m_LastKey; meas.pin = m_LastPin; meas.hot = m_LastHot;
    meas.d01 = m_LastR01; meas.d50 = m_LastD50; meas.d99 = m_LastD99;
    meas.valid = true;

    oga::SampleSet& S = m_LastSamples;
    const og::grade::MagicResult R = og::grade::solve_magic(
        S, m_LastThumbSrc, meas, og::grade::kCreativeCamera, og::grade::kCreativeEncode,
        lutOk ? m_Lut.data.data() : nullptr, lutOk ? m_Lut.size : 0,
        tun, cycle, 1.0, wbFirst, segfn);

    // What the alternatives are re-run from. Creative's grade, before any subject was chosen, so
    // switching options cannot compound one subject's colour move onto the next. Instance state:
    // it dies with the session, which is exactly as long as the segmentation behind it lives.
    for (int k = 0; k < oga::kParamN; ++k) m_MagicBaseP[k] = R.Pcreative[k];
    m_HaveMagicBase = R.ok;
    m_MagicLutOk    = lutOk;

    applyMagicResult(R, src, /*repopulate=*/true);
}

// WRITE A RESULT TO THE NODE. Shared by the button and by the Subject dropdown, so the two cannot
// come to mean different things -- the panel, the anchors and the tone targets are all part of
// "a grade was applied" and every one of them was a step someone could forget.
void OneGrade::applyMagicResult(const og::grade::MagicResult& R, const char* src, bool repopulate)
{
    // P[] IS THE WHOLE GRADE. Writing it wholesale rather than parameter by parameter is what
    // stops the two implementations diverging again -- there is no step here that could be
    // forgotten, because there are no steps.
    m_Temp->setValue(R.P[0]);     m_Tint->setValue(R.P[1]);
    m_Density->setValue(R.P[2]);  m_Lift->setValue(R.P[3]);
    m_Gamma->setValue(R.P[4]);    m_Gain->setValue(R.P[5]);
    m_OffTemp->setValue(R.P[6]);  m_OffTint->setValue(R.P[7]);
    m_PostExp->setValue(R.P[8]);  m_PostCon->setValue(R.P[9]);
    m_RawExp->setValue(R.P[10]);  m_RawTemp->setValue(R.P[11]);
    m_RawExpMirror->setValue(R.P[10]);
    m_Rolloff->setValue(R.P[12]);
    m_LastGain = R.P[5];

    char wbNote[64] = {0};
    if (R.wbRan) {
        if (R.wb.ok) snprintf(wbNote, sizeof wbNote, "  WB %.0fK", R.wb.kelvin);
        else         snprintf(wbNote, sizeof wbNote, "  WB none: %s", R.wb.why);
    }

    // Bias re-solves from these; cleared when the tone solve declined, so a drag cannot re-solve
    // against a previous shot's subject.
    //
    // ...AND CLEARED FOR ANY SUBJECT THE RE-SOLVE IS NOT FITTED FOR, which is the same gate Tone
    // Separation takes below and for the same measured reason: Lift acts near black, so it places
    // a face's floor at 0.125 comfortably and runs to its -0.500 bound on a sky's at 0.486. Bias 0
    // on 00096619 returns L-0.500 against an armed L-0.044 -- a 0.456 step the instant the slider
    // is touched, with the whole negative half flat against the bound.
    //
    // Clearing the anchors is the mechanism that already exists for "the tone solve declined": it
    // drops Bias back to its OFFSET law, which predates all of this and works on any subject. So
    // sky keeps a working Bias rather than losing one, and the grade itself is untouched -- it is
    // only the re-solve that has nowhere to go.
    const bool toneResolvable = R.tone.ok && R.choice.ok && R.choice.subject == oga::R_SKIN;
    if (toneResolvable) {
        m_ToneLo->setValue(R.tone.sLo);   m_ToneMid->setValue(R.tone.sMid);
        m_ToneShi->setValue(R.tone.sHi);  m_ToneHi->setValue(R.tone.fHi);
        m_ToneFLo->setValue(R.tone.fLo);
    } else {
        m_ToneLo->setValue(-1.0);  m_ToneMid->setValue(-1.0);
        m_ToneShi->setValue(-1.0); m_ToneHi->setValue(-1.0); m_ToneFLo->setValue(-1.0);
    }

    // WHICH WAY "FURTHER APART" POINTS, measured once and frozen. The subject is stamped onto the
    // sample set here rather than inside solve_magic, which chooses it -- so the descriptor and
    // the grade cannot disagree about who the subject was. Zero means the two sit at the same
    // lightness and the question has no answer; the slider then does nothing and says so.
    {
        double dir = 0.0;
        // SKIN ONLY, and not because skin is special to the descriptor -- because the SOLVE the
        // slider re-runs is. Lift is lift*(1 - min(v,1)), so it acts most near black: it has full
        // authority over a face's floor at 0.125 and barely half over a sky's at 0.486. Re-solving
        // a bright subject runs Lift to its -0.500 bound, the ceiling then gives way, and the
        // picture blows out -- measured on 00096619, where Bias 0 alone returns L-0.500 against an
        // armed L-0.044 and the whole negative half sits flat against the bound.
        //
        // Grading once is unaffected and stays enabled for every region: that path solves from
        // neutral, where Lift still has room. It is only the RE-solve, from an already-bright
        // grade, that has nowhere to go.
        //
        // So the slider goes inert rather than wrong. Bad cases impossible, not rare -- the same
        // bar as the non-face gate on the tone solve itself, and for very nearly the same reason.
        if (toneResolvable) {
            m_LastSamples.subject = R.choice.subject;
            const oga::Desc d = oga::describe(m_LastSamples, og::grade::kCreativeCamera,
                                              og::grade::kCreativeEncode <= 2
                                                  ? og::grade::kCreativeEncode : 1, R.P);
            dir = og::grade::tone_sep_dir(d.v[oga::D_RDL]);
        }
        m_ToneSepDir->setValue(dir);
        m_ToneSep->setValue(0.0);

        // WHICH KIND OF UNAVAILABLE, not just that it is. A face shot can fail three different
        // ways and only one of them -- a mask so large it stopped being a face -- is fixed by
        // parking on another frame and pressing again. "Unavailable" alone would send the user
        // hunting on every shot, including the ones where hunting cannot work.
        char note[64];
        if (dir != 0.0)              snprintf(note, sizeof note, "Active");
        else if (!R.choice.ok)       snprintf(note, sizeof note, "No subject found on this frame");
        else if (!R.tone.ok)         snprintf(note, sizeof note, "Not solved: %s",
                                              R.tone.why[0] ? R.tone.why : "declined");
        else if (R.choice.subject != oga::R_SKIN)
                                     snprintf(note, sizeof note, "Faces only - subject is %s",
                                              oga::region_name(R.choice.subject));
        else                         snprintf(note, sizeof note, "Face matches its surround in tone");
        m_SepNote->setValue(note);
    }

    const oga::MagicChoice& c = R.choice;
    char msg[160];
    if (!c.ok) {
        m_MagicNote->setValue("No subject to separate - this is Creative Grade");
        snprintf(msg, sizeof msg, "%s [%s]",
                 R.tone.why[0] ? R.tone.why : "need 2 regions over the floor", src);
        m_MagicWhy->setValue(msg);
        m_MagicParam->setValue(-1);
        m_MagicBase->setValue(0.0);
        armBias(true);
        armToneTargets();   // clears them: a declined tone solve leaves nothing to lean away from
        // ...and the list, or it would still be offering the PREVIOUS frame's subjects.
        if (repopulate) { populateMagicSubject(); m_MagicSubject->setValue(0); }
        refreshRangeLatch();   // a declined tone solve still wrote Creative's grade
        setEnabledness();
        return;
    }
    // (the cycle counter is no longer advanced -- the Subject dropdown owns the choice)

    // The Separation slider rescales the stored move, so its anchor is the parameter WITHOUT it.
    // solve_magic already applied the move at Separation 1.0, so subtracting it recovers the
    // anchor exactly rather than re-deriving it.
    const double base   = R.magicBase;
    const double sep    = 1.0;
    const double anchor = (double)R.P[c.param] - base;
    m_MagicParam->setValue(c.param);
    m_MagicBase->setValue(base);
    m_MagicAnchor->setValue(anchor);
    m_MagicSepAt->setValue(0.0);       // a fresh decision: Separation 1.0 means the full move
    m_Separation->setValue(1.0);

    snprintf(msg, sizeof msg, "%d/%d  %s %.0f%% -> %s %+.3f  [%s]",
             c.option + 1, c.options, oga::region_name(c.subject), c.cover,
             (c.param == 6) ? "Offset Temp" : "Gain Temp", base * sep, src);
    if (wbNote[0]) strncat(msg, wbNote, sizeof msg - strlen(msg) - 1);
    m_MagicNote->setValue(msg);

    // RE-ARM BIAS ON THE FINAL GRADE, not the halfway one.
    //
    // applyAutoGrade arms the anchor, and everything after it -- the tone solve, the colour move,
    // and the re-solve that follows the colour move -- changes Lift, Gamma and Gain again. So the
    // anchor held values the node had passed THROUGH rather than the ones it ended on, and
    // applyBias's coefficient path computes lift as anchor + bias*0.06: an absolute value, not a
    // nudge. The first touch of the slider therefore snapped the picture back to the intermediate
    // grade, a full jump from a move of 0.003.
    //
    // It appeared on some shots and not others, which is what made it read as a slider problem
    // rather than an ordering one: a trusted face takes the tone path, which re-solves from the
    // current parameters against barely-moved targets and is continuous by construction, while
    // everything else falls through to the coefficient path and its stale anchor.
    //
    // Third discontinuity-at-its-own-default in this project, after Rolloff at 0 and RAW Temp at
    // 6500. The tell was identical all three times: the first nudge is a step, not a ramp.
    armBias(true);
    // Same instant, same reason: the conditions Bias leans away from are the ones this grade
    // ends on. Derived rather than assumed to equal the fitted constants -- the solve can land
    // on its frame-floor or ceiling branch, in which case it deliberately did NOT hit them.
    armToneTargets();

    // WHY, in the panel, in a sentence. The feature exists to surface a move an inexperienced
    // colourist would not have considered, and a suggestion with no visible reasoning teaches
    // nothing. It also makes a wrong pick legible rather than mysterious, which matters more
    // here than usual: this tool is fallible by design, so it has to show its working or there
    // is no way to tell a bad guess from a bad tool.
    // Both causal links inside the panel's real width, which is about 50 characters -- measured,
    // not guessed: the first version came to 55 and was visibly cut off mid-word in Resolve.
    // The region name is dropped because the row directly above already names it, and the
    // direction word is dropped because that row also shows the signed value. What is left is
    // the two things nothing else says: why THAT control, and why THAT direction.
    const char* ctl = (c.param == 6) ? "Offset" : "Gain";
    if (oga::region_protected(c.subject)) {
        snprintf(msg, sizeof msg, "protected; rest %s (L%.0fv%.0f) -> %s",
                 (c.restL > c.subjL) ? "brighter" : "darker", c.restL, c.subjL, ctl);
    } else {
        snprintf(msg, sizeof msg, "%s (L%.0fv%.0f) -> %s; %s (b%.0fv%.0f)",
                 (c.subjL > c.restL) ? "brighter" : "darker", c.subjL, c.restL, ctl,
                 (c.sign > 0) ? "warm" : "cool", c.subjB, c.restB);
    }
    m_MagicWhy->setValue(msg);
    // The list of alternatives belongs to the press that produced the segmentation, not to a
    // selection made from it -- rebuilding it on every pick would reset the dropdown under the
    // user's cursor.
    if (repopulate) { populateMagicSubject(); m_MagicSubject->setValue(c.option); }
    refreshRangeLatch();               // the grade under the mask just moved
    setEnabledness();
}

// Rescale the stored move. Deliberately does NOT re-decide: dragging Separation has to feel like
// one control getting stronger, not like the button being pressed again with a different answer.
void OneGrade::applySeparation()
{
    int which = -1; m_MagicParam->getValue(which);
    if (which != 0 && which != 6) return;          // nothing chosen yet
    double base = 0.0, anchor = 0.0, sep = 1.0, sepAt = 0.0;
    m_MagicBase->getValue(base);
    m_MagicAnchor->getValue(anchor);
    m_Separation->getValue(sep);
    // Same treatment as Bias: offset from the slider position the anchor was captured at, so a
    // manual Offset Temp / Gain Temp tweak survives and the next drag continues from it.
    m_MagicSepAt->getValue(sepAt);
    const double v = std::min(1.0, std::max(-1.0, anchor + base * (sep - sepAt)));
    if (which == 6) m_OffTemp->setValue(v); else m_Temp->setValue(v);
}

void OneGrade::setEnabledness()
{
    int role = 0, mode = 0;
    m_NodeRole->getValue(role);
    m_LutMode->getValue(mode);

    const bool input  = (role == 1);
    const bool output = (role == 2);
    const bool look   = !input;    // look/grade layer belongs to Full + Output Transform
    const bool src    = !output;   // camera + scene exp/WB belong to Full + Input Transform

    m_Camera->setEnabled(src);
    m_RawExp->setEnabled(src);
    m_RawTemp->setEnabled(src);

    // Per-stage bypass greys its own group's sliders on top of whatever the role allows.
    // The checkboxes themselves stay live for any stage the role owns, so auditioning is
    // one click in and one click out.
    bool bypBal = false, bypDen = false, bypExp = false, bypLut = false, bypTrim = false;
    bool bypRange = false;
    m_BypBalance->getValue(bypBal);
    m_BypDensity->getValue(bypDen);
    m_BypExposure->getValue(bypExp);
    m_BypRange->getValue(bypRange);
    m_BypLut->getValue(bypLut);
    m_BypTrim->getValue(bypTrim);

    m_BypBalance->setEnabled(look);
    m_BypDensity->setEnabled(look);
    m_BypExposure->setEnabled(look);
    m_BypRange->setEnabled(look);
    {
        // Range Balance belongs to the look, and its own sliders are inert until the latch is
        // set -- the pipeline tests latch > 0, so the panel says the same thing rather than
        // leaving three live-looking sliders that do nothing.
        double latch = 0.0; m_RangeLatch->getValue(latch);
        const bool rangeLive = look && !bypRange && latch > 0.0;
        m_RangeLatch->setEnabled(look && !bypRange);
        m_RangeShow->setEnabled(look && !bypRange && latch > 0.0);
        m_RangeSoft->setEnabled(rangeLive);
        m_RangeHigh->setEnabled(rangeLive);
        m_RangeShadow->setEnabled(rangeLive);
        m_RangeMid->setEnabled(rangeLive);
        m_RangeHiMid->setEnabled(rangeLive);
        m_RangeLoGain->setEnabled(rangeLive);
        m_RangeLock->setEnabled(rangeLive);
        // The shape's own sliders follow the shape being something other than None, so eight
        // controls do not sit live on a stage that is ignoring them.
        int shp = 0; m_RangeShape->getValue(shp);
        const bool shapeLive = rangeLive && shp > 0;
        m_RangeShape->setEnabled(rangeLive);
        m_RangeShapeFit->setEnabled(rangeLive);   // it SETS the shape, so it leads rather than follows
        m_RangeShapeX->setEnabled(shapeLive);   m_RangeShapeY->setEnabled(shapeLive);
        m_RangeShapeW->setEnabled(shapeLive);   m_RangeShapeH->setEnabled(shapeLive);
        m_RangeShapeR->setEnabled(shapeLive);   m_RangeShapeS->setEnabled(shapeLive);
        m_RangeShapeInv->setEnabled(shapeLive);
        // The note carries the LOCK state because the lock is invisible otherwise: a locked and an
        // unlocked mask look identical until you move exposure, and by then you are already
        // wondering why the selection did or did not follow. Same reason encodeNote and biasNote
        // exist -- greying a control is only half the truth.
        bool lock = false; m_RangeLock->getValue(lock);
        m_RangeNote->setValue(latch <= 0.0 ? "Set the latch to switch this on"
                              : lock       ? "Mask locked: exposure will not move it"
                                           : "Holding above the latch");
    }
    {
        // The shoulder's two numbers follow the checkbox; the button leads, because it SETS them.
        bool tmOn = false; m_ToneMap->getValue(tmOn);
        m_ToneMapKnee->setEnabled(look && tmOn);
        m_ToneMapWhite->setEnabled(look && tmOn);
        m_ToneMap->setEnabled(look);
        m_ToneMapFit->setEnabled(look);
        // Only stamp the idle text over the FACTORY default. The fit writes its finding here --
        // "nothing to contain (peak 0.48)" is the answer on a frame that needs no shoulder, and
        // replacing it with "Off" would throw away the one line that says why.
        if (!tmOn) {
            std::string cur; m_ToneMapNote->getValue(cur);
            if (cur.empty() || cur.rfind("Shoulder on", 0) == 0)
                m_ToneMapNote->setValue("Off - highlights may clip");
        }
    }
    m_BypLut->setEnabled(look);
    m_BypTrim->setEnabled(look);

    m_Preset->setEnabled(look);
    m_Temp->setEnabled(look && !bypBal);
    m_Tint->setEnabled(look && !bypBal);
    m_OffTemp->setEnabled(look && !bypBal);
    m_OffTint->setEnabled(look && !bypBal);
    m_Density->setEnabled(look && !bypDen);
    m_Lift->setEnabled(look && !bypExp);
    m_Gamma->setEnabled(look && !bypExp);
    m_Gain->setEnabled(look && !bypExp);
    m_PostExp->setEnabled(look && !bypTrim);
    m_PostCon->setEnabled(look && !bypTrim);
    m_Rolloff->setEnabled(look && !bypTrim);
    m_LutMode->setEnabled(look && !bypLut);

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
    // Analysis UI. Gated twice: on the compile-time master switch (see kAnalysisDebugUI —
    // currently off, so a colorist sees only Auto Grade and Bias) and, when that's on, on the
    // runtime checkbox. Driven from setEnabledness() rather than only from changedParam so
    // the state survives a project load.
    bool showAnalysis = false;
    m_ShowAnalysis->getValue(showAnalysis);
    const bool debug = kAnalysisDebugUI && showAnalysis;
    m_ShowAnalysis->setIsSecret(!kAnalysisDebugUI);
    m_ProbeBtn->setIsSecret(!debug);       // Analyze Frame — measures without applying
    m_ProbeApplied->setIsSecret(!debug);   // what the last Auto Grade wrote
    m_ProbeScene->setIsSecret(!debug);
    m_ProbeDisplay->setIsSecret(!debug);
    m_ProbePeak->setIsSecret(!debug);
    m_ProbeShape->setIsSecret(!debug);
    m_ProbeSubject->setIsSecret(!debug);
    m_ProbeStatus->setIsSecret(!debug);
    m_ProbeColour->setIsSecret(!debug);
    m_ProbeGraded->setIsSecret(!debug);
    m_ProbeRegions->setIsSecret(!debug);
    m_ProbeResponse->setIsSecret(!debug);
    m_ProbeDriveB->setIsSecret(!debug);
    m_ProbeDriveC->setIsSecret(!debug);
    m_ProbeDriveS->setIsSecret(!debug);
    m_ProbeSepTriple->setIsSecret(!debug);
    m_ProbeTone->setIsSecret(!debug);
    // Containment targets are exposed only in the debug panel: they are how the Clean
    // constants get fitted on footage, and a shipping panel should carry the result, not the
    // dials that produced it.
    m_CleanHigh->setIsSecret(!debug);
    m_CleanLow->setIsSecret(!debug);
    m_CleanMid->setIsSecret(!debug);
    m_CleanMidStr->setIsSecret(!debug);
    m_CleanMaxGain->setIsSecret(!debug);
    m_CleanShoulder->setIsSecret(!debug);
    m_CleanMaxExp->setIsSecret(!debug);
    m_CreativeLow->setIsSecret(!debug);

    const bool lutOn = lutSelected();
    m_Encode->setEnabled(look && !lutOn);
    if (!look)
        m_EncodeNote->setValue("Pinned by Node Role: DaVinci Intermediate");
    else if (lutOn)
        m_EncodeNote->setValue(mode == 2 ? "Film LUT owns this: Cineon in, 709 out"
                                         : "Look LUT owns this: Rec.709 (Scene)");
    else if (bypLut && mode != 0)
        m_EncodeNote->setValue("LUT bypassed - the encode above is used");
    else if (mode != 0)
        m_EncodeNote->setValue("No LUT found - the encode above is used");
    else
        m_EncodeNote->setValue("");

    m_FilmLut->setEnabled(look && !bypLut && mode == 2);
    m_LookGroup->setEnabled(look && !bypLut && mode == 1);
    m_LookLut->setEnabled(look && !bypLut && mode == 1);
    m_LutMix->setEnabled(look && !bypLut && mode != 0);

    // WHICH BIAS THE USER IS HOLDING. Exactly the test applyBias() makes -- armed targets mean
    // the solving path -- so the line cannot drift from the behaviour it describes.
    //
    // Both laws are defensible; the surprise was that one control silently runs whichever, and
    // that after a Magic grade a hand edit to Lift/Gamma/Gain is re-solved away on the next
    // touch of the slider. The second half is the one that bites, so it is what the line spends
    // its characters on.
    {
        double tLo = -1.0, tMid = -1.0, tShi = -1.0, tHi = -1.0, tFLo = -1.0;
        m_ToneLo->getValue(tLo);  m_ToneMid->getValue(tMid);
        m_ToneShi->getValue(tShi); m_ToneHi->getValue(tHi); m_ToneFLo->getValue(tFLo);
        const bool solving = (tLo >= 0.0 && tMid >= 0.0 && tShi >= 0.0 &&
                              tHi >= 0.0 && tFLo >= 0.0);
        m_BiasNote->setValue(solving ? "Re-solves L/G/G around your edits"
                                     : "Offsets Lift/Gamma/Gain together");
    }
    // FACE TONE SEPARATION IS OFTEN UNAVAILABLE, so the panel has to show that rather than let
    // the user drag a live-looking slider that does nothing. Greyed rather than hidden, and for
    // the reason the user asked for it: a control that vanishes cannot tell you that another
    // frame might arm it, and this one frequently is armed on the next frame along. The note
    // beside it carries the reason -- greying alone is only half the truth, the same lesson as
    // the encode override.
    {
        double dir = 0.0;
        m_ToneSepDir->getValue(dir);
        m_ToneSep->setEnabled(dir != 0.0);
        std::string sn; m_SepNote->getValue(sn);
        if (sn.empty()) m_SepNote->setValue("Press Magic Grade to arm this");
    }

    // Nothing to choose between until a press has produced a segmentation. Greyed rather than
    // hidden, so the control is visible as something the button will fill in.
    m_MagicSubject->setEnabled(m_HaveMagicBase);
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
// Balance, Density, Lift/Gamma/Gain, LUT and Trim. Scene Exposure/WB and Output Encode are never
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
    // A preset replaces the whole grade too, so the mask underneath it is just as stale. This runs
    // a second time when applyAutoGrade() calls us on its way through -- harmless, and the final
    // pass is the one that sticks, which is the right answer either way.
    refreshRangeLatch();
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
                applyPreset(0);         // neutral look; leaves Camera and the scene stage alone
            } else if (role == 2) {     // Output Transform -> takes the pre-clip's DWG/DI
                m_Camera->setValue(1);
                m_RawExp->setValue(0.0);
                m_RawExpMirror->setValue(0.0);
                m_RawTemp->setValue(6500.0);
            }
        }
        setEnabledness();
    }
    // Experimental Auto Grade probe. Guarded on eChangeUserEdit like the preset: a project
    // load must never trigger a frame fetch.
    else if (p_ParamName == "autoGradeClean" && p_Args.reason == OFX::eChangeUserEdit) {
        applyAutoGradeClean(p_Args.time);
    }
    else if (p_ParamName == "setupCheck" && p_Args.reason == OFX::eChangeUserEdit) {
        probeSetup(p_Args.time);
    }
    else if (p_ParamName == "lutExportBtn" && p_Args.reason == OFX::eChangeUserEdit) {
        exportCube(p_Args.time);
    }
    else if (p_ParamName == "probeAnalyze" && p_Args.reason == OFX::eChangeUserEdit) {
        probeAnalyze(p_Args.time);
    }
    else if (p_ParamName == "rangeSet" && p_Args.reason == OFX::eChangeUserEdit) {
        setRangeLatch(p_Args.time);
        setEnabledness();
    }
    // The latch and the lock both change what the note says, and the latch also decides whether
    // the rest of the group is live at all. Neither re-runs any measurement -- this is the panel
    // catching up with a value, which is why it is not guarded on eChangeUserEdit.
    else if (p_ParamName == "toneMapFit" && p_Args.reason == OFX::eChangeUserEdit) {
        fitToneMap(p_Args.time);
        setEnabledness();
    }
    else if (p_ParamName == "toneMap") {
        setEnabledness();
    }
    else if (p_ParamName == "rangeShapeFit" && p_Args.reason == OFX::eChangeUserEdit) {
        fitRangeShape(p_Args.time);
        setEnabledness();
    }
    else if (p_ParamName == "rangeLock" || p_ParamName == "rangeLatch" ||
             p_ParamName == "rangeShape") {
        setEnabledness();
    }
    else if (p_ParamName == "probeApply" && p_Args.reason == OFX::eChangeUserEdit) {
        applyAutoGrade(p_Args.time);
    }
    // Live: re-derive Rolloff/Lift as the slider moves. No re-analysis, so it keeps up.
    // MANUAL EDITS RE-ANCHOR THE SLIDERS THAT OWN THOSE CONTROLS.
    //
    // Without this the workflow is quietly destructive: press Creative, hand-tweak Gain, nudge
    // Bias, and the tweak is gone -- applyBias() recomputes from an anchor captured before the
    // edit ever happened. The user's hand work loses to a stored number, silently, which is the
    // worst way for it to lose.
    //
    // Re-arming at the CURRENT slider position rather than at zero is what makes it seamless:
    // the offset is zero at that instant, so nothing jumps and nothing moves, and the next drag
    // simply continues from where the hand left off. Guarded on eChangeUserEdit so the plugin's
    // own setValue calls -- which is most of what touches these params -- never re-anchor.
    else if (p_Args.reason == OFX::eChangeUserEdit &&
             (p_ParamName == "lift" || p_ParamName == "gamma" ||
              p_ParamName == "gain" || p_ParamName == "rolloff")) {
        armBias(false);
        // The hand becomes the new zero. Re-anchoring alone preserves the edit on the offset
        // path and cannot on the solving path, which re-solves to stored conditions -- so move
        // the conditions to what the hand just achieved. Rolloff is in here because it is part
        // of the render the conditions are measured through.
        armToneTargets();
        setEnabledness();
    }
    else if (p_Args.reason == OFX::eChangeUserEdit &&
             (p_ParamName == "offTemp" || p_ParamName == "temp")) {
        int which = -1; m_MagicParam->getValue(which);
        if (which == 6 || which == 0) {
            m_MagicAnchor->setValue((which == 6) ? m_OffTemp->getValue() : m_Temp->getValue());
            m_MagicSepAt->setValue(m_Separation->getValue());
        }
    }
    else if (p_ParamName == "autoBiasMirror" && p_Args.reason == OFX::eChangeUserEdit) {
        m_AutoBias->setValue(m_BiasMirror->getValue());
        applyBias();
    }
    else if (p_ParamName == "magicGrade" && p_Args.reason == OFX::eChangeUserEdit) {
        applyMagicGrade(p_Args.time);
    }
    // Cheap by construction: the segmentation is already in hand, so this re-grades rather than
    // re-analyses. Guarded like every other button so a project load cannot trigger a solve.
    else if (p_ParamName == "magicSubject" && p_Args.reason == OFX::eChangeUserEdit) {
        applyMagicSubject();
    }
    else if (p_ParamName == "separation" && p_Args.reason == OFX::eChangeUserEdit) {
        applySeparation();
    }
    else if (p_ParamName == "rawExpMirror" && p_Args.reason == OFX::eChangeUserEdit) {
        m_RawExp->setValue(m_RawExpMirror->getValue());
    }
    else if (p_ParamName == "rawExp" && p_Args.reason == OFX::eChangeUserEdit) {
        m_RawExpMirror->setValue(m_RawExp->getValue());
    }
    else if (p_ParamName == "toneSep" && p_Args.reason == OFX::eChangeUserEdit) {
        applyBias();        // same solve; Tone Separation is the other target it moves
        setEnabledness();
    }
    else if (p_ParamName == "autoBias" && p_Args.reason == OFX::eChangeUserEdit) {
        m_BiasMirror->setValue(m_AutoBias->getValue());
        applyBias();
    }
    else if (p_ParamName == "showAnalysis") setEnabledness();
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

// Resolve EVERYTHING the node does at one time, in one place: camera, encode, which LUT,
// and the 13 pipeline values after Node Role and per-stage Bypass have both had their say.
//
// Split out of setupAndProcess() when the .cube exporter arrived (2026-08-03). The exporter
// has to bake precisely what the node renders, and the reliable way to guarantee that is to
// have one definition of "what the node renders" rather than two that are meant to agree.
// Every override below is load-bearing -- role forcing, the LUT encode coupling, bypass --
// and an exporter that reimplemented them would drift on the first change to any of them.
RenderConfig OneGrade::resolveConfig(double p_Time)
{
    RenderConfig cfg;
    int role = 0, camera = 0, encode = 0, lutMode = 0;
    m_NodeRole->getValueAtTime(p_Time, role);
    m_Camera->getValueAtTime(p_Time, camera);
    m_Encode->getValueAtTime(p_Time, encode);
    m_LutMode->getValueAtTime(p_Time, lutMode);

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
    m_LookGroup->getValueAtTime(p_Time, lookGroup);
    m_LookLut->getValueAtTime(p_Time, lookLut);
    m_FilmLut->getValueAtTime(p_Time, filmLut);
    const std::string lutPath = resolveLutPath(lutMode, lookGroup, lookLut, filmLut);
    const float lutMix = (float)m_LutMix->getValueAtTime(p_Time);

    // Per-stage bypass, read before the encode coupling below because bypassing the LUT
    // stage has to release the encode override too — otherwise the "bypass" would still be
    // rendering the LUT's working curve, which is a visible change and therefore not a
    // bypass. Everything else is enforced further down by forcing the stage's params
    // neutral, so a bypassed stage IS a neutral stage: no second code path to keep in sync,
    // and nothing for the golden-rule mirror to worry about (the kernels never learn that
    // bypass exists).
    bool bypBal = false, bypDen = false, bypExp = false, bypLut = false, bypTrim = false;
    bool bypRange = false;
    m_BypBalance->getValueAtTime(p_Time, bypBal);
    m_BypDensity->getValueAtTime(p_Time, bypDen);
    m_BypExposure->getValueAtTime(p_Time, bypExp);
    m_BypRange->getValueAtTime(p_Time, bypRange);
    m_BypLut->getValueAtTime(p_Time, bypLut);
    m_BypTrim->getValueAtTime(p_Time, bypTrim);

    const bool lutOk = !bypLut && !lutPath.empty() && m_Lut.load(lutPath);

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
    params[0] = (float)m_Temp->getValueAtTime(p_Time);
    params[1] = (float)m_Tint->getValueAtTime(p_Time);
    params[2] = (float)m_Density->getValueAtTime(p_Time);
    params[3] = (float)m_Lift->getValueAtTime(p_Time);
    params[4] = (float)m_Gamma->getValueAtTime(p_Time);
    params[5] = (float)m_Gain->getValueAtTime(p_Time);
    params[6] = (float)m_OffTemp->getValueAtTime(p_Time);
    params[7] = (float)m_OffTint->getValueAtTime(p_Time);
    params[8] = (float)m_PostExp->getValueAtTime(p_Time);
    params[9] = (float)m_PostCon->getValueAtTime(p_Time);
    params[10] = (float)m_RawExp->getValueAtTime(p_Time);
    params[11] = (float)m_RawTemp->getValueAtTime(p_Time);
    params[12] = (float)m_Rolloff->getValueAtTime(p_Time);
    params[13] = (float)m_RangeLatch->getValueAtTime(p_Time);
    params[14] = (float)m_RangeSoft->getValueAtTime(p_Time);
    params[15] = (float)m_RangeHigh->getValueAtTime(p_Time);
    params[16] = (float)m_RangeShadow->getValueAtTime(p_Time);
    params[17] = (float)m_RangeMid->getValueAtTime(p_Time);
    { bool sw = false; m_RangeShow->getValueAtTime(p_Time, sw); params[18] = sw ? 1.f : 0.f; }
    params[19] = (float)m_RangeHiMid->getValueAtTime(p_Time);
    params[20] = (float)m_RangeLoGain->getValueAtTime(p_Time);

    {
        int sh = 0; m_RangeShape->getValueAtTime(p_Time, sh);
        bool inv = false; m_RangeShapeInv->getValueAtTime(p_Time, inv);
        params[24] = (float)sh;
        params[25] = (float)m_RangeShapeX->getValueAtTime(p_Time);
        params[26] = (float)m_RangeShapeY->getValueAtTime(p_Time);
        params[27] = (float)m_RangeShapeW->getValueAtTime(p_Time);
        params[28] = (float)m_RangeShapeH->getValueAtTime(p_Time);
        params[29] = (float)m_RangeShapeR->getValueAtTime(p_Time);
        params[30] = (float)m_RangeShapeS->getValueAtTime(p_Time);
        params[31] = inv ? 1.f : 0.f;
    }

    // The shoulder. OFF is expressed as white <= knee, which tone_map() reads as identity -- so the
    // four render paths carry two floats and no branch, the same arrangement as Lock Mask.
    {
        bool on = false; m_ToneMap->getValueAtTime(p_Time, on);
        params[32] = (float)m_ToneMapKnee->getValueAtTime(p_Time);
        params[33] = on ? (float)m_ToneMapWhite->getValueAtTime(p_Time) : 0.f;
    }

    // THE MASK'S REFERENCE GRADE, resolved here rather than branched on in the kernel. Unlocked it
    // is the live grade, so the mask reads the graded picture exactly as it did before the lock
    // existed; locked it is the grade captured when the latch was measured. Either way the four
    // render paths see one number each and have no idea a lock exists -- a branch out there would
    // be a second definition of the mask with nothing to say which one a frame used.
    {
        bool lock = false; m_RangeLock->getValueAtTime(p_Time, lock);
        params[21] = lock ? (float)m_RangeRefLift ->getValueAtTime(p_Time) : params[3];
        params[22] = lock ? (float)m_RangeRefGamma->getValueAtTime(p_Time) : params[4];
        params[23] = lock ? (float)m_RangeRefGain ->getValueAtTime(p_Time) : params[5];
    }

    // Force the params the role doesn't own to neutral, so the two nodes chain cleanly:
    // the look must be applied once (on the output node), the scene exp/WB stage once (input).
    if (role == 1) {            // Input Transform: no look at all
        params[0]=0.f; params[1]=0.f; params[2]=0.f;              // temp, tint, density
        params[3]=0.f; params[4]=1.f; params[5]=1.f;              // lift, gamma, gain
        params[6]=0.f; params[7]=0.f;                             // offset temp/tint
        params[8]=0.f; params[9]=1.f; params[12]=0.f;             // trim exp/contrast/rolloff
        // Range Balance is part of the LOOK -- it runs in the display curve beside Lift/Gamma/
        // Gain -- so an Input Transform node must not apply it, or a group split would apply it
        // twice. Latch 0 is the off switch the pipeline already tests.
        params[13]=0.f; params[15]=1.f; params[16]=0.f; params[17]=1.f; params[18]=0.f;
        params[19]=1.f; params[20]=1.f;
        params[21]=0.f; params[22]=1.f; params[23]=1.f;   // ...and the mask's reference grade
        params[24]=0.f;                                   // ...and its shape
    } else if (role == 2) {     // Output Transform: scene exp/WB already happened upstream
        params[10]=0.f; params[11]=6500.f;                        // rawExp, rawTemp
    }

    // Per-stage bypass, applied on top of the role. Both force to neutral, so the order
    // between them doesn't matter — but bypass has to come second to be authoritative when
    // a stage the role owns is also switched off. The slider VALUES are untouched: bypass
    // is a render-time mute, so flipping it back restores the grade exactly, which is the
    // whole point (auditioning a stage shouldn't cost you the numbers you dialled in).
    if (bypBal)  { params[0]=0.f; params[1]=0.f; params[6]=0.f; params[7]=0.f; }  // gain+offset balance
    if (bypDen)  { params[2]=0.f; }                                              // density
    if (bypExp)  { params[3]=0.f; params[4]=1.f; params[5]=1.f; }                // lift/gamma/gain
    if (bypRange){ params[13]=0.f; params[15]=1.f; params[16]=0.f; params[17]=1.f; params[18]=0.f;
                   params[19]=1.f; params[20]=1.f; params[21]=0.f; params[22]=1.f; params[23]=1.f;
                   params[24]=0.f; }
    if (bypTrim) { params[8]=0.f; params[9]=1.f; params[12]=0.f; }               // exp/contrast/rolloff
    // bypLut needs no entry here: it already cleared lutOk above, which drops both the LUT
    // sample and its encode override in one go.

    std::memcpy(cfg.params, params, sizeof params);
    cfg.camera = camera;
    cfg.encode = encode;
    cfg.lutOk  = lutOk;
    cfg.lutMix = lutMix;
    return cfg;
}

// LUT EXPORT — bake the whole node into a .cube (forum feedback, 2026-08-03).
//
// The strongest professional objection to a plugin like this is the dependency it creates:
// a project that needs OneGrade to open correctly needs OneGrade archived alongside it, and
// because this node fulfils the entire grading pipeline, substituting it later would mean
// starting over rather than replacing one effect. That's a real problem and it doesn't have
// a rebuttal — it has a fix, and the person who raised it also named it.
//
// It is possible at all because OneGrade has **no spatial operations**: every stage is a
// function of one pixel's RGB and nothing else — no neighbourhood, no frame history, no
// resolution dependence. A plugin with a blur or a spatial key could not do this at any
// lattice size. Being a per-pixel function is necessary; it is NOT sufficient, which is the
// part worth writing down, because the first version of this comment claimed the bake was
// exact and measurement said otherwise:
//
//   ON-LATTICE the bake is exact — worst error 1.5e-08 across cameras (test 13 guards it,
//   and it is a perfect detector for the index-order transpose that is the easy bug here).
//
//   OFF-LATTICE it depends entirely on how smooth the pipeline is between lattice points,
//   and ours is not smooth everywhere. The output encode HARD-CLIPS out-of-gamut channels
//   to 0, which puts a discontinuity surface through the middle of the colour cube: on one
//   side blue is 0, a hair across it blue is large. Trilinear interpolation cannot follow a
//   step. Measured on Gen 5 -> Rec.709 2.2, neutral params, 33^3:
//       grey axis, log 0.10-0.70 .................  4 LSB
//       mildly tinted (+/-15% of grey) ...........  152 LSB
//       median over the whole cube ...............  0 LSB
//   So most of the cube is perfect and the error is concentrated where a bright, saturated
//   channel crosses the Rec.709 gamut boundary. 65^3 roughly halves it but cannot remove it
//   — the limit is the discontinuity, not the sampling.
//
// That makes this an excellent archival stand-in and NOT a bit-exact one. The honest
// framing, which the hint and the docs both use: the exported LUT matches the node through
// the normal tonal range and can differ on blown, saturated highlights.
//
// FUTURE WORK: a soft gamut compression before the clip would make the function continuous
// and the bake near-exact. That is a pipeline change (golden-rule 4-file mirror) and it
// changes the look, so it is not smuggled in behind an export button.
//
// Domain: camera code values in [0,1], because that is what the node is fed — the cube
// replaces the node in front of the same log footage, not somewhere else in a chain. Source
// values above 1.0 clamp to the top of the lattice.
void OneGrade::exportCube(double p_Time)
{
    static const char* kCamNames[] = {
        "Blackmagic Gen 5 Film", "DaVinci Wide Gamut / Intermediate", "Sony S-Log3",
        "ARRI LogC3", "ARRI LogC4", "Canon Log3", "RED Log3G10", "DJI D-Log",
        "Fuji F-Log2", "Panasonic V-Log", "Rec.2100 HLG", "Rec.2100 PQ - Smooth Decode" };
    static const char* kEncNames[] = {
        "Rec.709 (Scene)", "Rec.709 (Gamma 2.2)", "Rec.709 (Gamma 2.4)",
        "Cineon Log", "DaVinci Wide Gamut / Intermediate", "Linear" };

    std::string path;
    m_LutExportPath->getValue(path);
    if (path.empty()) { m_LutExportStatus->setValue("Set a file path first"); return; }
    if (path.size() < 5 || path.compare(path.size() - 5, 5, ".cube") != 0) path += ".cube";

    int sizeIdx = 1;
    m_LutExportSize->getValue(sizeIdx);
    const int N = (sizeIdx == 0) ? 17 : (sizeIdx == 2) ? 65 : 33;

    // Exactly what the node renders at this time — role forcing, LUT encode coupling and
    // per-stage bypass all already applied. Not a re-derivation.
    const RenderConfig cfg = resolveConfig(p_Time);
    const float* lut  = cfg.lutOk ? m_Lut.data.data() : nullptr;
    const int lutSize = cfg.lutOk ? m_Lut.size : 0;
    const float lutMix= cfg.lutOk ? cfg.lutMix : 0.0f;

    std::ofstream f(path.c_str());
    if (!f.is_open()) { m_LutExportStatus->setValue("Could not open file for writing"); return; }

    const char* camName = (cfg.camera >= 0 && cfg.camera < 12) ? kCamNames[cfg.camera] : "?";
    const char* encName = (cfg.encode >= 0 && cfg.encode < 6)  ? kEncNames[cfg.encode]  : "?";

    f << "# Generated by OneGrade " << kPluginVersionMajor << "." << kPluginVersionMinor << "\n"
      << "# Input:  " << camName << " code values, domain 0-1\n"
      << "# Output: " << encName << "\n"
      << "# Feed this the same camera-log footage the node was fed, with no other\n"
      << "# transform in front of it. Source values above 1.0 clamp to the lattice top.\n"
      << "TITLE \"OneGrade " << camName << " to " << encName << "\"\n"
      << "LUT_3D_SIZE " << N << "\n"
      << "DOMAIN_MIN 0.0 0.0 0.0\n"
      << "DOMAIN_MAX 1.0 1.0 1.0\n";

    // .cube stores red varying fastest, matching og::apply_lut's ((b*N + g)*N + r) layout.
    f.setf(std::ios::fixed); f.precision(6);
    const float d = 1.0f / (float)(N - 1);
    for (int bi = 0; bi < N; ++bi)
        for (int gi = 0; gi < N; ++gi)
            for (int ri = 0; ri < N; ++ri) {
                float ro, go, bo;
                og_full_chain(cfg.camera, cfg.encode, cfg.params, lut, lutSize, lutMix,
                              ri * d, gi * d, bi * d, ro, go, bo);
                f << ro << " " << go << " " << bo << "\n";
            }

    const bool ok = f.good();
    f.close();

    char msg[128];
    if (ok) snprintf(msg, sizeof msg, "Wrote %d^3 LUT (%d points)", N, N * N * N);
    else    snprintf(msg, sizeof msg, "Write failed after opening the file");
    m_LutExportStatus->setValue(msg);
}

void OneGrade::setupAndProcess(OneGradeProcessor& p_Proc, const OFX::RenderArguments& p_Args)
{
    std::unique_ptr<OFX::Image> dst(m_DstClip->fetchImage(p_Args.time));
    std::unique_ptr<OFX::Image> src(m_SrcClip->fetchImage(p_Args.time));

    if ((src->getPixelDepth() != dst->getPixelDepth()) || (src->getPixelComponents() != dst->getPixelComponents()))
        OFX::throwSuiteStatusException(kOfxStatErrValue);

    const RenderConfig cfg = resolveConfig(p_Args.time);

    p_Proc.setDstImg(dst.get());
    p_Proc.setSrcImg(src.get());
    p_Proc.setGPURenderArgs(p_Args);
    p_Proc.setRenderWindow(p_Args.renderWindow);
    p_Proc.setParams(cfg.params, cfg.camera, cfg.encode);
    p_Proc.setLut(cfg.lutOk ? m_Lut.data.data() : nullptr, cfg.lutOk ? m_Lut.size : 0, cfg.lutOk ? cfg.lutMix : 0.0f);
    p_Proc.process();
}

////////////////////////////////////////////////////////////////////////////////

using namespace OFX;

OneGradeFactory::OneGradeFactory()
    : OFX::PluginFactoryHelper<OneGradeFactory>(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor)
{
}


////////////////////////////////////////////////////////////////////////////////
// THE ON-SCREEN SHAPE — an OFX overlay interact.
//
// SPIKE AND FEATURE AT ONCE. OFX has the API for this (OverlayInteract, draw/penDown/penMotion,
// registered with setOverlayInteractDescriptor) but whether DAVINCI RESOLVE honours overlay
// interacts is not something the OFX headers can answer -- its OFX support is partial, and plenty
// of plugins ship numeric position sliders precisely because on-screen widgets do not always
// appear. Same class of unknown as setOpen() on a group.
//
// So this is deliberately small: draw the shape's outline and a centre handle, and let the centre
// be dragged. If Resolve draws it, the answer is yes and dragging a window into place already
// works. If not, nothing is lost -- the sliders drive the identical parameters, the render never
// consults this class, and it can be deleted in one block.
//
// COORDINATES. Interacts work in CANONICAL coordinates (project pixels, y up). The shape params
// are centre-origin and normalised by half-height, matching shape_mask(). The conversion is the
// only fiddly part and it lives in one pair of lambdas below, so a mismatch between what is drawn
// and what is rendered can only come from one place.
class RangeShapeInteract : public OFX::OverlayInteract
{
public:
    RangeShapeInteract(OfxInteractHandle p_Handle, OFX::ImageEffect* p_Effect)
        : OFX::OverlayInteract(p_Handle), _effect(p_Effect)
    {
        _shape = p_Effect->fetchChoiceParam("rangeShape");
        _cx    = p_Effect->fetchDoubleParam("rangeShapeX");
        _cy    = p_Effect->fetchDoubleParam("rangeShapeY");
        _sx    = p_Effect->fetchDoubleParam("rangeShapeW");
        _sy    = p_Effect->fetchDoubleParam("rangeShapeH");
        _rot   = p_Effect->fetchDoubleParam("rangeShapeR");
        _note  = p_Effect->fetchStringParam("rangeShapeNote");
    }

    virtual bool draw(const OFX::DrawArgs& p_Args);
    virtual bool penDown(const OFX::PenArgs& p_Args);
    virtual bool penMotion(const OFX::PenArgs& p_Args);
    virtual bool penUp(const OFX::PenArgs& p_Args);

private:
    // Half-height units <-> canonical.
    //
    // THE SOURCE CLIP'S REGION OF DEFINITION, NOT THE PROJECT SIZE. The render normalises against
    // the destination image's BOUNDS, so the overlay has to use the same rectangle or the outline
    // is drawn somewhere the mask is not -- and if a host returns a degenerate project size the
    // outline collapses to a half-pixel dot at the origin, which looks exactly like "overlays do
    // not work". Project size is kept only as a fallback for when the clip has no RoD yet.
    void frame(double t, double& ox, double& oy, double& halfH) const
    {
        OFX::Clip* src = _effect->fetchClip(kOfxImageEffectSimpleSourceClipName);
        if (src) {
            const OfxRectD r = src->getRegionOfDefinition(t);
            const double w = r.x2 - r.x1, h = r.y2 - r.y1;
            if (w > 1.0 && h > 1.0) {
                halfH = 0.5*h; ox = r.x1 + 0.5*w; oy = r.y1 + 0.5*h;
                return;
            }
        }
        const OfxPointD sz = _effect->getProjectSize();
        const OfxPointD of = _effect->getProjectOffset();
        halfH = (sz.y > 1.0) ? 0.5*sz.y : 1.0;
        ox = of.x + 0.5*sz.x;
        oy = of.y + 0.5*sz.y;
    }
    void toCanonical(double t, double u, double v, double& x, double& y) const
    {
        double ox, oy, hh; frame(t, ox, oy, hh);
        x = ox + u*hh; y = oy + v*hh;
    }
    void toUnits(double t, double x, double y, double& u, double& v) const
    {
        double ox, oy, hh; frame(t, ox, oy, hh);
        u = (x - ox)/hh; v = (y - oy)/hh;
    }

    OFX::ImageEffect*  _effect;
    OFX::ChoiceParam*  _shape;
    OFX::DoubleParam*  _cx; OFX::DoubleParam* _cy;
    OFX::DoubleParam*  _sx; OFX::DoubleParam* _sy;
    OFX::DoubleParam*  _rot;
    OFX::StringParam*  _note;
    bool _dragging = false;
    bool _reported = false;   // the note is written ONCE; setValue per redraw would loop
};

bool RangeShapeInteract::draw(const OFX::DrawArgs& p_Args)
{
    int shp = 0; _shape->getValueAtTime(p_Args.time, shp);
    if (shp <= 0) return false;

    const double cx = _cx->getValueAtTime(p_Args.time), cy = _cy->getValueAtTime(p_Args.time);
    const double sx = _sx->getValueAtTime(p_Args.time), sy = _sy->getValueAtTime(p_Args.time);
    const double rd = _rot->getValueAtTime(p_Args.time) * 0.01745329252;
    const double cs = std::cos(rd), sn = std::sin(rd);

    // Rotate OUT of the shape's frame -- shape_mask() rotates into it, so this is the inverse.
    auto pt = [&](double nx, double ny, double& X, double& Y) {
        const double du = nx*sx, dv = ny*sy;
        toCanonical(p_Args.time, cx + du*cs - dv*sn, cy + du*sn + dv*cs, X, Y);
    };

    // Twice, dark then light, so the outline reads over any picture underneath it.
    for (int pass = 0; pass < 2; ++pass) {
        glLineWidth(pass ? 1.5f : 3.0f);
        if (pass) glColor3f(1.f, 0.9f, 0.2f); else glColor3f(0.f, 0.f, 0.f);
        glBegin(GL_LINE_LOOP);
        if (shp == 1) {
            for (int i = 0; i < 64; ++i) {
                const double a = 2.0*3.14159265358979*i/64.0;
                double X, Y; pt(std::cos(a), std::sin(a), X, Y); glVertex2d(X, Y);
            }
        } else {
            const double c[4][2] = { {-1,-1}, {1,-1}, {1,1}, {-1,1} };
            for (int i = 0; i < 4; ++i) { double X, Y; pt(c[i][0], c[i][1], X, Y); glVertex2d(X, Y); }
        }
        glEnd();
        // The centre handle, sized in VIEWPORT pixels so it stays grabbable at any zoom.
        double hx, hy; toCanonical(p_Args.time, cx, cy, hx, hy);
        const double r = 6.0 * p_Args.pixelScale.x;
        glBegin(GL_LINES);
        glVertex2d(hx - r, hy); glVertex2d(hx + r, hy);
        glVertex2d(hx, hy - r); glVertex2d(hx, hy + r);
        glEnd();
    }
    // REPORT, ONCE, THAT THIS RAN. Three separate things have to hold for an outline to appear --
    // the host must advertise overlays, it must actually call us, and the GL context it hands over
    // must accept fixed-function drawing (glBegin/glColor exist only in a compatibility profile,
    // and a core-profile context would swallow every call above). Each fails silently and they are
    // indistinguishable from the outside, so the panel says which one you are in.
    //
    // Only ever ONCE, and only while the note still holds its untouched default -- the same line
    // reports what Fit To Frame measured, and that is the message worth keeping. Measured in
    // Resolve 2026-08-17: it ADVERTISES overlay support, our interact registers, and this function
    // is never called on the Color page. So the line stays on its default there and the outline
    // is simply unavailable; nothing about the shape depends on it.
    if (!_reported) {
        _reported = true;
        std::string cur; _note->getValue(cur);
        if (cur.rfind("Set the latch", 0) == 0) {
            const GLenum e = glGetError();
            char msg[64];
            if (e == GL_NO_ERROR) snprintf(msg, sizeof msg, "on-screen handle is live");
            else                  snprintf(msg, sizeof msg, "host GL rejected the outline (0x%04x)", (unsigned)e);
            _note->setValue(msg);
        }
    }
    return true;
}

bool RangeShapeInteract::penDown(const OFX::PenArgs& p_Args)
{
    int shp = 0; _shape->getValueAtTime(p_Args.time, shp);
    if (shp <= 0) return false;
    double hx, hy;
    toCanonical(p_Args.time, _cx->getValueAtTime(p_Args.time),
                _cy->getValueAtTime(p_Args.time), hx, hy);
    const double grab = 12.0 * p_Args.pixelScale.x;
    if (std::fabs(p_Args.penPosition.x - hx) > grab ||
        std::fabs(p_Args.penPosition.y - hy) > grab) return false;
    _dragging = true;
    return true;
}

bool RangeShapeInteract::penMotion(const OFX::PenArgs& p_Args)
{
    if (!_dragging) return false;
    double u, v; toUnits(p_Args.time, p_Args.penPosition.x, p_Args.penPosition.y, u, v);
    _cx->setValue(u); _cy->setValue(v);
    requestRedraw();
    return true;
}

bool RangeShapeInteract::penUp(const OFX::PenArgs& /*p_Args*/)
{
    if (!_dragging) return false;
    _dragging = false;
    return true;
}

class RangeShapeOverlayDescriptor
    : public OFX::DefaultEffectOverlayDescriptor<RangeShapeOverlayDescriptor, RangeShapeInteract> {};

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
    // OFF: nothing fetches a frame other than the render time. This was on briefly for the
    // Match Clip probe (deferred -- see docs/ROADMAP.md) and went off with it. Advertising a
    // capability we do not use is the same class of mistake as the OpenCL black frame.
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

// One "Bypass" checkbox per pipeline group. Auditioning a stage used to mean zeroing its
// sliders and putting the numbers back afterwards, which is a bad answer and cost people
// their values when they mis-remembered (forum feedback, 2026-08-03). Enforced at render by
// forcing the stage neutral — see setupAndProcess — so a bypassed stage is precisely a
// neutral stage, with no parallel code path and nothing new for the GPU kernels to mirror.
static BooleanParamDescriptor* defineBypass(OFX::ImageEffectDescriptor& p_Desc, PageParamDescriptor* page,
                                            const char* name, const char* hint, GroupParamDescriptor* parent)
{
    BooleanParamDescriptor* b = p_Desc.defineBooleanParam(name);
    b->setLabels("Bypass", "Bypass", "Bypass");
    b->setHint(hint);
    b->setDefault(false);
    b->setParent(*parent);
    page->addChild(*b);
    return b;
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
    // Register the on-screen shape. THE SUPPORT LIBRARY DROPS THIS SILENTLY when the host reports
    // kOfxImageEffectPropSupportsOverlays == 0 (ofxsImageEffect.cpp:545) -- no error, no log, the
    // property simply never gets set and no overlay ever appears. That is the fifth instance of
    // this project's oldest shape: a missing capability degrading in silence rather than saying
    // so. So we read the same flag and put the answer on the panel, where "I don't see the shape"
    // becomes a fact instead of a guess.
    p_Desc.setOverlayInteractDescriptor(new RangeShapeOverlayDescriptor);
    const bool hostOverlays = OFX::getImageEffectHostDescription()->supportsOverlays;

    ClipDescriptor* srcClip = p_Desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    ClipDescriptor* dstClip = p_Desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->setSupportsTiles(kSupportsTiles);

    PageParamDescriptor* page = p_Desc.definePageParam("Controls");

    // WHICH GROUPS START OPEN. setOpen() is kOfxParamPropGroupOpen, an OFX 1.2 property that the
    // Support library sets with throwOnFailure=false -- so a host that does not implement it
    // ignores the request rather than failing to load, and every group is stated explicitly
    // instead of some being left to the host's default.
    //
    // OPEN is nearly everything, judged on footage (2026-08-18) rather than reasoned about: the
    // first split had the refinement groups closed, and scrolling past a wall of collapsed headers
    // to reach a slider you use on every shot is worse than scrolling past the slider itself.
    //
    // CLOSED is only what you touch ONCE or NEVER: Role / Preset is set when the node is created,
    // Export LUT is a delivery action rather than a control, and Setup / Help is reference. Those
    // three are the ones that earn a collapse.
    //
    // This is the one call that decides what a colorist sees when they drop the node on a clip.

    // ---- 0. Role + Preset ----
    GroupParamDescriptor* gPreset = p_Desc.defineGroupParam("gPreset");
    gPreset->setLabels("0  Role / Preset", "0  Role / Preset", "0  Role / Preset");
    gPreset->setOpen(false);

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
    preset->setHint("One-click starting points on the happy path: every preset sets Camera to 'Rec.2100 PQ - Smooth Decode' (also the default) plus Balance, Density, Lift/Gamma/Gain, LUT and Trim — every slider stays live to tweak per clip; Scene Exposure, Scene White Balance and Output Encode are never touched. Film Emulation presets drive Resolve's print-film stocks (swap in Film Look LUT); Custom LUT presets drive OneGrade's built-in looks, shipped inside the plugin (swap in Look LUT; six looks available). Trim any LUT with LUT Mix. None / Reset Look returns the look params to neutral (Camera stays put).");
    preset->appendOption("None / Reset Look");
    preset->appendOption("Cinematic Film Emulation (Kodak 2383 D60)");
    preset->appendOption("Cinematic Film Emulation (Fujifilm 3513DI D60)");
    preset->appendOption("Custom LUT - Cinematic Landscape");
    preset->appendOption("Custom LUT - Teal Orange");
    preset->setDefault(0);
    preset->setParent(*gPreset);
    page->addChild(*preset);

    // ---- Auto Grade ----
    //
    // PANEL ORDER IS WORKFLOW ORDER, NOT PIPELINE ORDER, and 0-7 now number the second. The two
    // used to be the same thing and that was a coincidence of a smaller plugin: the buttons that
    // most sessions START with sat below eight stages of manual controls, and Scene Exposure lived
    // under Input Transform because that is where it acts rather than because that is when anyone
    // reaches for it.
    //
    // So: Role/Preset, then the two buttons, then the manual stages. Within the stages the numbers
    // still climb, but they no longer pretend to be the order pixels are touched -- Output sits
    // after Trim because choosing a delivery encode is the last decision, though it is applied
    // several steps earlier. og::process() remains the authority on what actually happens when.
    // First in the panel, at the user's request: it's the one-click entry point, so it
    // shouldn't be buried under nine groups of manual controls. Deliberately UNNUMBERED
    // while it's experimental — the 0-8 sequence below is the pipeline in the order it's
    // applied, and this isn't a pipeline stage, it's a way of setting those stages. It also
    // means the numbering users and the docs already know doesn't shift for a feature that
    // may still change shape. Number it 0 and renumber the rest if it graduates.
    // Magic Grade gets its own section. It is a different KIND of thing from the other two --
    // Base and Creative correct and stylise the whole frame, while this one makes a single
    // opinionated colour decision about one object in it -- and mixing them in one group made
    // the panel read as four buttons of equal standing.
    GroupParamDescriptor* gMagic = p_Desc.defineGroupParam("gMagic");
    gMagic->setLabels("Magic Grade (experimental)", "Magic Grade", "Magic Grade");

    GroupParamDescriptor* gAuto = p_Desc.defineGroupParam("gAuto");
    gAuto->setLabels("Auto Grade", "Auto Grade", "Auto Grade");
    gAuto->setOpen(true);
    gMagic->setOpen(true);
    {
        // MAGIC GRADE, in its own section. It IS Creative plus one step -- run Creative, then
        // make a single colour decision from what the classifier found in the frame -- but it is
        // a different KIND of thing from the other two. Base and Creative correct and stylise the
        // whole picture; this makes one opinionated claim about one object in it. Grouped with
        // them, the panel read as four buttons of equal standing, which is not what they are.
        //
        // Defined last so the section lands after every Auto Grade control: OFX places a group
        // where its first child appears in page order, and defined in place it wedged itself
        // between Base Grade's tuning sliders and the Creative button.
        {
            PushButtonParamDescriptor* mg = p_Desc.definePushButtonParam("magicGrade");
            mg->setLabels("Magic Grade", "Magic Grade", "Magic Grade");
            mg->setHint("Applies Creative Grade, then looks at what is actually in the frame - sky, water, foliage, a person - decides which of those the shot is about, and makes ONE colour move to set it off against the rest. The move is chosen, not calculated to a target: which slider depends on whether the subject is the bright or the dark part of the frame, and which direction depends on the way it already leans. It does the looking ONCE: the frame fetch, the measurement and the segmentation all happen on this press, and the other subjects it found are listed in Subject below, where switching between them is instant. Pressing again simply redoes the analysis on the current frame. Some shots have nothing to separate - a flat aerial, a macro of leaves - and on those it simply leaves you with Creative Grade and says so.");
            mg->setParent(*gMagic);
            page->addChild(*mg);

            // THE FRAME'S OTHER ANSWERS, offered rather than hidden behind repeated presses.
            //
            // Rebuilt at runtime from the cached segmentation, the same way the Look LUT list is
            // rebuilt when its group changes. It has to start with a prompt rather than an empty
            // list: choice params save by index and the options cannot be rebuilt on load without
            // fetching a frame, so a reopened project would otherwise show a stale subject that
            // reads as a promise the node cannot keep.
            ChoiceParamDescriptor* ms = p_Desc.defineChoiceParam("magicSubject");
            ms->setLabels("Subject", "Subject", "Subject");
            ms->setHint("Which of the things in the frame the grade is built around. Magic Grade picks the most likely one and lists the rest here - selecting a different one re-grades around it immediately, because the expensive part (looking at the frame) is already done. The subject decides a lot: the grade holds ITS shadows and midtone in place, so a small subject like a face constrains the picture much more than a large one like sand or sky, and Bias has correspondingly less room afterwards. Which one is more pleasing is a judgement this plugin does not make - try them. The list is rebuilt each time you press Magic Grade, and is empty until you do.");
            ms->appendOption("- press Magic Grade -");
            ms->setDefault(0);
            ms->setParent(*gMagic);
            page->addChild(*ms);

            BooleanParamDescriptor* wb = p_Desc.defineBooleanParam("wbFirst");
            wb->setLabels("White Balance First", "White Balance First", "White Balance First");
            wb->setHint("Neutralise the frame's colour cast before Magic Grade looks at it. Magic Grade decides by comparing the subject against the rest of the scene, so a cast the camera introduced gets read as something in the room and pushed further -- balancing first means every difference it acts on is really there. It balances on surfaces that ought to be neutral, walls and floors and pavement, and deliberately ignores sky, water and foliage, which are coloured on purpose; a sunset over water has no neutral surface in it, so on those it leaves the balance alone and says so. Writes an ordinary Scene White Balance value you can drag afterwards.");
            wb->setDefault(false);
            wb->setParent(*gMagic);
            page->addChild(*wb);

            page->addChild(*defineSlider(p_Desc, "separation", "Separation",
                "How far to push the colour move Magic Grade chose. 1.0 is the move as decided, 0 removes it entirely, and past 1 exaggerates it. It rescales the SAME decision rather than making a new one, so dragging it feels like one control getting stronger rather than like pressing the button again. Negative reverses the move, which is occasionally what you want when the automatic direction reads backwards on a particular shot.",
                1.0, -2.0, 3.0, 0.01, gMagic));

            StringParamDescriptor* mn = p_Desc.defineStringParam("magicNote");
            mn->setLabels("Chose", "Chose", "Chose");
            mn->setStringType(eStringTypeLabel);
            mn->setDefault("");
            mn->setHint("Which option you are on, out of how many the frame offers, then the subject it picked and the slider move it made. 'This is Creative Grade' means the frame has no separable regions - one flat surface, or a single subject filling the frame - which is a real answer rather than a failure.");
            mn->setEnabled(false);
            mn->setParent(*gMagic);
            page->addChild(*mn);

            StringParamDescriptor* mw = p_Desc.defineStringParam("magicWhy");
            mw->setLabels("Why", "Why", "Why");
            mw->setStringType(eStringTypeLabel);
            mw->setDefault("");
            mw->setHint("The reasoning behind the choice above, in a sentence: what it found, why that slider, and why that direction. Which control is picked follows from where the subject sits in the frame's brightness - Offset Temp is an additive move so it has most grip on the dark parts, Gain Temp is multiplicative so it grips the bright parts. The direction follows from the way the subject already leans against everything else, pushed further that way. Worth reading even when the result is wrong, because it says exactly which of those two readings it got wrong.");
            mw->setEnabled(false);
            mw->setParent(*gMagic);
            page->addChild(*mw);

            // Saved with the project so Separation keeps scaling the chosen move after a reload
            // without needing the frame back. Same reasoning as the Bias anchor.
            IntParamDescriptor* mc = p_Desc.defineIntParam("magicCycle");
            mc->setDefault(0); mc->setIsSecret(true); mc->setParent(*gAuto);
            page->addChild(*mc);
            IntParamDescriptor* mp = p_Desc.defineIntParam("magicParam");
            mp->setDefault(-1); mp->setIsSecret(true); mp->setParent(*gAuto);
            page->addChild(*mp);
            auto hid = [&](const char* n) {
                DoubleParamDescriptor* d = p_Desc.defineDoubleParam(n);
                d->setDefault(0.0); d->setRange(-1e6, 1e6);
                d->setIsSecret(true); d->setParent(*gMagic);
                page->addChild(*d);
            };
            hid("magicBase"); hid("magicAnchor"); hid("magicSepAt");

            // A SECOND BIAS SLIDER, MIRRORING THE FIRST. OFX has no way to show one parameter
            // in two groups, so the choice is a duplicate that is kept in step or sending the
            // user back up the panel mid-thought. Magic Grade's output is a Creative grade with
            // one colour move on top, and the first thing anyone reaches for after looking at it
            // is the tonal lean -- so it belongs here as well.
            //
            // Kept in step in changedParam: each writes the other with setValue, which arrives
            // as eChangePluginEdit rather than eChangeUserEdit, so the handlers ignore it and
            // there is no loop. Same value, two places, one source of truth.
            page->addChild(*defineSlider(p_Desc, "autoBiasMirror", "Bias",
                "The same Bias slider as the one under Base and Creative Grade - the two always hold the same value, and moving either moves both. It is repeated here because Magic Grade produces a Creative grade with a colour move on top, and the tonal lean is the next thing you will want after looking at the result.",
                0.0, -2.0, 2.0, 0.01, gMagic));

            // LABEL ONLY -- the param stays "toneSep", so no saved project notices. Same rule as
            // the DWG/DI relabel: an OFX double saves by name, and the name is not the UI.
            //
            // "Face" rather than "Skin" because "skin tone" is an idiom meaning the COLOUR of
            // skin, and this control is about tonal PLACEMENT -- a colorist reading "Skin Tone
            // Separation" would reasonably expect a hue control. "Face" also states the current
            // limit out loud, which the panel has to do while the slider is inert everywhere else.
            page->addChild(*defineSlider(p_Desc, "toneSep", "Face Tone Separation",
                "How far the face sits from everything around it in lightness. Magic Grade places the face at a fixed target; this leans that placement, so the face separates from its surround rather than the whole picture moving together. Positive pushes them further apart, negative brings them closer, zero is the grade exactly as Magic Grade left it. Like Bias it moves the TARGETS and solves again rather than nudging sliders, so Lift, Gamma and Gain will move by different amounts to keep the rest of the grade coherent. It needs a Magic grade to lean on, and it is deliberately inert in two cases. On a frame where the subject and its surround already sit at the same lightness there is no direction for 'further apart' to point in. It leans FACES only, which is what the name says: the placement it re-solves was fitted to a subject near the bottom of the tonal range, where Lift has the authority to move a floor, and on a bright subject such as sky that same solve runs Lift to its limit and blows the highlights - so it does nothing there rather than something wrong. The grade itself is unaffected on every subject; it is only the leaning that is limited.",
                0.0, -1.0, 1.0, 0.01, gMagic));

            StringParamDescriptor* sn = p_Desc.defineStringParam("sepNote");
            sn->setLabels("Separation", "Separation", "Separation");
            sn->setStringType(eStringTypeLabel);
            sn->setDefault("");
            sn->setHint("Whether Face Tone Separation can act on this shot, and when it cannot, why. It needs Magic Grade to have solved a FACE: it re-solves the placement the grade was fitted to, and that fit assumes a subject near the bottom of the tonal range, so it is offered on faces and withheld everywhere else rather than applied wrongly. 'Not solved' means the face was found but the grade declined it - a mask covering too much of frame is the common one, and parking on a frame where the face is smaller and pressing Magic Grade again will often arm it. 'Faces only' means this shot's subject is something else, and no frame of it will change that.");
            sn->setEnabled(false);
            sn->setParent(*gMagic);
            page->addChild(*sn);

            // SCENE EXPOSURE, MIRRORED, AND LAST. Magic Grade sets it itself when it decides a
            // subject is underexposed rather than low-key -- the one correction it makes BEFORE
            // the camera transform -- so it is the control most likely to want a nudge
            // afterwards, and sending the user to another section to find it breaks the loop
            // they are in.
            //
            // Below Bias and Face Tone Separation rather than above them because it is not
            // another way to lean the same solve: those two move the TARGETS and re-solve,
            // this one changes what the frame was exposed to. It reads as the final trim on
            // whatever they settled on, which is how it is used.
            //
            // Two faces of one value, like Bias: each writes the other with setValue, which
            // arrives as eChangePluginEdit and is ignored by both handlers, so there is no loop.
            page->addChild(*defineSlider(p_Desc, "rawExpMirror", "Scene Exposure",
                "The same Scene Exposure slider as the one under Exposure and White Balance - the two always hold the same value, and moving either moves both. It is repeated here because Magic Grade sets it itself when it reads the subject as underexposed rather than deliberately dark, correcting before the camera transform the way exposing properly would have. That makes it the control most likely to want a nudge after looking at the result.",
                0.0, -5.0, 5.0, 0.01, gMagic));


            // WHICH BIAS YOU HAVE, said out loud. The slider runs two different control laws
            // and picks between them on state nothing on the panel shows: with Magic Tone's
            // targets armed it re-solves them, and otherwise it offsets the three sliders
            // together. Both are right; being unable to tell which one you are holding is not.
            // Same rule as the encode note above - a silent override is a bug even when the
            // math is right - and the same ~45-character ASCII budget.
            StringParamDescriptor* bn = p_Desc.defineStringParam("biasNote");
            bn->setLabels("Bias mode", "Bias mode", "Bias mode");
            bn->setStringType(eStringTypeLabel);
            bn->setDefault("");
            bn->setHint("Which way Bias is working right now. After Magic Grade it moves the TARGETS the grade was solved for and solves again, so Lift, Gamma and Gain move by different amounts and in different directions to keep the subject where it was put - the numbers look erratic while the picture stays coherent, because they are results rather than settings. It also means a hand edit to those three is re-solved away the next time you touch Bias. Without a Magic grade it is a plain offset: all three move together, the whole image up or down.");
            bn->setEnabled(false);
            bn->setParent(*gMagic);
            page->addChild(*bn);
        }
        BooleanParamDescriptor* show = p_Desc.defineBooleanParam("showAnalysis");
        show->setLabels("Show analysis", "Show analysis", "Show analysis");
        show->setHint("Reveal the frame measurements Auto Grade works from - exposure key, dynamic range, display percentiles, highlight shape, source clipping and the skin read. Off by default so the panel stays a grading panel; turn it on when a shot behaves oddly and you want to see why. Purely informational, it changes nothing.");
        show->setDefault(false);
        show->setParent(*gAuto);
        page->addChild(*show);

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
                  "Measured on scene light (XYZ luminance after the camera decode, before any grade). Y50 is the median. 'key' is how far that median sits from 18% mid-gray in stops - the exposure correction the shot is asking for, since Scene Exposure is a linear gain in stops. 'DR' is p1 to p99 in stops: how much usable range the shot actually has.");
        probeLine("probeDisplay", "Display",
                  "Luma percentiles after the full pipeline at NEUTRAL grade, in your current Output Encode: 1st, 50th, 99th. This is the space Lift/Gamma/Gain work in, so these are the numbers a black-point or highlight target would be set against. No LUT is applied.");
        probeLine("probeShape", "Shape",
                  "'hot' is the share above 1.0 in display - bright, but it pulls back fine if the range was captured. 'pin' is the share of the frame sitting ON the source ceiling, with that ceiling's code value after the @ - this is clipping at the sensor, where no exposure move brings anything back. Measured against the clip's own maximum rather than 1.0, because log formats don't all reach the top of the code range (Blackmagic peaks near 0.75). A low pin % means the highlights roll off and the range was captured; a high one means they are stacked on the ceiling and gone. 'sat' is mean HSV saturation over mid-tones only, which is what a Density move would act on.");
        // Bias anchor: hidden, saved with the project. See applyBias().
        {
            BooleanParamDescriptor* ba = p_Desc.defineBooleanParam("biasArmed");
            ba->setDefault(false); ba->setIsSecret(true); ba->setParent(*gAuto);
            page->addChild(*ba);
            auto anch = [&](const char* n, double def) {
                DoubleParamDescriptor* d = p_Desc.defineDoubleParam(n);
                d->setDefault(def); d->setRange(-1e6, 1e6);
                d->setIsSecret(true); d->setParent(*gAuto);
                page->addChild(*d);
            };
            anch("biasGain", 1.0); anch("biasLift", 0.0); anch("biasGamma", 1.0);
            anch("biasRoll", 0.0); anch("biasHot", 0.0);
            // The slider position the anchor was taken at. Without it, a manual edit either
            // jumps the slider to zero or silently discards the edit on the next drag.
            anch("biasAt", 0.0);
            // The three NEUTRAL percentiles Magic Tone solved against, plus a flag. Hidden and
            // saved, like the rest of the anchor, so Bias keeps re-solving after a reload rather
            // than falling back to nudging parameters.
            anch("toneLo", -1.0); anch("toneMid", -1.0);
            anch("toneShi", -1.0); anch("toneHi", -1.0); anch("toneFLo", -1.0);
            // ...and what the grade currently ACHIEVES at the first, second and fourth of them.
            // Bias offsets from these, so re-deriving them after a hand edit is what makes the
            // edit survive: at bias 0 the solve is asked for what is already on screen.
            anch("toneTFloor", -1.0); anch("toneTMid", -1.0); anch("toneTCeil", -1.0);
            anch("toneTFMax", -1.0);
            // The measured direction, saved with the project so Tone Separation still knows which
            // way to lean after a reload rather than going inert until the button is pressed again.
            anch("toneSepDir", 0.0);
        }

        PushButtonParamDescriptor* apply = p_Desc.definePushButtonParam("probeApply");

        // Two buttons, one pair: Base fixes the range, Creative applies a look. They are
        // adjacent and named as siblings because that is the whole affordance — OFX has no
        // layout hint in this SDK and Resolve lays parameters out one per row, so a shared
        // horizontal line isn't available. Naming and order carry it instead.
        {
            StringParamDescriptor* tip = p_Desc.defineStringParam("autoTip");
            tip->setLabels("Start", "Start", "Start");
            tip->setStringType(eStringTypeLabel);
            tip->setDefault("Base = correct the range. Creative = add a look.");
            tip->setHint("Base Grade measures the frame and places its range so nothing is crushed at 0 or clipped at 1023 - a neutral, gradable starting point with no LUT. Creative Grade does the same measurement but applies the Cinematic Film Emulation look on top. Base first if you intend to grade; Creative if you want a finished-looking image straight away.");
            tip->setEnabled(false);
            tip->setParent(*gAuto);
            page->addChild(*tip);
        }

        // Base: the range-correcting starting point. Listed before the creative button
        // because it is the one most users want — no LUT, no tint, just the smooth decode
        // with the frame's range brought inside 0-1023 so there is somewhere to grade from.
        PushButtonParamDescriptor* cb = p_Desc.definePushButtonParam("autoGradeClean");
        cb->setLabels("Base Grade", "Base Grade", "Base Grade");
        cb->setHint("Measure the frame and set Lift/Gamma/Gain so the picture sits inside the displayable range - nothing crushed at 0, nothing clipped at 1023 - with NO LUT and no film tint. The camera is set to the smooth decode and left to do the work. This is a starting point to grade FROM, not a look: it corrects range, never taste. Balance and Density are explicitly zeroed. Highlight Rolloff still comes from measured source clipping. Everything it writes is an ordinary slider value you can drag afterwards.");
        cb->setParent(*gAuto);
        page->addChild(*cb);

        // Containment targets — the knobs the Clean constants get fitted with. Debug-only.
        auto tune = [&](const char* name, const char* label, const char* hint,
                        double def, double lo, double hi) {
            DoubleParamDescriptor* d = p_Desc.defineDoubleParam(name);
            d->setLabels(label, label, label);
            d->setHint(hint);
            d->setDefault(def); d->setRange(lo, hi); d->setDisplayRange(lo, hi);
            d->setIncrement(0.01); d->setParent(*gAuto);
            page->addChild(*d);
        };
        tune("cleanHigh", "Target High", "Where the 99th percentile should land after grading, measured AFTER Highlight Rolloff. 0.95 fills the range without sitting on the clip point.", 0.94, 0.50, 1.00);
        tune("cleanLow",  "Target Low",  "Where the 0.1st percentile - effectively the darkest part of the picture - should land. Just off zero, so shadows sit above black rather than crushing into it. This is deliberately a much deeper percentile than the highlight end uses: placing p1 here left 1% of the frame below the target and that 1% was visibly crushed.", 0.05, 0.00, 0.30);
        tune("cleanMid",  "Target Mid",  "Where the median should land if the midtone solve were applied in full. Only a fraction of it is - see Mid Strength.", 0.70, 0.10, 0.90);
        tune("cleanMaxGain","Max Gain","Ceiling on the Gain the solve may use. 1.0 means it can only ever darken, which is deliberate: a shot whose highlights sit below the target is not clipping, it is just dark, and brightening it destroys the intent. Raise above 1.0 only if you want genuinely underexposed footage pushed up.", 2.00, 0.50, 2.00);
        tune("cleanMaxExp","Max Exposure","The most Base may BRIGHTEN a shot, in stops. Darkening is never limited - pulling a blown frame down is always safe - but pushing a dark one up destroys a deliberately low-key image, and the solver cannot tell the difference: it only knows whether it reached the target. A dark car interior asked for +1.74 stops without this; the same shot graded by hand used +0.55.", 0.85, 0.00, 2.00);
        tune("cleanShoulder","Shoulder","How much Highlight Rolloff to apply per unit of highlight overshoot - how far the channels run past display white before grading. This is the shoulder that stands in for a film stock's, since Lift/Gamma/Gain cannot make an S-curve on its own. Source clipping (pin) sets a floor underneath it. 0 disables the overshoot term and leaves rolloff on source clipping alone, which is what Creative uses.", 0.216, 0.00, 1.50);
        tune("creativeLow","Creative Black","Where Creative Grade places its black point, measured before the print stock. The preset used to stamp a fixed Lift of 0.11, which lands a different black point on every shot depending on where the footage's own floor already sits - and on three hand-graded shots in a row the user pulled it back down, describing it as the shadows being lifted too far. Solving for a target instead makes the result consistent across footage, the same way Base has always placed its floor. To fit this number, grade a shot by hand until it looks right and read the Tone row's graded black value.", 0.050, 0.00, 0.30);
        tune("cleanMidStr","Mid Strength","How much of the midtone solve to apply. 0 leaves Gamma at 1.0 and only the two ends are corrected; 1.0 drives every shot's median to Target Mid, which flattens deliberately dark shots into mid-gray. The default is halfway: containment at the ends is objective, the midtone is intent.", 0.838, 0.00, 1.00);

        apply->setLabels("Creative Grade", "Creative Grade", "Creative Grade");
        apply->setHint("Analyses the frame and applies the Cinematic Film Emulation look on top - use this when you want a finished-looking image straight away rather than something to grade from. Sets Gain from the measured key. Fitted to hand-graded shots rather than to a textbook target: a bright shot gets Gain pulled down, a dark one is left at the preset - deliberately, since a low-key shot is meant to sit low. Everything it writes is an ordinary slider value you can drag afterwards.");
        apply->setParent(*gAuto);
        page->addChild(*apply);

        // Range is +/-2, not +/-1. Widened once Creative started SOLVING its black point
        // instead of stamping Lift 0.11: the solved floor lands lower, which is right on most
        // shots and a touch dark on some, and at the old limit there was no way to open those
        // back up. The coefficients are unchanged, so 1.0 still means exactly what it always
        // did and no saved grade moves; there is simply more travel past it. Measured:
        //
        //     bias        0     +1    +1.5    +2     (old fixed-lift black point)
        //     beach     0.050  0.181  0.255  0.330          0.229
        //     city      0.050  0.178  0.250  0.324          0.161
        //
        // so the old stamped result sits between +1 and +1.5 depending on the shot, and the
        // travel past that is genuinely new headroom rather than just restoring the old look.
        //
        // NOTE the negative half now does much less to the shadows, because at bias 0 the black
        // point already sits near the floor: both -1 and -2 land on the anti-crush guard at
        // 0.006. That is correct — there is nothing below black to take away — but it means
        // negative Bias is now mostly a highlight and midtone control, which is a change in
        // what the slider feels like either side of zero.
        page->addChild(*defineSlider(p_Desc, "autoBias", "Bias",
            "Leans the result across the whole tonal range. Negative protects the highlights (shoulder up, floor down, mids darker, gain pulled); positive opens the image up (floor and mids up, shoulder off). Zero is the grade exactly as Base or Creative left it. It is an OFFSET from that grade rather than a recalculation, so it works with either button, keeps working after the project is reopened, and can be used on a grade you built by hand - the first move adopts the current settings as its zero point. The range runs to +/-2 so there is room past a normal correction; around +1.5 the shadows sit about where a fixed lift used to put them, which is useful on a shot the measured black point reads a little dark on.",
            0.0, -2.0, 2.0, 0.01, gAuto));

        probeLine("probePeak", "Peak",
                  "p99.9 in display, and how far it runs past p99. A compact blown specular - a window, a lamp - sits far above the bulk of the highlights and gives a high multiplier; a broad bright field like sunlit sand sits just above it. This is the shape of the top end rather than its size, which is what decides whether a shot wants Highlight Rolloff.");
        probeLine("probeSubject", "Subject",
                  "The same exposure question asked of skin-toned pixels only, plus what share of the frame matched. Frame-median exposure is subject-blind: a dark interior drags the median down and asks for a push that would blow the windows. Where the two keys disagree, the frame median is the wrong one. Note the mask cannot tell skin from sand - a high coverage % on a landscape means it matched the scene, not a face.");
        probeLine("probeApplied", "Applied",
                  "What the Auto Grade button last wrote, and the measurement it came from. Blank until you press it. Analyze Frame never changes anything; only Auto Grade does.");
        probeLine("probeColour", "Colour",
                  "The frame's colour, in CIELAB over the mid-tones: a* is green-to-magenta, b* is cool-to-warm, C is overall colourfulness. 'sep' is how far apart the two dominant colour populations sit - a low number on a frame that visibly has two subjects (sky over water, say) means they are sharing a colour and would separate if pushed apart. Lab rather than HSV because b* lines up one-for-one with the Temp controls and a* with the Tint ones, which is what makes the Response row below readable.");
        probeLine("probeGraded", "Graded",
                  "The same colour measurements as the row above, but for the grade currently on this node instead of a neutral one - so the two lines together say what your grade DID. Every other row here deliberately measures the ungraded footage, which makes them identical no matter what you set; this is the one that moves. Camera and Output Encode are held the same as the neutral row so the only difference is the sliders. It is measured before the LUT, so with a film stock selected this is the grade underneath the stock rather than the picture on screen - the row says 'pre-LUT' when that is the case.");
        probeLine("probeTone", "Tone",
                  "The tonal shape of the picture, neutral > graded: the black point (0.1st percentile per channel), the midtone, the white point (99th percentile per channel), and how far the channels run past display white on average. Per channel rather than luma because a channel is what actually clips - on a saturated highlight the three spread far apart while a luma number says everything is fine. This is the half of a grade the colour rows cannot see, and on some shots it is the whole grade.");
        probeLine("probeRegions", "Regions",
                  "The two dominant colour populations found by clustering the frame, cooler one first: what share of the frame each holds and its hue angle in degrees. Then 'db*', how much warmer the top third of the frame is than the bottom third - a large positive number is the signature of a warm sky over a cooler foreground. Membership is decided once, from the ungraded picture, so these describe the footage rather than the grade currently on it.");
        probeLine("probeDriveB", "Drives b*",
                  "Which controls actually produced the warm/cool change between a neutral node and the grade currently on it, biggest contributor first. 'act' is the measured change, 'lin' is what the measured response predicted, and the gap between them says how far outside the linear range your grade sits - a big gap means the sliders are being pushed hard enough that their effect is tailing off. This row exists because naming the obvious control by eye does not work: on a real grade colourfulness rose while Density had actually been LOWERED, with Lift, Gain and Offset Temp pushing it up between them.");
        probeLine("probeDriveC", "Drives dL*",
                  "Which controls pushed the frame's two regions apart in LIGHTNESS, and which flattened them together. Tone separation is half of what makes a frame read as dynamic - the other half is hue, on the row below - and it was the axis missing from the first version of this measurement entirely.");
        probeLine("probeDriveS", "Drives db*",
                  "Which controls pushed the two regions apart on the warm/cool axis - the move that lets a cool ocean separate from a warm sky. Read with 'Drives dL*' above: together they are the two axes of separation, and a grade can gain one while losing the other.");
        probeLine("probeSepTriple", "Separation",
                  "The separation between the frame's two regions - currently its top and bottom third - as three signed numbers, shown as neutral > graded so you can see what your grade did to each. dL* is TONE separation, da* and db* are HUE separation on the green-magenta and cool-warm axes. Three signed components rather than one distance, because a distance cannot be solved against: it is built from squares, so it cannot express one axis opening while another closes, and on real footage it predicted the wrong direction outright.");
        probeLine("probeResponse", "Response",
                  "What the controls actually DO on this shot, measured rather than assumed: how far b* (cool-to-warm) moves per nudge of each balance control, and how far colourfulness moves per nudge of Density. This is the plugin working out for itself that negative Offset Temp is what adds blue. It is shot-dependent - the same slider does something different to a saturated sunset than to a snowfield - which is why it is measured on every analyse instead of written down once.");


    }

    // ---- 1. Input Transform (CST) ----
    // JUST THE CAMERA. Scene Exposure and Scene White Balance used to live here because they act
    // at the same POINT in the pipeline -- immediately after the decode -- but that is a fact
    // about the maths, not about the work. Both are exposure and balance decisions, so they sit
    // with the other ones below and this group answers a single question: what shot this.
    GroupParamDescriptor* gInput = p_Desc.defineGroupParam("gInput");
    gInput->setLabels("1  Input Transform", "1  Input Transform", "1  Input Transform");
    gInput->setOpen(true);
    ChoiceParamDescriptor* cam = p_Desc.defineChoiceParam("camera");
    cam->setLabels("Camera", "Camera", "Camera");
    cam->setHint("Source camera log/gamut, decoded to the DaVinci Wide Gamut linear working space. Every entry except the last is a colorimetric decode: pick your camera and you get a faithful transform - Blackmagic Gen 5 Film for Pocket/URSA/Pyxis clips, DaVinci Wide Gamut / Intermediate for clips already in that space, and so on. The default, 'Rec.2100 PQ - Smooth Decode', is the exception and is NOT a camera match: it runs log footage through the PQ inverse EOTF, a strongly compressive curve that happens to land log material with a near-perfect highlight rolloff and smooth color. It is a look wearing a transfer function, and it is the happy path all presets build on. Use it when you want a good image fast; pick your real camera when you want a faithful one.");
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
    // Index 11 is a transfer function used deliberately "wrong" — a compressive curve that
    // flatters log footage — not a camera. It used to read "Rec.2100 PQ / ST.2084 (HDR)",
    // which put a look in a slot the rest of the list reserves for colorimetric decodes;
    // a forum reader called that out and they were right (2026-08-03). Renamed, NOT moved:
    // choice params save by index, so reordering would silently repoint every saved grade.
    cam->appendOption("Rec.2100 PQ - Smooth Decode");
    cam->setDefault(11);    // the creative "smooth decode" default (see hint)
    cam->setParent(*gInput);
    page->addChild(*cam);

    // Exposure + white balance on scene light, before the gamut transform. These were
    // called "RAW Exposure" / "RAW Temperature" until 2026-08-03 because they stand in for
    // the Camera RAW tab's controls — but no sensor data reaches an OFX plugin, so the name
    // promised a relationship that doesn't exist and confused beginners (forum feedback).
    // Labels only; the param IDs stay rawExp/rawTemp so saved grades are unaffected.

    // ---- 2. Balance ----  (white balance in linear; watch the vectorscope while adjusting)
    GroupParamDescriptor* gBal = p_Desc.defineGroupParam("gBalance");
    gBal->setLabels("2  Balance & Density", "2  Balance & Density", "2  Balance & Density");
    gBal->setOpen(true);
    defineBypass(p_Desc, page, "bypassBalance",
                 "Mute this stage at render without losing its values. Gain and Offset balance are held neutral; the sliders grey out but keep their numbers, so switching back restores the grade exactly.", gBal);
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

    page->addChild(*defineSlider(p_Desc, "density", "Density", "Color density: saturation gain in HSV (the green-channel-of-Gain-in-HSV trick). -1 = grayscale, +1 = double saturation.", 0.0, -1.0, 1.0, 0.001, gBal));
    defineBypass(p_Desc, page, "bypassDensity",
                 "Mute this stage at render without losing its value. Density is held at 0 (no saturation change); the slider greys out but keeps its number.", gBal);


    // ---- 3. Exposure and White Balance ----
    //
    // WHAT THE SHOT IS EXPOSED AND BALANCED TO, in one place, regardless of where in the pipeline
    // each control acts. Scene Exposure and Scene White Balance run right after the camera decode
    // and Lift/Gamma/Gain runs in the display curve, with the LUT in between -- but a colourist
    // reaching for "this is too dark" or "this is too green" does not care which side of the LUT
    // the fix lands on. Highlight Rolloff joins them for the same reason: it is the top end of the
    // exposure decision, and it was in Trim only because it happens last.
    GroupParamDescriptor* gExp = p_Desc.defineGroupParam("gExposure");
    gExp->setLabels("3  Exposure and White Balance", "3  Exposure", "3  Exposure");
    gExp->setOpen(true);
    defineBypass(p_Desc, page, "bypassExposure",
                 "Mute this stage at render without losing its values. Lift/Gamma/Gain are held neutral (0/1/1); the sliders grey out but keep their numbers. Note Auto Grade drives Gain, so bypassing this also mutes the auto exposure.", gExp);
    page->addChild(*defineSlider(p_Desc, "rawExp", "Scene Exposure", "Exposure in stops applied to scene light immediately after the camera decode, before the gamut transform - a linear gain on the scene, which is mechanically the same operation the Camera RAW tab's Exposure performs. Called 'Scene' rather than 'RAW' because this acts on the decoded image, not on the raw file: no sensor data reaches an OpenFX plugin.", 0.0, -5.0, 5.0, 0.01, gExp));
    page->addChild(*defineSlider(p_Desc, "rawTemp", "Scene White Balance", "White-balance color temperature in Kelvin, applied as a Bradford chromatic adaptation in XYZ right after the camera decode - the closest point in the chain to the sensor. Raise = warmer, lower = cooler; 6500 = neutral. This is a physically real white balance, but NOT the Camera RAW tab's: reproducing a raw decoder's WB needs sensor metadata, which an OpenFX plugin never receives.", 6500.0, 2000.0, 15000.0, 10.0, gExp));

    page->addChild(*defineSlider(p_Desc, "lift",  "Lift",  "Raise/lower shadows (offset)", 0.0, -0.5, 0.5, 0.001, gExp));
    page->addChild(*defineSlider(p_Desc, "gamma", "Gamma", "Midtone brightness (power)",    1.0,  0.2, 3.0, 0.001, gExp));
    page->addChild(*defineSlider(p_Desc, "gain",  "Gain",  "Highlights / overall (multiply)", 1.0, 0.0, 3.0, 0.001, gExp));

    page->addChild(*defineSlider(p_Desc, "rolloff", "Highlight Rolloff", "Soft-clips bright highlights per channel so lamps/speculars roll off to white instead of clipping to a flat neon patch. Higher = earlier, stronger shoulder. Only active on display-referred output (Rec.709 encodes or any LUT path).", 0.0, 0.0, 1.0, 0.001, gExp));


    // ---- 4. Range Balance ----
    //
    // For footage whose range WAS captured -- a window and an unlit room both inside the sensor's
    // latitude -- where one curve has to blow one end to serve the other. In Resolve this is a
    // qualifier, an invert and a second node; here it is a latch and three sliders, which is the
    // whole reason the plugin exists.
    GroupParamDescriptor* gRange = p_Desc.defineGroupParam("gRange");
    gRange->setLabels("4  Range Balance", "4  Range Balance", "4  Range Balance");
    gRange->setOpen(true);

    page->addChild(*defineSlider(p_Desc, "rangeLatch", "Latch",
        "Where the highlight mask starts, on the same 0-100 scale as Resolve's Luminance qualifier - everything brighter than this is held, everything below it is opened up. 0 switches the whole stage off, which is the default. Press 'Set From Frame' to measure it from the shot rather than guessing: it reads the bright population off the current frame and puts the latch where that population starts. Measured against a hand-dialled qualifier on a bedroom interior it landed within half a point.",
        0.0, 0.0, 100.0, 0.1, gRange));

    PushButtonParamDescriptor* rset = p_Desc.definePushButtonParam("rangeSet");
    rset->setLabels("Set From Frame", "Set From Frame", "Set From Frame");
    rset->setHint("Measure the latch from the current frame. Reads the picture as it arrives at this node, before the grade curve, and puts the latch at the start of the bright population. Park the playhead on a frame that shows the highlight you care about - a window, a sky, a practical - and press. It is measured once and then stays put, so the mask cannot drift while you work; press again on another frame if the shot changes.");
    rset->setParent(*gRange);
    page->addChild(*rset);

    BooleanParamDescriptor* rsh = p_Desc.defineBooleanParam("rangeShow");
    rsh->setLabels("Show Mask", "Show Mask", "Show Mask");
    rsh->setHint("Show the mask itself instead of the picture: white is held, black is opened up, grey is the soft edge between them. Turned on automatically by 'Set From Frame' so you can see what was measured, and meant to be turned off again once the latch looks right. The matte bypasses the output encode, the LUT and the trim, so what you see is the mask exactly as the maths has it rather than a picture of it - 50% grey really is half coverage. Range Balance counts as ON while this is ticked, even with the three moves left at neutral, so the mask can be dialled before deciding what to do with it.");
    rsh->setDefault(false);
    rsh->setParent(*gRange);
    page->addChild(*rsh);

    // LOCK THE MASK AGAINST THE GRADE THAT MOVES UNDER IT.
    //
    // The mask reads the picture after the grade curve -- which is what lets it separate a window
    // from a bright pillow, and also what makes it slide when you change exposure. So the held
    // region grows or shrinks under the very control you are using to adjust it, and "pull the
    // window down" ends up changing WHAT the window is.
    //
    // Locked, the mask is evaluated against the Lift/Gamma/Gain that were in effect when the latch
    // was measured, and stops moving. The three anchors are ordinary saved params (hidden, since
    // they are a captured state rather than a control) so a reloaded project keeps its mask --
    // instance state would have made the lock quietly evaporate, the way the Bias anchor did
    // before it was persisted.
    BooleanParamDescriptor* rlk = p_Desc.defineBooleanParam("rangeLock");
    rlk->setLabels("Lock Mask", "Lock Mask", "Lock Mask");
    rlk->setHint("Freeze the mask against the exposure underneath it. The mask normally reads the graded picture, which is what lets it tell a window from a bright pillow - but it also means changing Lift, Gamma or Gain moves the selection while you are working on it. Tick this and the mask is measured against the grade that was in effect when you pressed 'Set From Frame', so you can pull the held area right down and it stays exactly the same shape - like changing the power of the sun rather than re-choosing what the sun is lighting. Press 'Set From Frame' again to re-capture at the current grade. NOTE it locks against the grade curve only: RAW Exposure and Density sit upstream of the mask and will still move it.");
    rlk->setDefault(false);
    rlk->setParent(*gRange);
    page->addChild(*rlk);

    // The captured grade. Hidden: it is a measurement the Lock takes, not a number to type.
    for (int k = 0; k < 3; ++k) {
        static const char* nm[3] = { "rangeRefLift", "rangeRefGamma", "rangeRefGain" };
        static const double dv[3] = { 0.0, 1.0, 1.0 };
        DoubleParamDescriptor* rp = p_Desc.defineDoubleParam(nm[k]);
        rp->setDefault(dv[k]);
        rp->setRange(-1000.0, 1000.0);
        rp->setIsSecret(true);
        rp->setParent(*gRange);
        page->addChild(*rp);
    }

    page->addChild(*defineSlider(p_Desc, "rangeSoft", "Softness",
        "How gradually the mask fades in at the latch, in the same 0-100 units. Resolve splits this into separate low and high softness; one number covers it here because the two are almost always set together. Raise it if the boundary shows as an edge in a gradient. NOTE this is softness in BRIGHTNESS, not a spatial blur: it feathers across tones rather than across the picture, so it cleans up a hard edge in a smooth gradient but cannot settle a mask boundary that lands inside noise.",
        2.6, 0.0, 25.0, 0.1, gRange));

    page->addChild(*defineSlider(p_Desc, "rangeHigh", "Held: Brightness",
        "What happens to the held area - the window, the sky, whatever sits above the latch. Below 1 pulls it down and brings its detail back; 1 leaves it exactly as it was. This is the half that recovers: simply protecting highlights cannot restore a window an earlier stage already blew, so this is what makes the range usable rather than merely undamaged. On a shot exposed FOR the highlights, leave it at 1 and work with the two 'Rest' controls below.",
        1.0, 0.05, 2.0, 0.001, gRange));

    page->addChild(*defineSlider(p_Desc, "rangeHiMid", "Held: Midtones",
        "Midtone contrast inside the held area, after Brightness has pulled it down. Pulling a cloud bank or a bright window down with Brightness alone flattens the detail that was the reason for keeping it - this puts that detail back. Above 1 opens the held area's midtones, below 1 closes them. There is deliberately no lift here: lift's authority falls away toward white, so on a region selected FOR being bright it does essentially nothing.",
        1.0, 0.2, 3.0, 0.001, gRange));

    page->addChild(*defineSlider(p_Desc, "rangeShadow", "Rest: Shadows",
        "Raises the floor of everything the mask does NOT hold - the room, in the window case. The same lift as the one under Exposure, but applied only outside the mask, so opening the interior cannot touch the highlight you just recovered.",
        0.0, -0.5, 0.5, 0.001, gRange));

    page->addChild(*defineSlider(p_Desc, "rangeMid", "Rest: Midtones",
        "Midtone brightness of everything outside the mask. This is usually the main move: a room that was correctly exposed but dark comes up here while the window stays where 'Held: Brightness' put it. Above 1 opens the interior, below 1 closes it down.",
        1.0, 0.2, 3.0, 0.001, gRange));

    page->addChild(*defineSlider(p_Desc, "rangeLoGain", "Rest: Brightness",
        "Overall brightness of everything outside the mask, multiplied rather than gamma'd - it pivots on black, so it opens the whole of the rest while leaving its floor where it is. Reach for this when the whole unmasked area is simply too dark; reach for Midtones when it is the middle that needs opening and the shadows are already where you want them.",
        1.0, 0.05, 3.0, 0.001, gRange));

    // ---- the shape: WHERE Range Balance acts, as opposed to on what ----
    //
    // A luminance qualifier cannot tell a silk specular from a mountain -- measured on the bedroom
    // frame, they are the same brightness AND the same colour (b* +1.52 against +1.49), so neither
    // a higher latch nor a chroma gate separates them. What does is that they are in different
    // PLACES. The shape multiplies the luminance mask, so the held region is "bright AND inside
    // the shape", which is a qualifier plus a power window.
    {
        ChoiceParamDescriptor* sh = p_Desc.defineChoiceParam("rangeShape");
        sh->setLabels("Shape", "Shape", "Shape");
        sh->appendOption("None (whole frame)");
        sh->appendOption("Ellipse");
        sh->appendOption("Rectangle");
        sh->setDefault(0);
        sh->setHint("Restrict Range Balance to part of the frame. The shape multiplies the brightness mask rather than replacing it, so what gets held is whatever is BOTH above the latch and inside the shape - a window pane picked out by the latch, with a bright pillow across the room excluded because it is somewhere else. None means the whole frame, which is how this behaves with the shape switched off. TO AIM IT: turn Show Mask on and drag the Centre and Size sliders - the matte shows the shape's edge directly. Some hosts also draw a draggable outline on the viewer; the Host line under Setup / Help says whether this one does.");
        sh->setParent(*gRange);
        page->addChild(*sh);
    }
    {
        PushButtonParamDescriptor* sf = p_Desc.definePushButtonParam("rangeShapeFit");
        sf->setLabels("Fit To Frame", "Fit To Frame", "Fit To Frame");
        sf->setHint("Put the shape around whatever the latch is already holding, measured off the current frame - on an interior that is the window. Aiming a rectangle with four sliders is worse than Resolve's own power window, so this does not try to compete on drawing; measuring is the thing a power window cannot do. Fitted to the 2nd and 98th percentiles of the held positions rather than to their bounding box, so one stray specular across the room cannot stretch the shape over the whole picture. Set the latch first, then press this, then adjust if you want it tighter.");
        sf->setParent(*gRange);
        page->addChild(*sf);
    }

    // Centre-origin and normalised by HALF-HEIGHT on both axes, so a circle is round on a 16:9
    // frame and Size means the same distance whichever way you go. X therefore runs past 1 at the
    // sides, which is the price of that.
    page->addChild(*defineSlider(p_Desc, "rangeShapeX", "Shape: Centre X",
        "Horizontal centre of the shape. 0 is the middle of frame, -1 and +1 are one half-frame-HEIGHT out - so on a 16:9 image the left and right edges sit near -1.78 and +1.78. Measured in height units on both axes so that a circle stays circular.",
        0.0, -2.0, 2.0, 0.001, gRange));
    page->addChild(*defineSlider(p_Desc, "rangeShapeY", "Shape: Centre Y",
        "Vertical centre of the shape. 0 is the middle of frame, -1 the bottom edge, +1 the top.",
        0.0, -2.0, 2.0, 0.001, gRange));
    page->addChild(*defineSlider(p_Desc, "rangeShapeW", "Shape: Size X",
        "Half-width of the shape, in the same height units as the centre. 1.0 reaches from the middle of frame to a half-height out.",
        0.5, 0.01, 4.0, 0.001, gRange));
    page->addChild(*defineSlider(p_Desc, "rangeShapeH", "Shape: Size Y",
        "Half-height of the shape. Set it equal to Size X for a circle or a square.",
        0.5, 0.01, 4.0, 0.001, gRange));
    page->addChild(*defineSlider(p_Desc, "rangeShapeR", "Shape: Rotation",
        "Rotation in degrees, for a window or a skylight that is not square to frame.",
        0.0, -180.0, 180.0, 0.1, gRange));
    page->addChild(*defineSlider(p_Desc, "rangeShapeS", "Shape: Softness",
        "How far the shape's edge feathers, as a fraction of its size. Feathered symmetrically about the boundary, so softening does not shrink the selection. 0 is a hard edge. This IS a spatial feather - unlike the Softness above it, which feathers across brightness.",
        0.25, 0.0, 1.0, 0.001, gRange));
    {
        BooleanParamDescriptor* si = p_Desc.defineBooleanParam("rangeShapeInv");
        si->setLabels("Shape: Invert", "Shape: Invert", "Shape: Invert");
        si->setHint("Act everywhere EXCEPT inside the shape. Use it to exclude one bright thing you do not want held - a practical, a specular - rather than to pick out the one you do.");
        si->setDefault(false);
        si->setParent(*gRange);
        page->addChild(*si);
    }

    {
        StringParamDescriptor* sn = p_Desc.defineStringParam("rangeShapeNote");
        sn->setLabels("Shape", "Shape", "Shape");
        sn->setStringType(eStringTypeLabel);
        sn->setDefault("Set the latch, then press Fit To Frame");
        sn->setHint("What the shape is doing, and how much of the frame it ended up around. Also where the on-screen outline reports itself if this host ever draws one - Resolve advertises OFX overlay support but never asks a Color page effect to draw, so the outline is unavailable there and Fit To Frame is the way to place a shape.");
        sn->setEnabled(false);
        sn->setParent(*gRange);
        page->addChild(*sn);
    }

    defineBypass(p_Desc, page, "bypassRange",
                 "Mute this stage at render without losing its values. Range Balance is held off; the sliders grey out but keep their numbers.", gRange);

    StringParamDescriptor* rn = p_Desc.defineStringParam("rangeNote");
    rn->setLabels("Range", "Range", "Range");
    rn->setStringType(eStringTypeLabel);
    rn->setDefault("Set the latch to switch this on");
    rn->setHint("Whether Range Balance is doing anything. The stage is off entirely while the latch is 0, which is the default, so a fresh node renders exactly as it did before this feature existed.");
    rn->setEnabled(false);
    rn->setParent(*gRange);
    page->addChild(*rn);

    // ---- 5. Look / Film LUT ----
    scanLuts();
    GroupParamDescriptor* gLut = p_Desc.defineGroupParam("gLut");
    gLut->setLabels("5  Look / Film LUT", "5  Look / Film LUT", "5  Look / Film LUT");
    gLut->setOpen(true);
    defineBypass(p_Desc, page, "bypassLut",
                 "Mute the LUT at render without losing the selection. This also hands Output Encode back to you: a selected LUT normally pins the encode to the curve it was authored for, so a bypass that left the encode pinned would still be changing the picture. The 'In effect' line under Output Encode says so while this is on.", gLut);

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
    gTrim->setLabels("6  Trim (after LUT)", "6  Trim (after LUT)", "6  Trim (after LUT)");
    gTrim->setOpen(true);
    defineBypass(p_Desc, page, "bypassTrim",
                 "Mute this stage at render without losing its values. Exposure Trim, Contrast and Highlight Rolloff are held neutral; the sliders grey out but keep their numbers.", gTrim);
    {
        StringParamDescriptor* tip = p_Desc.defineStringParam("trimTip");
        tip->setLabels("Tip", "Tip", "Tip");
        tip->setStringType(eStringTypeLabel);
        tip->setDefault("Finishing touches. Most grades need nothing here.");
        tip->setHint("This group runs after the LUT, on the finished picture. It is for small final adjustments, not for grading: set exposure and contrast with the Lift/Gamma/Gain wheels in group 4, which work in the grade curve where they belong. If you find yourself pulling these a long way, the grade above wants changing instead.");
        tip->setEnabled(false);
        tip->setParent(*gTrim);
        page->addChild(*tip);
    }
    // "Exposure" here was read as a second, competing exposure control — two places to set
    // brightness, one of them after the LUT, which is a workflow trap rather than a feature
    // (user's call, 2026-08-03). Renamed to say it is a trim, and the SLIDER now spans only
    // +/-1 stop so it reads as the light touch it is meant to be.
    //
    // The HARD range deliberately stays at +/-3: setRange is a clamp the host applies to
    // saved values, so narrowing it would quietly rewrite existing grades — and the film
    // emulation presets legitimately sit at +0.55, bringing brightness back after a print
    // stock crushes it. Narrow what the slider shows, never what a project can hold.
    {
        DoubleParamDescriptor* pe = defineSlider(p_Desc, "postExp", "Exposure Trim",
            "A small brightness nudge on the finished picture, in stops - typically to bring level back after a film-emulation LUT has crushed it. This is NOT the exposure control: set exposure with Gain in group 4, which works in the grade curve. The slider spans +/-1 stop because that is the intended range; larger values can still be typed in and older grades keep whatever they were saved with.",
            0.0, -3.0, 3.0, 0.01, gTrim);
        pe->setDisplayRange(-1.0, 1.0);
        page->addChild(*pe);
    }
    page->addChild(*defineSlider(p_Desc, "postCon", "Contrast", "Post-LUT contrast trim about mid (0.5), applied after the LUT.", 1.0, 0.0, 2.0, 0.001, gTrim));

    // ---- 5. Output ----
    GroupParamDescriptor* gOut = p_Desc.defineGroupParam("gOutput");
    gOut->setLabels("7  Output", "7  Output", "7  Output");
    gOut->setOpen(true);

    // THE TONE MAP. Lives with Output because it is part of the display transform: the question it
    // answers is "does this clip fit in the delivery range", not "what should it look like".
    //
    // Measured over the training corpus at NEUTRAL parameters, 9 of 18 frames pushed data past 1.0
    // -- up to 47.8% of channels -- while the SOURCE was pinned essentially nowhere. The footage
    // had the range and the plugin threw it away.
    //
    // DEFAULT OFF, and that is a staging decision rather than a preference. Every Auto/Magic
    // constant was fitted against a render with no shoulder, and switching one on breaks six tests
    // structurally: the Clean solve predicts the render as lgg_core() on a measured percentile,
    // and a curve after lgg_core makes that identity false. Turning it on is a refit against the
    // hand-graded ground truth, not a flag flip.
    {
        BooleanParamDescriptor* tm = p_Desc.defineBooleanParam("toneMap");
        tm->setLabels("Highlight Tone Map", "Highlight Tone Map", "Highlight Tone Map");
        tm->setHint("Fit the picture into the 0-1023 delivery range instead of clipping whatever will not fit. Log footage carries far more range than a display encode holds, and without a shoulder everything above display white is simply lost - measured across the training footage, half the frames threw away highlight data the camera had actually captured. ON by default with a curve fitted to that footage, which contains every frame tested; press Fit From Frame to measure THIS shot instead, which is better still. Untick to render exactly as the plugin did before this existed.");
        // ON BY DEFAULT, STATICALLY. Measuring the frame on apply is the obvious better answer
        // and it is a documented crash: fetchImage() from a lifecycle hook trips an assertion
        // inside Resolve and calls abort(). See docs/ROADMAP.md 2. So the default is a fitted
        // constant rather than a measurement -- footage-blind, but it contained every frame in
        // the training corpus with 15 of 18 medians bit-identical, and Fit From Frame tunes it
        // per shot with one click.
        tm->setDefault(true);
        tm->setParent(*gOut);
        page->addChild(*tm);
    }
    page->addChild(*defineSlider(p_Desc, "toneMapKnee", "Tone Map: Start",
        "Where the shoulder begins, in display units. Everything below this is left exactly alone - the curve is flat-on at this point, so there is no seam - and everything above is compressed to make room for the highlights. Lower gives the highlights more range at the cost of compressing the upper mid-tones; higher leaves more of the picture untouched but flattens the top. 0.40 was fitted on the training corpus as the point where the median stops moving.",
        0.40, 0.05, 0.95, 0.001, gOut));
    page->addChild(*defineSlider(p_Desc, "toneMapWhite", "Tone Map: White Point",
        "The value that becomes display white. Everything between the shoulder start and this is mapped into the top of the range, so raising it packs more highlight range into the same space and lowering it clips sooner. 3.0 contained every frame in the training corpus with the median untouched; a frame peaking above this is held at white rather than allowed past it.",
        3.0, 1.0, 20.0, 0.01, gOut));

    {
        PushButtonParamDescriptor* tf = p_Desc.definePushButtonParam("toneMapFit");
        tf->setLabels("Fit From Frame", "Fit From Frame", "Fit From Frame");
        tf->setHint("Measure THIS frame and reshape the shoulder to it, instead of the fitted default that ships with the plugin. Worth pressing on any shot you care about: the default is one curve chosen against a corpus, and a measurement beats it on both ends - a shot that already fits gets no shoulder at all, and a shot three times over white gets the room it needs. A frame that only just exceeds white gets a high start and is barely touched; one that runs three times over gets the room it needs. IMPORTANT: anything already clipped at the SENSOR is excluded from the measurement - a pixel the camera lost is flat whatever we do, and making room for it would compress everything real to protect data that is not there. The status line reports how much of the frame that was, which is the number that tells you whether the shot was recoverable in the first place.");
        tf->setParent(*gOut);
        page->addChild(*tf);
    }
    {
        StringParamDescriptor* tn = p_Desc.defineStringParam("toneMapNote");
        tn->setLabels("Tone Map", "Tone Map", "Tone Map");
        tn->setStringType(eStringTypeLabel);
        tn->setDefault("Shoulder on - press Fit From Frame to tune it");
        tn->setHint("What the shoulder is doing on this frame. After Fit From Frame it reports the recoverable peak it measured and how much of the frame was already clipped at the sensor - the second number is the one that says whether a blown sky can be brought back at all.");
        tn->setEnabled(false);
        tn->setParent(*gOut);
        page->addChild(*tn);
    }

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

    // ---- Export LUT ----
    // Answers the archival objection: a project graded with OneGrade otherwise needs
    // OneGrade archived alongside it, and since this node is the whole pipeline, replacing
    // it later would mean starting over. Baking to a .cube removes the dependency entirely.
    // Only possible because nothing here is spatial — see exportCube().
    {
        GroupParamDescriptor* gExport = p_Desc.defineGroupParam("gExport");
        gExport->setLabels("Export LUT", "Export LUT", "Export LUT");
        gExport->setOpen(false);

        StringParamDescriptor* ep = p_Desc.defineStringParam("lutExportPath");
        ep->setLabels("File", "File", "File");
        ep->setStringType(eStringTypeFilePath);
        ep->setHint("Where to write the .cube. The extension is added if you leave it off.");
        ep->setDefault("");
        ep->setParent(*gExport);
        page->addChild(*ep);

        ChoiceParamDescriptor* es = p_Desc.defineChoiceParam("lutExportSize");
        es->setLabels("Size", "Size", "Size");
        es->setHint("Lattice resolution. 65 is the default here rather than the more usual 33, because this pipeline hard-clips out-of-gamut channels and a coarse lattice interpolates across that step badly on bright saturated colour - 65 roughly halves the error for a file that is still small. Drop to 33 if a tool you are handing it to expects that size. 17 is for quick checks only.");
        es->appendOption("17 (draft)");
        es->appendOption("33 (standard)");
        es->appendOption("65 (high)");
        es->setDefault(2);
        es->setParent(*gExport);
        page->addChild(*es);

        PushButtonParamDescriptor* eb = p_Desc.definePushButtonParam("lutExportBtn");
        eb->setLabels("Export .cube", "Export .cube", "Export .cube");
        eb->setHint("Bake this node - camera transform, balance, density, grade, output encode, any LUT, and the trim - into a single .cube, so a project can be archived or handed on without needing the plugin installed. Feed the exported LUT the same camera-log footage this node is fed, with nothing else in front of it. Accuracy: the bake matches the node through the normal tonal range (about 4/255 on the grey axis at 33, better at 65) but can differ on blown, saturated highlights, where the output encode clips a channel to zero and no lattice can follow that step exactly. It is an excellent stand-in, not a bit-exact one. Two other things a .cube cannot carry: source values above 1.0, and the sliders staying live - it is a snapshot, so re-export after changing the grade. Node Role and any Bypass are honoured, so what you export is what you see.");
        eb->setParent(*gExport);
        page->addChild(*eb);

        StringParamDescriptor* est = p_Desc.defineStringParam("lutExportStatus");
        est->setLabels("Status", "Status", "Status");
        est->setStringType(eStringTypeLabel);
        est->setDefault("");
        est->setHint("Result of the last export.");
        est->setEnabled(false);
        est->setParent(*gExport);
        page->addChild(*est);
    }

    // ---- 8. Setup / Help ----
    GroupParamDescriptor* gHelp = p_Desc.defineGroupParam("gHelp");
    gHelp->setLabels("8  Setup / Help", "8  Setup / Help", "8  Setup / Help");
    gHelp->setOpen(false);
    // The panel truncates these strings, so `text` must stay short enough to read at the
    // default OpenFX panel width (~45 chars) and carry the instruction on its own; the
    // full explanation goes in the hint, which the host shows on hover.
    auto helpLine = [&](const char* name, const char* label, const char* text,
                        const char* hint = nullptr) {  // NOLINT
        StringParamDescriptor* s = p_Desc.defineStringParam(name);
        s->setLabels(label, label, label);
        s->setStringType(eStringTypeLabel);
        s->setDefault(text);
        s->setHint(hint ? hint : text);
        s->setEnabled(false);
        s->setParent(*gHelp);
        page->addChild(*s);
    };

    // What this host actually supports, rather than what OFX allows. Right now that is one line,
    // because one capability is in question; it is the place to put the next one.
    helpLine("hostCaps", "Host",
             hostOverlays ? "On-screen shape handles: supported"
                          : "On-screen shape handles: not supported",
             hostOverlays
               ? "This host reports OFX overlay support, so Range Balance's Shape draws an outline and a draggable centre handle over the viewer."
               : "This host reports no OFX overlay support, so the Shape cannot draw an outline or a draggable handle on the viewer - position it with the Centre and Size sliders instead. The shape itself works exactly the same either way; only the on-screen widget is missing.");
    helpLine("help0", "Requires", "Project > Color Management, NOT color managed:");
    helpLine("help1", "Color Science", "DaVinci YRGB");
    helpLine("help2", "Timeline Color Space", "Rec.709 (Scene) - required on macOS",
             "Rec.709 (Scene). On macOS this is REQUIRED for Resolve's viewer to match QuickTime and YouTube: macOS reads a Rec.709 tag via the scene OETF, and this is the only timeline setting under which the viewer agrees. It is NOT tied to Output Encode - leave that on your delivery curve. Windows/Linux: unverified, start by matching Output Encode.");
    helpLine("help3", "Output Color Space", "Same as Timeline");
    helpLine("help4", "macOS Preference", "'Use Mac display color profiles' = ON",
             "Preferences > General > 'Use Mac display color profiles for viewers' ON. That enables its sub-option 'Viewers match QuickTime player when using Rec.709 Scene', which only engages on a Rec.709 Scene timeline - see Timeline Color Space above.");
    helpLine("help5", "Clips", "Camera raw/log defaults - no CST or LUT first",
             "Leave clips at their camera raw/log defaults - no input CST or LUT before this node. OneGrade does the camera transform itself.");
    helpLine("help6", "Camera control", "Default 'Smooth Decode' is a look, not a camera",
             "The default Camera entry, 'Rec.2100 PQ - Smooth Decode', is a deliberately compressive curve that flatters log footage - the look the presets build on, not a colorimetric match. Every other entry in the list IS a faithful camera decode; pick yours for an accurate transform instead.");
    helpLine("help7", "Output Encode", "Delivery curve - NOT the Timeline setting",
             "Your DELIVERY curve, baked into the render. Independent of Timeline Color Space - do NOT change it to match. Rec.709 (Gamma 2.2) is the default (web/YouTube); Gamma 2.4 for broadcast; Rec.709 (Scene) for a scene-referred hand-off.");
    // Live check of the one setup mistake that IS detectable. Sits with the static setup
    // advice because that is where a user goes when the picture looks wrong. See
    // probeSetup(): it reads the input pixels, not the timeline — Timeline Color Space is a
    // monitoring setting downstream of this node and cannot be read from an OFX plugin.
    {
        PushButtonParamDescriptor* sb = p_Desc.definePushButtonParam("setupCheck");
        sb->setLabels("Check Input", "Check Input", "Check Input");
        sb->setHint("Look at the frame and say whether this node is being fed camera log, which is what it expects. Catches the mistakes that silently ruin a grade: a color-managed timeline, a Color Space Transform node in front of this one, or an input LUT on the clip - all of which hand OneGrade something already transformed. It cannot read your Timeline Color Space (that is a monitoring setting applied after this node, invisible from inside a plugin) and it does not need to: every one of those mistakes changes the input, and the input is measurable. Deliberately cautious - it only calls a verdict when the frame is clearly one thing or the other, and always shows the numbers it judged on. Changes nothing about the picture.");
        sb->setParent(*gHelp);
        page->addChild(*sb);

        auto srow = [&](const char* name, const char* label, const char* hint) {
            StringParamDescriptor* s = p_Desc.defineStringParam(name);
            s->setLabels(label, label, label);
            s->setStringType(eStringTypeLabel);
            s->setDefault("");
            s->setHint(hint);
            s->setEnabled(false);
            s->setParent(*gHelp);
            page->addChild(*s);
        };
        srow("setupStatus", "Input", "The verdict. 'OK' means the frame has the lifted floor and rolled-off top that camera log has. 'WARNING' means it uses the full 0-1 range the way display-referred material does, which usually means something transformed it before this node. 'Inconclusive' means it sits between the two and you should read the numbers yourself.");
        srow("setupStats",  "Levels", "1st, 50th and 99th percentile of the incoming code values. Camera log sits well inside 0-1 - Blackmagic log peaks around 0.75 on real footage. A p1 near 0.00 together with a p99 near 1.00 is the signature of an already-transformed image.");
        srow("setupHost",   "Host", "What Resolve reports through the OFX 1.5 colour management API. '(absent)' means the host does not provide it, which is expected and harmless - the pixel check above is the one that matters. Read-only: OneGrade does not declare a colour management style, because doing so is what would let the host start converting the input and override the plugin's own camera transform.");
    }

    helpLine("help8", "Monitor", "Calibrate; check on a second screen",
             "Calibrate your monitor and have Resolve show your delivery space; check the grade on a second screen before committing.");

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
