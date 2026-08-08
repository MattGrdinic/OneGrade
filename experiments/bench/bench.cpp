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
                              float ri, float gi, float bi, float& ro, float& go, float& bo)
{
    og::process(cam, enc, P, ri, gi, bi, ro, go, bo);
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
    float N[oga::kParamN] = {0.f,0.f,0.f, 0.f,1.f,1.f, 0.f,0.f, 0.f,1.f, 0.f,6500.f, 0.f};

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
    tun.frameCeiling = argd(argc, argv, "--frame-ceiling", tun.frameCeiling);
    tun.magicUnit  = argd(argc, argv, "--unit",         tun.magicUnit);
    const int   cam = (int)argd(argc, argv, "--camera", og::grade::kCreativeCamera);
    const int   enc = (int)argd(argc, argv, "--encode", og::grade::kCreativeEncode);
    const double sep = argd(argc, argv, "--sep", 1.0);
    const bool  wb  = argf(argc, argv, "--wb");
    const bool  noTone = argf(argc, argv, "--no-tone");
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
    printf("%-24s %6s %6s %6s %6s %6s %6s  %s\n",
           "frame", "key", "gain", "lift", "roll", "blk", "mid", "decision");

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
        const int dispEnc = (enc <= 2) ? enc : 1;
        og::grade::Measurements m = measure(f, cam, enc, S);
        float P[oga::kParamN];
        og::grade::solve_creative_px(S, cam, enc, m, tun, P, lutData, lutSize);

        // WHITE BALANCE FIRST. --wb used to be parsed, printed as "wb on", and never acted on.
        // Mirrors the plugin: segment a NEUTRAL thumbnail, solve on the surfaces that should be
        // neutral, then stamp the result into Scene White Balance after the grade solve, which
        // is the order applyMagicGrade() uses.
        char wbNote[48] = "";
        char toneNote[96] = "";
        if (wb && seg.ready()) {
            std::vector<float>         wsrc((size_t)512 * 512 * 3);
            std::vector<unsigned char> wthumb((size_t)512 * 512 * 3);
            float Nn[oga::kParamN] = {0.f,0.f,0.f, 0.f,1.f,1.f, 0.f,0.f, 0.f,1.f, 0.f,6500.f, 0.f};
            for (int y = 0; y < 512; ++y)
                for (int x = 0; x < 512; ++x) {
                    const float* q = &f.px[(((size_t)(y * f.h / 512) * f.w) + (x * f.w / 512)) * 3];
                    const size_t o = ((size_t)y * 512 + x) * 3;
                    wsrc[o] = q[0]; wsrc[o+1] = q[1]; wsrc[o+2] = q[2];
                    float r, g, b;
                    og::process(cam, dispEnc, Nn, q[0], q[1], q[2], r, g, b);
                    wthumb[o+0] = (unsigned char)(og::clamp01(r) * 255.f + .5f);
                    wthumb[o+1] = (unsigned char)(og::clamp01(g) * 255.f + .5f);
                    wthumb[o+2] = (unsigned char)(og::clamp01(b) * 255.f + .5f);
                }
            std::vector<unsigned char> wmask; int ww = 0, wh = 0;
            if (seg.run(wthumb.data(), 512, 512, wmask, ww, wh)) {
                std::vector<unsigned char> full((size_t)512 * 512);
                for (int y = 0; y < 512; ++y)
                    for (int x = 0; x < 512; ++x)
                        full[(size_t)y * 512 + x] = wmask[(size_t)(y * wh / 512) * ww + (x * ww / 512)];
                const og::grade::WhiteBalance W =
                    og::grade::solve_white_balance(wsrc, full, cam, dispEnc);
                if (W.ok) { P[11] = (float)W.kelvin;
                            snprintf(wbNote, sizeof wbNote, " WB %.0fK (%.0f%% ref, b0 %+.1f)", W.kelvin, W.cover, W.b0); }
                else      { snprintf(wbNote, sizeof wbNote, " WB declined (%.0f%% ref)", W.cover); }
            }
        }

        // What the grade actually achieved, measured the same way the plugin measures it.
        oga::classify(S, cam, enc <= 2 ? enc : 1);
        oga::Desc d = oga::describe(S, cam, enc <= 2 ? enc : 1, P);

        // Segment the GRADED picture, as the plugin does.
        std::string decision = "no model";
        if (seg.ready()) {
            std::vector<unsigned char> th((size_t)512 * 512 * 3);
            for (int y = 0; y < 512; ++y)
                for (int x = 0; x < 512; ++x) {
                    const float* q = &f.px[(((size_t)(y * f.h / 512) * f.w) + (x * f.w / 512)) * 3];
                    float r, g, b;
                    full_chain(cam, enc, P, lutData, lutSize, 1.f, q[0], q[1], q[2], r, g, b);
                    unsigned char* o = &th[((size_t)y * 512 + x) * 3];
                    o[0] = (unsigned char)(og::clamp01(r) * 255.f + .5f);
                    o[1] = (unsigned char)(og::clamp01(g) * 255.f + .5f);
                    o[2] = (unsigned char)(og::clamp01(b) * 255.f + .5f);
                }
            std::vector<unsigned char> mask; int mw = 0, mh = 0;
            if (seg.run(th.data(), 512, 512, mask, mw, mh) && oga::assign_regions(S, mask, mw, mh)) {
                oga::RegionStat st[oga::kRegionN];
                oga::region_stats(S, cam, enc <= 2 ? enc : 1, P, st);
                oga::MagicChoice c = oga::magic_decide(st, 0);
                if (c.ok) {
                    // APPLY IT. The bench used to render Creative Grade alone and report the
                    // decision without making it, so anything the move itself did was invisible
                    // -- and Offset Temp is additive, so on a dark frame it subtracts from blue
                    // and can drive the channel through zero. Exactly the kind of thing an
                    // offline check exists to catch.
                    // MAGIC TONE: place the subject for legibility and leave Bias somewhere to
                    // go. Runs before the colour move so the colour is chosen against the tone
                    // the picture will actually have.
                    const og::grade::MagicTone mt =
                        og::grade::solve_magic_tone(S, c.subject, cam, enc, lutData, lutSize, P, tun);
                    if (mt.ok && !noTone) {
                        P[3] = mt.lift; P[4] = mt.gamma; P[5] = mt.gain;
                        snprintf(toneNote, sizeof toneNote,
                                 " tone L%+.3f G%.3f g%.3f -> subj %.3f/%.3f/%.3f spread %.3f  hi %.3f",
                                 mt.lift, mt.gamma, mt.gain, mt.subjLo, mt.mid, mt.subjHi,
                                 mt.subjHi - mt.subjLo, mt.frameHi);
                    }
                    const double base = og::grade::solve_magic_base(S, cam, enc <= 2 ? enc : 1, c, st, tun);
                    P[c.param] = (float)std::min(1.0, std::max(-1.0, (double)P[c.param] + base * sep));

                    // RE-SOLVE THE FLOOR AFTER THE COLOUR MOVE. Offset Temp is additive and Gain
                    // Temp multiplicative, so either one shifts the channels the black point was
                    // just placed on -- and it is a CHANNEL that crushes. Solving first and
                    // colouring second left the frame's floor somewhere nobody had checked: the
                    // pre-LUT black read 0.375 on one shot while 7% of the picture sat at zero.
                    //
                    // Same shape as the first-press bug: a grade has to be solved for the
                    // configuration it ends in, not the one it passed through.
                    og::grade::solve_black_px(S, cam, enc, P, tun.blackTarget, lutData, lutSize);
                    if (mt.ok && !noTone) {
                        const og::grade::MagicTone m2 =
                            og::grade::solve_magic_tone(S, c.subject, cam, enc, lutData, lutSize, P, tun);
                        if (m2.ok) { P[3] = m2.lift; P[4] = m2.gamma; P[5] = m2.gain; }
                    }
                    char buf[96];
                    snprintf(buf, sizeof buf, "%d/%d %s %.0f%% -> %s %+.3f",
                             c.option + 1, c.options, oga::region_name(c.subject), c.cover,
                             c.param == 6 ? "OffTmp" : "GainTmp", base * sep);
                    decision = buf;
                } else {
                    decision = "no move";
                }
            }
        }

        const char* nm = strrchr(argv[i], '/'); nm = nm ? nm + 1 : argv[i];
        printf("%-24s %+6.2f %6.3f %+6.3f %6.3f %6.3f %6.3f  %s\n",
               nm, m.key, P[5], P[3], P[12], d.v[oga::D_BLACK], d.v[oga::D_MID],
               (decision + wbNote + toneNote).c_str());

        // Write the graded frame, and measure it ON THE WAY OUT.
        //
        // The `blk` above is the number the solve TARGETS, and it is measured pre-LUT because
        // that is where the solve works. What the viewer judges is post-LUT, after a print
        // stock with a toe of its own. If the stock pulls the bottom down further, the target
        // is being hit and the picture is still crushed -- which is exactly the shape of
        // complaint that "the blacks are crushed" is.
        std::vector<unsigned char> out((size_t)f.w * f.h * 3);
        std::vector<float> outCh; outCh.reserve((size_t)f.w * f.h * 3 / 64 + 8);
        long long crushed = 0, total = 0;
        for (size_t k = 0; k < (size_t)f.w * f.h; ++k) {
            float r, g, b;
            full_chain(cam, enc, P, lutData, lutSize, 1.f,
                       f.px[k*3], f.px[k*3+1], f.px[k*3+2], r, g, b);
            if ((k & 63) == 0) { outCh.push_back(r); outCh.push_back(g); outCh.push_back(b); }
            const float mn = std::min(r, std::min(g, b));
            if (mn <= 0.004f) ++crushed;          // at or under 1/255: detail that is gone
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
            for (size_t k = 0; k < (size_t)f.w * f.h; k += 64)
                srcL.push_back(std::min(f.px[k*3], std::min(f.px[k*3+1], f.px[k*3+2])));
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
        printf("%-24s %6s %6s %6s %6.3f %6.2f %6.3f  post-LUT blk / %% crushed / shadow sep\n",
               "", "", "", "", postBlk, 100.0 * (double)crushed / (double)total, sep10);
        std::string op = outDir + "/" + std::string(nm);
        stbi_write_png(op.c_str(), f.w, f.h, 3, out.data(), f.w * 3);
    }
    return 0;
}
