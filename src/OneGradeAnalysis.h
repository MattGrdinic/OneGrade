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

static const int kParamN = 13;   // matches P[] in OneGradePipeline.h

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
enum {
    D_BLACK,    // per-channel 0.1st percentile — where the floor sits (what Base places)
    D_MID,      // luma median — the midtone
    D_WHITE,    // per-channel 99th percentile — a channel is what clips, not luma
    D_OVER,     // mean per-sample overshoot above 1.0 — smooth clipping pressure
    D_A,        // mean a* over mid-tones — green/magenta cast
    D_B,        // mean b* over mid-tones — warm/cool cast. THE one the sunset move needed.
    D_CHROMA,   // mean C* over mid-tones — overall colourfulness, what Density acts on
    D_SEP,      // ab distance between the two dominant colour populations — separation
    D_DL,       // top-band minus bottom-band luma — sky-vs-ground balance
    D_DB,       // top-band minus bottom-band b* — is the frame split warm-over-cool?
    D_SKINL,    // luma median over the skin mask (0 when coverage is too low to trust)
    D_SKINB,    // b* over the skin mask (0 when coverage is too low to trust)
    kDescN
};

static inline const char* desc_name(int i)
{
    static const char* n[kDescN] = { "black","mid","white","over","a*","b*",
                                     "chroma","sep","dL","db*","skinL","skinb*" };
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
    size_t size() const { return band.size(); }
};

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
    }
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
    ex.skinOk  = (skinN >= 200);

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
    double bandL[3] = {0,0,0}, bandB[3] = {0,0,0}; long long bandN[3] = {0,0,0};
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
        if (bd < 3) { bandL[bd] += Y; bandB[bd] += bb; ++bandN[bd]; }
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
    if (bandN[0] > 0 && bandN[2] > 0) {
        d.v[D_DL] = (float)(bandL[2]/bandN[2] - bandL[0]/bandN[0]);
        d.v[D_DB] = (float)(bandB[2]/bandN[2] - bandB[0]/bandN[0]);
    }
    // Zeroed rather than reported when coverage is too low: a skin number off 40 pixels is
    // noise, and a caller that weights it would be steering on nothing.
    if (skN >= 200) {
        d.v[D_SKINL] = pct_of(skinLum, 0.50);
        d.v[D_SKINB] = (float)(skB / (double)skN);
    }
    return d;
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
    static const float s[kParamN] = { 0.05f,0.05f,0.05f,0.02f,0.05f,0.05f,0.05f,0.05f,0.05f,0.05f,0.05f,250.f,0.05f };
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

static inline Jac jacobian(const SampleSet& S, int cam, int enc, const float* P0)
{
    Jac J; for (int i = 0; i < kDescN*kParamN; ++i) J.m[i] = 0.f;
    const float* st = param_steps();
    for (int p = 0; p < kParamN; ++p) {
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
    static const float lo[kParamN] = { -1.f,-1.f,-1.f, -0.5f, 0.20f, 0.20f, -1.f,-1.f, -3.f, 0.20f, -5.f, 2000.f, 0.f };
    static const float hi[kParamN] = {  1.f, 1.f, 1.f,  0.5f, 3.00f, 3.00f,  1.f, 1.f,  3.f, 3.00f,  5.f,20000.f, 0.8f };
    const float* st = param_steps();
    for (int i = 0; i < kParamN; ++i) {
        float v = P0[i] + dpNorm[i]*st[i];
        Pout[i] = std::min(hi[i], std::max(lo[i], v));
    }
}

} // namespace analysis
} // namespace og

namespace oga = og::analysis;
