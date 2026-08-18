// OneGrade — offline Magic Grade bench.
//
// Log stills in, graded stills and a parameter table out, every constant overridable from the
// command line. Exists so a constant can be tuned in seconds instead of a build-install-restart-
// press cycle in Resolve, which is the rate every number in this feature has been fitted at so
// far -- roughly one observation per minute, which is why so few of them have any evidence.
//
// IT CALLS THE PLUGIN'S OWN CODE. The grade solve is in src/OneGradeCreative.h, the render is
// og_full_chain's arithmetic, the decision is og::analysis::magic_decide, the segmentation is
// og::seg::Segmenter. Nothing here reimplements anything, because on this feature every bug that
// survived more than a few minutes was a paraphrase: a neutral render standing in for a graded
// one, a pre-LUT render standing in for the real one, a threshold verified in Python and never
// carried across. All three produced plausible output while being wrong, and all three were
// found only when two implementations were finally compared on the same frame.
//
// INPUT IS CAMERA LOG, NOT A GRADED EXPORT. 8- or 16-bit PNG; 16 is much better, since log in
// 8 bits throws away most of what the shadows are for. This is what the plugin actually
// receives from OFX, so a still exported before any grading is the right thing to feed it.
//
// USAGE
//   bench MODEL_DIR OUT_DIR FRAME.png [...]  [--key=..] [--gain-per-key=..] [--black=..]
//                                            [--unit=..] [--wb] [--camera=N] [--encode=N]
#include <cmath>
#include <cstring>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "OneGradePipeline.h"
#include "OneGradeAnalysis.h"
#include "OneGradeCreative.h"
#include "OneGradeSegment.h"
#include "CubeLUT.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace oga = og::analysis;

// The complete render, mirroring og_full_chain() in OneGrade.cpp. The one thing here that IS
// duplicated, because that function lives inside the plugin translation unit -- if it ever moves
// to a header, this should call it instead.
static inline void full_chain(int cam, int enc, const float* P,
                              const float* lut, int lutSize, float lutMix,
                              float ri, float gi, float bi, float& ro, float& go, float& bo,
                              float shapeM = 1.f)
{
    og::process(cam, enc, P, ri, gi, bi, ro, go, bo, shapeM);
    if (P[18] > 0.5f && P[13] > 0.f) return;   // matte: no LUT, no trim (mirrors og_full_chain)
    const bool lutOn = (lut && lutSize >= 2 && lutMix > 0.f);
    if (lutOn) og::apply_lut(lut, lutSize, lutMix, ro, go, bo);
    og::apply_trim(P[8], P[9], ro, go, bo);
    if (P[12] > 0.f && (enc <= 2 || lutOn)) {
        ro = og::softclip(ro, P[12]); go = og::softclip(go, P[12]); bo = og::softclip(bo, P[12]);
    }
}

struct Frame {
    int w = 0, h = 0;
    int bits = 8;               // of the FILE, which decides whether the shadows survived export
    std::vector<float> px;      // w*h*3, camera log, 0..1
};

static bool load_log(const char* path, Frame& f)
{
    int c = 0;
    f.bits = stbi_is_16_bit(path) ? 16 : 8;
    if (stbi_is_16_bit(path)) {
        unsigned short* d = stbi_load_16(path, &f.w, &f.h, &c, 3);
        if (!d) return false;
        f.px.resize((size_t)f.w * f.h * 3);
        for (size_t i = 0; i < f.px.size(); ++i) f.px[i] = d[i] / 65535.f;
        stbi_image_free(d);
    } else {
        unsigned char* d = stbi_load(path, &f.w, &f.h, &c, 3);
        if (!d) return false;
        f.px.resize((size_t)f.w * f.h * 3);
        for (size_t i = 0; i < f.px.size(); ++i) f.px[i] = d[i] / 255.f;
        stbi_image_free(d);
    }
    return true;
}

// probeAnalyze()'s measurements, on the same coarse grid and in the same order.
static og::grade::Measurements measure(const Frame& f, int cam, int enc, oga::SampleSet& S)
{
    og::grade::Measurements m;
    const int step = std::max(1, (int)(std::sqrt((double)(f.w * f.h) / 200000.0) + 0.5));
    float N[oga::kParamN]; oga::neutral_params(N);

    // TWO ENCODES, mirroring probeAnalyze(). `hot` is a threshold chosen in display space, so it
    // needs a display-referred encode; d01/d99 get pushed through og_lgg by the solve, so they
    // need the space the grade curve actually runs in. Passing one encode to both is what made
    // this bench and the plugin disagree by 0.06 of Lift on the same frame.
    const int dispEnc = (enc <= 2) ? enc : 1;

    std::vector<float> lum, chn, srcTop;
    std::vector<float> sceneY;
    for (int y = 0; y < f.h; y += step) {
        for (int x = 0; x < f.w; x += step) {
            const float* p = &f.px[((size_t)y * f.w + x) * 3];
            srcTop.push_back(std::max(p[0], std::max(p[1], p[2])));

            float lin[3] = { og::decode_log(cam, p[0]), og::decode_log(cam, p[1]), og::decode_log(cam, p[2]) };
            float xyz[3]; og::to_XYZ(cam, lin, xyz);
            sceneY.push_back(xyz[1]);

            float r, g, b;
            og::process(cam, dispEnc, N, p[0], p[1], p[2], r, g, b);
            lum.push_back(0.2126f*r + 0.7152f*g + 0.0722f*b);
            if (enc != dispEnc) og::process(cam, enc, N, p[0], p[1], p[2], r, g, b);
            chn.push_back(r); chn.push_back(g); chn.push_back(b);

            S.rgb.push_back(p[0]); S.rgb.push_back(p[1]); S.rgb.push_back(p[2]);
            S.band.push_back((uint8_t)std::min(2, (int)((long long)(f.h - 1 - y) * 3 / f.h)));
            S.u.push_back((float)x / (float)f.w);
            S.v.push_back(1.f - (float)y / (float)f.h);
        }
    }
    if (lum.empty()) return m;

    auto pct = [](std::vector<float>& v, double q) {
        size_t k = (size_t)(q * (v.size() - 1));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return (double)v[k];
    };
    m.d01 = pct(chn, 0.001); m.d99 = pct(chn, 0.99);
    m.d50 = pct(lum, 0.50);
    const double y50 = pct(sceneY, 0.50);
    m.key = (y50 > 1e-6) ? std::log2(0.18 / y50) : 0.0;

    // pin: a pile-up at the clip's OWN ceiling, not at 1.0 -- log formats do not all reach the
    // top of the code range and Blackmagic peaks near 0.75.
    float mx = 0.f; for (float v : srcTop) mx = std::max(mx, v);
    const float eps = std::max(0.002f, mx * 0.004f);
    long long pinned = 0, hot = 0;
    for (float v : srcTop) if (v >= mx - eps) ++pinned;
    for (float v : lum)    if (v > 1.f) ++hot;
    m.pin = 100.0 * (double)pinned / (double)srcTop.size();
    m.hot = 100.0 * (double)hot / (double)lum.size();
    m.valid = true;
    return m;
}

static double argd(int argc, char** argv, const char* key, double def)
{
    const size_t n = strlen(key);
    for (int i = 1; i < argc; ++i)
        if (!strncmp(argv[i], key, n) && argv[i][n] == '=') return atof(argv[i] + n + 1);
    return def;
}
static bool argf(int argc, char** argv, const char* key)
{
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], key)) return true;
    return false;
}

int main(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: bench MODEL_DIR OUT_DIR FRAME.png [...] [--opts]\n"); return 2; }
    const char* modelDir = argv[1];
    const std::string outDir = argv[2];

    og::grade::Tunables tun;
    tun.gainBase   = argd(argc, argv, "--gain-base",    tun.gainBase);
    tun.gainPerKey = argd(argc, argv, "--gain-per-key", tun.gainPerKey);
    tun.gainMin    = argd(argc, argv, "--gain-min",     tun.gainMin);
    tun.gainMax    = argd(argc, argv, "--gain-max",     tun.gainMax);
    tun.blackTarget= argd(argc, argv, "--black",        tun.blackTarget);
    tun.subjFloor    = argd(argc, argv, "--subj-floor",    tun.subjFloor);
    tun.subjMid      = argd(argc, argv, "--subj-mid",      tun.subjMid);
    tun.rawExpMax    = argd(argc, argv, "--raw-exp-max",   tun.rawExpMax);
    tun.subjNeutralMid = argd(argc, argv, "--subj-neutral-mid", tun.subjNeutralMid);
    // The SKIN credibility ceiling, so the guard can be walked rather than argued about. It is a
    // proxy for an infeasible mask and the only way to see what it is refusing is to lift it.
    tun.region[og::analysis::R_SKIN].maxCover =
        argd(argc, argv, "--skin-max-cover", tun.region[og::analysis::R_SKIN].maxCover);
    tun.frameCeiling = argd(argc, argv, "--frame-ceiling", tun.frameCeiling);
    tun.frameCeilingLow = argd(argc, argv, "--frame-ceiling-low", tun.frameCeilingLow);
    tun.frameFloorMin= argd(argc, argv, "--frame-floor-min", tun.frameFloorMin);
    tun.frameFloorMax= argd(argc, argv, "--frame-floor-max", tun.frameFloorMax);
    tun.magicUnit  = argd(argc, argv, "--unit",         tun.magicUnit);
    const int   cam = (int)argd(argc, argv, "--camera", og::grade::kCreativeCamera);
    const int   enc = (int)argd(argc, argv, "--encode", og::grade::kCreativeEncode);
    const double sep = argd(argc, argv, "--sep", 1.0);
    const bool  wb  = argf(argc, argv, "--wb");
    const bool  noTone = argf(argc, argv, "--no-tone");
    // Which press. Magic Grade offers a different subject each time it is pressed, and until
    // now the bench could only ever see press one -- so a grade the user reached on press two
    // could not be reproduced here at all.
    const int   cycle = (int)argd(argc, argv, "--cycle", 0);
    // BIAS, swept. The slider re-solves the tone targets rather than nudging sliders, and that
    // re-solve is shared code -- so the bench can walk it and show where it stops converging,
    // which is the only way to see a discontinuity without dragging a slider in Resolve.
    // Both separation triples per frame -- the band stand-in and the region version beside it.
    const bool  sepReport = argf(argc, argv, "--sep-report");
    // The rdL*/rdb* rows of the descriptor Jacobian, to choose the slider's controls by measurement.
    const bool  sepJac    = argf(argc, argv, "--sep-jac");
    // HIGHLIGHT MASK PROTOTYPE. Numbers are on Resolve's 0..100 qualifier scale so a colourist's
    // own settings can be typed straight in and the two compared on one frame.
    const bool   hlOn    = argf(argc, argv, "--hl");
    const double hlLow   = argd(argc, argv, "--hl-low",   -1.0);   // -1 = derive it from the frame
    const double hlSoft  = argd(argc, argv, "--hl-soft",    2.6);
    const double hlLift  = argd(argc, argv, "--hl-lift",    0.0);
    const double hlGamma = argd(argc, argv, "--hl-gamma",   1.0);
    const double hlGain  = argd(argc, argv, "--hl-gain",    1.0);
    const double hlHiGain= argd(argc, argv, "--hl-hi-gain", 1.0);   // pull the HELD area down
    const double hlHiGamma=argd(argc, argv, "--hl-hi-gamma",1.0);   // ...and put its detail back
    const bool   hlShow  = argf(argc, argv, "--hl-show");           // write the mask itself
    // Lock the mask against the grade: the reference grade the mask reads is captured BEFORE the
    // exposure move under test, so the selection stops following it. Pass the grade to lock at.
    // The SHAPE: where Range Balance may act. Centre-origin, half-height units on both axes.
    const double shType  = argd(argc, argv, "--shape",    0.0);   // 0 off, 1 ellipse, 2 rect
    const double shX     = argd(argc, argv, "--shape-x",  0.0);
    const double shY     = argd(argc, argv, "--shape-y",  0.0);
    const double shW     = argd(argc, argv, "--shape-w",  0.5);
    const double shH     = argd(argc, argv, "--shape-h",  0.5);
    const double shR     = argd(argc, argv, "--shape-rot",0.0);
    const double shS     = argd(argc, argv, "--shape-soft",0.25);
    const bool   shInv   = argf(argc, argv, "--shape-invert");
    const bool   hlLock  = argf(argc, argv, "--hl-lock");
    const double hlLockL = argd(argc, argv, "--hl-lock-lift",  0.0);
    const double hlLockG = argd(argc, argv, "--hl-lock-gamma", 1.0);
    const double hlLockN = argd(argc, argv, "--hl-lock-gain",  1.0);
    // The Tone Separation slider: walk it, and vary how far one unit moves the subject's midtone.
    const bool  toneSepSweep = argf(argc, argv, "--tone-sep-sweep");
    const double toneSepPer  = argd(argc, argv, "--tone-sep-per", og::grade::kToneSepMidPer);
    const bool  biasSweep = argf(argc, argv, "--bias-sweep");
    // How often the sweep writes a frame. 0 prints the table and writes nothing.
    const double biasStep = argd(argc, argv, "--bias-step", 0.5);
    const double biasInc  = argd(argc, argv, "--bias-inc",  0.1);
    // Walk the slider out from zero in each direction, the way a hand moves it, which is the
    // only way direction-dependent behaviour shows up at all.
    const bool  biasDrag  = argf(argc, argv, "--bias-drag");
    // ...and feed each result forward as the next solve's starting point, which is what
    // applyBias used to do by reading the live sliders. This reproduces the 2-cycle that
    // caused: it is the hazard probe, NOT what the plugin does now. Keep it, because a sweep
    // from a fixed reference cannot show hysteresis and this is what found it.
    const bool  biasFeedback = argf(argc, argv, "--bias-feedback");
    // Render ONE frame at exactly this slider position. For looking at either side of a step,
    // where a sweep's fixed stops will not land where you need them.
    const double biasAt = argd(argc, argv, "--bias-at", -999.0);
    // SIMULATE A HAND EDIT after Magic Grade: set one of the three to a value of your choosing,
    // re-derive the conditions the grade now meets, and sweep Bias from there. What this is
    // checking is that bias 0 gives the edit back rather than solving it away.
    const double handLift  = argd(argc, argv, "--hand-lift",  -999.0);
    const double handGamma = argd(argc, argv, "--hand-gamma", -999.0);
    const double handGain  = argd(argc, argv, "--hand-gain",  -999.0);
    const char* lutPath = nullptr;
    for (int i = 1; i < argc; ++i) if (!strncmp(argv[i], "--lut=", 6)) lutPath = argv[i] + 6;

    CubeLUT lut;
    if (lutPath && !lut.load(lutPath)) fprintf(stderr, "warn: could not load %s\n", lutPath);
    const float* lutData = lut.valid() ? lut.data.data() : nullptr;
    const int lutSize = lut.valid() ? lut.size : 0;

    og::seg::Segmenter seg;
    {
        std::string pp = std::string(modelDir) + "/ade20k.param";
        std::string bp = std::string(modelDir) + "/ade20k.bin";
        if (!seg.load(pp, bp)) fprintf(stderr, "warn: no model at %s, regions unavailable\n", modelDir);
    }

    printf("gain %.2f%+.2f*key [%.2f..%.2f]  black %.3f  unit %.1f  sep %.2f  lut %s  wb %s\n",
           tun.gainBase, tun.gainPerKey, tun.gainMin, tun.gainMax,
           tun.blackTarget, tun.magicUnit, sep, lut.valid() ? "yes" : "no", wb ? "on" : "off");
    printf("%-24s %6s %6s %6s %6s %6s %6s %6s  %s\n",
           "frame", "key", "src99", "gain", "lift", "roll", "blk", "mid", "decision");

    for (int i = 3; i < argc; ++i) {
        if (argv[i][0] == '-') continue;
        Frame f;
        if (!load_log(argv[i], f)) { fprintf(stderr, "cannot read %s\n", argv[i]); continue; }

        // SAY SO WHEN THE INPUT CANNOT ANSWER THE QUESTION.
        //
        // Log packs the whole dynamic range into whatever code values are available, so 8 bits
        // leaves the shadows very few -- and a dark shot fewer still, because it does not reach
        // the top of the range either. One 8-bit still spanned 0.098 to 0.412, about 80 code
        // values for the entire image, and the bench scored it the healthiest of five while
        // Resolve visibly crushed it. Both were right about their own input.
        //
        // The existing log tell ("max never reaches 1.0") only guards the ceiling. This reports
        // what is actually left at the bottom, which is where the grade is being judged. A
        // silent measurement of an input that cannot carry the answer is the same defect class
        // as the CUDA CPU fallback and the Windows LUT directory: degrading without saying so.
        {
            float lo = 1e9f, hi = -1e9f;
            for (size_t k = 0; k < f.px.size(); k += 3 * 64)
                { lo = std::min(lo, f.px[k]); hi = std::max(hi, f.px[k]); }
            const float peak = (f.bits == 16) ? 65535.f : 255.f;
            const int span = (int)((hi - lo) * peak + 0.5f);
            if (f.bits < 16 || span < 600)
                printf("  ! %d-bit, %.3f..%.3f = ~%d levels, ~%d in the darkest tenth."
                       "  Export 16-bit from Deliver; see experiments/bench/README.md\n",
                       f.bits, lo, hi, span, std::max(1, span / 10));
        }

        oga::SampleSet S;
        og::grade::Measurements m = measure(f, cam, enc, S);

        // THE WHOLE SEQUENCE COMES FROM ONE PLACE. This used to spell out the order -- creative,
        // segment, decide, tone, colour, re-solve -- and so did applyMagicGrade, and they drifted:
        // the re-solve after the colour move landed here and not there, and the two produced
        // different pictures from the same still. Only the segmentation is supplied locally,
        // because the model belongs to the caller.
        std::vector<float> tsrc((size_t)512 * 512 * 3);
        for (int y = 0; y < 512; ++y)
            for (int x = 0; x < 512; ++x) {
                const float* q = &f.px[(((size_t)(y * f.h / 512) * f.w) + (x * f.w / 512)) * 3];
                const size_t o = ((size_t)y * 512 + x) * 3;
                tsrc[o] = q[0]; tsrc[o+1] = q[1]; tsrc[o+2] = q[2];
            }
        og::grade::SegmentFn segfn = [&](const unsigned char* rgb, int w, int h,
                                         std::vector<unsigned char>& regions) {
            if (!seg.ready()) return false;
            std::vector<unsigned char> mk; int mw = 0, mh = 0;
            if (!seg.run(rgb, w, h, mk, mw, mh)) return false;
            regions.assign((size_t)512 * 512, (unsigned char)oga::R_OTHER);
            for (int y = 0; y < 512; ++y)
                for (int x = 0; x < 512; ++x)
                    regions[(size_t)y * 512 + x] = mk[(size_t)(y * mh / 512) * mw + (x * mw / 512)];
            return true;
        };

        const og::grade::MagicResult R =
            og::grade::solve_magic(S, tsrc, m, cam, enc, lutData, lutSize, tun, cycle, sep, wb, segfn);
        float P[oga::kParamN];
        for (int k = 0; k < oga::kParamN; ++k) P[k] = R.P[k];
        if (noTone) { P[3] = 0.11f; P[4] = 1.f; P[10] = 0.f; }

        oga::classify(S, cam, enc <= 2 ? enc : 1);
        // The subject is chosen inside solve_magic, so it is stamped on afterwards -- which is
        // also what the plugin will do. Without it the region separation triple reads zero.
        if (R.choice.ok) S.subject = R.choice.subject;
        oga::Desc d = oga::describe(S, cam, enc <= 2 ? enc : 1, P);

        std::string decision = seg.ready() ? "no move" : "no model";
        char wbNote[80] = "", toneNote[128] = "";
        if (R.choice.ok) {
            char buf[96];
            snprintf(buf, sizeof buf, "%d/%d %s %.0f%% -> %s %+.3f", R.choice.option + 1,
                     R.choice.options, oga::region_name(R.choice.subject), R.choice.cover,
                     R.choice.param == 6 ? "OffTmp" : "GainTmp", R.magicBase * sep);
            decision = buf;
        }
        if (R.wbRan) {
            if (R.wb.ok) snprintf(wbNote, sizeof wbNote, " WB %.0fK (%.0f%% ref, b0 %+.1f)",
                                  R.wb.kelvin, R.wb.cover, R.wb.b0);
            else         snprintf(wbNote, sizeof wbNote, " WB declined: %s (%.0f%% ref)",
                                  R.wb.why, R.wb.cover);
        }
        if (!noTone) {
            if (R.tone.ok)
                snprintf(toneNote, sizeof toneNote,
                         " tone L%+.3f G%.3f g%.3f x%.2fEV -> subj %.3f/%.3f/%.3f spread %.3f"
                         "  hi %.3f (want %.3f, br %d)",
                         R.tone.lift, R.tone.gamma, R.tone.gain, R.tone.rawExp, R.tone.subjLo,
                         R.tone.mid, R.tone.subjHi, R.tone.subjHi - R.tone.subjLo, R.tone.frameHi,
                         R.tone.ceil, R.tone.branch);
            else if (R.tone.why[0])
                snprintf(toneNote, sizeof toneNote,
                         " tone declined: %s (neutral subj mid %.3f, %.2fEV,"
                         " neutral subj %.3f/%.3f/%.3f spread %.3f)",
                         R.tone.why, R.tone.sMidNeutral, R.tone.rawExp,
                         R.tone.sLo, R.tone.sMid, R.tone.sHi, R.tone.sHi - R.tone.sLo);
        }

        const char* nm = strrchr(argv[i], '/'); nm = nm ? nm + 1 : argv[i];
        printf("%-24s %+6.2f %6.3f %6.3f %+6.3f %6.3f %6.3f %6.3f  %s\n",
               nm, m.key, m.d99, P[5], P[3], P[12], d.v[oga::D_BLACK], d.v[oga::D_MID],
               (decision + wbNote + toneNote).c_str());

        // BIAS SWEEP. Walks the slider the way a drag does and prints what the tone re-solve
        // returns at each stop, so a discontinuity is visible as a number rather than as "the
        // picture jumped". Mirrors applyBias()'s target arithmetic exactly -- the two shifted
        // targets and the same clamps -- because that arithmetic is the thing under test.
        // HAND EDIT, then Bias. Mirrors what the plugin does on a manual Lift/Gamma/Gain move:
        // re-derive the conditions from the edited grade so Bias leans away from THAT.
        og::grade::ToneTargets handBase;
        if (R.tone.ok && (handLift > -998.0 || handGamma > -998.0 || handGain > -998.0)) {
            float Ph[oga::kParamN];
            for (int k = 0; k < oga::kParamN; ++k) Ph[k] = P[k];
            if (handLift  > -998.0) Ph[3] = (float)handLift;
            if (handGamma > -998.0) Ph[4] = (float)handGamma;
            if (handGain  > -998.0) Ph[5] = (float)handGain;
            handBase = og::grade::tone_targets_of(R.tone.sLo, R.tone.sMid, R.tone.fHi,
                                                  R.tone.fLo, Ph, lutData, lutSize);
            const og::grade::MagicTone back = og::grade::solve_magic_tone_bias(
                R.tone.sLo, R.tone.sMid, R.tone.sHi, R.tone.fHi, Ph,
                lutData, lutSize, tun, R.tone.fLo, 0.0, handBase);
            printf("    hand edit  L%+.3f G%.3f g%.3f  -> conditions %.3f/%.3f/%.3f\n",
                   Ph[3], Ph[4], Ph[5], handBase.floor, handBase.mid, handBase.ceil);
            printf("    bias 0     L%+.3f G%.3f g%.3f  %s\n", back.lift, back.gamma, back.gain,
                   (std::fabs(back.lift - Ph[3]) < 0.005 &&
                    std::fabs(back.gamma - Ph[4]) < 0.02 &&
                    std::fabs(back.gain - Ph[5]) < 0.01) ? "<- PRESERVED" : "<- LOST");
            for (int k = 0; k < oga::kParamN; ++k) P[k] = Ph[k];   // sweep from the edited grade
        }

        // TONE SEPARATION, walked the way a hand walks it, with the ACHIEVED rdL* beside the
        // parameters. The parameters alone cannot answer the only question that matters -- whether
        // the subject actually ended up further from its surround -- and a slider that moves Gamma
        // convincingly while separation stays put is exactly the kind of plausible-but-wrong result
        // this bench exists to catch.
        if (toneSepSweep && R.tone.ok) {
            const double dir = og::grade::tone_sep_dir(d.v[oga::D_RDL]);
            printf("    surround: neutral %.3f -> rendered %.3f (contrast pivots at 0.500)\n",
                   R.tone.sSur, R.tone.surr);
            printf("    tone separation: rdL* %+.2f -> direction %+.0f%s\n",
                   d.v[oga::D_RDL], dir,
                   dir == 0.0 ? "  (INERT: subject and surround are at the same lightness)" : "");
            // THE CONDITIONS THE GRADE ACTUALLY MEETS, exactly as applyBias() supplies them.
            // Passing a default ToneTargets here made sep 0 return a different grade than the one
            // on screen -- the fitted ceiling is 0.968 and this frame was solved at 0.890, so the
            // solve was asked to reproduce a grade it had never made.
            const og::grade::ToneTargets sepBase = og::grade::tone_targets_of(
                R.tone.sLo, R.tone.sMid, R.tone.fHi, R.tone.fLo, P, lutData, lutSize);
            printf("    sep      lift   gamma    gain   contr    rdL*   d(rdL*)\n");
            double prevR = d.v[oga::D_RDL], prevL = R.tone.lift;
            for (double s = -1.0; s <= 1.0001; s += biasInc) {
                const og::grade::MagicTone t = og::grade::solve_magic_tone_bias(
                    R.tone.sLo, R.tone.sMid, R.tone.sHi, R.tone.fHi, P,
                    lutData, lutSize, tun, R.tone.fLo, 0.0, sepBase,
                    s, dir, toneSepPer);
                if (!t.ok) { printf("   %+5.3f   DECLINED: %s\n", s, t.why); continue; }
                float Ps[oga::kParamN];
                for (int k = 0; k < oga::kParamN; ++k) Ps[k] = P[k];
                Ps[3] = t.lift; Ps[4] = t.gamma; Ps[5] = t.gain; Ps[9] = t.con;
                const oga::Desc ds = oga::describe(S, cam, enc <= 2 ? enc : 1, Ps);
                printf("   %+5.3f  %+7.3f %7.3f %7.3f %7.3f %+7.2f  %+7.2f%s\n",
                       s, t.lift, t.gamma, t.gain, t.con, ds.v[oga::D_RDL],
                       ds.v[oga::D_RDL] - prevR,
                       std::fabs(t.lift - prevL) > 0.02 ? "   <-- JUMP" : "");
                prevR = ds.v[oga::D_RDL]; prevL = t.lift;
            }
        }

        if (biasAt > -998.0 && R.tone.ok) {
            const og::grade::MagicTone t = og::grade::solve_magic_tone_bias(
                R.tone.sLo, R.tone.sMid, R.tone.sHi, R.tone.fHi, P,
                lutData, lutSize, tun, R.tone.fLo, biasAt);
            printf("    bias %+0.3f -> lift %+0.3f gamma %.3f gain %.3f  branch %d  %s\n",
                   biasAt, t.lift, t.gamma, t.gain, t.branch, t.ok ? "ok" : t.why);
            if (t.ok) {
                float Pb[oga::kParamN];
                for (int k = 0; k < oga::kParamN; ++k) Pb[k] = P[k];
                Pb[3] = t.lift; Pb[4] = t.gamma; Pb[5] = t.gain;
                std::vector<unsigned char> ob((size_t)f.w * f.h * 3);
                for (size_t k = 0; k < (size_t)f.w * f.h; ++k) {
                    float r, g, b;
                    full_chain(cam, enc, Pb, lutData, lutSize, 1.f,
                               f.px[k*3], f.px[k*3+1], f.px[k*3+2], r, g, b);
                    ob[k*3+0] = (unsigned char)(og::clamp01(r) * 255.f + .5f);
                    ob[k*3+1] = (unsigned char)(og::clamp01(g) * 255.f + .5f);
                    ob[k*3+2] = (unsigned char)(og::clamp01(b) * 255.f + .5f);
                }
                const char* nm1 = strrchr(argv[i], '/'); nm1 = nm1 ? nm1 + 1 : argv[i];
                std::string st(nm1);
                const size_t d1 = st.rfind('.');
                if (d1 != std::string::npos) st = st.substr(0, d1);
                char bp[64];
                snprintf(bp, sizeof bp, "-at%c%04d.png", biasAt < 0 ? 'm' : 'p',
                         (int)std::llround(std::fabs(biasAt) * 1000.0));
                const std::string op3 = outDir + "/" + st + bp;
                stbi_write_png(op3.c_str(), f.w, f.h, 3, ob.data(), f.w * 3);
            }
        }
        if (biasSweep && R.tone.ok) {
            og::grade::Tunables tn = tun;
            // The tone triple this frame stands on, printed so a real configuration can be
            // lifted straight into a unit test. A synthetic one is not good enough here: the
            // first version of the continuity test used made-up percentiles that never reached
            // the feasibility limit, so it passed with the hold-at-limit logic deleted.
            printf("    tone inputs: sLo %.4f sMid %.4f sHi %.4f fHi %.4f fLo %.4f\n",
                   R.tone.sLo, R.tone.sMid, R.tone.sHi, R.tone.fHi, R.tone.fLo);
            // DRAG MODE: out from zero in each direction, the way a hand moves the slider, with
            // the previous result fed forward. Direction matters for hysteresis, so a sweep that
            // runs +2 -> -2 would miss it even with feedback.
            if (biasDrag) {
                printf(biasFeedback
                       ? "    (drag WITH FEEDBACK: the pre-fix hazard, results fed forward)\n"
                       : "    (drag from the armed anchor, as the plugin does)\n");
                printf("    bias     lift   gamma    gain   d(lift)\n");
                for (int dir = 0; dir < 2; ++dir) {
                    float Pf[oga::kParamN];
                    for (int k = 0; k < oga::kParamN; ++k) Pf[k] = P[k];
                    double prevL = P[3];
                    for (double bs = 0.0; dir ? (bs >= -2.0001) : (bs <= 2.0001);
                         bs += dir ? -biasInc : biasInc) {
                        const og::grade::MagicTone t = og::grade::solve_magic_tone_bias(
                            R.tone.sLo, R.tone.sMid, R.tone.sHi, R.tone.fHi, Pf,
                            lutData, lutSize, tn, R.tone.fLo, bs);
                        if (!t.ok) { printf("   %+5.3f   NOT ARMED: %s\n", bs, t.why); break; }
                        const double d = t.lift - prevL;
                        printf("   %+5.3f  %+7.3f %7.3f %7.3f  %+7.3f%s\n",
                               bs, t.lift, t.gamma, t.gain, d,
                               std::fabs(d) > 0.02 ? "   <-- JUMP" : "");
                        if (biasFeedback) { Pf[3] = t.lift; Pf[4] = t.gamma; Pf[5] = t.gain; }
                        prevL = t.lift;
                    }
                }
                continue;
            }
            printf("    bias   subjFloor  ceiling     lift   gamma    gain   result\n");
            for (double bs = 2.0; bs >= -2.001; bs -= biasInc) {
                const double sf = std::min(0.40, std::max(0.00,
                                      tn.subjFloor + bs * og::grade::kBiasSubjFloorPer));
                const double fc = std::min(og::grade::kFrameCeilingMax, std::max(0.60,
                                      tn.frameCeiling - bs * og::grade::kBiasCeilingPer));
                // The SAME call applyBias() makes, holding included -- so "held" below is what
                // the slider actually does, not what a copy of it would do.
                const og::grade::MagicTone t = og::grade::solve_magic_tone_bias(
                    R.tone.sLo, R.tone.sMid, R.tone.sHi, R.tone.fHi, P,
                    lutData, lutSize, tn, R.tone.fLo, bs);
                const og::grade::MagicTone raw = og::grade::solve_magic_tone_from(
                    R.tone.sLo, R.tone.sMid, R.tone.sHi, R.tone.fHi, P,
                    lutData, lutSize, sf, tn.subjMid, fc, R.tone.fLo, tn.frameFloorMax);
                if (!t.ok) {
                    printf("   %+5.2f   %7.3f  %7.3f        -       -       -   NOT ARMED: %s\n",
                           bs, sf, fc, t.why);
                    continue;
                }
                printf("   %+5.2f   %7.3f  %7.3f  %+7.3f %7.3f %7.3f   %s\n",
                       bs, sf, fc, t.lift, t.gamma, t.gain,
                       raw.ok ? "ok" : "held (targets unreachable)");

                // AND WRITE THE PICTURE. Numbers say the curve is continuous; only the frames
                // say whether the grade at each stop is one anybody would want. Written at
                // --bias-step so a sweep is a contact sheet you can flip through in order,
                // rather than 41 versions of a 4K still.
                if (biasStep > 0.0 &&
                    std::fabs(bs / biasStep - std::floor(bs / biasStep + 0.5)) < 1e-6) {
                    float Pb[oga::kParamN];
                    for (int k = 0; k < oga::kParamN; ++k) Pb[k] = P[k];
                    Pb[3] = t.lift; Pb[4] = t.gamma; Pb[5] = t.gain;
                    std::vector<unsigned char> ob((size_t)f.w * f.h * 3);
                    for (size_t k = 0; k < (size_t)f.w * f.h; ++k) {
                        float r, g, b;
                        full_chain(cam, enc, Pb, lutData, lutSize, 1.f,
                                   f.px[k*3], f.px[k*3+1], f.px[k*3+2], r, g, b);
                        ob[k*3+0] = (unsigned char)(og::clamp01(r) * 255.f + .5f);
                        ob[k*3+1] = (unsigned char)(og::clamp01(g) * 255.f + .5f);
                        ob[k*3+2] = (unsigned char)(og::clamp01(b) * 255.f + .5f);
                    }
                    const char* nm0 = strrchr(argv[i], '/'); nm0 = nm0 ? nm0 + 1 : argv[i];
                    std::string stem(nm0);
                    const size_t dot = stem.rfind('.');
                    if (dot != std::string::npos) stem = stem.substr(0, dot);
                    char bp[64];
                    // Sortable, so the contact sheet reads in slider order: p10 is +1.0.
                    snprintf(bp, sizeof bp, "-bias%c%04d.png", bs < 0 ? 'm' : 'p',
                             (int)std::llround(std::fabs(bs) * 100.0));
                    const std::string op2 = outDir + "/" + stem + bp;
                    stbi_write_png(op2.c_str(), f.w, f.h, 3, ob.data(), f.w * 3);
                }
            }
        }

        // Write the graded frame, and measure it ON THE WAY OUT.
        //
        // The `blk` above is the number the solve TARGETS, and it is measured pre-LUT because
        // that is where the solve works. What the viewer judges is post-LUT, after a print
        // stock with a toe of its own. If the stock pulls the bottom down further, the target
        // is being hit and the picture is still crushed -- which is exactly the shape of
        // complaint that "the blacks are crushed" is.
        std::vector<unsigned char> out((size_t)f.w * f.h * 3);
        std::vector<float> outCh; outCh.reserve((size_t)f.w * f.h * 3 / 64 + 8);
        // TWO CRUSH MEASURES, because the first one lies on saturated footage.
        //
        // `crushed` is per-pixel MIN CHANNEL at or under 1/255, and on a deep blue sky the min
        // channel is red essentially everywhere -- near zero because that is the COLOUR, not
        // because detail was lost. It reported 43.8% on a desert landscape the user confirmed
        // looks correct, with the waveform showing red pinned flat across the full width and the
        // picture holding texture throughout.
        //
        // `crushedY` asks the question that was meant: is the LUMINANCE at the floor, which is
        // when detail is actually gone. Same shape as hot versus pin -- a threshold on the wrong
        // quantity describes the filter rather than the footage.
        // THE MASK READS THE PICTURE AFTER THE GRADE CURVE, before Range Balance's own moves --
        // which is what a Resolve qualifier dropped on the node sees, and the distinction the
        // first version got wrong. Pre-grade the picture is flat, so a bright pillow and a window
        // sit within a few points of each other and NO threshold separates them; the user could
        // not match Resolve's window selection at any latch for exactly that reason.
        //
        // Only Range Balance's OWN moves could make the mask chase itself, so only those are
        // switched off here. Lift/Gamma/Gain run once and are not driven by the mask.
        //
        // Read in a DISPLAY-REFERRED encode, mirroring the plugin: a film LUT forces Cineon, and
        // a threshold in Cineon means something else entirely.
        float Pmask[oga::kParamN];
        for (int k = 0; k < oga::kParamN; ++k) Pmask[k] = P[k];
        Pmask[13] = 0.f;                                  // Range Balance off: this IS its input
        Pmask[18] = 0.f;                                  // never the matte
        const int maskEnc = (enc <= 2) ? enc : 1;

        double hlLowUse = hlLow;
        if (hlOn) {
            // DERIVE THE THRESHOLD RATHER THAN TYPE IT, which is what makes this a slider.
            // og::grade::range_latch() is the plugin's own Set From Frame -- called, not
            // paraphrased, so the bench cannot report a latch the button would not produce.
            std::vector<float> ymask;
            for (size_t k = 0; k < (size_t)f.w * f.h; k += 64) {
                float r, g, b;
                og::process(cam, maskEnc, Pmask, f.px[k*3], f.px[k*3+1], f.px[k*3+2], r, g, b);
                ymask.push_back(0.2126f*r + 0.7152f*g + 0.0722f*b);
            }
            const og::grade::RangeLatch RL = og::grade::range_latch(ymask);
            if (ymask.size() > 8) {
                auto q = [&](double t) {
                    size_t i = (size_t)(t * (ymask.size() - 1));
                    std::nth_element(ymask.begin(), ymask.begin() + i, ymask.end());
                    return (double)ymask[i];
                };
                printf("    highlight mask: mask luma p50 %.1f p90 %.1f p98 %.1f p99.9 %.1f"
                       " | split %.1f (%.2f%% of frame) gap %.1f%s\n",
                       100.0*q(0.50), 100.0*q(0.90), 100.0*q(0.98), 100.0*q(0.999),
                       RL.latch, RL.cover, RL.gap, RL.ok ? "" : "  DECLINED");
            }
            if (hlLow < 0.0 && RL.ok) hlLowUse = RL.latch;
            // AND THEN LET THE PIPELINE DO IT. Range Balance lives inside og::process(), before
            // the encode and the LUT, so the bench sets the parameters and renders -- it does not
            // re-implement the partition next to full_chain(). The version that did drifted the
            // moment the plugin's mask moved after the grade curve, which is the paraphrase-class
            // bug this whole shared-header arrangement exists to prevent.
            P[13] = (float)hlLowUse;  P[14] = (float)hlSoft;   P[15] = (float)hlHiGain;
            P[16] = (float)hlLift;    P[17] = (float)hlGamma;   P[18] = hlShow ? 1.f : 0.f;
            P[19] = (float)hlHiGamma; P[20] = (float)hlGain;
            // The mask's reference grade -- the live grade unless locked, mirroring resolveConfig().
            P[21] = hlLock ? (float)hlLockL : P[3];
            P[22] = hlLock ? (float)hlLockG : P[4];
            P[23] = hlLock ? (float)hlLockN : P[5];
            // THE COVERAGE PROBE HAS TO READ THE REFERENCE GRADE, NOT THE LIVE ONE. Pmask renders
            // through P[3..5], and the mask inside og::process() runs lgg_core on P[21..23] -- so
            // the probe reproduces it only if those are the numbers it grades with. Left as the
            // live grade it silently measured the OLD, unlockable definition and reported a locked
            // mask drifting exactly as much as an unlocked one.
            Pmask[3] = P[21]; Pmask[4] = P[22]; Pmask[5] = P[23];
            P[24]=(float)shType; P[25]=(float)shX; P[26]=(float)shY;
            P[27]=(float)shW;    P[28]=(float)shH; P[29]=(float)shR;
            P[30]=(float)shS;    P[31]= shInv ? 1.f : 0.f;
        }
        long long masked = 0;

        long long crushed = 0, crushedY = 0, total = 0;
        // Same normalisation the four render paths use: centre-origin, half-height on both axes.
        const float shHalf = 0.5f*(float)f.h;
        for (size_t k = 0; k < (size_t)f.w * f.h; ++k) {
            const float px = (float)(k % (size_t)f.w), py = (float)(k / (size_t)f.w);
            // Y IS FLIPPED. A PNG row index counts DOWN from the top; OFX canonical coordinates
            // count UP from the bottom, which is what the plugin's render loop and all three
            // kernels use. Left unflipped the bench would mirror every shape vertically and
            // quietly disagree with the plugin about where "above" is.
            const float shM = og::shape_mask((px - 0.5f*f.w)/shHalf, (0.5f*f.h - py)/shHalf,
                                             (int)(P[24]+0.5f), P[25], P[26], P[27], P[28],
                                             P[29], P[30], P[31] > 0.5f);
            float r, g, b;
            full_chain(cam, enc, P, lutData, lutSize, 1.f,
                       f.px[k*3], f.px[k*3+1], f.px[k*3+2], r, g, b, shM);
            if (hlOn) {
                // Coverage only -- the picture itself came out of full_chain() above, mask and
                // all. Recomputed here rather than returned, because it is a diagnostic.
                float mr, mg, mb;
                og::process(cam, maskEnc, Pmask, f.px[k*3], f.px[k*3+1], f.px[k*3+2], mr, mg, mb);
                const float Y = 100.f * (0.2126f*mr + 0.7152f*mg + 0.0722f*mb);
                if (og::highlight_mask(Y, (float)hlLowUse, (float)hlSoft)*shM > 0.5f) ++masked;
            }
            if ((k & 63) == 0) { outCh.push_back(r); outCh.push_back(g); outCh.push_back(b); }
            const float mn = std::min(r, std::min(g, b));
            if (mn <= 0.004f) ++crushed;          // min channel -- confounded by saturation
            if (0.2126f*r + 0.7152f*g + 0.0722f*b <= 0.004f) ++crushedY;
            ++total;
            out[k*3+0] = (unsigned char)(og::clamp01(r) * 255.f + .5f);
            out[k*3+1] = (unsigned char)(og::clamp01(g) * 255.f + .5f);
            out[k*3+2] = (unsigned char)(og::clamp01(b) * 255.f + .5f);
        }
        double postBlk = 0.0;
        if (!outCh.empty()) {
            size_t q = (size_t)(0.001 * (outCh.size() - 1));
            std::nth_element(outCh.begin(), outCh.begin() + q, outCh.end());
            postBlk = outCh[q];
        }

        // SHADOW SEPARATION, because counting pixels at zero measures the wrong thing.
        //
        // "Crushed" to the eye means the shadows have lost SEPARATION -- a dark object's form
        // collapsing into a single flat tone. Nothing has to reach zero for that to happen: a
        // range of 0.02 to 0.06 has no black pixels at all and still reads as a black hole. The
        // count said 0.00% on a frame that visibly had the problem, which is a metric answering
        // a question nobody asked.
        //
        // So: take the darkest tenth of the SOURCE, push both ends through the grade, and report
        // how much output range they still occupy. That is exactly the quantity the eye is
        // judging -- how much of the shadow detail survived.
        double sep10 = 0.0;
        {
            std::vector<float> srcL; srcL.reserve((size_t)f.w * f.h / 64 + 8);
            // ANCHOR ON LUMA, NOT MIN CHANNEL. Selecting the two anchor pixels by percentiles of
            // per-pixel MIN picks whatever is least saturated in the frame's dominant hue -- on a
            // blue sky that is a red-starved SKY pixel, so this measured how far apart two bits of
            // sky ended up rather than anything about shadows. Placing sky on a target compresses
            // exactly that, which is how a working grade read as destroyed shadow detail.
            for (size_t k = 0; k < (size_t)f.w * f.h; k += 64)
                srcL.push_back(0.2126f*f.px[k*3] + 0.7152f*f.px[k*3+1] + 0.0722f*f.px[k*3+2]);
            if (srcL.size() > 8) {
                auto q = [&](double t) {
                    size_t i = (size_t)(t * (srcL.size() - 1));
                    std::nth_element(srcL.begin(), srcL.begin() + i, srcL.end());
                    return srcL[i];
                };
                const float lo = q(0.01), hi = q(0.10);
                float r0,g0,b0, r1,g1,b1;
                full_chain(cam, enc, P, lutData, lutSize, 1.f, lo, lo, lo, r0, g0, b0);
                full_chain(cam, enc, P, lutData, lutSize, 1.f, hi, hi, hi, r1, g1, b1);
                sep10 = (double)(g1 - g0);
            }
        }
        printf("%-24s %6s %6s %6.3f %6.2f %6.2f %6.3f  blk / %%crushMin / %%crushY / shadowSep\n",
               "", "", "", postBlk, 100.0 * (double)crushed / (double)total,
               100.0 * (double)crushedY / (double)total, sep10);
        // BOTH SEPARATION TRIPLES, side by side, because the whole question is whether the region
        // version says something the band version cannot. Printing only the new one would make it
        // impossible to tell a real improvement from a differently-scaled number.
        // WHICH CONTROLS MOVE THE SUBJECT AWAY FROM ITS SURROUND, measured rather than reasoned
        // about. The slider has to spend controls the tone solve does not own, or it undoes the
        // three conditions that place the subject -- and which controls those are is a property of
        // the footage and the region, not something to argue from the pipeline order.
        if (sepJac) {
            const oga::Jac J = oga::jacobian(S, cam, enc <= 2 ? enc : 1, P);
            printf("%-24s   d(rdL*)/dp:", "");
            for (int p = 0; p < oga::kParamN; ++p)
                printf(" %s%+.2f", oga::param_name(p), J.at(oga::D_RDL, p));
            printf("\n%-24s   d(rdb*)/dp:", "");
            for (int p = 0; p < oga::kParamN; ++p)
                printf(" %s%+.2f", oga::param_name(p), J.at(oga::D_RDB, p));
            printf("\n");
        }
        if (sepReport) {
            printf("%-24s   band dL* %+7.2f da* %+7.2f db* %+7.2f | region %s dL* %+7.2f "
                   "da* %+7.2f db* %+7.2f\n", "",
                   d.v[oga::D_DL], d.v[oga::D_DA], d.v[oga::D_DB],
                   R.choice.ok ? oga::region_name(R.choice.subject) : "-",
                   d.v[oga::D_RDL], d.v[oga::D_RDA], d.v[oga::D_RDB]);
        }
        if (hlOn)
            printf("    highlight mask: latch %.1f soft %.1f -> %.2f%% of frame held"
                   " | held gain %.3f gamma %.3f | rest lift %+.3f gamma %.3f gain %.3f%s\n",
                   hlLowUse, hlSoft, 100.0 * (double)masked / (double)total,
                   hlHiGain, hlHiGamma, hlLift, hlGamma, hlGain,
                   hlLock ? "  [mask LOCKED]" : "");
        std::string op = outDir + "/" + std::string(nm);
        stbi_write_png(op.c_str(), f.w, f.h, 3, out.data(), f.w * 3);
    }
    return 0;
}
