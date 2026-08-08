// OneGrade — the Creative / Magic grade solve, as a pure function.
// Copyright (C) 2026 Matthew Grdinic
// SPDX-License-Identifier: GPL-3.0-or-later
//
// NOT PART OF THE GOLDEN RULE (CPU only, produces parameter values rather than pixels), but it
// carries the same discipline for a different reason.
//
// ---------------------------------------------------------------------------------------
// WHY THIS IS A HEADER AND NOT A METHOD
//
// The grade used to live inside OneGrade::applyAutoGrade(), tangled up with OFX parameter
// objects, which meant the only way to see what it did was to build the plugin, install it,
// restart Resolve and press a button. Every constant in it was therefore fitted at a rate of
// about one observation per minute.
//
// The alternative — an offline bench that reimplements the same arithmetic — is worse, and this
// project has already paid for that lesson three times on this one feature: the model was fed a
// neutral render instead of a graded one, then a pre-LUT render instead of the real one, then a
// threshold verified in Python and never ported. Every instance was a paraphrase of something
// that already existed, and every instance produced plausible output while being wrong.
//
// So the solve moves HERE, where the plugin and the bench call the same function and cannot
// disagree. The plugin writes the result to sliders; the bench renders it to a PNG.
//
// ---------------------------------------------------------------------------------------
// EVERY CONSTANT IS A TUNABLE, AND EVERY ONE IS FITTED ON VERY LITTLE
//
// The Gain line came from four hand-graded shots. The black point came from three. The Magic
// unit is an unfitted guess. They are gathered into one struct so a bench can sweep them, and
// so nobody has to go looking for which magic number lives in which function.
#pragma once
#include "OneGradePipeline.h"
#include "OneGradeAnalysis.h"
#include "CubeLUT.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace og {
namespace grade {

// What probeAnalyze() measures, and all the grade needs from a frame.
struct Measurements {
    double key  = 0.0;   // stops from mid-gray: log2(0.18 / Y50)
    double pin  = 0.0;   // % of frame sitting on the source ceiling
    double hot  = 0.0;   // % above display white
    double d01  = 0.0;   // per-channel 0.1st percentile, display, at neutral
    double d50  = 0.0;
    double d99  = 0.0;
    bool   valid = false;
};

struct Tunables {
    // GAIN, from exposure. Fitted to four hand-graded shots; three land within 0.02. The clamp
    // at the preset value for key >= 0 is the load-bearing half: a dark shot is never pushed up,
    // because a low-key interior is supposed to sit low and chasing mid-gray flattens it.
    double gainBase   = 0.80;
    double gainPerKey = 0.19;
    double gainMin    = 0.30;   // the fit is only evidenced to about -2 EV
    double gainMax    = 0.80;

    // ROLLOFF, from source clipping. One non-zero data point (an interview at pin 6.18% graded
    // to 0.557) and three controls correctly at zero.
    double rolloffPerPin = 0.090;
    double rolloffMax    = 0.80;

    // BLACK POINT. Creative used to stamp Lift 0.11, which lands somewhere different on every
    // shot; three hand grades all pulled it back down. Solved to this target instead.
    double blackTarget = 0.050;

    // MAGIC. How far the chosen region's b* should travel at Separation 1.0. Never fitted.
    double magicUnit = 6.0;

    // MAGIC TONE -- legibility of the subject, measured POST-LUT because that is the picture
    // being judged. Both from ONE hand-graded interview frame (2026-08-07), so treat them as
    // placeholders with the right shape rather than as fitted values.
    //
    // subjFloor is where the subject's own shadows sit, and it is the dial for how much contrast
    // the subject carries. Creative left the face at p10 0.078; the user's correction put it at
    // 0.135, and the frame-wide floor moving 0.050 -> 0.085 was a consequence rather than a goal.
    //
    // Set slightly below the hand grade at the user's call -- "a bit more contrast in our subject,
    // just less than we were allowing before". Measured subject spread on that frame: hand grade
    // 0.336, this 0.372, the crunchy pre-tone Magic 0.379. Costs nothing at the top, because the
    // ceiling is solved last (see the pass order).
    //
    // frameCeiling exists because Creative's picture was pinned: p99.9 at 0.993 with 1.12% of
    // the frame already clipped, so there was nowhere for Bias to go before it destroyed
    // something. The hand grade landed at 0.968 with nothing clipped.
    double subjFloor    = 0.125;
    double subjMid      = 0.278;
    double frameCeiling = 0.968;
};

// The Cinematic Film Emulation recipe, which Creative Grade starts from. User-validated, and the
// tint in particular (Gain Temp -0.22 / Gain Tint 0.09) is what gives the look its character --
// it was identical across all four shots the Gain fit came from.
//
// Mirrors applyPreset(1). The LUT is not a parameter so it cannot live in P[]: Creative also
// selects the Kodak 2383 D60 print stock at full mix, which forces the pre-LUT encode to Cineon.
static const int kCreativeCamera = 11;    // Rec.2100 PQ - Smooth Decode
static const int kCreativeEncode = 3;     // Cineon, forced by the film LUT

static inline void creative_preset(float P[analysis::kParamN])
{
    P[0]  = -0.22f;   // Gain Temp   -- the film tint, identical across every shot it was fitted on
    P[1]  =  0.09f;   // Gain Tint
    P[2]  =  0.10f;   // Density
    P[3]  =  0.11f;   // Lift        -- overwritten by the black-point solve below
    P[4]  =  1.00f;   // Gamma
    P[5]  =  0.80f;   // Gain        -- overwritten by the exposure fit below
    P[6]  = -0.02f;   // Offset Temp
    P[7]  =  0.01f;   // Offset Tint
    P[8]  =  0.55f;   // Post Exposure
    P[9]  =  1.00f;   // Contrast
    P[10] =  0.00f;   // Scene Exposure
    P[11] =  6500.f;  // Scene White Balance
    P[12] =  0.00f;   // Rolloff     -- overwritten from `pin` below
}

// Monotonic bisection. Used rather than a closed form because the controls interact and a closed
// form would need re-deriving every time the pipeline changed; this cannot drift.
static inline double solve1d(double lo, double hi, double target,
                             const std::function<double(double)>& probe)
{
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (probe(mid) < target) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

// Creative Grade: the film recipe, with Gain from exposure, Rolloff from clipping, and Lift
// solved so the black point lands on target whatever the footage's own floor happens to be.
static inline void solve_creative(const Measurements& m, const Tunables& t,
                                  float P[analysis::kParamN],
                                  const float* lut = nullptr, int lutSize = 0)
{
    creative_preset(P);
    if (!m.valid) return;

    const double gain = std::min(t.gainMax, std::max(t.gainMin,
                                 t.gainBase + t.gainPerKey * m.key));
    const double rolloff = std::min(t.rolloffMax, std::max(0.0, t.rolloffPerPin * m.pin));

    // The whole chain in closed form, which works because the grade curve is monotonic and a
    // monotonic map commutes with percentiles: one measured scalar stands in for the frame.
    //
    // THE LUT IS PART OF THAT CHAIN, and leaving it out placed the black point somewhere the
    // print stock then took away. Measured across twelve frames: the solve reported hitting
    // 0.050 every time while the picture on screen had a black point of 0.000 and, on the worst
    // shot, 46% of its pixels at or below 1/255. It was not wrong about its target; it was
    // aiming at the wrong picture.
    //
    // Third instance of one mistake in one day -- a number measured or solved in one space and
    // judged in another. The other two were the black point measured in the display fallback
    // while the curve ran in Cineon, and the analysis reading the node's encode before the preset
    // that changes it. The rule from docs/AUTO-GRADE.md 2 covers this too: a number pushed
    // through the pipeline needs the space the pipeline runs in, and the LUT is in the pipeline.
    const double pe = P[8], gm = P[4], con = P[9];
    const double lift = solve1d(-0.50, 0.50, t.blackTarget, [&](double lf) {
        float x = og::lgg_core((float)m.d01, (float)lf, (float)gm, (float)gain);
        float r = x, g = x, b = x;
        if (lut && lutSize >= 2) og::apply_lut(lut, lutSize, 1.f, r, g, b);
        og::apply_trim((float)pe, (float)con, r, g, b);
        if (rolloff > 0.0) g = og::softclip(g, (float)rolloff);
        return (double)g;
    });

    P[3]  = (float)lift;
    P[5]  = (float)gain;
    P[12] = (float)rolloff;
}

// The same black point, solved on the frame's ACTUAL DARK PIXELS instead of on a grey scalar.
//
// WHY A SCALAR IS NOT ENOUGH ONCE A LUT IS INVOLVED. The version above pushes one number through
// the chain as neutral grey, r = g = b. That is exact for everything before the LUT, because the
// grade curve is per-channel and monotonic, so it maps percentiles. A print stock is a 3D lookup
// and does no such thing: a saturated dark blue and a neutral grey of the same level land in
// completely different parts of the cube, and 2383's toe is far harder on saturated darks than
// on the neutral axis.
//
// Measured across twelve frames. The three with a near-neutral subject -- faces -- came out with
// 0.00 to 0.13% of pixels crushed. The nine landscapes, all saturated foliage and sky in the
// shadows, came out at up to 46%, with the solve reporting its target met every time. Adding the
// LUT to the scalar solve only moved 46% to 42%, which is the tell that the error is chromatic
// rather than tonal: the neutral axis simply is not where those pixels are.
//
// So the darkest samples are carried through in colour. Cheap because only the bottom slice
// matters -- a few thousand triples through a bisection, against a solve that already renders
// 200k samples once.
// The black point alone, on real pixels, LEAVING EVERY OTHER PARAMETER ALONE.
//
// Separate from solve_creative_px because that one begins with creative_preset(), which rewrites
// the whole array -- including Gain Temp and Offset Temp. Re-solving the floor after the Magic
// colour move therefore erased the colour move, and it did so DIFFERENTLY on each side: the bench
// rendered the reset array, so its picture lost the move entirely, while the plugin copied only
// Lift and Gain back into params that still held it. Two implementations, one bug, two different
// wrong answers -- which is exactly why they stopped matching on one frame.
static inline void solve_black_px(const analysis::SampleSet& S, int cam, int enc,
                                  float P[analysis::kParamN], double blackTarget,
                                  const float* lut, int lutSize)
{
    const size_t n = S.rgb.size() / 3;
    if (n < 512) return;

    // Rank by the neutral render's min channel: what crushes is a CHANNEL, not a luminance, and
    // on a saturated shadow the channels sit far apart.
    float Pn[analysis::kParamN] = {0.f,0.f,0.f, 0.f,1.f,1.f, 0.f,0.f, 0.f,1.f, 0.f,6500.f, 0.f};
    Pn[11] = P[11];
    std::vector<std::pair<float,size_t>> key; key.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        float r, g, b;
        og::process(cam, enc, Pn, S.rgb[i*3], S.rgb[i*3+1], S.rgb[i*3+2], r, g, b);
        key.push_back({ std::min(r, std::min(g, b)), i });
    }
    const size_t keep = std::min<size_t>(4000, n / 4 + 1);
    std::nth_element(key.begin(), key.begin() + keep, key.end(),
                     [](const std::pair<float,size_t>& a, const std::pair<float,size_t>& b2) { return a.first < b2.first; });
    key.resize(keep);

    const double pe = P[8], con = P[9], roll = P[12], gm = P[4], gn = P[5];
    // p1 OF THE DARK SLICE, not its minimum. A minimum is one pixel and would let a single
    // speck of sensor noise set the whole frame's lift.
    const size_t q = keep / 100;
    auto floorAt = [&](double lf) {
        std::vector<float> v; v.reserve(keep);
        for (size_t k = 0; k < keep; ++k) {
            const size_t i = key[k].second;
            float r, g, b;
            float Pt[analysis::kParamN];
            for (int j = 0; j < analysis::kParamN; ++j) Pt[j] = P[j];
            Pt[3] = (float)lf;
            og::process(cam, enc, Pt, S.rgb[i*3], S.rgb[i*3+1], S.rgb[i*3+2], r, g, b);
            if (lut && lutSize >= 2) og::apply_lut(lut, lutSize, 1.f, r, g, b);
            og::apply_trim((float)pe, (float)con, r, g, b);
            if (roll > 0.0) { r = og::softclip(r, (float)roll); g = og::softclip(g, (float)roll);
                              b = og::softclip(b, (float)roll); }
            v.push_back(std::min(r, std::min(g, b)));
        }
        std::nth_element(v.begin(), v.begin() + q, v.end());
        return (double)v[q];
    };
    (void)gm; (void)gn;
    P[3] = (float)solve1d(-0.50, 0.50, blackTarget, floorAt);
}

// Creative's full solve: gain and rolloff from the measurements, then the black point on pixels.
static inline void solve_creative_px(const analysis::SampleSet& S, int cam, int enc,
                                     const Measurements& m, const Tunables& t,
                                     float P[analysis::kParamN],
                                     const float* lut, int lutSize)
{
    solve_creative(m, t, P, lut, lutSize);         // gain, rolloff, and a starting lift
    if (m.valid) solve_black_px(S, cam, enc, P, t.blackTarget, lut, lutSize);
}

// The Magic move's magnitude, MEASURED on the shot rather than assumed.
//
// Nudge the chosen control, see how far the subject's b* actually travels, scale to the target.
// Necessary because the response is wildly shot-dependent -- Offset Temp moved b* by 2.63 per
// step on one frame and 3.96 on another -- so a fixed slider value would be a different move on
// every piece of footage, which is the exact defect that made Creative's stamped Lift wrong.
//
// Shared with the bench so the offline result is the same move the plugin makes. The bench used
// to render Creative Grade alone, which meant it could not see anything the magic move did --
// including crushing the blue channel on a dark frame, since Offset Temp is additive and
// subtracts from blue.
static inline double solve_magic_base(const analysis::SampleSet& S, int cam, int enc,
                                      const analysis::MagicChoice& c,
                                      const analysis::RegionStat* st, const Tunables& t)
{
    if (!c.ok) return 0.0;
    analysis::SampleSet D = analysis::decimate(S, 8000);
    const float step = analysis::param_steps()[c.param];
    float Pn[analysis::kParamN] = {0.f,0.f,0.f, 0.f,1.f,1.f, 0.f,0.f, 0.f,1.f, 0.f,6500.f, 0.f};
    float Pp[analysis::kParamN];
    for (int i = 0; i < analysis::kParamN; ++i) Pp[i] = Pn[i];
    Pp[c.param] += step;

    analysis::RegionStat s0[analysis::kRegionN], sp[analysis::kRegionN];
    analysis::region_stats(D, cam, enc, Pn, s0);
    analysis::region_stats(D, cam, enc, Pp, sp);

    // For a protected subject the move is spent on the SURROUND, so the grip is measured there.
    // Sizing a move by the response of the region we have decided not to move would be sizing
    // it by how hard it is to do the thing we are not doing.
    int measured = c.subject;
    if (analysis::region_protected(c.subject)) {
        float best = -1.f;
        for (int r = 0; r < analysis::kRegionN; ++r)
            if (r != c.subject && st[r].cover > best) { best = st[r].cover; measured = r; }
    }
    const float grip = sp[measured].b - s0[measured].b;
    double base = 0.0;
    if (std::fabs(grip) > 1e-4)
        base = c.sign * t.magicUnit * (double)step / std::fabs((double)grip);
    return std::min(0.35, std::max(-0.35, base));   // a colour cast, not a transform
}

// ---------------------------------------------------------------------------------------
// WHITE BALANCE FIRST, solved on the surfaces that ought to be neutral.
//
// Shared with the bench for the same reason everything else here is: `--wb` used to be a flag
// the bench parsed, printed as "wb on", and then never acted on -- advertising a capability it
// did not have, which is this project's single most repeated defect.
struct WhiteBalance {
    double kelvin = 6500.0;
    float  cover  = 0.f;   // % of frame that is USABLE reference, not just labelled as such
    float  b0     = 0.f;   // the reference's warm/cool error before correction
    bool   ok     = false;
};

// thumbSrc: 512*512*3 camera log. regions: 512*512 region labels. enc: display-referred.
static inline WhiteBalance solve_white_balance(const std::vector<float>& thumbSrc,
                                               const std::vector<unsigned char>& regions,
                                               int cam, int enc)
{
    WhiteBalance out;
    const size_t N = (size_t)512 * 512;
    if (thumbSrc.size() != N * 3 || regions.size() != N) return out;

    float Pn[analysis::kParamN] = {0.f,0.f,0.f, 0.f,1.f,1.f, 0.f,0.f, 0.f,1.f, 0.f,6500.f, 0.f};

    // A LIGHT SOURCE IS NOT A REFERENCE SURFACE, even when the model is right about what it is.
    // ADE20K's `windowpane` and `curtain` are both BUILT, so a blown window -- the brightest,
    // warmest thing in a daylit interior -- lands in the reference set and drags it yellow. The
    // solve then cools the entire frame to bring that back to zero, which is a blue cast over
    // everything, arrived at honestly.
    //
    // Excluded by luminance rather than by class, because the property that disqualifies a pixel
    // is physical, not semantic: once any channel is near its ceiling the channels have clipped
    // at different times and the hue is an artefact of clipping, not a measurement of the
    // illuminant. That covers practicals, specular highlights and blown sky for free, and needs
    // no change to the region table. The floor excludes pixels dark enough to be noise, where
    // chromaticity is equally meaningless.
    std::vector<size_t> ref;
    ref.reserve(4096);
    for (size_t i = 0; i < N; ++i) {
        const unsigned char r = regions[i];
        if (r != analysis::R_BUILT && r != analysis::R_GROUND) continue;
        float dr, dg, db;
        og::process(cam, enc, Pn, thumbSrc[i*3], thumbSrc[i*3+1], thumbSrc[i*3+2], dr, dg, db);
        const float mx = std::max(dr, std::max(dg, db));
        if (mx >= 0.90f || mx <= 0.05f) continue;
        ref.push_back(i);
    }
    // Coverage is of the USABLE reference, which is the quantity actually being trusted. A wall
    // that is 40% of frame but mostly blown window is not 40% of anything worth balancing on.
    out.cover = 100.f * (float)ref.size() / (float)N;
    if (out.cover < 15.f) return out;

    // MEDIAN, NOT MEAN -- the design rule this feature was built on and this function broke.
    // "Percentiles, never means: one blown practical wrecks a mean." The luminance filter above
    // already removes the worst offenders, but the median is what makes the estimate robust to
    // whatever survives it, and it costs a partial sort over about a thousand samples.
    auto refB = [&](double kelvin) {
        float P[analysis::kParamN];
        for (int i = 0; i < analysis::kParamN; ++i) P[i] = Pn[i];
        P[11] = (float)kelvin;
        std::vector<float> bs; bs.reserve(ref.size() / 4 + 1);
        for (size_t k = 0; k < ref.size(); k += 4) {
            const size_t i = ref[k];
            float r, g, b;
            og::process(cam, enc, P, thumbSrc[i*3], thumbSrc[i*3+1], thumbSrc[i*3+2], r, g, b);
            float L, a, bb; analysis::display_to_Lab(enc, r, g, b, L, a, bb);
            bs.push_back(bb);
        }
        if (bs.empty()) return 0.0;
        const size_t m = bs.size() / 2;
        std::nth_element(bs.begin(), bs.begin() + m, bs.end());
        return (double)bs[m];
    };

    // IS THE REFERENCE ACTUALLY NEUTRAL? Coverage says a lot of the frame is labelled wall or
    // floor; it does not say the model was right. A macro of grass comes back 98% "wall", and
    // balancing on it would correct the green out of the foliage -- a confident, wrong move on a
    // frame the rest of Magic Grade correctly declines to touch.
    //
    // Fitted on eight frames -- city 6.4, car interior 7.5, grass-as-wall 11.7 -- so 9 separates
    // them, and the margin is thin. The failure it buys is a false negative: a genuinely neutral
    // wall under a STRONG cast also reads as strongly coloured and gets declined, which is
    // exactly the case worth correcting. That is the safe direction to fail in, since declining
    // changes nothing and the checkbox is optional, but if real interiors start being refused
    // this number is the reason.
    {
        std::vector<float> as, bs;
        as.reserve(ref.size() / 4 + 1); bs.reserve(ref.size() / 4 + 1);
        for (size_t k = 0; k < ref.size(); k += 4) {
            const size_t i = ref[k];
            float r, g, b;
            og::process(cam, enc, Pn, thumbSrc[i*3], thumbSrc[i*3+1], thumbSrc[i*3+2], r, g, b);
            float L, a, bb; analysis::display_to_Lab(enc, r, g, b, L, a, bb);
            as.push_back(a); bs.push_back(bb);
        }
        if (as.empty()) return out;
        const size_t m = as.size() / 2;
        std::nth_element(as.begin(), as.begin() + m, as.end());
        std::nth_element(bs.begin(), bs.begin() + m, bs.end());
        const double ma = as[m], mb = bs[m];
        if (std::sqrt(ma*ma + mb*mb) > 9.0) return out;
    }

    out.b0 = (float)refB(6500.0);
    double lo = 2500.0, hi = 15000.0;
    if (refB(lo) > 0.0 || refB(hi) < 0.0) return out;   // target unreachable in a sane range
    for (int i = 0; i < 24; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (refB(mid) < 0.0) lo = mid; else hi = mid;
    }
    out.kelvin = 0.5 * (lo + hi);

    // white_balance() forces identity on 6499 < T < 6501 while the Planckian locus at 6500 K is
    // not D65, so stepping just off the default jumps a neutral grey by a* +2.06 (see
    // docs/AUTO-GRADE.md 9). Snapping a near-neutral answer to exactly 6500 avoids paying that
    // for a correction too small to see.
    if (std::fabs(out.kelvin - 6500.0) < 120.0) out.kelvin = 6500.0;
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------------------
// MAGIC TONE: place the SUBJECT so it is legible, and leave Bias somewhere to go.
//
// WHAT MAGIC GRADE IS FOR, as distinct from Creative. Creative makes the pronounced grade;
// Magic makes a smooth starting point whose Bias slider begins from a neutral place, so contrast
// can be added or removed from there. Creative's picture was pinned at both ends -- floor on its
// 0.050 target, ceiling at 0.993 with 1.12% of the frame already clipped -- so a Bias move in
// either direction immediately destroyed something. That is the defect this solves.
//
// LEGIBILITY IS A PROPERTY OF THE SUBJECT, so the floor condition is stated on the subject and
// not on the frame. The existing anti-crush guard protects the frame's black point, which is why
// it did not help: on the interview frame the frame floor sat correctly on target while the face
// was already at p10 0.078, and lowering Bias pushed the face into the mud with every guard
// reporting success.
//
// THREE CONDITIONS, THREE CONTROLS, and they are the three moves the user made by hand, in the
// order they made them:
//
//     subject's shadows -> subjFloor     Lift      ("reduce the contrast on the face")
//     frame's highlight -> frameCeiling  Gain      ("remove the hot spot")
//     subject's midtone -> subjMid       Gamma     ("bring the overall contrast down")
//
// TWO OF THE THREE ARE ABOUT THE SUBJECT, which is the point: legibility is a property of the
// thing being looked at. The first attempt held the FRAME's midtone instead, on the observation
// that it barely moved between the two grades (0.537 -> 0.529) -- and the solve diverged, lift
// running to its bound with gamma collapsing to 0.315. The frame mid barely moved because the
// subject came up (0.213 -> 0.278) while its surround came down (0.594 -> 0.570); holding the
// net was pinning a quantity that only looked still because two real moves cancelled inside it.
//
// Solved by coordinate passes rather than iteration-with-remeasurement. Each condition is
// monotone in its own control, and with Density gone the whole chain is closed form -- the same
// argument that retired the old loop in c4ec540. Re-measuring would also mean re-segmenting,
// which is how Magic Grade came to read its own output and converge over three presses.
//
// LUMA FOR THE SUBJECT, PER-CHANNEL FOR THE CEILING. Legibility is a luminance property; clipping
// is not. A waveform shows R, G and B independently and on a saturated highlight they spread far
// apart, so containing luma would let a channel clip while the number said the target was met.
// Swept on the interview frame: 1 pass lands subj 0.131 against a 0.135 target, 3 gets 0.133,
// 8 gets 0.134, 30 gets 0.135. Each pass is three scalar bisections, so 8 is free and well past
// the point the picture stops changing.
static const int kMagicTonePasses = 8;

struct MagicTone {
    float lift = 0.f, gamma = 1.f, gain = 1.f;
    float subjLo = 0.f, subjHi = 0.f, frameHi = 0.f, mid = 0.f;   // what it ACHIEVED
    // The subject's own contrast, subjHi - subjLo. Not a condition -- it FALLS OUT of the
    // floor and midtone targets -- but it is the quantity being judged when someone says the
    // face has too much or too little contrast, so it is reported rather than inferred.
    // The three NEUTRAL percentiles the solve was run against. They do not depend on Lift, Gamma
    // or Gain, so caching them lets Bias re-solve from three scalars instead of re-measuring
    // 200k samples -- which is what makes Bias a target move rather than a parameter drift.
    float sLo = 0.f, sMid = 0.f, sHi = 0.f, fHi = 0.f;
    bool  ok = false;
};

// THE SOLVE ITSELF, over three cached scalars. Separated from the measurement so Bias can call
// it on every drag: Bias shifts the TARGETS and re-solves, rather than nudging the parameters the
// solve just chose. Nudging them breaks all three conditions at once -- which is exactly what
// happened, one slider move undoing the whole grade.
static inline MagicTone solve_magic_tone_from(double sLo, double sMid, double sHi, double fHi,
                                              const float P0[analysis::kParamN],
                                              const float* lut, int lutSize,
                                              double subjFloor, double subjMid, double frameCeiling)
{
    MagicTone out;
    const double pe = P0[8], con = P0[9], roll = P0[12];
    auto render = [&](double v, double lf, double gm, double gn) {
        float x = og::lgg_core((float)v, (float)lf, (float)gm, (float)gn);
        float r = x, g = x, b = x;
        if (lut && lutSize >= 2) og::apply_lut(lut, lutSize, 1.f, r, g, b);
        og::apply_trim((float)pe, (float)con, r, g, b);
        if (roll > 0.f) g = og::softclip(g, (float)roll);
        return (double)g;
    };

    double lf = P0[3], gm = P0[4], gn = P0[5];
    for (int pass = 0; pass < kMagicTonePasses; ++pass) {
        lf = solve1d(-0.50, 0.50, subjFloor,    [&](double v) { return render(sLo,  v, gm, gn); });
        gm = solve1d( 0.30,  3.00, subjMid,     [&](double v) { return render(sMid, lf, v, gn); });
        gn = solve1d( 0.05,  3.00, frameCeiling,[&](double v) { return render(fHi,  lf, gm, v); });
    }
    // THE CEILING GIVES WAY TO THE SUBJECT, never the reverse.
    //
    // On a dark room with a bright window, holding the frame's highlight drove Gain to 0.217 and
    // Gamma into its 3.00 bound, and the subject STILL landed at 0.157 against a 0.278 target --
    // a face crushed into the floor to protect a practical. The user's read of that class of
    // shot: the window is not clipped at the sensor, so whether to contain it is an editorial
    // choice, and the face is not.
    //
    // Detected by the subject missing its midtone, which is what the failure actually looks like,
    // rather than by testing whether a control sits on a bound -- a bound can be reached
    // legitimately. The fallback drops the ceiling condition entirely and re-solves the two
    // subject conditions against Creative's gain, which converges because it is then 2x2.
    if (std::fabs(render(sMid, lf, gm, gn) - subjMid) > 0.01) {
        // GAIN CARRIES THE MIDTONE HERE, not Gamma, and it is allowed past Creative's ceiling.
        //
        // That ceiling (gainMax 0.80) exists so a deliberately low-key shot is never pushed up --
        // the clamp that made `key` descriptive rather than prescriptive, and it is right for a
        // frame median. It is wrong for a face: a subject too dark to read is underexposure, not
        // intent, and the whole point of finding the subject is to be able to tell those apart.
        //
        // Gamma goes back to neutral rather than being solved. Left to carry the midtone it ran
        // straight into its 3.00 bound and stretched the subject across 0.875 of the range, which
        // is a face blown at the top instead of crushed at the bottom -- the same failure wearing
        // the other end. Exposure is Gain's job; Gamma's business is the shape in between.
        gm = P0[4];
        for (int pass = 0; pass < kMagicTonePasses; ++pass) {
            lf = solve1d(-0.50, 0.50, subjFloor, [&](double v) { return render(sLo,  v, gm, gn); });
            gn = solve1d( 0.05,  2.00, subjMid,  [&](double v) { return render(sMid, lf, gm, v); });
        }
    }

    out.lift = (float)lf; out.gamma = (float)gm; out.gain = (float)gn;
    out.sLo = (float)sLo; out.sMid = (float)sMid; out.sHi = (float)sHi; out.fHi = (float)fHi;
    out.subjLo  = (float)render(sLo,  lf, gm, gn);
    out.subjHi  = (float)render(sHi,  lf, gm, gn);
    out.frameHi = (float)render(fHi,  lf, gm, gn);
    out.mid     = (float)render(sMid, lf, gm, gn);
    out.ok = true;
    return out;
}


static inline MagicTone solve_magic_tone(const analysis::SampleSet& S, int subject,
                                         int cam, int enc,
                                         const float* lut, int lutSize,
                                         const float P0[analysis::kParamN], const Tunables& t)
{
    MagicTone out;
    const size_t n = S.rgb.size() / 3;
    if (n < 64 || S.region.size() != n) return out;

    // ONLY WHERE THE TARGETS MEAN SOMETHING. All three came from a face, and a face is the one
    // subject whose correct lightness is not a matter of taste -- lit skin sits a stop or so over
    // mid-gray whatever the shot is. Nothing else does: sky belongs near the top, foliage low,
    // sand bright, and forcing any of them to a face's numbers is not a grade, it is damage.
    //
    // Observed on a beach frame where the subject came back VEGETATION: driving dark foliage to a
    // subject midtone of 0.278 needed enough lift and gamma to put the sky into neon cyan, and
    // the picture was destroyed by a solve that met every condition it was given.
    //
    // So it declines rather than guesses, and Creative's behaviour stands. Extending this to other
    // subjects is a DATA question, not a code one: it needs a hand-graded landscape the way the
    // face targets needed a hand-graded interview. Until then a wrong target is worse than none,
    // because the whole point of the button is that its bad cases are impossible rather than rare.
    if (subject != analysis::R_SKIN) return out;

    // AND ONLY WHEN THE MASK PLAUSIBLY IS A FACE. Coverage is the tell, the same tell
    // skin_trustworthy() already uses at 25% for the chromatic mask: a face occupies a modest
    // share of frame, and when the number climbs the label has stopped meaning what it says.
    //
    // One frame came back SKIN 43% with the region spanning 0.875 of the tonal range and its p90
    // pinned at 1.000. No monotone curve can place p10 and p50 for a region that wide and still
    // keep its top inside the picture, and the solve duly ran Gain to its bound trying: the
    // symptom of an infeasible target is a control on a bound, and the cause was that the mask
    // covered a dark interior AND a window rather than a face.
    //
    // Set above the chromatic mask's 25% because this one is a segmentation label rather than a
    // hue window, so a genuine close-up can legitimately read higher.
    {
        size_t cover = 0;
        for (size_t i = 0; i < n; ++i) if ((int)S.region[i] == subject) ++cover;
        if ((double)cover > 0.35 * (double)n) return out;
    }

    float Pn[analysis::kParamN] = {0.f,0.f,0.f, 0.f,1.f,1.f, 0.f,0.f, 0.f,1.f, 0.f,6500.f, 0.f};
    Pn[11] = P0[11];   // keep the white balance; it is not a tone control

    // Neutral render once, in the encode the grade curve runs in. Everything below is a solve on
    // these numbers, so they must come from the render's own space (see docs/AUTO-GRADE.md 2).
    std::vector<float> subjL, allL, allC;
    subjL.reserve(n / 4 + 1); allL.reserve(n); allC.reserve(n * 3);
    for (size_t i = 0; i < n; ++i) {
        float r, g, b;
        og::process(cam, enc, Pn, S.rgb[i*3], S.rgb[i*3+1], S.rgb[i*3+2], r, g, b);
        const float L = 0.2126f*r + 0.7152f*g + 0.0722f*b;
        allL.push_back(L);
        allC.push_back(r); allC.push_back(g); allC.push_back(b);
        if ((int)S.region[i] == subject) subjL.push_back(L);
    }
    if (subjL.size() < 32) return out;   // no subject worth placing

    auto pct = [](std::vector<float> v, double q) {
        const size_t k = (size_t)(q * (v.size() - 1));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return (double)v[k];
    };
    return solve_magic_tone_from(pct(subjL, 0.10), pct(subjL, 0.50), pct(subjL, 0.90),
                                 pct(allC, 0.999),
                                 P0, lut, lutSize, t.subjFloor, t.subjMid, t.frameCeiling);
}

} // namespace grade
} // namespace og
