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
                                  float P[analysis::kParamN])
{
    creative_preset(P);
    if (!m.valid) return;

    const double gain = std::min(t.gainMax, std::max(t.gainMin,
                                 t.gainBase + t.gainPerKey * m.key));
    const double rolloff = std::min(t.rolloffMax, std::max(0.0, t.rolloffPerPin * m.pin));

    // The whole chain in closed form, which works because the grade curve is monotonic and a
    // monotonic map commutes with percentiles: one measured scalar stands in for the frame.
    const double pe = P[8], gm = P[4];
    const double lift = solve1d(-0.50, 0.50, t.blackTarget, [&](double lf) {
        const double v = (double)og::lgg_core((float)m.d01, (float)lf, (float)gm, (float)gain)
                       * std::exp2(pe);
        return rolloff > 0.0 ? (double)og::softclip((float)v, (float)rolloff) : v;
    });

    P[3]  = (float)lift;
    P[5]  = (float)gain;
    P[12] = (float)rolloff;
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

} // namespace grade
} // namespace og
