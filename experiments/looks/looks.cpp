// OneGrade — reference-still measurement, for the Look targets.
//
// Graded reference stills in, tone targets out, plus the one number that decides whether the
// feature is worth building: how far apart the looks actually sit.
//
// ---------------------------------------------------------------------------------------
// WHY THE INPUT IS THE OPPOSITE OF THE BENCH'S
//
// experiments/bench feeds the plugin CAMERA LOG, because that is what OFX hands it. This tool
// feeds it FINISHED PICTURES, because a look is a property of the output and cannot be seen in a
// log frame at all -- every shot is the same flat grey going in. A reference still is already the
// post-LUT display picture that solve_magic_tone() places its targets in, so its code values are
// directly the numbers the solve consumes. Nothing is decoded, rendered or graded here.
//
// Keeping the two tools separate is deliberate. Passing one image to both roles is the exact
// mistake that cost this project the black-point encode bug (docs/AUTO-GRADE.md 2) and the bench's
// own copy of it: one encode standing in for two different questions.
//
// ---------------------------------------------------------------------------------------
// IT DOES NOT REIMPLEMENT THE MEASUREMENT
//
// The five points a tone target is made of come from og::grade::pick_tone_samples(), the same
// function solve_magic_tone() picks with -- subject p10/p50/p90 ranked by luma, frame p99.9/p0.1
// ranked by max channel. Region identity comes from og::seg::Segmenter and the subject from
// og::analysis::magic_decide(), so a reference is measured with the subject the plugin would
// actually choose on it. On this feature every bug that survived more than a few minutes was a
// paraphrase of something that already existed.
//
// The one thing computed differently is the per-region Lab, and legitimately: region_stats() gets
// there by rendering camera log through og::process(), which a finished still has already had done
// to it. Same display_to_Lab(), one stage earlier.
//
// ---------------------------------------------------------------------------------------
// THE QUESTION THIS EXISTS TO ANSWER
//
// The workflow is: press Magic Grade, then cycle looks until one is right. That only works if the
// looks are meaningfully different, so the headline output is a SEPARABILITY table -- the gap
// between two looks' medians measured in units of their own within-look spread. Under about 2 the
// two looks are inside each other's noise and cycling between them would not read as anything.
//
// If they do not separate on these four numbers, the answer is more AXES (the colour/separation
// triple, the print stock) rather than more stills.
//
// USAGE
//   looks MODEL_DIR LOOK_DIR [LOOK_DIR ...]  [--encode=N] [--min-cover=P] [--no-crop]
//                                            [--crop=T] [--region=NAME] [--csv] [--all-rows]
//
// Each LOOK_DIR is one look; its folder name is the label. Put ~30 stills in each.
#include <cmath>
#include <cstring>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "OneGradePipeline.h"
#include "OneGradeAnalysis.h"
#include "OneGradeCreative.h"
#include "OneGradeSegment.h"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace oga = og::analysis;
namespace ogg = og::grade;

// Below this many stills per look, per region, a separability verdict is not reported at all.
// Learned from the null test rather than chosen: splitting ONE look in half gave n=3 on SKIN and
// the first metric called the two halves distinct.
static const size_t kMinSeparable = 8;

// How large the effect has to be to count. 0.60 means one look sits above the other in 80% of
// cross pairs -- a large effect, which is the right bar for "cycling looks reads as a change".
static const double kDistinctEffect = 0.60;

// ---------------------------------------------------------------------------------------
// A still, in IMAGE convention throughout: row 0 is the top.
//
// The plugin carries an OFX bottom-up coordinate and flips it when it reads the mask, which
// assign_regions() warns is invisible if you get it backwards -- sky ends up underfoot and every
// statistic stays plausible. This tool never enters OFX convention at all, so there is no flip to
// get wrong rather than a flip that happens to be right.
struct Still {
    int w = 0, h = 0;
    std::vector<unsigned char> px;   // w*h*3, display-referred 8-bit
};

static bool load_still(const char* path, Still& s)
{
    int c = 0;
    unsigned char* d = stbi_load(path, &s.w, &s.h, &c, 3);
    if (!d) return false;
    s.px.assign(d, d + (size_t)s.w * s.h * 3);
    stbi_image_free(d);
    return true;
}

// LETTERBOX BARS ARE NOT SHADOW DETAIL, and left in they destroy the two frame numbers.
//
// A film still is usually matted to 2.39:1 inside a 16:9 file. Those bars are hard zeros, so the
// frame's p0.1 -- the darkest channel of the darkest pixel, which is what frameFloorMax is
// compared against -- reads 0.000 on every still that has them, and the measured floor becomes a
// property of the matte rather than of the grade. Cropping is not tidying, it is the difference
// between measuring the picture and measuring the container.
//
// Reported rather than silent: a still cropped to nothing, or not cropped when it should have
// been, is something to see rather than to discover later in an aggregate.
static void crop_bars(Still& s, double thresh)
{
    if (s.w <= 0 || s.h <= 0) return;
    const int t = (int)(thresh * 255.0 + 0.5);
    auto rowDark = [&](int y) {
        for (int x = 0; x < s.w; ++x) {
            const unsigned char* p = &s.px[((size_t)y * s.w + x) * 3];
            if (p[0] > t || p[1] > t || p[2] > t) return false;
        }
        return true;
    };
    auto colDark = [&](int x) {
        for (int y = 0; y < s.h; ++y) {
            const unsigned char* p = &s.px[((size_t)y * s.w + x) * 3];
            if (p[0] > t || p[1] > t || p[2] > t) return false;
        }
        return true;
    };
    int y0 = 0, y1 = s.h - 1, x0 = 0, x1 = s.w - 1;
    while (y0 < y1 && rowDark(y0)) ++y0;
    while (y1 > y0 && rowDark(y1)) --y1;
    while (x0 < x1 && colDark(x0)) ++x0;
    while (x1 > x0 && colDark(x1)) --x1;
    const int nw = x1 - x0 + 1, nh = y1 - y0 + 1;
    if (nw == s.w && nh == s.h) return;
    if (nw < 64 || nh < 64) return;                      // a nearly-black still, not a matte
    std::vector<unsigned char> out((size_t)nw * nh * 3);
    for (int y = 0; y < nh; ++y)
        memcpy(&out[(size_t)y * nw * 3], &s.px[((size_t)(y + y0) * s.w + x0) * 3], (size_t)nw * 3);
    s.px.swap(out); s.w = nw; s.h = nh;
}

// One still's measurement, for one region.
struct Row {
    std::string file;
    int    region = oga::R_OTHER;
    double cover = 0.0;
    double floor = 0.0, mid = 0.0, hi = 0.0;   // the subject triple, luma
    double ceil = 0.0, fLo = 0.0;              // frame max-channel p99.9, min-channel p0.1
    double dL = 0.0, da = 0.0, db = 0.0;       // this region minus everything else
    bool   chosen = false;                     // magic_decide() would pick this one
    bool   viable = true;                      // solve_magic_tone() would accept it as a subject
    const char* why = "";                      // ...and if not, which of its rules said no
};

// MEASURE A REFERENCE ONLY WHERE THE PLUGIN WOULD HAVE ACCEPTED IT AS A SUBJECT.
//
// The first pass over real film stills made the case on its own. ADE20K class 12 is "person" --
// the whole body, wardrobe and hair -- not "face", and in wide narrative framing that region is
// mostly dark clothing. One still came back SKIN 25.7% with p10, p50 and p90 ALL at 0.008: a flat
// silhouette with no tonal range at all, reported as a subject midtone.
//
// Averaging those into a look would fit subjMid to how films frame people rather than to where a
// face belongs, and the target it feeds was measured on an interview where the region genuinely
// is a lit face. Same quantity by name, different thing entirely.
//
// The filter is not invented for this tool: it is the two tests solve_magic_tone() already
// applies, so a still contributes to a look exactly when the plugin would have graded to it.
// No new constant, and nothing to keep in step by hand.
static bool subject_viable(double cover, double mid, const char** why)
{
    if (cover > 35.0)  { *why = "region too large to be one"; return false; }
    if (mid   < 0.01)  { *why = "black, not dark";            return false; }
    return true;
}

static double median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n & 1) ? v[n/2] : 0.5 * (v[n/2 - 1] + v[n/2]);
}

// MEDIAN ABSOLUTE DEVIATION, not standard deviation. Same reason the whole project uses
// percentiles: one atypical still in a folder of thirty should not decide how wide the look is.
static double mad(const std::vector<double>& v)
{
    if (v.size() < 2) return 0.0;
    const double m = median(v);
    std::vector<double> d; d.reserve(v.size());
    for (double x : v) d.push_back(std::fabs(x - m));
    return median(d) * 1.4826;                            // scaled to be comparable to a sigma
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
static const char* args(int argc, char** argv, const char* key, const char* def)
{
    const size_t n = strlen(key);
    for (int i = 1; i < argc; ++i)
        if (!strncmp(argv[i], key, n) && argv[i][n] == '=') return argv[i] + n + 1;
    return def;
}

static bool is_image(const std::string& f)
{
    auto ends = [&](const char* e) {
        const size_t n = strlen(e);
        if (f.size() < n) return false;
        for (size_t i = 0; i < n; ++i)
            if (tolower(f[f.size()-n+i]) != e[i]) return false;
        return true;
    };
    return ends(".jpg") || ends(".jpeg") || ends(".png") || ends(".webp") || ends(".tif") || ends(".tiff");
}

static std::vector<std::string> list_dir(const std::string& dir)
{
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        std::string f = e->d_name;
        if (f.empty() || f[0] == '.') continue;
        if (is_image(f)) out.push_back(dir + "/" + f);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

static std::string basename_of(std::string p)
{
    while (!p.empty() && p.back() == '/') p.pop_back();
    const size_t k = p.find_last_of('/');
    return (k == std::string::npos) ? p : p.substr(k + 1);
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: looks MODEL_DIR LOOK_DIR [LOOK_DIR ...] [--encode=N] "
                        "[--min-cover=P] [--crop=T] [--no-crop] [--region=NAME] [--csv]\n");
        return 2;
    }
    const char* modelDir = argv[1];

    // The still is a Rec.709 display picture, so Lab needs a display-referred encode. 1 is
    // Rec.709 Gamma 2.2, which is both the plugin's default output and what a web JPEG is closest
    // to. Same fallback rule as probeAnalyze: only 0/1/2 are display-referred.
    const int    enc      = (int)argd(argc, argv, "--encode", 1);
    const double minCover = argd(argc, argv, "--min-cover", 2.0);
    const double cropT    = argd(argc, argv, "--crop", 0.02);
    const bool   noCrop   = argf(argc, argv, "--no-crop");
    const bool   csv      = argf(argc, argv, "--csv");
    const char*  onlyReg  = args(argc, argv, "--region", "");
    // Off by default: a still the plugin would refuse to grade to is not evidence about where a
    // subject belongs. On, for inspecting what the filter is removing.
    const bool   allRows  = argf(argc, argv, "--all-rows");

    og::seg::Segmenter seg;
    {
        const std::string pp = std::string(modelDir) + "/ade20k.param";
        const std::string bp = std::string(modelDir) + "/ade20k.bin";
        if (!seg.load(pp, bp)) {
            fprintf(stderr, "error: no segmentation model at %s\n", modelDir);
            fprintf(stderr, "       a look's numbers only mean something per subject class, so\n"
                            "       there is nothing useful to measure without it.\n");
            return 3;
        }
    }

    std::vector<std::string> lookDirs;
    for (int i = 2; i < argc; ++i) {
        if (argv[i][0] == '-') continue;
        struct stat st;
        if (!stat(argv[i], &st) && S_ISDIR(st.st_mode)) lookDirs.push_back(argv[i]);
    }
    if (lookDirs.empty()) { fprintf(stderr, "error: no look folders given\n"); return 2; }

    if (csv) printf("look,file,region,cover,floor,mid,hi,ceil,flo,dL,da,db,chosen\n");

    std::vector<std::string> lookName;
    std::vector<std::vector<Row>> lookRows;

    for (const std::string& dir : lookDirs) {
        const std::string name = basename_of(dir);
        std::vector<std::string> files = list_dir(dir);
        std::vector<Row> rows;
        if (!csv) printf("\n=== %s  (%zu stills) ===\n", name.c_str(), files.size());

        for (const std::string& path : files) {
            Still s;
            if (!load_still(path.c_str(), s)) {
                fprintf(stderr, "warn: cannot read %s\n", path.c_str());
                continue;
            }
            const int w0 = s.w, h0 = s.h;
            if (!noCrop) crop_bars(s, cropT);
            const bool cropped = (s.w != w0 || s.h != h0);

            // Same subsampling rule as probeAnalyze and the bench (~200k samples). Coverage
            // percentages feed magic_decide, and v1.4.2 was a bug where the plugin and the
            // offline tools sampled at different densities and disagreed about coverage by half
            // a point -- which on a close subject call was the entire decision.
            const int step = std::max(1, (int)(std::sqrt((double)(s.w * s.h) / 200000.0) + 0.5));

            std::vector<unsigned char> mask;
            int mw = 0, mh = 0;
            if (!seg.run(s.px.data(), s.w, s.h, mask, mw, mh)) {
                fprintf(stderr, "warn: segmentation failed on %s\n", path.c_str());
                continue;
            }

            std::vector<float> R, G, B;
            std::vector<unsigned char> reg;
            for (int y = 0; y < s.h; y += step) {
                for (int x = 0; x < s.w; x += step) {
                    const unsigned char* p = &s.px[((size_t)y * s.w + x) * 3];
                    R.push_back(p[0] / 255.f); G.push_back(p[1] / 255.f); B.push_back(p[2] / 255.f);
                    // Nearest-neighbour, top-down on both sides. Labels are not quantities, so
                    // interpolating between "sky" and "water" would invent a region.
                    int mx = (int)((long long)x * mw / s.w), my = (int)((long long)y * mh / s.h);
                    mx = mx < 0 ? 0 : (mx >= mw ? mw - 1 : mx);
                    my = my < 0 ? 0 : (my >= mh ? mh - 1 : my);
                    reg.push_back(mask[(size_t)my * mw + mx]);
                }
            }
            const size_t n = R.size();
            if (n < 64) continue;

            // Per-region coverage and mean Lab, straight off the finished picture. This is what
            // region_stats() computes; it just has to render camera log first to get here.
            oga::RegionStat st[oga::kRegionN];
            {
                double sL[oga::kRegionN] = {0}, sa[oga::kRegionN] = {0}, sb[oga::kRegionN] = {0};
                long long cnt[oga::kRegionN] = {0};
                for (size_t i = 0; i < n; ++i) {
                    float L, a, bb;
                    oga::display_to_Lab(enc, R[i], G[i], B[i], L, a, bb);
                    const int k = reg[i] < oga::kRegionN ? reg[i] : oga::R_OTHER;
                    sL[k] += L; sa[k] += a; sb[k] += bb; ++cnt[k];
                }
                for (int k = 0; k < oga::kRegionN; ++k) {
                    if (!cnt[k]) continue;
                    st[k].cover = 100.f * (float)cnt[k] / (float)n;
                    st[k].L = (float)(sL[k]/cnt[k]); st[k].a = (float)(sa[k]/cnt[k]);
                    st[k].b = (float)(sb[k]/cnt[k]);
                }
            }
            const oga::MagicChoice choice = oga::magic_decide(st, 0);

            if (!csv) {
                printf("%-34s %dx%d%s\n", basename_of(path).c_str(), s.w, s.h,
                       cropped ? "  (bars cropped)" : "");
            }

            // Every region above the floor, not just the chosen one. The target table is indexed
            // by subject class -- target[look][region] -- so a landscape still is only useful if
            // TERRAIN and SKY are measured, and the roadmap's "Magic Tone beyond faces" wants
            // exactly these rows.
            for (int r = 0; r < oga::kRegionN; ++r) {
                if (st[r].cover < minCover) continue;
                if (*onlyReg && strcmp(oga::region_name(r), onlyReg)) continue;

                const ogg::TonePick pk = ogg::pick_tone_samples(n, reg.data(), r,
                    [&](size_t i, float& rr, float& gg, float& bpx) {
                        rr = R[i]; gg = G[i]; bpx = B[i];
                    });
                if (!pk.ok) continue;

                Row row;
                row.file   = basename_of(path);
                row.region = r;
                row.cover  = st[r].cover;
                row.floor  = ogg::tone_luma(R[pk.iLo],  G[pk.iLo],  B[pk.iLo]);
                row.mid    = ogg::tone_luma(R[pk.iMid], G[pk.iMid], B[pk.iMid]);
                row.hi     = ogg::tone_luma(R[pk.iHi],  G[pk.iHi],  B[pk.iHi]);
                row.ceil   = ogg::tone_hi  (R[pk.iTop], G[pk.iTop], B[pk.iTop]);
                row.fLo    = ogg::tone_lo  (R[pk.iBot], G[pk.iBot], B[pk.iBot]);

                // This region against everything else, as the three SIGNED Lab components.
                // Signed on purpose: a distance cannot be solved against, and predicted the wrong
                // sign outright when it was tried (docs/AUTO-GRADE.md 9).
                {
                    double oL = 0, oa = 0, ob = 0; long long on = 0;
                    for (int k = 0; k < oga::kRegionN; ++k) {
                        if (k == r || st[k].cover <= 0.f) continue;
                        const long long c = (long long)(st[k].cover * 1000.f);
                        oL += st[k].L * c; oa += st[k].a * c; ob += st[k].b * c; on += c;
                    }
                    if (on) { row.dL = st[r].L - oL/on; row.da = st[r].a - oa/on; row.db = st[r].b - ob/on; }
                }
                row.chosen = choice.ok && choice.subject == r;
                row.viable = subject_viable(row.cover, row.mid, &row.why);
                rows.push_back(row);

                if (csv) {
                    printf("%s,%s,%s,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%d\n",
                           name.c_str(), row.file.c_str(), oga::region_name(r), row.cover,
                           row.floor, row.mid, row.hi, row.ceil, row.fLo,
                           row.dL, row.da, row.db, row.chosen ? 1 : 0);
                } else {
                    printf("   %-8s %5.1f%%%s  floor %.3f  mid %.3f  hi %.3f  spr %.3f | "
                           "ceil %.3f  fLo %.3f | dL %+6.1f da %+5.1f db %+5.1f%s%s\n",
                           oga::region_name(r), row.cover, row.chosen ? " *" : "  ",
                           row.floor, row.mid, row.hi, row.hi - row.floor,
                           row.ceil, row.fLo, row.dL, row.da, row.db,
                           row.viable ? "" : "   EXCLUDED: ", row.viable ? "" : row.why);
                }
            }
        }
        lookName.push_back(name);
        lookRows.push_back(rows);
    }

    if (csv) return 0;

    // ---------------------------------------------------------------------------------------
    // THE AGGREGATE, per (look, region). This is the target table the plugin would carry.
    printf("\n\n================ TARGETS  (median, MAD in brackets) ================\n");
    printf("A look is only usable for a region it has stills of -- an interview folder\n"
           "says nothing about TERRAIN, and guessing is what destroyed the beach frame.\n\n");
    for (size_t l = 0; l < lookName.size(); ++l) {
        printf("%s\n", lookName[l].c_str());
        for (int r = 0; r < oga::kRegionN; ++r) {
            std::vector<double> f, m, c, fl;
            size_t dropped = 0;
            for (const Row& row : lookRows[l]) if (row.region == r) {
                if (!row.viable && !allRows) { ++dropped; continue; }
                f.push_back(row.floor); m.push_back(row.mid);
                c.push_back(row.ceil);  fl.push_back(row.fLo);
            }
            // NEVER SILENTLY. A corpus that quietly discards half its stills reads as a thin
            // source rather than as a filter doing its job -- the most repeated bug shape in this
            // project is a resource that degrades without saying so.
            if (dropped && f.size() < 3)
                printf("   %-8s  all %zu excluded (not usable as a subject)\n",
                       oga::region_name(r), dropped);
            if (f.size() < 3) continue;          // fewer than three is not a median
            printf("   %-8s n=%-3zu subjFloor %.3f (%.3f)  subjMid %.3f (%.3f)  "
                   "frameCeiling %.3f (%.3f)  frameFloor %.3f (%.3f)\n",
                   oga::region_name(r), f.size(),
                   median(f), mad(f), median(m), mad(m),
                   median(c), mad(c), median(fl), mad(fl));
        }
    }

    // ---------------------------------------------------------------------------------------
    // SEPARABILITY -- the go/no-go. The workflow is "press Magic Grade, then cycle looks", which
    // only works if a look change is visible. Gap between two medians, in units of the looks' own
    // within-look spread: under about 2 they overlap and cycling would not read as anything.
    //
    // Reported per region because the comparison is only meaningful within a subject class.
    if (lookName.size() >= 2) {
        printf("\n================ SEPARABILITY ================\n");
        printf("effect = P(a random still from A sits above one from B), rescaled so 0.00 is\n"
               "indistinguishable and 1.00 never overlaps. Rank-based ON PURPOSE: the first\n"
               "version of this divided the gap by the MAD, and a random split of ONE look\n"
               "reported 3.6 sigma on SKIN -- with n=3 the MAD collapses toward zero and the\n"
               "ratio explodes. A go/no-go number that says yes to noise is worse than none.\n");
        const char* axis[4] = { "subjFloor", "subjMid", "frameCeiling", "frameFloor" };
        for (int r = 0; r < oga::kRegionN; ++r) {
            for (size_t i = 0; i < lookName.size(); ++i)
                for (size_t j = i + 1; j < lookName.size(); ++j) {
                    std::vector<double> A[4], Bv[4];
                    for (const Row& row : lookRows[i]) if (row.region == r) {
                        if (!row.viable && !allRows) continue;
                        A[0].push_back(row.floor); A[1].push_back(row.mid);
                        A[2].push_back(row.ceil);  A[3].push_back(row.fLo);
                    }
                    for (const Row& row : lookRows[j]) if (row.region == r) {
                        if (!row.viable && !allRows) continue;
                        Bv[0].push_back(row.floor); Bv[1].push_back(row.mid);
                        Bv[2].push_back(row.ceil);  Bv[3].push_back(row.fLo);
                    }
                    const size_t na = A[0].size(), nb = Bv[0].size();
                    if (!na || !nb) continue;
                    printf("\n  [%s] %s (n=%zu) vs %s (n=%zu)\n", oga::region_name(r),
                           lookName[i].c_str(), na, lookName[j].c_str(), nb);

                    // A HARD FLOOR ON n, not a soft warning. Below this the effect size is being
                    // read off a handful of pairs and will happily report a large one by chance.
                    if (na < kMinSeparable || nb < kMinSeparable) {
                        printf("     too few stills to judge -- need %d each\n", (int)kMinSeparable);
                        continue;
                    }
                    double best = 0.0;
                    for (int k = 0; k < 4; ++k) {
                        // Common-language effect size: the share of cross pairs where A exceeds B.
                        // No denominator, so nothing can blow up; defined at any n; and it is the
                        // question the workflow actually asks -- pick a still from each look, how
                        // often does the look decide which is brighter?
                        size_t above = 0, ties = 0;
                        for (double a : A[k]) for (double b : Bv[k]) {
                            if (a > b) ++above; else if (a == b) ++ties;
                        }
                        const double p = ((double)above + 0.5 * (double)ties) / ((double)na * nb);
                        const double e = std::fabs(2.0 * p - 1.0);
                        printf("     %-13s %.3f vs %.3f   d %+.3f   effect %.2f\n",
                               axis[k], median(A[k]), median(Bv[k]),
                               median(A[k]) - median(Bv[k]), e);
                        if (e > best) best = e;
                    }
                    printf("     -> %s\n", best >= kDistinctEffect ? "DISTINCT" : "overlapping");
                }
        }
        printf("\nOverlapping on every axis means the tone quadruple alone cannot carry these\n"
               "looks apart. That is an argument for more AXES -- the colour/separation triple,\n"
               "or a different print stock per look -- not for more stills.\n");
    }
    return 0;
}
