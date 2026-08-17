// OneGrade — CPU-only frame ANALYSIS: scene descriptors and the control Jacobian.
// Copyright (C) 2026 Matthew Grdinic
// SPDX-License-Identifier: GPL-3.0-or-later
//
// THIS FILE IS NOT PART OF THE GOLDEN RULE. OneGradePipeline.h is the single source of truth
// for colour math and the three GPU kernels mirror it exactly; NOTHING here is mirrored, and
// nothing here may ever be called from a kernel. It runs once per button press, on the CPU,
// over a few thousand samples, and it produces PARAMETER VALUES rather than pixels. Separate
// header and separate namespace so that boundary is structural instead of a comment someone
// has to read.
//
// ---------------------------------------------------------------------------------------
// WHY THIS EXISTS
//
// probeAnalyze() already measures exposure well enough to drive Gain and Rolloff. What it
// cannot answer is anything about COLOUR: on a sunset-over-ocean frame the user's own grade
// pushed Offset Temp negative to separate the water from the sky, and no number the plugin
// measured could have asked for that. Two pieces are missing, and they are different kinds
// of thing:
//
//   1. DESCRIPTORS — a small vector that says what the frame currently looks like, including
//      where its dominant colour populations sit and how far apart they are. `Desc` below.
//
//   2. THE JACOBIAN — how each control moves each descriptor, ON THIS FOOTAGE. This is the
//      part that replaces writing down what the sliders mean. We do not tell the system that
//      "negative Offset Temp adds blue"; we perturb Offset Temp, measure that b* fell, and
//      invert. A written description would drift the first time the pipeline changed and
//      could not be tested — the same reason og_solve() uses bisection instead of a closed
//      form. A measured one cannot drift, and it is shot-dependent for free: Offset Temp
//      does something quite different to a saturated sunset than to a snowfield.
//
// ---------------------------------------------------------------------------------------
// THE ONE RULE THAT MAKES THE NUMBERS MEAN ANYTHING: MEMBERSHIP IS FIXED AT NEUTRAL.
//
// Every mask here — the mid-tone window, the skin mask, which of the two colour populations
// a pixel belongs to — is decided ONCE, from the neutral render, by classify(). describe()
// then only ever recomputes STATISTICS over those fixed memberships.
//
// This is not an optimisation. It is the same trap the skin mask already fell into once: a
// selection rule that constrains the quantity being measured produces a number describing
// the filter rather than the footage. If the mid-tone window were re-selected after every
// perturbation, "the mids got brighter" would be unmeasurable by construction, because the
// window would slide along with them. Fixing membership also makes every descriptor a smooth
// function of the parameters, which is what a finite-difference Jacobian needs — otherwise
// pixels hop between clusters as you nudge a slider and the derivative is noise.
//
// Cluster membership is a property of the FOOTAGE. The grade is what we are differentiating.
#pragma once
#include "OneGradePipeline.h"
#include <vector>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <algorithm>

namespace og {
namespace analysis {

static const int kParamN = 21;   // matches P[] in OneGradePipeline.h

// ---------------------------------------------------------------------------------------
// CIELAB, for the colour half of the descriptor set.
//
// HSV (what the pipeline uses for Density) is the wrong space to MEASURE a cast in: its hue
// is an angle on a hexagon, its saturation is scale-dependent, and neither has a warm-cool
// axis. Lab gives one directly — b* IS warm/cool and a* IS green/magenta, which lines the
// descriptors up with Offset Temp and Offset Tint one-for-one and keeps the Jacobian
// well-conditioned instead of smearing one control across several rows.
static const float k709_to_XYZ[9] = {
    0.4123908f, 0.3575843f, 0.1804808f,
    0.2126390f, 0.7151687f, 0.0721923f,
    0.0193308f, 0.1191948f, 0.9505322f
};

// Undo the output encode. Analysis only ever runs in a display-referred encode (probeAnalyze
// falls back to Gamma 2.2 when the effective encode is Cineon or DI), so these three cover it.
static inline float display_to_linear(int enc, float v)
{
    if (enc == 1) return og::r709_g_dec(v, 2.2f);
    if (enc == 2) return og::r709_g_dec(v, 2.4f);
    return og::r709_dec(v);                       // 0 = Rec.709 Scene OETF
}

static inline float lab_f(float t)
{
    const float d = 6.0f/29.0f;
    return (t > d*d*d) ? cbrtf(t) : (t/(3.0f*d*d) + 4.0f/29.0f);
}

// Encoded display RGB -> CIELAB (D65). L* 0..100, a*/b* roughly -128..128.
static inline void display_to_Lab(int enc, float r, float g, float b,
                                  float& L, float& a, float& bb)
{
    float lin[3] = { display_to_linear(enc, r), display_to_linear(enc, g), display_to_linear(enc, b) };
    float xyz[3]; og::mul33(k709_to_XYZ, lin, xyz);
    const float xn = 0.95047f, yn = 1.0f, zn = 1.08883f;
    float fx = lab_f(xyz[0]/xn), fy = lab_f(xyz[1]/yn), fz = lab_f(xyz[2]/zn);
    L  = 116.0f*fy - 16.0f;
    a  = 500.0f*(fx - fy);
    bb = 200.0f*(fy - fz);
}

// ---------------------------------------------------------------------------------------
// One sample all the way to the rendered picture: process() plus the trim stage the CALLER
// normally owns (postExp/postCon then the highlight softclip). Analysis has to include the
// trim or the Jacobian would report that Post Exposure does nothing.
//
// NO LUT. Deliberate, and the first limitation to revisit: a Creative grade renders through
// a print stock, so the magnitudes here are pre-stock. Direction is preserved (the stocks are
// monotone per channel) but the scale is not, which matters the moment we fit constants to a
// Creative-mode move. Adding it is a hook, not a redesign — sample the loaded cube between
// process() and apply_trim, exactly where the render does.
static inline void render_sample(int cam, int enc, const float* P,
                                 float inR, float inG, float inB,
                                 float& r, float& g, float& b)
{
    og::process(cam, enc, P, inR, inG, inB, r, g, b);
    og::apply_trim(P[8], P[9], r, g, b);
    const float rolloff = P[12];
    if (rolloff > 0.0f && enc <= 2) {             // display-referred only, as the render gates it
        r = og::softclip(r, rolloff);
        g = og::softclip(g, rolloff);
        b = og::softclip(b, rolloff);
    }
}

// ---------------------------------------------------------------------------------------
// THE DESCRIPTOR VECTOR — what the frame looks like, in twelve numbers.
//
// Chosen so that (a) each is something a colourist would actually name, and (b) each is a
// SMOOTH function of the parameters, so a finite-difference Jacobian is meaningful. That
// second constraint is why D_OVER is mean overshoot rather than the `hot` percentage the
// panel reports: a share-above-threshold is a step function, so its derivative is counting
// noise, while the amount by which the channels run past white is continuous.
// ---------------------------------------------------------------------------------------
// SIGNED AXES STEER. MAGNITUDES DO NOT. Measured on the beach sunset, neutral -> grade,
// linear prediction against measurement:
//
//     b*      signed axis                     5%   error
//     C*      magnitude sqrt(a^2 + b^2)     37-57%  error
//     sep     distance between centroids    WRONG SIGN on one of the two grades
//
// A distance cannot be linearised over a whole grade: it is a positive quantity built from
// squares, so the model has no way to express "these moved apart in a" cancelling "they moved
// together in b". `sep` predicted +1.1 where the measurement was -3.8.
//
// So the steerable set is signed components only. C and sep survive as REPORT-ONLY
// diagnostics — they measure fine, they just cannot be solved against.
//
// THE SEPARATION TRIPLE is the user's own definition of what makes a frame dynamic:
//   "push those objects to be more separated from others of a different hue OR TONE LEVEL"
// Two axes, not one. The original sep was a distance in (a*, b*) — hue and chroma only, with
// no tone axis in it at all. It is now the three signed components of the Lab difference
// between the frame's two regions: dL* is tone separation, da*/db* are hue separation.
//
// REGIONS ARE THE TOP AND BOTTOM THIRD, and that is a stand-in. It works on this footage —
// db* came back +43 and cleanly found sky-over-water where colour clustering returned two
// populations both at orange (h29 and h44) — because a landscape separates its objects by
// height. It fails the moment they do not: two people side by side, a car against a wall, a
// face against a window. THIS IS THE SEAM WHERE SEGMENTATION PLUGS IN — supplying real region
// masks changes nothing else in this file, because the descriptors only ever ask "region A
// minus region B".
enum {
    D_BLACK,    // per-channel 0.1st percentile — where the floor sits (what Base places)
    D_MID,      // luma median — the midtone
    D_WHITE,    // per-channel 99th percentile — a channel is what clips, not luma
    D_OVER,     // mean per-sample overshoot above 1.0 — smooth clipping pressure
    D_A,        // mean a* over mid-tones — green/magenta cast
    D_B,        // mean b* over mid-tones — warm/cool cast. THE one the sunset move needed.
    D_DL,       // TONE separation: region A minus region B in L*
    D_DA,       // HUE separation, green/magenta axis
    D_DB,       // HUE separation, warm/cool axis
    // The same triple asked of the SUBJECT against everything else, once a segmentation has
    // said which region the subject is. Separate descriptors rather than a redefinition of the
    // three above, so the band version and everything fitted against it stay exactly as they
    // were and the two can be compared on the same frame. Zero when there is no subject.
    D_RDL,      // TONE separation: subject minus surround in L*
    D_RDA,      // HUE separation, green/magenta axis
    D_RDB,      // HUE separation, warm/cool axis
    D_SKINL,    // luma median over the skin mask (0 when coverage is too low to trust)
    D_SKINB,    // b* over the skin mask (0 when coverage is too low to trust)
    // --- report-only below: measured honestly, but NOT steerable. See the note above. ---
    D_CHROMA,   // mean C* over mid-tones — overall colourfulness, what Density acts on
    D_SEP,      // ab distance between the two dominant colour populations
    kDescN
};

// Everything from here up is a signed axis and safe to put in a solve. Callers should weight
// D_CHROMA and D_SEP at zero — they are diagnostics.
static const int kSteerableDescN = D_SKINB + 1;

static inline const char* desc_name(int i)
{
    static const char* n[kDescN] = { "black","mid","white","over","a*","b*",
                                     "dL*","da*","db*", "rdL*","rda*","rdb*",
                                     "skinL","skinb*","chroma","sep" };
    return (i >= 0 && i < kDescN) ? n[i] : "?";
}

struct Desc { float v[kDescN]; };

// Report-only numbers. Not steered on, not differentiated — these exist so the debug panel
// can show what the classification actually found, which is how a bad mask gets caught.
struct Extras {
    float hotPct    = 0.f;   // share above display white (the panel's existing `hot`)
    float skinPct   = 0.f;   // skin-mask coverage; high on a landscape means it matched sand
    float share[2]  = {0.f, 0.f};   // population shares, cool-first
    float hue[2]    = {0.f, 0.f};   // population hue angles in degrees
    float chroma[2] = {0.f, 0.f};   // population chroma
    bool  skinOk    = false; // whether the skin descriptors carry any weight
};

// ---------------------------------------------------------------------------------------
// The sampled frame. `rgb` and `band` are filled by the caller (only it knows the geometry);
// everything else is assigned once by classify().
struct SampleSet {
    std::vector<float>   rgb;    // 3N source (camera-log) values
    std::vector<uint8_t> band;   // N: 0 bottom third, 1 middle, 2 top
    std::vector<uint8_t> group;  // N: 0 cooler population, 1 warmer, 2 excluded
    std::vector<uint8_t> mid;    // N: 1 if inside the mid-tone window
    std::vector<uint8_t> skin;   // N: 1 if inside the skin mask
    std::vector<uint8_t> region; // N: semantic region — see Region below
    // Normalised position, 0..1, ORIGIN BOTTOM-LEFT to match OFX. Needed so a segmentation
    // mask -- which is a picture, and therefore top-down -- can be read at each sample's
    // location. The flip is the whole reason these are stored rather than recomputed: getting
    // it wrong turns sky into ground silently, and the numbers would all still look plausible.
    std::vector<float> u, v;
    // WHICH REGION IS THE SUBJECT, once something has decided -- -1 until then. Carried on the
    // sample set rather than passed to describe() so the Jacobian, which calls describe() a
    // couple of dozen times, cannot take its derivative around a different subject than the
    // operating point was measured with. Same reasoning as decimate() copying memberships
    // instead of re-deriving them.
    int subject = -1;
    size_t size() const { return band.size(); }
};

// ---------------------------------------------------------------------------------------
// SEMANTIC REGIONS — what the frame is made of, which is the one thing measurement cannot
// recover on its own.
//
// A luminance or chroma split can say WHERE a frame divides. It cannot say which way to push,
// because direction and permission are properties of what a thing IS: cooling the dark half is
// right when it is water and wrong when it is skin in shadow. That is the whole reason this
// exists, and the reason the offline experiment in experiments/segmentation/ concluded a
// classifier earns its place.
// GROUND is split out of BUILT because a grade treats a facade and the street below it
// differently -- they are lit by different things. Without the split a downward city view
// collapsed to one region (building 82.7% and ceiling 13.4% both landing in BUILT) and Magic
// Grade correctly reported nothing to separate in a frame that visibly has two of everything.
enum Region { R_SKY, R_WATER, R_SKIN, R_VEG, R_TERRAIN, R_GROUND, R_BUILT, R_OTHER, kRegionN };

static inline const char* region_name(int r)
{
    static const char* n[kRegionN] = { "SKY","WATER","SKIN","FOLIAGE","TERRAIN","GROUND","BUILT","OTHER" };
    return (r >= 0 && r < kRegionN) ? n[r] : "?";
}

// How much a region matters beyond its share of the frame. Coverage is most of importance --
// the user's rule -- but a face is the subject of a shot at 15% and a wall is not at 70%.
static inline float region_salience(int r)
{
    static const float s[kRegionN] = { 0.7f, 1.2f, 3.0f, 1.0f, 0.7f, 0.6f, 0.6f, 0.4f };
    return (r >= 0 && r < kRegionN) ? s[r] : 0.4f;
}

// Skin is never pushed. A move that cools the shadows also cools skin sitting in shadow, which
// is the most visible way to wreck a frame, so when skin is the subject the SURROUND moves
// instead. See magic_decide().
static inline bool region_protected(int r) { return r == R_SKIN; }

struct RegionStat { float cover = 0.f, L = 0.f, a = 0.f, b = 0.f; };

// Is the skin mask worth believing on this frame? A FLOOR ALONE IS NOT ENOUGH, which the
// first real frame proved: a 4K beach at 230k samples returned 46% coverage and sailed past a
// `count >= 200` gate, so descriptors describing sand and sky would have been handed to a
// solver marked trustworthy. The panel has always printed coverage precisely because a HIGH
// number means the mask failed — that lesson needs applying to the machine-readable path too,
// not just the human-readable one.
//
// The 25% ceiling separates every case measured so far: real faces came in at 3.4% and 10.3%,
// false positives at 39.7% (desert sand), 46.5% (cactus), 46% (this beach) and 72.2%. It is
// fitted to those six observations, so widen it if a genuine tight close-up ever lands above.
// The ceiling is a named constant because magic_decide() needs the SAME band to decide whether
// a skin region is a face or a false positive, and two copies of 0.25 would be one more pair of
// numbers meant to agree.
static const double kSkinMaxCover = 0.25;

static inline bool skin_trustworthy(long long skinN, size_t n)
{
    return skinN >= 200 && (double)skinN <= kSkinMaxCover * (double)n;
}

// Thin the set for the Jacobian, which pays 2 x kParamN describe() passes and does not need
// the full percentile resolution. MEMBERSHIPS ARE COPIED, NEVER RE-DERIVED: the derivative has
// to be taken around the same masks the operating point was measured with, or the two are not
// describing the same thing. Call after classify().
static inline SampleSet decimate(const SampleSet& S, size_t target)
{
    SampleSet D;
    const size_t n = S.size();
    if (n == 0 || target == 0) return D;
    const size_t k = (n > target) ? (n / target) : 1;
    for (size_t i = 0; i < n; i += k) {
        D.rgb.push_back(S.rgb[i*3]); D.rgb.push_back(S.rgb[i*3+1]); D.rgb.push_back(S.rgb[i*3+2]);
        D.band.push_back(S.band[i]);
        D.group.push_back(S.group.empty() ? 2 : S.group[i]);
        D.mid.push_back(S.mid.empty() ? 0 : S.mid[i]);
        D.skin.push_back(S.skin.empty() ? 0 : S.skin[i]);
        // `region` too, or region_stats() on a decimated set silently returns nothing: it
        // requires region.size() == size() and bails otherwise. That path is how Magic Grade
        // measures the chosen control's grip on the subject, so a missing copy here makes every
        // move come out at exactly zero — a feature that runs, reports, and does nothing.
        D.region.push_back(S.region.empty() ? (uint8_t)R_OTHER : S.region[i]);
        if (!S.u.empty()) { D.u.push_back(S.u[i]); D.v.push_back(S.v[i]); }
    }
    // Which region is the subject is a membership like any other, and the same argument applies:
    // dropping it would leave the Jacobian's describe() calls with no subject while the operating
    // point had one, so the separation triple would read a derivative of zero against a non-zero
    // value -- the "runs, reports, and does nothing" shape the region copy above was added for.
    D.subject = S.subject;
    return D;
}

// Fixed-membership pass. Everything that decides WHICH pixels a statistic is taken over
// happens here, once, on the neutral render — see the header note.
static inline Extras classify(SampleSet& S, int cam, int enc)
{
    const size_t n = S.size();
    Extras ex;
    S.group.assign(n, 2); S.mid.assign(n, 0); S.skin.assign(n, 0);
    if (n == 0) return ex;

    float P[kParamN] = {0.f,0.f,0.f, 0.f,1.f,1.f, 0.f,0.f, 0.f,1.f, 0.f,6500.f, 0.f};
    std::vector<float> la(n), lb(n);
    std::vector<uint32_t> pool; pool.reserve(n);
    long long hot = 0, skinN = 0;

    for (size_t i = 0; i < n; ++i) {
        float r, g, b;
        render_sample(cam, enc, P, S.rgb[i*3], S.rgb[i*3+1], S.rgb[i*3+2], r, g, b);
        const float Y = 0.2126f*r + 0.7152f*g + 0.0722f*b;
        if (Y > 1.0f) ++hot;

        float L, a, bb; display_to_Lab(enc, r, g, b, L, a, bb);
        la[i] = a; lb[i] = bb;

        // Mid-tone window, on the same encoded luma the existing panel numbers use, so the
        // thresholds stay comparable with every reading taken so far.
        if (Y > 0.15f && Y < 0.85f) S.mid[i] = 1;

        // Skin mask: the existing chromaticity-only window from probeAnalyze, unchanged.
        // It still cannot tell skin from sand — Extras::skinPct is the tell, and the two
        // skin descriptors are zeroed below when coverage is too low to mean anything.
        float h, s, v; og::rgb2hsv(r, g, b, h, s, v);
        if (h >= 0.01f && h <= 0.11f && s >= 0.10f && s <= 0.65f && v >= 0.03f && v <= 1.05f) {
            S.skin[i] = 1; ++skinN;
        }

        // Clustering pool: everything except crushed blacks and blown whites, where a
        // chromaticity carries no information.
        if (L > 5.0f && L < 99.0f) pool.push_back((uint32_t)i);
    }
    ex.hotPct  = 100.0f * (float)hot   / (float)n;
    ex.skinPct = 100.0f * (float)skinN / (float)n;
    ex.skinOk  = skin_trustworthy(skinN, n);

    if (pool.size() < 64) return ex;   // too little chromatic information to split

    // TWO-MEANS IN (a*, b*), SEEDED BY PCA so it is deterministic — no random init, so the
    // same frame always yields the same two populations and a Jacobian taken around it is
    // reproducible. Seeds go at +/- one standard deviation along the principal axis, which is
    // by construction the direction the frame's colour actually spreads in.
    double ma = 0, mb = 0;
    for (uint32_t i : pool) { ma += la[i]; mb += lb[i]; }
    ma /= (double)pool.size(); mb /= (double)pool.size();
    double sxx = 0, sxy = 0, syy = 0;
    for (uint32_t i : pool) {
        const double da = la[i] - ma, db = lb[i] - mb;
        sxx += da*da; sxy += da*db; syy += db*db;
    }
    sxx /= (double)pool.size(); sxy /= (double)pool.size(); syy /= (double)pool.size();
    const double tr = sxx + syy, det = std::sqrt((sxx - syy)*(sxx - syy) + 4.0*sxy*sxy);
    const double lam = 0.5*(tr + det);
    double ea, eb;
    if (std::fabs(sxy) > 1e-9) { ea = sxy; eb = lam - sxx; }
    else if (sxx >= syy)       { ea = 1.0; eb = 0.0; }
    else                       { ea = 0.0; eb = 1.0; }
    const double en = std::sqrt(ea*ea + eb*eb);
    if (en > 1e-12) { ea /= en; eb /= en; }
    const double sd = std::sqrt(lam > 0.0 ? lam : 0.0);

    double ca[2] = { ma - ea*sd, ma + ea*sd };
    double cb[2] = { mb - eb*sd, mb + eb*sd };
    std::vector<uint8_t> asg(pool.size(), 0);
    for (int it = 0; it < 12; ++it) {
        for (size_t k = 0; k < pool.size(); ++k) {
            const uint32_t i = pool[k];
            const double d0 = (la[i]-ca[0])*(la[i]-ca[0]) + (lb[i]-cb[0])*(lb[i]-cb[0]);
            const double d1 = (la[i]-ca[1])*(la[i]-ca[1]) + (lb[i]-cb[1])*(lb[i]-cb[1]);
            asg[k] = (d1 < d0) ? 1 : 0;
        }
        double sa[2] = {0,0}, sb[2] = {0,0}; long long cnt[2] = {0,0};
        for (size_t k = 0; k < pool.size(); ++k) {
            const uint32_t i = pool[k];
            sa[asg[k]] += la[i]; sb[asg[k]] += lb[i]; ++cnt[asg[k]];
        }
        for (int c = 0; c < 2; ++c) if (cnt[c] > 0) { ca[c] = sa[c]/cnt[c]; cb[c] = sb[c]/cnt[c]; }
    }

    // Order cool-first (lower b*) so `share`/`hue` mean the same thing shot to shot and the
    // readout can be compared across frames.
    const bool swap = (cb[0] > cb[1]);
    for (size_t k = 0; k < pool.size(); ++k) {
        const uint8_t c = swap ? (uint8_t)(1 - asg[k]) : asg[k];
        S.group[pool[k]] = c;
    }
    for (int c = 0; c < 2; ++c) {
        const int s = swap ? (1 - c) : c;
        ex.hue[c]    = (float)(std::atan2(cb[s], ca[s]) * 180.0 / 3.14159265358979);
        ex.chroma[c] = (float)std::sqrt(ca[s]*ca[s] + cb[s]*cb[s]);
    }
    long long g0 = 0, g1 = 0;
    for (size_t i = 0; i < n; ++i) { if (S.group[i] == 0) ++g0; else if (S.group[i] == 1) ++g1; }
    ex.share[0] = 100.0f * (float)g0 / (float)n;
    ex.share[1] = 100.0f * (float)g1 / (float)n;
    return ex;
}

// Percentile by nth_element — exact, and O(n) per rank, which beats a histogram's bin error
// for the same reason probeAnalyze already prefers it.
static inline float pct_of(std::vector<float>& v, double frac)
{
    if (v.empty()) return 0.f;
    const size_t k = (size_t)(frac * (double)(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

// ---------------------------------------------------------------------------------------
// describe() — a PURE function of the parameter vector. Same samples, same memberships,
// only P changes. That is exactly what makes the Jacobian below a legitimate derivative.
static inline Desc describe(const SampleSet& S, int cam, int enc, const float* P)
{
    Desc d; for (int i = 0; i < kDescN; ++i) d.v[i] = 0.f;
    const size_t n = S.size();
    if (n == 0) return d;

    std::vector<float> lum; lum.reserve(n);
    std::vector<float> chn; chn.reserve(n*3);
    std::vector<float> skinLum;
    double over = 0.0;
    double sA = 0, sB = 0, sC = 0; long long midN = 0;
    double gA[2] = {0,0}, gB[2] = {0,0}; long long gN[2] = {0,0};
    // Band means in LAB, not in display luma. L* puts tone on the same perceptual footing as
    // a*/b*, so the separation triple is one coherent Lab difference between two regions
    // rather than a tone number and two colour numbers that cannot be compared with each other.
    double bandL[3] = {0,0,0}, bandA[3] = {0,0,0}, bandB[3] = {0,0,0}; long long bandN[3] = {0,0,0};
    // Subject and surround, as the same Lab means over two populations that partition the frame.
    // Index 0 is the surround and 1 the subject, so the difference below reads the same way round
    // as the band triple: the thing of interest minus the thing it has to stand out from.
    double regL[2] = {0,0}, regA[2] = {0,0}, regB[2] = {0,0}; long long regN[2] = {0,0};
    const bool haveSubject = (S.subject >= 0 && S.subject < kRegionN && S.region.size() == n);
    double skB = 0; long long skN = 0;

    for (size_t i = 0; i < n; ++i) {
        float r, g, b;
        render_sample(cam, enc, P, S.rgb[i*3], S.rgb[i*3+1], S.rgb[i*3+2], r, g, b);
        const float Y = 0.2126f*r + 0.7152f*g + 0.0722f*b;
        lum.push_back(Y);
        chn.push_back(r); chn.push_back(g); chn.push_back(b);
        const float mx = std::max(r, std::max(g, b));
        if (mx > 1.0f) over += (double)(mx - 1.0f);

        float L, a, bb; display_to_Lab(enc, r, g, b, L, a, bb);

        if (S.mid[i]) {
            sA += a; sB += bb; sC += std::sqrt(a*a + bb*bb); ++midN;
        }
        const uint8_t gp = S.group[i];
        if (gp < 2) { gA[gp] += a; gB[gp] += bb; ++gN[gp]; }
        const uint8_t bd = S.band[i];
        if (bd < 3) { bandL[bd] += L; bandA[bd] += a; bandB[bd] += bb; ++bandN[bd]; }
        if (haveSubject) {
            const int k = ((int)S.region[i] == S.subject) ? 1 : 0;
            regL[k] += L; regA[k] += a; regB[k] += bb; ++regN[k];
        }
        if (S.skin[i]) { skinLum.push_back(Y); skB += bb; ++skN; }
    }

    d.v[D_BLACK] = pct_of(chn, 0.001);
    d.v[D_MID]   = pct_of(lum, 0.50);
    d.v[D_WHITE] = pct_of(chn, 0.99);
    d.v[D_OVER]  = (float)(over / (double)n);

    if (midN > 0) {
        d.v[D_A]      = (float)(sA / (double)midN);
        d.v[D_B]      = (float)(sB / (double)midN);
        d.v[D_CHROMA] = (float)(sC / (double)midN);
    }
    if (gN[0] > 0 && gN[1] > 0) {
        const double da = gA[0]/gN[0] - gA[1]/gN[1];
        const double db = gB[0]/gN[0] - gB[1]/gN[1];
        d.v[D_SEP] = (float)std::sqrt(da*da + db*db);
    }
    // The separation triple: region A (top third) minus region B (bottom third), as the three
    // signed components of their Lab difference. Signed on purpose -- see the enum note.
    if (bandN[0] > 0 && bandN[2] > 0) {
        d.v[D_DL] = (float)(bandL[2]/bandN[2] - bandL[0]/bandN[0]);
        d.v[D_DA] = (float)(bandA[2]/bandN[2] - bandA[0]/bandN[0]);
        d.v[D_DB] = (float)(bandB[2]/bandN[2] - bandB[0]/bandN[0]);
    }
    // THE SAME TRIPLE OVER REAL REGIONS. Gated on BOTH populations, with the same minimum, because
    // the two sides are symmetric here in a way the skin mask's bounds are not: a subject at 1% of
    // frame and a subject at 99% both leave one mean built from noise, and neither difference means
    // anything. Reusing skin_trustworthy()'s upper bound would be wrong -- that 25% says "this
    // stopped being a face", which is a statement about the label, not about the arithmetic.
    if (regN[0] >= 200 && regN[1] >= 200) {
        d.v[D_RDL] = (float)(regL[1]/regN[1] - regL[0]/regN[0]);
        d.v[D_RDA] = (float)(regA[1]/regN[1] - regA[0]/regN[0]);
        d.v[D_RDB] = (float)(regB[1]/regN[1] - regB[0]/regN[0]);
    }
    // Zeroed rather than reported unless the mask is believable — too few pixels is noise, too
    // many means it matched the scene rather than a face. Same gate classify() reports through
    // Extras::skinOk, shared so the two cannot drift. See skin_trustworthy().
    if (skin_trustworthy(skN, n)) {
        d.v[D_SKINL] = pct_of(skinLum, 0.50);
        d.v[D_SKINB] = (float)(skB / (double)skN);
    }
    return d;
}

// ---------------------------------------------------------------------------------------
// REGION ASSIGNMENT — THIS IS A STAND-IN AND MUST BE REPLACED.
//
// Everything downstream consumes SampleSet::region and nothing else, so a real segmentation
// model swaps in HERE and changes nothing else in this file or in the plugin. That isolation is
// the entire point of doing it this way round: prove the chain end to end in Resolve first,
// then drop the model into a slot that is already known to work.
//
// What this does is crude on purpose — position, lightness and hue, no understanding at all. It
// will call a beige wall TERRAIN and a blue car WATER. It exists so the button, the cycle, the
// magnitude solve and the readout can be exercised on real footage, NOT because it is good.
//
// Measured against the real thing (experiments/segmentation, SegFormer-B0 on ADE20K), on a 4K
// beach: the model returned sea 51% / sky 36% / mountain 8% / person 1%, following the horizon
// and separating the headland from both. Nothing below will do that. The offline harness also
// showed the semantic mask reading 0.7% skin where the plugin's chromaticity mask reads 46% --
// the sand-versus-skin failure this cannot fix either.
//
// DO NOT FIT ANY CONSTANT TO THIS CLASSIFIER'S OUTPUT. Its numbers are plumbing, not evidence.
static inline void stub_regions(SampleSet& S, int cam, int enc)
{
    const size_t n = S.size();
    S.region.assign(n, (uint8_t)R_OTHER);
    if (n == 0) return;

    float P[kParamN] = {0.f,0.f,0.f, 0.f,1.f,1.f, 0.f,0.f, 0.f,1.f, 0.f,6500.f, 0.f};
    std::vector<float> vL(n), va(n), vb(n);
    double sumL = 0.0;
    for (size_t i = 0; i < n; ++i) {
        float r, g, b;
        render_sample(cam, enc, P, S.rgb[i*3], S.rgb[i*3+1], S.rgb[i*3+2], r, g, b);
        display_to_Lab(enc, r, g, b, vL[i], va[i], vb[i]);
        sumL += vL[i];
    }
    const float meanL = (float)(sumL / (double)n);

    for (size_t i = 0; i < n; ++i) {
        const float L = vL[i], a = va[i], b = vb[i];
        uint8_t r;
        if (!S.skin.empty() && S.skin[i])            r = R_SKIN;      // reuses the existing mask,
                                                                      // with all its known faults
        else if (S.band[i] == 2 && L > meanL)        r = R_SKY;       // bright and up top
        else if (b < -6.f && L < meanL)              r = R_WATER;     // dark and blue-leaning
        else if (a < -5.f && b > 0.f)                r = R_VEG;       // green-leaning
        else if (L < meanL)                          r = R_TERRAIN;   // the darker remainder
        else                                         r = R_BUILT;     // the brighter remainder
        S.region[i] = r;
    }
}

// Fill SampleSet::region from a segmentation mask.
//
// The mask is a picture: row 0 is the TOP of the frame. SampleSet::v is OFX's convention, 0 at
// the BOTTOM. So the row index is (1 - v), and getting that backwards would put sky underfoot
// while every downstream number stayed perfectly plausible -- coverage percentages, Lab means,
// the lot. There is no way to notice it from the statistics, only from the picture.
//
// Nearest-neighbour on purpose. The mask is already coarse (128x128 for a 512 input) and its
// values are labels, not quantities: interpolating between "sky" and "water" would produce
// "somewhere in between", which is not a thing.
static inline bool assign_regions(SampleSet& S, const std::vector<uint8_t>& mask,
                                  int mw, int mh)
{
    const size_t n = S.size();
    if (n == 0 || mw <= 0 || mh <= 0 || mask.size() != (size_t)mw * mh) return false;
    if (S.u.size() != n || S.v.size() != n) return false;
    S.region.assign(n, (uint8_t)R_OTHER);
    for (size_t i = 0; i < n; ++i) {
        int x = (int)(S.u[i] * (float)mw);
        int y = (int)((1.0f - S.v[i]) * (float)mh);      // OFX bottom-up -> image top-down
        x = x < 0 ? 0 : (x >= mw ? mw - 1 : x);
        y = y < 0 ? 0 : (y >= mh ? mh - 1 : y);
        S.region[i] = mask[(size_t)y * mw + x];
    }
    return true;
}

// Per-region colour, for a given parameter vector. Same fixed-membership rule as everything
// else here: regions are decided once from the neutral render and only the STATISTICS move.
static inline void region_stats(const SampleSet& S, int cam, int enc, const float* P,
                                RegionStat* out)
{
    for (int r = 0; r < kRegionN; ++r) out[r] = RegionStat();
    const size_t n = S.size();
    if (n == 0 || S.region.size() != n) return;

    double sL[kRegionN] = {0}, sa[kRegionN] = {0}, sb[kRegionN] = {0};
    long long cnt[kRegionN] = {0};
    for (size_t i = 0; i < n; ++i) {
        float r, g, b;
        render_sample(cam, enc, P, S.rgb[i*3], S.rgb[i*3+1], S.rgb[i*3+2], r, g, b);
        float L, a, bb; display_to_Lab(enc, r, g, b, L, a, bb);
        const int k = S.region[i];
        sL[k] += L; sa[k] += a; sb[k] += bb; ++cnt[k];
    }
    for (int k = 0; k < kRegionN; ++k) {
        if (!cnt[k]) continue;
        out[k].cover = 100.0f * (float)cnt[k] / (float)n;
        out[k].L = (float)(sL[k] / cnt[k]);
        out[k].a = (float)(sa[k] / cnt[k]);
        out[k].b = (float)(sb[k] / cnt[k]);
    }
}

// ---------------------------------------------------------------------------------------
// THE MAGIC GRADE DECISION: one subject, one slider, one direction.
//
// The user's design. Identify the objects, rank them by how present they are, pick a subject,
// read it against the rest of the scene, choose Offset Temp or Gain Temp, choose a direction.
// The Separation slider then scales that one decision; pressing the button again picks a
// different subject and runs the same process.
//
// IT DELIBERATELY HAS NO METRIC TO MAXIMISE. An earlier design maximised a separation number
// and was falsified against four of the user's own hand grades: two of three NARROWED region
// separation rather than widening it, by 2-11%, riding on common-mode global moves several
// times larger. This makes a choice and lets the user accept it, tune it, or cycle past it, so
// the rule only ever has to be reasonable -- being wrong costs one more press instead of
// failing silently.
//
// CHOOSING THE CONTROL IS NOT A GUESS. It falls out of og::process():
//
//     offset:  w[0] += offTemp*0.10       additive -- a large RELATIVE shift on a dark region,
//                                         a small one on a bright region
//     gain:    w[0] *= (1 + temp*0.20)    multiplicative -- scales with value, grips the top
//
// so a dark subject is reached with Offset Temp and a bright one with Gain Temp. On the beach
// that yields Offset Temp negative, which is exactly the control and direction the user reached
// for by hand (-0.167) -- from the pipeline's own arithmetic, with nothing fitted.
struct MagicChoice {
    int  subject = -1;      // Region
    int  param   = -1;      // index into P[]: 6 = Offset Temp, 0 = Gain Temp
    int  sign    = 0;       // +1 warmer, -1 cooler
    int  option  = 0;       // which press produced this
    int  options = 0;       // how many distinct moves the frame offers
    bool ok      = false;
    // THE NUMBERS THE DECISION WAS MADE FROM, carried out so the panel can explain itself.
    //
    // This is not diagnostics. The feature's stated job is to surface a move an inexperienced
    // colourist would not have considered -- "there is water in this shot, cool it and see" --
    // and a suggestion nobody can see the reasoning behind teaches nothing and cannot be argued
    // with. It also makes a WRONG pick legible instead of mysterious, which matters more here
    // than usual: the tool is admittedly fallible by design, so every call it makes has to show
    // its working or the user has no way to tell a bad guess from a bad tool.
    float cover  = 0.f;     // how much of the frame the subject holds
    float subjL  = 0.f, restL = 0.f;   // lightness, subject against the rest — picks the control
    float subjB  = 0.f, restB = 0.f;   // warm/cool, likewise — picks the direction
};

static const int kMagicMinCover = 6;     // below this a region is scenery, not a subject
// Above this, one region IS the frame and the rest is a sliver -- a macro of leaves comes back
// 98% wall against 2% foliage, and pushing those apart is a colour cast justified by speckle.
//
// DERIVED FROM THE FLOOR RATHER THAN PICKED. The two were independent numbers, 6 and 88, and
// they disagreed: a downward city view measured 92.9% structure against 7.1% roofs and streets,
// so the 7.1% cleared the floor as a real region while the 92.9% simultaneously tripped the
// ceiling as "the whole frame". The frame was both separable and not, depending on which
// constant you asked.
//
// Complementary by construction now: two regions must each clear the floor, and nothing else is
// being asserted. The macro of leaves still declines -- its 1.7% never clears the floor -- and
// the city now acts, which is what its two visibly different halves deserve.
static const float kMagicDominant = 100.f - (float)kMagicMinCover;

static inline MagicChoice magic_decide(const RegionStat* st, int click)
{
    MagicChoice out;
    int idx[kRegionN], nb = 0;
    for (int r = 0; r < kRegionN; ++r)
        if (st[r].cover >= (float)kMagicMinCover) idx[nb++] = r;
    if (nb < 2) return out;             // nothing to read a subject against
    float biggest = 0.f;
    for (int i = 0; i < nb; ++i) biggest = std::max(biggest, st[idx[i]].cover);
    if (biggest >= kMagicDominant) return out;   // one region is the whole frame

    // A BELIEVABLE FACE IS THE SUBJECT. It does not compete for the slot on square footage.
    //
    // Ranking on cover * salience alone put SKIN 15.7% x 3.0 = 47.1 against BUILT 77.3% x 0.6 =
    // 46.4 on a dark interview -- a 1.5% margin deciding whether the frame gets a tone solve at
    // all. Half a percentage point of coverage flips it, and the two answers are not neighbours:
    // the face branch places the subject (RAW Exposure +2.29 EV, midtone 0.632) while the wall
    // branch declines as "not a face" and leaves the shot at 0.339. Changing nothing but the
    // number of samples measured was enough to swap them, so the button was picking between two
    // completely different pictures on noise.
    //
    // This is not a new opinion about faces. region_protected() already refuses to push skin,
    // and two of Magic Tone's three conditions are already about the subject because legibility
    // is a property of the thing being looked at. All that changes is that the point stops being
    // re-argued against a wall's coverage on every press.
    //
    // Only inside the band skin_trustworthy() already vouches for: above it the mask is not a
    // face at all -- sand at 39.7%, a cactus at 46.5%, a beach at 46% -- and those fall through
    // to the score, which is the case the ceiling was fitted for in the first place. The floor
    // is the same kMagicMinCover every candidate here has already cleared.
    //
    // Ordering only. Press-again still cycles through every candidate.
    auto believable_face = [&](int r) {
        return r == R_SKIN && st[r].cover <= (float)(kSkinMaxCover * 100.0);
    };
    std::sort(idx, idx + nb, [&](int A, int B) {
        const bool fa = believable_face(A), fb = believable_face(B);
        if (fa != fb) return fa;
        return st[A].cover * region_salience(A) > st[B].cover * region_salience(B);
    });

    // DEDUPED BY THE MOVE, NOT BY THE SUBJECT. Two subjects can resolve to the same control and
    // the same sign -- on a car portrait "protect the face" and "push the interior" both give
    // Gain Temp negative -- and offering that twice makes the second press do nothing visible.
    // A button whose whole affordance is "press again for a different answer" has to give one.
    MagicChoice cand[kRegionN]; int nc = 0;
    for (int i = 0; i < nb; ++i) {
        const int s = idx[i];
        // The scene the subject is read AGAINST, which excludes the subject itself.
        double wsum = 0.0, wL = 0.0, wb = 0.0;
        for (int j = 0; j < nb; ++j) {
            if (idx[j] == s) continue;
            const double w = st[idx[j]].cover;
            wsum += w; wL += w * st[idx[j]].L; wb += w * st[idx[j]].b;
        }
        if (wsum <= 0.0) continue;
        const float restL = (float)(wL / wsum), restB = (float)(wb / wsum);

        float db = st[s].b - restB;
        // No lean to enhance: push away from wherever the scene sits, so the press still does
        // something rather than resolving to zero.
        if (std::fabs(db) < 0.5f) db = (std::fabs(restB) > 0.5f) ? -restB : 1.0f;

        MagicChoice c;
        c.subject = s;
        c.cover = st[s].cover;
        c.subjL = st[s].L; c.restL = restL;
        c.subjB = st[s].b; c.restB = restB;
        if (region_protected(s)) {
            // Never push the subject; move the surround. Grip whatever the surround is, and
            // push away from the subject's hue -- cool the room, let the face come forward.
            c.param = (restL > st[s].L) ? 0 : 6;
            c.sign  = (db > 0.f) ? -1 : +1;
        } else {
            c.param = (st[s].L > restL) ? 0 : 6;
            c.sign  = (db > 0.f) ? +1 : -1;
        }
        bool dup = false;
        for (int k = 0; k < nc; ++k) dup |= (cand[k].param == c.param && cand[k].sign == c.sign);
        if (!dup) cand[nc++] = c;
    }
    if (nc == 0) return out;

    const int pick = ((click % nc) + nc) % nc;
    out = cand[pick];
    out.option = pick;
    out.options = nc;
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------------------
// THE JACOBIAN.
//
// Column j is the descriptor change produced by nudging control j by ONE NATURAL STEP (the
// table below), not by one unit of its raw range. Working in step-normalised units is what
// keeps the matrix conditioned: raw units would put RAW Temperature (hundreds of Kelvin)
// and Lift (hundredths) in the same matrix, and the solve would be numerically dominated by
// whichever control happens to have big numbers rather than by which control actually helps.
//
// Central differences, so the estimate is second-order accurate and symmetric around the
// operating point — a forward difference would bias every column in the direction of
// increasing parameter, which shows up immediately in the round-trip test.
static inline const float* param_steps()
{
    //                temp  tint  dens  lift  gamma gain  oTmp  oTnt  pExp  pCon  rExp  rTemp  roll
    //                then range balance: latch  soft  high  rbLift rbGamma
    static const float s[kParamN] = { 0.05f,0.05f,0.05f,0.02f,0.05f,0.05f,0.05f,0.05f,0.05f,0.05f,0.05f,250.f,0.05f,
                                      2.0f, 0.5f, 0.05f, 0.02f, 0.05f, 0.f, 0.05f, 0.05f };
    return s;
}

// ---------------------------------------------------------------------------------------
// TWO CONTROLS ARE NOT DIFFERENTIABLE AT THEIR DEFAULTS. Both were found by this Jacobian,
// not by looking for them, and both are properties of the SHIPPING PIPELINE rather than of
// the analysis — the linear model is reporting them honestly.
//
//   ROLLOFF, at 0. softclip() early-outs on amt <= 0, but for ANY amt > 0 it asymptotes hard
//   at 1.0, so every superwhite collapses onto white the instant the slider leaves zero:
//        amt 0.0000 -> softclip(1.26) = 1.26000
//        amt 0.0001 -> softclip(1.26) = 1.00000
//   measured mean overshoot goes 0.031 -> 0.000 across that same gap. The control has two
//   regimes: "is there a ceiling at all" (the step at 0) and "how far down does the knee
//   reach" (the rest of the range, which is smooth). A linear solve spanning zero is
//   meaningless, and this is the same shape as the LUT-encode dead end already recorded in
//   CLAUDE.md — the first nudge off zero is a cliff, not a ramp.
//
//   RAW TEMPERATURE, at 6500 K. white_balance() early-outs on 6499 < T < 6501 to mean
//   "identity at 6500", but the Kim et al. PLANCKIAN locus at 6500 K is (0.31349, 0.32366)
//   while D65 is (0.31270, 0.32900) — dy = -0.0053, because D65 is a daylight illuminant and
//   sits above the blackbody locus. So the adaptation the function skips is not actually an
//   identity, and stepping 1 K off the default jumps a neutral mid-gray by a* +2.06, b* +1.25:
//        T = 6499  ->  rgb 0.54311 0.55807 0.54535   (a* -2.064)
//        T = 6500  ->  rgb 0.55397 0.55397 0.55397   (a* -0.002, forced neutral)
//        T = 6501  ->  rgb 0.54315 0.55806 0.54528   (a* -2.063)
//   A visible green cast appears out of nowhere on the first nudge. Fixing it is a colour-math
//   change and therefore a golden-rule four-file edit — adapt to blackbody(6500) instead of to
//   D65 and the stated contract becomes true by construction, with no early-out needed.
//
// Neither is excluded because it is hard. They are excluded because a derivative taken across
// a step is a secant, and a solver handed one will confidently ask for a move that does not
// do what it predicts.
static inline void steer_mask(const float* P0, bool* allow)
{
    for (int i = 0; i < kParamN; ++i) allow[i] = true;
    allow[12] = false;                                        // rolloff: step at 0, and the
                                                              // pin fit already owns it
    if (std::fabs(P0[11] - 6500.0f) < 2.0f) allow[11] = false; // rawTemp: sitting in the dead band
}

struct Jac {
    float m[kDescN * kParamN];              // row-major: m[d*kParamN + p]
    float at(int d, int p) const { return m[d*kParamN + p]; }
};

static inline const char* param_name(int p)
{
    static const char* n[kParamN] = { "tmp","tnt","dns","lft","gam","gan",
                                      "oTm","oTn","pEx","pCn","rEx","rTm","rol",
                                      "rbL","rbS","rbH","rbF","rbG","rbW","rbHg","rbLg" };
    return (p >= 0 && p < kParamN) ? n[p] : "?";
}

// `allow` (optional) restricts which columns are measured at all. Excluded columns stay zero
// and cost nothing — with three controls allowed that is 6 describe() passes instead of 26,
// which is what makes solve_intent_iter() below affordable enough to run inside a button.
static inline Jac jacobian(const SampleSet& S, int cam, int enc, const float* P0,
                           const bool* allow = nullptr)
{
    Jac J; for (int i = 0; i < kDescN*kParamN; ++i) J.m[i] = 0.f;
    const float* st = param_steps();
    for (int p = 0; p < kParamN; ++p) {
        if (allow && !allow[p]) continue;
        float Pp[kParamN], Pm[kParamN];
        for (int i = 0; i < kParamN; ++i) { Pp[i] = P0[i]; Pm[i] = P0[i]; }
        Pp[p] += st[p]; Pm[p] -= st[p];
        // Gamma and Contrast are divisors/multipliers that misbehave at or below zero, and
        // Rolloff is defined only from 0 up. Clamping the MINUS side would silently halve the
        // step and report a derivative half the true size, so shift the whole stencil instead
        // and keep the interval symmetric.
        if (p == 4 || p == 9) { const float lo = 0.10f; if (Pm[p] < lo) { const float sh = lo - Pm[p]; Pm[p] += sh; Pp[p] += sh; } }
        if (p == 12)          { if (Pm[p] < 0.f)       { const float sh = -Pm[p];    Pm[p] += sh; Pp[p] += sh; } }
        const Desc dp = describe(S, cam, enc, Pp);
        const Desc dm = describe(S, cam, enc, Pm);
        for (int d = 0; d < kDescN; ++d) J.m[d*kParamN + p] = 0.5f*(dp.v[d] - dm.v[d]);
    }
    return J;
}

// Linear prediction: descriptor change for a step-normalised parameter move.
static inline void jac_predict(const Jac& J, const float* dpNorm, float* ddOut)
{
    for (int d = 0; d < kDescN; ++d) {
        float s = 0.f;
        for (int p = 0; p < kParamN; ++p) s += J.at(d, p) * dpNorm[p];
        ddOut[d] = s;
    }
}

// ---------------------------------------------------------------------------------------
// solve_intent() — the inverse. "I want b* down 4 and chroma up 2" becomes "Offset Temp
// -0.11, Density +0.15", without anyone ever having written down what those controls mean.
//
// Damped least squares (Tikhonov):   (J^T W^2 J + lambda I) dp = J^T W^2 dd
// The damping is doing real work, not just guarding singularity. The system is wildly
// underdetermined — twelve descriptors, thirteen controls, several of which overlap almost
// exactly (Gain and Post Exposure both raise the midtone) — so an undamped solve would find
// some enormous cancelling pair that is correct to first order and absurd on the picture.
// lambda buys the SMALLEST move that gets close, which is also the one a colourist would make.
//
// `allow` restricts which controls may move, so a caller can say "fix this with Balance only"
// and get an answer in the controls it is willing to spend.
static inline void solve_intent(const Jac& J, const float* dd, const float* w,
                                const bool* allow, float lambda, float* dpNorm)
{
    double A[kParamN][kParamN], rhs[kParamN];
    for (int i = 0; i < kParamN; ++i) {
        rhs[i] = 0.0;
        for (int j = 0; j < kParamN; ++j) A[i][j] = 0.0;
    }
    for (int d = 0; d < kDescN; ++d) {
        const double ww = (double)w[d] * (double)w[d];
        if (ww <= 0.0) continue;
        for (int i = 0; i < kParamN; ++i) {
            if (allow && !allow[i]) continue;
            const double ji = J.at(d, i);
            rhs[i] += ww * ji * (double)dd[d];
            for (int j = 0; j < kParamN; ++j) {
                if (allow && !allow[j]) continue;
                A[i][j] += ww * ji * J.at(d, j);
            }
        }
    }
    for (int i = 0; i < kParamN; ++i) {
        A[i][i] += (double)lambda;
        if (allow && !allow[i]) { for (int j = 0; j < kParamN; ++j) A[i][j] = A[j][i] = 0.0; A[i][i] = 1.0; rhs[i] = 0.0; }
    }

    // Cholesky: A is symmetric positive definite once damped, and 13x13 makes the cost
    // irrelevant next to the 26 describe() passes that built J.
    double L[kParamN][kParamN];
    for (int i = 0; i < kParamN; ++i) for (int j = 0; j < kParamN; ++j) L[i][j] = 0.0;
    for (int i = 0; i < kParamN; ++i) {
        for (int j = 0; j <= i; ++j) {
            double s = A[i][j];
            for (int k = 0; k < j; ++k) s -= L[i][k]*L[j][k];
            if (i == j) L[i][i] = std::sqrt(s > 1e-12 ? s : 1e-12);
            else        L[i][j] = s / L[j][j];
        }
    }
    double y[kParamN];
    for (int i = 0; i < kParamN; ++i) {
        double s = rhs[i];
        for (int k = 0; k < i; ++k) s -= L[i][k]*y[k];
        y[i] = s / L[i][i];
    }
    for (int i = kParamN - 1; i >= 0; --i) {
        double s = y[i];
        for (int k = i + 1; k < kParamN; ++k) s -= L[k][i]*dpNorm[k];
        dpNorm[i] = (float)(s / L[i][i]);
    }
    for (int i = 0; i < kParamN; ++i) if (allow && !allow[i]) dpNorm[i] = 0.f;
}

// Step-normalised move -> real parameter values, with the ranges the panel enforces.
static inline void apply_move(const float* P0, const float* dpNorm, float* Pout)
{
    // THREE TABLES ARE SIZED BY kParamN AND INITIALISED BY HAND, and C++ zero-fills any it is
    // short of rather than refusing to compile. Adding Range Balance silently gave the new
    // controls a step of 0 (a derivative of "does nothing"), a null name, and -- worst -- a
    // clamp range of [0,0] here, which wiped them to zero on every solve. Caught by one test;
    // the other two would have surfaced as a feature that quietly could not be steered.
    // If a param is added, all three grow with it.
    //                              temp  tint  dens   lift   gamma  gain  oTmp oTnt  pExp  pCon  rExp   rTemp  roll
    //                              then range balance: latch  soft  high  rbLift rbGamma
    static const float lo[kParamN] = { -1.f,-1.f,-1.f, -0.5f, 0.20f, 0.20f, -1.f,-1.f, -3.f, 0.20f, -5.f, 2000.f, 0.f,
                                       0.f,  0.f, 0.05f, -0.5f, 0.20f, 0.f, 0.20f, 0.20f };
    static const float hi[kParamN] = {  1.f, 1.f, 1.f,  0.5f, 3.00f, 3.00f,  1.f, 1.f,  3.f, 3.00f,  5.f,20000.f, 0.8f,
                                     100.f, 25.f, 2.00f,  0.5f, 3.00f, 1.f, 3.00f, 3.00f };
    const float* st = param_steps();
    for (int i = 0; i < kParamN; ++i) {
        float v = P0[i] + dpNorm[i]*st[i];
        Pout[i] = std::min(hi[i], std::max(lo[i], v));
    }
}

// ---------------------------------------------------------------------------------------
// ITERATIVE SOLVE — because one linear shot always undershoots a large move.
//
// Measured on the beach sunset, against the user's own hand grade. Their move was Offset Temp
// -0.167, which is 3.3 natural steps; the linear model says that buys b* -8.8 and it actually
// delivered -7.0, i.e. **84% of linear**. The response saturates as you push it, so a solver
// asked for b* -7.0 comes back with -0.13 when the honest answer is -0.167 — right control,
// right direction, a quarter short. Test 18 only ever validated the model to within a third of
// a step at a HALF-step move; three steps is extrapolation and behaves like it.
//
// So: solve, apply, RE-MEASURE, solve for what is left. Each round starts closer, so the
// linearisation is being used where it is accurate instead of where it is convenient. Cheap
// because the Jacobian is restricted to the controls the caller actually allows — three
// controls over three rounds is ~24 describe() passes, well inside a button press.
//
// Returns the final parameter vector, already range-clamped by apply_move().
static inline void solve_intent_iter(const SampleSet& S, int cam, int enc,
                                     const float* P0, const float* ddTarget, const float* w,
                                     const bool* allow, float lambda, int rounds,
                                     float* Pout)
{
    for (int i = 0; i < kParamN; ++i) Pout[i] = P0[i];
    const Desc d0 = describe(S, cam, enc, P0);
    for (int r = 0; r < rounds; ++r) {
        const Desc dc = describe(S, cam, enc, Pout);
        // What is still owed, not what was originally asked for. This is the whole difference
        // from a single shot: round 2 solves the residual of round 1.
        float remain[kDescN];
        for (int d = 0; d < kDescN; ++d) remain[d] = ddTarget[d] - (dc.v[d] - d0.v[d]);
        const Jac Jr = jacobian(S, cam, enc, Pout, allow);
        float dp[kParamN];
        solve_intent(Jr, remain, w, allow, lambda, dp);
        // apply_move() clamps to the panel ranges, so a target that cannot be reached pins the
        // slider and the next round sees no further progress rather than diverging.
        float Pn[kParamN]; apply_move(Pout, dp, Pn);
        for (int i = 0; i < kParamN; ++i) Pout[i] = Pn[i];
    }
}

// ---------------------------------------------------------------------------------------
// ATTRIBUTION — which control actually caused which descriptor change.
//
// Exists because reading a descriptor and naming the obvious control is WRONG, and I proved it
// on this project's own data: chroma rose 1.2 between the Creative grade and the hand grade, I
// said "more density", and the user had in fact LOWERED density by 0.053. Lift, Gain and
// Offset Temp were all pushing chroma up while Density pulled it down. The controls overlap far
// too much to attribute a change by inspection — which is the argument for the Jacobian, made
// by my own mistake.
//
// `linear` will not equal `actual` on a large move (see solve_intent_iter above); the gap is
// itself the useful signal, because it says how far outside the linear regime the grade sits.
struct Attribution {
    float actual[kDescN];            // measured: describe(P1) - describe(P0)
    float linear[kDescN];            // what the Jacobian predicted, = sum of `part` over p
    float part[kDescN * kParamN];    // per-control contribution
    float at(int d, int p) const { return part[d*kParamN + p]; }
};

static inline Attribution attribute(const SampleSet& S, int cam, int enc, const Jac& J,
                                    const float* P0, const float* P1)
{
    Attribution A;
    const float* st = param_steps();
    float dpNorm[kParamN];
    for (int p = 0; p < kParamN; ++p) dpNorm[p] = (st[p] > 0.f) ? (P1[p] - P0[p]) / st[p] : 0.f;

    const Desc d0 = describe(S, cam, enc, P0);
    const Desc d1 = describe(S, cam, enc, P1);
    for (int d = 0; d < kDescN; ++d) {
        A.actual[d] = d1.v[d] - d0.v[d];
        float sum = 0.f;
        for (int p = 0; p < kParamN; ++p) {
            const float c = J.at(d, p) * dpNorm[p];
            A.part[d*kParamN + p] = c;
            sum += c;
        }
        A.linear[d] = sum;
    }
    return A;
}

// Rank controls by |contribution| to descriptor d, biggest first. Returns how many were
// written (never more than `n`, and never any whose contribution rounds to nothing).
static inline int top_drivers(const Attribution& A, int d, int n, int* out)
{
    int idx[kParamN];
    for (int p = 0; p < kParamN; ++p) idx[p] = p;
    std::sort(idx, idx + kParamN, [&](int a, int b) {
        return std::fabs(A.at(d, a)) > std::fabs(A.at(d, b));
    });
    int k = 0;
    for (int i = 0; i < kParamN && k < n; ++i) {
        if (std::fabs(A.at(d, idx[i])) < 1e-4f) break;
        out[k++] = idx[i];
    }
    return k;
}

} // namespace analysis
} // namespace og

namespace oga = og::analysis;
