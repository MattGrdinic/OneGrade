// OneGrade — grade variants for preference labelling.
//
// Log stills in; several graded versions of each out, plus a manifest the labelling page reads.
//
// ---------------------------------------------------------------------------------------
// WHY VARIANTS OF ONE FRAME, AND NOT DIFFERENT FRAMES
//
// The question is what the user LIKES, and the cleanest way to ask it is two grades of the same
// shot. Comparing across shots would let content decide the answer -- which is exactly how the
// film-versus-OneGrade comparison went wrong earlier in this work, where a separator could have
// been reading "Hollywood interior versus desert" and scored brilliantly while measuring nothing.
// Within-scene pairs have no content confound at all.
//
// It also means sixteen stills is enough. The variety that matters comes from the GRADES, not the
// scenes: sixteen shots times eleven variants is a hundred and seventy-six pictures and over
// eight hundred possible within-scene pairs.
//
// ---------------------------------------------------------------------------------------
// EVERY VARIANT IS REACHABLE BY THE PLUGIN
//
// The base is solve_magic() -- the real choreography, the same call the bench and the plugin
// make -- and each variant moves ONE parameter off it. Nothing here invents a grade OneGrade
// could not produce, because a preference for something unreachable would train the model to
// want what the solver cannot deliver.
//
// One parameter at a time is deliberate. A pair differing along a single axis answers a question
// that can be acted on ("less lift on faces"); a pair differing in five answers nothing.
//
// USAGE
//   variants MODEL_DIR OUT_DIR STILL.png [...] [--lut=PATH] [--camera=N] [--encode=N] [--width=N]
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

// Mirrors og_full_chain() in OneGrade.cpp, same as the bench's copy. If that ever moves to a
// header, both should call it instead.
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

struct Frame { int w = 0, h = 0; std::vector<float> px; };

static bool load_log(const char* path, Frame& f)
{
    int c = 0;
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

// probeAnalyze()'s measurements, on the same grid and in the same order as the bench's copy.
static og::grade::Measurements measure(const Frame& f, int cam, int enc, oga::SampleSet& S)
{
    og::grade::Measurements m;
    const int step = std::max(1, (int)(std::sqrt((double)(f.w * f.h) / 200000.0) + 0.5));
    float N[oga::kParamN] = {0.f,0.f,0.f, 0.f,1.f,1.f, 0.f,0.f, 0.f,1.f, 0.f,6500.f, 0.f};
    const int dispEnc = (enc <= 2) ? enc : 1;
    std::vector<float> lum, chn, srcTop, sceneY;
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
    m.d01 = pct(chn, 0.001); m.d99 = pct(chn, 0.99); m.d50 = pct(lum, 0.50);
    const double y50 = pct(sceneY, 0.50);
    m.key = (y50 > 1e-6) ? std::log2(0.18 / y50) : 0.0;
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

// THE AXES A COLORIST ACTUALLY REACHES FOR, and the ones the tone solve already owns. Deltas are
// sized to be clearly visible without being absurd: the useful judgement is "which of these two
// plausible grades do I prefer", not "which of these is broken".
struct Axis { const char* name; int idx; float delta; };
static const Axis kAxes[] = {
    { "lift",    3, 0.055f },
    { "gamma",   4, 0.130f },
    { "gain",    5, 0.130f },
    { "warmth",  6, 0.090f },
    { "expo",    8, 0.260f },
};
static const int kAxisN = (int)(sizeof(kAxes) / sizeof(kAxes[0]));

static std::string stem_of(const char* path)
{
    std::string p = path;
    size_t k = p.find_last_of('/');
    if (k != std::string::npos) p = p.substr(k + 1);
    k = p.find_last_of('.');
    if (k != std::string::npos) p = p.substr(0, k);
    for (char& c : p) if (c == ' ') c = '_';
    return p;
}

static double argd(int argc, char** argv, const char* key, double def)
{
    const size_t n = strlen(key);
    for (int i = 1; i < argc; ++i)
        if (!strncmp(argv[i], key, n) && argv[i][n] == '=') return atof(argv[i] + n + 1);
    return def;
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: variants MODEL_DIR OUT_DIR STILL.png [...] "
                        "[--lut=PATH] [--camera=N] [--encode=N] [--width=N]\n");
        return 2;
    }
    const char* modelDir = argv[1];
    const std::string outDir = argv[2];
    const int cam = (int)argd(argc, argv, "--camera", og::grade::kCreativeCamera);
    const int enc = (int)argd(argc, argv, "--encode", og::grade::kCreativeEncode);
    const int outW = (int)argd(argc, argv, "--width", 1280);
    const char* lutPath = nullptr;
    for (int i = 1; i < argc; ++i) if (!strncmp(argv[i], "--lut=", 6)) lutPath = argv[i] + 6;

    CubeLUT lut;
    if (lutPath && !lut.load(lutPath)) fprintf(stderr, "warn: could not load %s\n", lutPath);
    const float* lutData = lut.valid() ? lut.data.data() : nullptr;
    const int lutSize = lut.valid() ? lut.size : 0;

    og::seg::Segmenter seg;
    {
        const std::string pp = std::string(modelDir) + "/ade20k.param";
        const std::string bp = std::string(modelDir) + "/ade20k.bin";
        if (!seg.load(pp, bp)) fprintf(stderr, "warn: no model at %s -- base grade will be weaker\n", modelDir);
    }

    std::string manifest = "{\n  \"stills\": [\n";
    bool firstStill = true;
    og::grade::Tunables tun;

    for (int i = 3; i < argc; ++i) {
        if (argv[i][0] == '-') continue;
        Frame f;
        if (!load_log(argv[i], f)) { fprintf(stderr, "cannot read %s\n", argv[i]); continue; }
        const std::string stem = stem_of(argv[i]);

        oga::SampleSet S;
        og::grade::Measurements m = measure(f, cam, enc, S);

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

        // The base is the real thing: same call, same order, same tunables the plugin uses.
        const og::grade::MagicResult R =
            og::grade::solve_magic(S, tsrc, m, cam, enc, lutData, lutSize, tun, 0, 1.0, false, segfn);

        const int outH = (int)((long long)f.h * outW / f.w);
        std::vector<unsigned char> img((size_t)outW * outH * 3);

        auto render = [&](const float* P, const std::string& file) {
            // Box-average each output pixel over its source footprint: a nearest-neighbour
            // downscale of a 4K frame aliases badly, and judging grain artefacts instead of the
            // grade would be its own content confound.
            const int sx = std::max(1, f.w / outW), sy = std::max(1, f.h / outH);
            for (int y = 0; y < outH; ++y) {
                const int y0 = (int)((long long)y * f.h / outH);
                for (int x = 0; x < outW; ++x) {
                    const int x0 = (int)((long long)x * f.w / outW);
                    double acc[3] = {0, 0, 0}; int n = 0;
                    for (int dy = 0; dy < sy; ++dy) {
                        const int yy = std::min(f.h - 1, y0 + dy);
                        for (int dx = 0; dx < sx; ++dx) {
                            const int xx = std::min(f.w - 1, x0 + dx);
                            const float* q = &f.px[((size_t)yy * f.w + xx) * 3];
                            float r, g, b;
                            full_chain(cam, enc, P, lutData, lutSize, 1.f, q[0], q[1], q[2], r, g, b);
                            acc[0] += r; acc[1] += g; acc[2] += b; ++n;
                        }
                    }
                    for (int k = 0; k < 3; ++k)
                        img[((size_t)y * outW + x) * 3 + k] =
                            (unsigned char)(og::clamp01((float)(acc[k] / n)) * 255.f + 0.5f);
                }
            }
            stbi_write_jpg((outDir + "/" + file).c_str(), outW, outH, 3, img.data(), 90);
        };

        if (!firstStill) manifest += ",\n";
        firstStill = false;
        manifest += "    {\"stem\": \"" + stem + "\", \"variants\": [\n";

        char buf[512];
        const std::string base = stem + "__base.jpg";
        render(R.P, base);
        snprintf(buf, sizeof buf, "      {\"file\": \"%s\", \"axis\": \"base\", \"dir\": 0}",
                 base.c_str());
        manifest += buf;

        for (int a = 0; a < kAxisN; ++a) {
            for (int dir = -1; dir <= 1; dir += 2) {
                float P[oga::kParamN];
                for (int k = 0; k < oga::kParamN; ++k) P[k] = R.P[k];
                P[kAxes[a].idx] += dir * kAxes[a].delta;
                // Gamma and gain are multiplicative and must stay positive; the others are free.
                if (kAxes[a].idx == 4 || kAxes[a].idx == 5) P[kAxes[a].idx] = std::max(0.05f, P[kAxes[a].idx]);
                const std::string file = stem + "__" + kAxes[a].name + (dir < 0 ? "_lo" : "_hi") + ".jpg";
                render(P, file);
                snprintf(buf, sizeof buf,
                         ",\n      {\"file\": \"%s\", \"axis\": \"%s\", \"dir\": %d}",
                         file.c_str(), kAxes[a].name, dir);
                manifest += buf;
            }
        }
        manifest += "\n    ]}";
        printf("%-30s base + %d variants%s\n", stem.c_str(), kAxisN * 2,
               R.tone.ok ? "" : "   (tone declined -- base is Creative only)");
        fflush(stdout);
    }
    manifest += "\n  ]\n}\n";

    const std::string mp = outDir + "/manifest.json";
    FILE* mf = fopen(mp.c_str(), "w");
    if (!mf) { fprintf(stderr, "cannot write %s\n", mp.c_str()); return 3; }
    fwrite(manifest.data(), 1, manifest.size(), mf);
    fclose(mf);
    printf("\nwrote %s\n", mp.c_str());
    return 0;
}
