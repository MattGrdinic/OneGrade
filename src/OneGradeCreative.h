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

} // namespace grade
} // namespace og
