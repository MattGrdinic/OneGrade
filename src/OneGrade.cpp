// OneGrade — cross-platform OpenFX color grade plugin for DaVinci Resolve.
// Copyright (C) 2026 Matthew Grdinic
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OneGrade.h"
#include "ofxColour.h"   // OFX 1.5 colour management properties (read-only probe)
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
#define kPluginVersionMinor 3

#define kSupportsTiles              false
#define kSupportsMultiResolution    false
#define kSupportsMultipleClipPARs   false

// Master switch for the Auto Grade analysis UI. While false the whole debug surface is
// hidden — the "Show analysis" checkbox, the Analyze Frame button, the six measurement rows
// and the Applied readout — leaving just Auto Grade and Bias, which is all a colorist needs.
// The params still exist and still work; only their visibility is off, so nothing about
// saved projects or the measurement itself depends on this.
//
// FUTURE WORK: flip this to true and rebuild to get the debug panel back. The checkbox
// reappears and toggles the rest at runtime, which is the mode to be in when fitting new
// constants or working out why a shot analysed oddly — the numbers are how every one of the
// current fits was found. See docs/AUTO-GRADE.md.
static const bool kAnalysisDebugUI = true;    // ON: fitting the Clean auto-grade constants

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
                                 float& ro, float& go, float& bo)
{
    og::process(camera, encode, P, ri, gi, bi, ro, go, bo);
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
    for (int y = p_ProcWindow.y1; y < p_ProcWindow.y2; ++y)
    {
        if (_effect.abort()) break;
        float* dstPix = static_cast<float*>(_dstImg->getPixelAddress(p_ProcWindow.x1, y));
        for (int x = p_ProcWindow.x1; x < p_ProcWindow.x2; ++x)
        {
            float* srcPix = static_cast<float*>(_srcImg ? _srcImg->getPixelAddress(x, y) : nullptr);
            if (srcPix)
            {
                og_full_chain(_camera, _encode, _params, _lut, _lutSize, _lutMix,
                              srcPix[0], srcPix[1], srcPix[2],
                              dstPix[0], dstPix[1], dstPix[2]);
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
    void probeAnalyze(double p_Time);   // measure the frame and report (writes m_LastKey)
    void probeSetup(double p_Time);     // is the input actually camera log? + what the host says
    void applyAutoGrade(double p_Time);      // measure, then set the film look + Gain from key
    void applyAutoGradeClean(double p_Time); // measure, then contain the range with no LUT
    void applyBias();                   // re-derive Rolloff/Lift from the cached measurement
    double m_LastKey = 0.0;             // scene key in stops from the last successful analyse
    double m_LastPin = 0.0;             // % of frame clipped at the source ceiling
    double m_LastGain = 0.80;           // Gain the measurement asked for (bias moves off this)
    double m_LastHot = 0.0;             // % of frame above display white — headroom for brightening
    // Display-space percentiles from the last analyse, at NEUTRAL params. Cached because the
    // Clean auto-grade solves against them directly: the grade curve is monotonic, so it maps
    // percentiles exactly, and three numbers stand in for the whole frame (see solveClean()).
    double m_LastD01 = 0.0;   // p0.1 — the BLACK POINT the solve places (not p1, see below)
    double m_LastD1  = 0.0;
    double m_LastD50 = 0.0;
    double m_LastD99 = 0.0;
    bool   m_HaveKey = false;
    bool   m_AutoApplied = false;       // has Auto Grade run? gates the live Bias drag
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
    OFX::DoubleParam* m_PostExp;
    OFX::DoubleParam* m_PostCon;
    OFX::DoubleParam* m_Rolloff;

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
    OFX::BooleanParam* m_BypLut;
    OFX::BooleanParam* m_BypTrim;

    // Auto Grade probe (experimental) — see probeAnalyze().
    OFX::StringParam* m_ProbeStatus;
    OFX::StringParam* m_ProbeScene;
    OFX::StringParam* m_ProbeDisplay;
    OFX::StringParam* m_ProbeShape;
    OFX::StringParam* m_ProbeSubject;
    OFX::DoubleParam* m_AutoBias;
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
    OFX::StringParam* m_ProbePeak;
    OFX::StringParam* m_ProbeApplied;

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
    m_BypLut      = fetchBooleanParam("bypassLut");
    m_BypTrim     = fetchBooleanParam("bypassTrim");
    m_ProbeStatus  = fetchStringParam("probeStatus");
    m_ProbeScene   = fetchStringParam("probeScene");
    m_ProbeDisplay = fetchStringParam("probeDisplay");
    m_ProbeShape   = fetchStringParam("probeShape");
    m_ProbeSubject = fetchStringParam("probeSubject");
    m_AutoBias     = fetchDoubleParam("autoBias");
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
    m_ProbePeak    = fetchStringParam("probePeak");
    m_ProbeApplied = fetchStringParam("probeApplied");
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
        // Per-CHANNEL display values, kept separately from luma. The Base solve places its
        // percentiles on these, because a channel is what clips: a waveform shows R, G and B
        // independently, and on a saturated highlight they spread far apart. Containing luma
        // at 0.95 put blue over 1023 on a real interview frame while the luma number said the
        // target had been hit exactly. Measuring a summary statistic instead of the quantity
        // that actually fails is the same mistake as measuring in the wrong encode.
        std::vector<float> dispC;
        double skinR = 0.0, skinG = 0.0, skinB = 0.0;   // skin chromaticity, for a warmth read
        sceneY.reserve(220000); dispL.reserve(220000); dispC.reserve(660000);
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
// with Scene Exposure already at -0.50, so part of its correction happened upstream of Gain.)
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
    m_LastGain = gain;   // applyBias() writes it, so a Bias drag stays anchored to this
    m_AutoApplied = true;
    applyBias();                       // sets Rolloff + Lift, and writes the Applied line
    setEnabledness();                  // the preset switches LUT Mode
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

    double tHigh = 0.95, tLow = 0.02, tMid = 0.42, midStr = 0.5, maxGain = 1.0, maxExp = 0.85;
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
    double shoulder = 0.476;
    m_CleanShoulder->getValue(shoulder);
    const double rolloff = std::min(0.80, std::max(0.00,
        std::max(0.090 * m_LastPin, shoulder * std::max(0.0, m_LastD99 - 1.0))));

    // The whole chain in closed form. With no LUT and Contrast at 1.0, trim is just a multiply
    // by 2^postExp, so the rendered value is exactly:
    //     softclip( lgg_core(d, lift, gamma, gain) * 2^postExp , rolloff )
    // Every stage Base touches is in there, which is why no iteration is needed: test 14
    // proves lgg_core predicts the render, and the two stages after it are this simple.
    const double d01 = m_LastD01, d50 = m_LastD50, d99 = m_LastD99;
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

    // Bias stays anchored to this gain. m_AutoApplied stays FALSE on purpose: applyBias()
    // re-derives Lift and Gamma from the FILM recipe's constants, which would undo the
    // containment the moment the slider moved.
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

// Bias: one slider trading highlight restraint against shadow openness, because the
// measurement can only get a shot into the right neighbourhood — which end of that
// neighbourhood you want is taste, and taste needs a knob rather than a constant.
// Negative tames the top (more rolloff, shadows sit down); positive opens the bottom
// (lift up, rolloff backed off). Zero is the fitted result.
//
// It moves Rolloff and Lift specifically because those are the two the user reached for in
// exactly this situation: "add a touch of highlight rolloff until we bring the highlights
// below 1023", and "lift darker images a bit". Gain deliberately stays on its measurement —
// it's the one parameter with a hard physical anchor (distance from mid-gray), and letting
// a taste control drag it would undo the part that works.
//
// Split out of applyAutoGrade so a Bias drag can re-derive both values from the CACHED
// measurement, with no re-analysis: it's pure arithmetic on two stored numbers, so it keeps
// up with a drag. Gated on m_AutoApplied — dragging Bias on a node that was never
// auto-graded must not silently stamp Lift and Rolloff. That flag and the cached
// measurement are instance state, so after a project reload the slider goes inert until
// Auto Grade is pressed again; deliberately inert rather than acting on a stale number.
void OneGrade::applyBias()
{
    if (!m_AutoApplied) return;
    double bias = 0.0; m_AutoBias->getValue(bias);

    // One slider, the whole tonal range. Bias moves all four together so the result stays a
    // coherent picture rather than a lifted floor on an unchanged image — the first version
    // drove Lift and Rolloff only, and since Rolloff clamps at 0 for positive bias, opening
    // a shot up visibly did nothing but raise the floor.
    //   negative -> protect: shoulder the top, deepen the floor, darken mids, pull gain
    //   positive -> open:    drop the shoulder, raise the floor, brighten mids and gain
    //
    // Gain's response is the one that's measurement-modulated. Brightening a frame that
    // already has a third of itself above display white just pushes more of it past clipping,
    // so the positive direction is scaled by remaining headroom and fades to nothing by ~40%
    // hot. The negative direction is never scaled: pulling gain down is always safe.
    const double headroom = std::max(0.0, 1.0 - m_LastHot / 40.0);
    const double gainDelta = (bias >= 0.0) ? bias * 0.08 * headroom : bias * 0.08;

    const double rolloff = std::min(0.80, std::max(0.00, 0.090 * m_LastPin - bias * 0.35));
    const double lift    = std::min(0.50, std::max(-0.50, 0.11 + bias * 0.06));
    const double gamma   = std::min(3.00, std::max(0.20, 1.00 + bias * 0.12));
    const double gain    = std::min(1.20, std::max(0.20, m_LastGain + gainDelta));
    m_Rolloff->setValue(rolloff);
    m_Lift->setValue(lift);
    m_Gamma->setValue(gamma);
    m_Gain->setValue(gain);

    char msg[128];
    if (bias != 0.0)
        snprintf(msg, sizeof msg, "G %.3f Gam %.3f L %.3f R %.3f  bias %+.2f",
                 gain, gamma, lift, rolloff, bias);
    else
        snprintf(msg, sizeof msg, "Gain %.3f (key %+.2f)  Roll %.3f (pin %.1f%%)",
                 gain, m_LastKey, rolloff, m_LastPin);
    m_ProbeApplied->setValue(msg);
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
    m_BypBalance->getValue(bypBal);
    m_BypDensity->getValue(bypDen);
    m_BypExposure->getValue(bypExp);
    m_BypLut->getValue(bypLut);
    m_BypTrim->getValue(bypTrim);

    m_BypBalance->setEnabled(look);
    m_BypDensity->setEnabled(look);
    m_BypExposure->setEnabled(look);
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
    else if (p_ParamName == "probeApply" && p_Args.reason == OFX::eChangeUserEdit) {
        applyAutoGrade(p_Args.time);
    }
    // Live: re-derive Rolloff/Lift as the slider moves. No re-analysis, so it keeps up.
    else if (p_ParamName == "autoBias" && p_Args.reason == OFX::eChangeUserEdit) {
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
    m_BypBalance->getValueAtTime(p_Time, bypBal);
    m_BypDensity->getValueAtTime(p_Time, bypDen);
    m_BypExposure->getValueAtTime(p_Time, bypExp);
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

    // Force the params the role doesn't own to neutral, so the two nodes chain cleanly:
    // the look must be applied once (on the output node), the scene exp/WB stage once (input).
    if (role == 1) {            // Input Transform: no look at all
        params[0]=0.f; params[1]=0.f; params[2]=0.f;              // temp, tint, density
        params[3]=0.f; params[4]=1.f; params[5]=1.f;              // lift, gamma, gain
        params[6]=0.f; params[7]=0.f;                             // offset temp/tint
        params[8]=0.f; params[9]=1.f; params[12]=0.f;             // trim exp/contrast/rolloff
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
    ClipDescriptor* srcClip = p_Desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    ClipDescriptor* dstClip = p_Desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->setSupportsTiles(kSupportsTiles);

    PageParamDescriptor* page = p_Desc.definePageParam("Controls");

    // ---- Auto Grade (experimental) ----
    // First in the panel, at the user's request: it's the one-click entry point, so it
    // shouldn't be buried under nine groups of manual controls. Deliberately UNNUMBERED
    // while it's experimental — the 0-8 sequence below is the pipeline in the order it's
    // applied, and this isn't a pipeline stage, it's a way of setting those stages. It also
    // means the numbering users and the docs already know doesn't shift for a feature that
    // may still change shape. Number it 0 and renumber the rest if it graduates.
    GroupParamDescriptor* gAuto = p_Desc.defineGroupParam("gAuto");
    gAuto->setLabels("Auto Grade (experimental)", "Auto Grade", "Auto Grade");
    gAuto->setOpen(true);
    {
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
        tune("cleanHigh", "Target High", "Where the 99th percentile should land after grading, measured AFTER Highlight Rolloff. 0.95 fills the range without sitting on the clip point.", 0.95, 0.50, 1.00);
        tune("cleanLow",  "Target Low",  "Where the 0.1st percentile - effectively the darkest part of the picture - should land. Just off zero, so shadows sit above black rather than crushing into it. This is deliberately a much deeper percentile than the highlight end uses: placing p1 here left 1% of the frame below the target and that 1% was visibly crushed.", 0.02, 0.00, 0.30);
        tune("cleanMid",  "Target Mid",  "Where the median should land if the midtone solve were applied in full. Only a fraction of it is - see Mid Strength.", 0.42, 0.10, 0.90);
        tune("cleanMaxGain","Max Gain","Ceiling on the Gain the solve may use. 1.0 means it can only ever darken, which is deliberate: a shot whose highlights sit below the target is not clipping, it is just dark, and brightening it destroys the intent. Raise above 1.0 only if you want genuinely underexposed footage pushed up.", 1.00, 0.50, 2.00);
        tune("cleanMaxExp","Max Exposure","The most Base may BRIGHTEN a shot, in stops. Darkening is never limited - pulling a blown frame down is always safe - but pushing a dark one up destroys a deliberately low-key image, and the solver cannot tell the difference: it only knows whether it reached the target. A dark car interior asked for +1.74 stops without this; the same shot graded by hand used +0.55.", 0.85, 0.00, 2.00);
        tune("cleanShoulder","Shoulder","How much Highlight Rolloff to apply per unit of highlight overshoot - how far the channels run past display white before grading. This is the shoulder that stands in for a film stock's, since Lift/Gamma/Gain cannot make an S-curve on its own. Source clipping (pin) sets a floor underneath it. 0 disables the overshoot term and leaves rolloff on source clipping alone, which is what Creative uses.", 0.476, 0.00, 1.50);
        tune("cleanMidStr","Mid Strength","How much of the midtone solve to apply. 0 leaves Gamma at 1.0 and only the two ends are corrected; 1.0 drives every shot's median to Target Mid, which flattens deliberately dark shots into mid-gray. The default is halfway: containment at the ends is objective, the midtone is intent.", 0.50, 0.00, 1.00);

        apply->setLabels("Creative Grade", "Creative Grade", "Creative Grade");
        apply->setHint("Analyses the frame and applies the Cinematic Film Emulation look on top - use this when you want a finished-looking image straight away rather than something to grade from. Sets Gain from the measured key. Fitted to hand-graded shots rather than to a textbook target: a bright shot gets Gain pulled down, a dark one is left at the preset - deliberately, since a low-key shot is meant to sit low. Everything it writes is an ordinary slider value you can drag afterwards.");
        apply->setParent(*gAuto);
        page->addChild(*apply);

        page->addChild(*defineSlider(p_Desc, "autoBias", "Bias",
            "Which way Auto Grade leans when you press it. 0 uses the measured result as-is. Negative protects the picture - shoulders the highlights, deepens the floor, darkens the mids and pulls Gain down - for a shot with blown windows or hot speculars. Positive opens it up - drops the shoulder, raises the floor, brightens the mids and Gain. It moves Lift, Gamma, Gain and Highlight Rolloff together so the whole tonal range stays coherent. The brightening half is limited by how much of the frame is already above white, so it will not push a blown shot further into clipping. Updates live once Auto Grade has been pressed - drag it and the image follows. It does nothing on a node that has not been auto-graded, and goes inert after a project reload until you press Auto Grade again.",
            0.0, -1.0, 1.0, 0.01, gAuto));

        probeLine("probePeak", "Peak",
                  "p99.9 in display, and how far it runs past p99. A compact blown specular - a window, a lamp - sits far above the bulk of the highlights and gives a high multiplier; a broad bright field like sunlit sand sits just above it. This is the shape of the top end rather than its size, which is what decides whether a shot wants Highlight Rolloff.");
        probeLine("probeSubject", "Subject",
                  "The same exposure question asked of skin-toned pixels only, plus what share of the frame matched. Frame-median exposure is subject-blind: a dark interior drags the median down and asks for a push that would blow the windows. Where the two keys disagree, the frame median is the wrong one. Note the mask cannot tell skin from sand - a high coverage % on a landscape means it matched the scene, not a face.");
        probeLine("probeApplied", "Applied",
                  "What the Auto Grade button last wrote, and the measurement it came from. Blank until you press it. Analyze Frame never changes anything; only Auto Grade does.");
    }

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
    preset->setHint("One-click starting points on the happy path: every preset sets Camera to 'Rec.2100 PQ - Smooth Decode' (also the default) plus Balance, Density, Lift/Gamma/Gain, LUT and Trim — every slider stays live to tweak per clip; Scene Exposure, Scene White Balance and Output Encode are never touched. Film Emulation presets drive Resolve's print-film stocks (swap in Film Look LUT); Custom LUT presets drive OneGrade's built-in looks, shipped inside the plugin (swap in Look LUT; six looks available). Trim any LUT with LUT Mix. None / Reset Look returns the look params to neutral (Camera stays put).");
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
    page->addChild(*defineSlider(p_Desc, "rawExp", "Scene Exposure", "Exposure in stops applied to scene light immediately after the camera decode, before the gamut transform - a linear gain on the scene, which is mechanically the same operation the Camera RAW tab's Exposure performs. Called 'Scene' rather than 'RAW' because this acts on the decoded image, not on the raw file: no sensor data reaches an OpenFX plugin.", 0.0, -5.0, 5.0, 0.01, gInput));
    page->addChild(*defineSlider(p_Desc, "rawTemp", "Scene White Balance", "White-balance color temperature in Kelvin, applied as a Bradford chromatic adaptation in XYZ right after the camera decode - the closest point in the chain to the sensor. Raise = warmer, lower = cooler; 6500 = neutral. This is a physically real white balance, but NOT the Camera RAW tab's: reproducing a raw decoder's WB needs sensor metadata, which an OpenFX plugin never receives.", 6500.0, 2000.0, 15000.0, 10.0, gInput));

    // ---- 2. Balance ----  (white balance in linear; watch the vectorscope while adjusting)
    GroupParamDescriptor* gBal = p_Desc.defineGroupParam("gBalance");
    gBal->setLabels("2  Balance", "2  Balance", "2  Balance");
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

    // ---- 3. Density ----  (HSV saturation gain — the green-of-Gain-in-HSV trick)
    GroupParamDescriptor* gDen = p_Desc.defineGroupParam("gDensity");
    gDen->setLabels("3  Density", "3  Density", "3  Density");
    defineBypass(p_Desc, page, "bypassDensity",
                 "Mute this stage at render without losing its value. Density is held at 0 (no saturation change); the slider greys out but keeps its number.", gDen);
    page->addChild(*defineSlider(p_Desc, "density", "Density", "Color density: saturation gain in HSV (the green-channel-of-Gain-in-HSV trick). -1 = grayscale, +1 = double saturation.", 0.0, -1.0, 1.0, 0.001, gDen));

    // ---- 4. Exposure (Lift / Gamma / Gain) ----
    GroupParamDescriptor* gExp = p_Desc.defineGroupParam("gExposure");
    gExp->setLabels("4  Exposure (Lift / Gamma / Gain)", "4  Exposure", "4  Exposure");
    defineBypass(p_Desc, page, "bypassExposure",
                 "Mute this stage at render without losing its values. Lift/Gamma/Gain are held neutral (0/1/1); the sliders grey out but keep their numbers. Note Auto Grade drives Gain, so bypassing this also mutes the auto exposure.", gExp);
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
    gTrim->setLabels("7  Trim (after LUT)", "7  Trim (after LUT)", "7  Trim (after LUT)");
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
    page->addChild(*defineSlider(p_Desc, "rolloff", "Highlight Rolloff", "Soft-clips bright highlights per channel so lamps/speculars roll off to white instead of clipping to a flat neon patch. Higher = earlier, stronger shoulder. Only active on display-referred output (Rec.709 encodes or any LUT path).", 0.0, 0.0, 1.0, 0.001, gTrim));

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
