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

    // AIM LOWER, SETTLE FOR WHAT THE FRAME ALLOWS.
    //
    // 0.968 came from one hand-graded interview and survived being tested against 851 films, which
    // is why it is still the ceiling of the range rather than replaced by the corpus median. But
    // that frame is underexposed, and fitting the only number to the worst shot is how a starting
    // point ends up tuned for footage nobody wants to shoot. Swept 0.890/0.920/0.968 on five clips:
    // four were VISUALLY IDENTICAL at all three and the fifth -- the badly lit one -- clipped a
    // face below 0.968. So the low end costs nothing on footage that was lit, and the high end is
    // needed only where the frame cannot give it.
    //
    // Hence a range, not a value: ask for frameCeilingLow, and walk back up only as far as the
    // frame forces. Infeasible here means the ceiling started fighting the subject (branch 2) --
    // the one branch whose far side is a washed-out picture, and the same test Bias holds at.
    // Equal to frameCeiling disables the search and restores the single-value behaviour exactly.
    double frameCeilingLow = 0.890;

    // Where an underexposed subject's NEUTRAL midtone has to reach before the tone solve runs.
    // Read off the frames that already work: their subjects sit near this without help, so a shot
    // below it is one the grade stage cannot rescue on its own.
    // Where an underexposed subject's NEUTRAL midtone has to reach before the tone solve runs.
    // 0.28 puts the user's own test frame at 2.20 EV against the 2.13 they chose by hand.
    double subjNeutralMid = 0.28;

    // THE FRAME'S OWN FLOOR, as a ceiling on what placing the subject may cost. Satisfying a very
    // dark subject's p10 is a global lift, and on an underexposed shot it dragged the frame's
    // black to 0.151 where every other frame in the set sits between 0.04 and 0.08 -- the shot
    // brightened, but "lost a good bit of its original intent". Low-key has to survive being
    // made legible.
    double frameFloorMax = 0.085;

    // THE OTHER SIDE OF THE SAME GUARD, and it was missing until sky.
    //
    // frameFloorMax stops a DARK subject dragging the frame's black up. Nothing stopped a BRIGHT
    // subject crushing it down, because with skin as the only subject nothing was ever bright
    // enough to push that way. Placing a sky's shadows sent Lift to -0.211 on one clip and took
    // its crushed share from 6.33% to 23.23%, shadow separation collapsing 0.049 -> 0.001.
    //
    // A guard fitted to the only case that had ever come up looked complete for as long as that
    // was the only case.
    // INERT AT THIS VALUE, AND THE VALUE IS NOT THE PROBLEM. Swept 0.00/0.02/0.04/0.06 on the
    // four sky clips: the first three changed nothing at all and 0.06 moved one clip from 23.2%
    // crushed to 12.5%, still worse than the 6.3% Creative gave it.
    //
    // The guard is anchored on the wrong statistic. `crushed%` counts EVERY pixel whose MIN
    // channel is at or under 1/255; fLo is the min channel of ONE pixel, the one ranked p0.1 by
    // MAX channel. A saturated pixel ranks high on max while its min sits at zero, so the pixel
    // being guarded is not the darkest pixel by the measure that matters and the two barely track.
    //
    // Same shape as the black-point encode bug and hot-versus-pin: the number compared against a
    // constant has to be the number that matters. Fixing it means a separate statistic for this
    // guard -- a percentile of per-pixel MIN across the frame -- rather than reusing fLo, which is
    // shared with frameFloorMax and would move every validated face grade if changed.
    double frameFloorMin = 0.020;
    double rawExpMax      = 4.0;   // stops; beyond this the shot is not underexposed, it is noise

    // ---------------------------------------------------------------------------------------
    // PER-SUBJECT TONE TARGETS -- why the solve declined everything that was not a face.
    //
    // subjFloor/subjMid above are absolute display values measured on ONE hand-graded interview,
    // and the old gate refused every other subject rather than apply them. That was right: a
    // beach frame whose subject came back VEGETATION was destroyed by being driven to a face's
    // midtone, sky in neon cyan and red pinned flat at zero, while the solve met every condition
    // it was given.
    //
    // But the reason given for the gate does not survive measurement. "A face is the one subject
    // whose correct lightness is not a matter of taste" predicts skin should be the most
    // consistent region across films, and it is not: over 350 films SKY lands at 0.634 with a
    // relative spread of 22%, three times TIGHTER than skin's 73%. (Skin's figure is inflated by
    // ADE20K class 12 being "person" -- whole body, wardrobe and hair -- so that comparison is
    // unfair to skin rather than damning; sky has no such confound.) What the numbers do support
    // is that sky is a better candidate for an absolute target than skin ever was.
    //
    // So the gate becomes a LOOKUP rather than a species test. A region with a measured target
    // gets the tone solve; one without still declines, so nothing is guessed and the default
    // behaviour for every unmeasured region is exactly what it was before.
    //
    // maxCover is per-region for the same reason the target is. The 0.35 ceiling is a
    // face-plausibility check -- a mask calling 43% of the frame skin has stopped meaning what it
    // says -- and it is nonsense elsewhere: sky is over 35% of the frame in HALF of all films, and
    // built in 77%. Applying one ceiling everywhere would reject the very frames the target was
    // measured on.
    struct RegionTone {
        double floor    = 0.0;
        double mid      = 0.0;
        double maxCover = 1.0;   // fraction of frame above which the label stops being credible
        bool   has      = false; // false -> decline, exactly as before
    };
    // Indexed by analysis::Region: SKY, WATER, SKIN, VEG, TERRAIN, GROUND, BUILT, OTHER.
    RegionTone region[analysis::kRegionN] = {
        // Measured over 851 films: floor 0.475 (MAD 0.218), mid 0.602 (MAD 0.207). Relative
        // spread 34%, the tightest of any region -- water is next at 42%, skin 71%, built 84%.
        // maxCover 0.90 because sky is over 35% of frame in half of all films and over 76% in a
        // tenth; the face ceiling would have rejected the very frames this was measured on.
        // MEASURED BUT NOT ENABLED, pending the guard below. On the user's four sky clips all
        // four solved and landed on target (mid 0.600-0.609 against 0.602), but one took Lift
        // -0.211 and its crushed share went 6.33% -> 23.23% with shadow separation 0.049 -> 0.001.
        // Two improved and one was unchanged; shipping one-in-four worse is not shipping.
        //
        // THE GUARD IS ONE-SIDED AND THAT IS THE BUG. frameFloorMax stops a DARK subject dragging
        // the frame's black up; nothing stops a BRIGHT subject crushing it down. Sky is simply the
        // first subject bright enough to push that way, so the asymmetry never surfaced. The fix
        // is the branch-swap that already exists for the upper bound: when placing the subject
        // would take the frame's floor below a minimum, Lift serves the frame and Gamma carries
        // the subject's midtone.
        // ENABLED once the lower guard was re-anchored on luma -- see the frameFloorMin branch.
        // The guard existed before this and was inert, because it read the min channel of a pixel
        // ranked by max channel, so it never saw the crushing it was there to prevent.
        /* SKY     */ { 0.475, 0.602, 0.90, true  },
        /* WATER   */ { 0.0,   0.0,   1.00, false },
        /* SKIN    */ { 0.125, 0.278, 0.35, true  },   // the hand-graded interview, unchanged
        /* VEG     */ { 0.0,   0.0,   1.00, false },
        /* TERRAIN */ { 0.0,   0.0,   1.00, false },
        /* GROUND  */ { 0.0,   0.0,   1.00, false },
        /* BUILT   */ { 0.0,   0.0,   1.00, false },
        /* OTHER   */ { 0.0,   0.0,   1.00, false },
    };
};

// A TARGET MAY NEVER ASK FOR WHAT THE ACCEPTANCE TEST REJECTS.
//
// solve_magic_tone_from() declines a result whose frame highlight reaches kFrameBlown, and Bias
// shifts the ceiling TARGET by -bias*0.03 -- so negative Bias walked the target upward until it
// clamped at 1.000, which is above the blown threshold. From about bias -1.07 down, on EVERY
// frame, the solve was being asked for precisely the thing the next line then refused, so it
// could not succeed no matter what the footage looked like. applyBias() then fell through to its
// coefficient path and the picture jumped.
//
// The clamp belongs here, next to the test it has to stay under, rather than at the two call
// sites that shift the target. Same lesson as the encode split: a number compared against a
// constant has to live in the same space as that constant -- here, on the same side of it.
static const double kFrameBlown      = 0.999;   // render(fHi) at or above this is blown
static const double kFrameCeilingMax = 0.990;   // ...so never TARGET above this

// WHERE A SOURCE VALUE LANDS under a grade, through the LUT and the trim -- the picture the
// viewer is actually judging. The tone solve places its targets with this, and callers ask it
// "where is the subject NOW?" with the same function, so the question and the answer cannot
// drift apart. It was a lambda inside the solve until a caller needed to ask.
static inline double tone_render(double v, double lf, double gm, double gn,
                                 double pe, double con, double roll,
                                 const float* lut, int lutSize)
{
    float x = og::lgg_core((float)v, (float)lf, (float)gm, (float)gn);
    float r = x, g = x, b = x;
    if (lut && lutSize >= 2) og::apply_lut(lut, lutSize, 1.f, r, g, b);
    og::apply_trim((float)pe, (float)con, r, g, b);
    if (roll > 0.f) g = og::softclip(g, (float)roll);
    return (double)g;
}

// The three conditions a grade currently MEETS, in the order the solve names them. This is what
// lets a hand edit become the thing Bias leans away from: measure what the hand did, and Bias
// offsets from there instead of from the constants the button was solved to.
struct ToneTargets { double floor = -1.0, mid = -1.0, ceil = -1.0, floorMax = -1.0,
                     surr = -1.0; };

// floorMax IS PART OF THE ANSWER, not a detail. Solving to the conditions a grade already meets
// only gives that grade back if EVERY constraint agrees it is acceptable, and the frame-floor cap
// is a constraint: a grade sitting above it gets its Lift taken away and reassigned, so the round
// trip lands somewhere else entirely. Caught by the bench refusing to preserve an edit of exactly
// zero -- re-solving the untouched grade moved Lift 0.084 -> 0.066 and Gamma 1.199 -> 1.264.
static inline ToneTargets tone_targets_of(double sLo, double sMid, double fHi, double fLo,
                                          const float P[analysis::kParamN],
                                          const float* lut, int lutSize, double sSur = -1.0)
{
    ToneTargets t;
    // The surround's CURRENT midtone, which is what makes Tone Separation's origin the identity:
    // at sep 0 the fourth condition asks for exactly what is on screen, so adding it changes
    // nothing. Left unset when the caller has no surround reading, which keeps the condition off.
    if (sSur >= 0.0)
        t.surr = tone_render(sSur, P[3], P[4], P[5], P[8], P[9], P[12], lut, lutSize);
    t.floor    = tone_render(sLo,  P[3], P[4], P[5], P[8], P[9], P[12], lut, lutSize);
    t.mid      = tone_render(sMid, P[3], P[4], P[5], P[8], P[9], P[12], lut, lutSize);
    t.ceil     = tone_render(fHi,  P[3], P[4], P[5], P[8], P[9], P[12], lut, lutSize);
    t.floorMax = tone_render(fLo,  P[3], P[4], P[5], P[8], P[9], P[12], lut, lutSize);
    return t;
}

// How far one unit of Bias moves each target. Taste, unlike the two above, but they belong
// together: these are what walk the ceiling into the clamp.
static const double kBiasSubjFloorPer = 0.06;
static const double kBiasCeilingPer   = 0.03;

// TONE SEPARATION moves the subject's MIDTONE, and nothing else -- which is what keeps it a
// separate control from Bias rather than a second way to drive the same one. Bias owns the
// subject's floor and the frame's ceiling (Lift and Gain); this owns the midtone (Gamma). Two
// sliders, two targets, two controls, so they can be compared on one frame without either
// explaining the other's result.
//
// WHY A TARGET AND NOT A PARAMETER. The rdL* row of the descriptor Jacobian, measured on the
// user's footage, is dominated by lift, gamma, gain and post contrast -- every control the tone
// solve already owns -- and its signs flip between frames depending on which side of the contrast
// pivot the subject sits. Writing any of them directly would undo the three conditions that place
// the subject, which is the failure Bias was rebuilt to avoid ("if I touch the bias slider we kill
// the grade"). Moving the target and re-solving cannot: the other two conditions still hold.
//
// MEASURED AND REJECTED AS THE SEPARATION AXIS -- kept because the plumbing is what any
// separation law needs, NOT because this one works. Walked across its full range on a face clip
// with the achieved rdL* beside it: rdL* moved -18.05 to -19.96, about 1.9 L* units or 10%, while
// Lift went 0.001 -> 0.173 and Gamma 1.914 -> 0.890. An enormous change in the picture for almost
// none in the quantity the control is named after.
//
// The reason is structural and should have been predictable: the subject and its surround are
// placed by ONE curve, so moving the subject's target drags the surround along with it and the gap
// between them barely opens. Separation needs the two moved DIFFERENTLY, which means a condition
// on the surround -- a fourth condition on a fourth control, with post contrast the candidate
// (it pivots at 0.5, between a subject at 0.278 and a brighter surround, and it carries the
// largest consistent d(rdL*)/dp in the Jacobian). See docs/ROADMAP.md.
//
// This is the fourth time on this feature that a plausible control law produced a confident number
// and no picture, and the fourth time the bench caught it by rendering the result instead of
// trusting the solve's own report.
static const double kToneSepMidPer = 0.05;

// Sentinels, named because -1.0 appears in four argument slots and they do not all mean the same
// thing: one turns a guard off, one says "no separate floor reading, reuse fLo".
static const double kFrameFloorMinOff = -1.0;
static const double kFloorReadUnset   = -1.0;

// Below this, the subject and its surround are at the same lightness and "further apart" has no
// direction to point in. The slider goes inert and says so rather than picking one: a control that
// moves the picture in an arbitrary direction is worse than a control that admits it has nothing
// to act on. In L* units, where the footage measured so far spans roughly 2 to 34.
static const double kToneSepMinDL = 3.0;

// The direction "more separated" points in, from the region separation triple. Returns 0 when the
// two are too close in tone for the question to have an answer.
static inline double tone_sep_dir(double rdL)
{
    if (std::fabs(rdL) < kToneSepMinDL) return 0.0;
    return (rdL > 0.0) ? 1.0 : -1.0;
}

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
// PRE-LUT BY DEFAULT, and the callers pass no LUT deliberately.
//
// Solving this post-LUT is defensible on paper -- the black point is judged on screen, and the
// print stock's toe eats most of the margin left before it -- and it flattened every landscape in
// the set. Chasing it moved pre-LUT floors from 0.050 to between 0.13 and 0.38, which is three to
// seven times higher on every frame, and the sunsets went first.
//
// It was chasing `crushed%`, a metric the user had already said did not match what they saw: the
// frames it flagged worst were ones they called good starting points. A shot with a large
// silhouette is SUPPOSED to sit on the floor. Same lesson as `hot` versus `pin` earlier in this
// project -- a measurement that reports an intended property as a defect, and optimising against
// it makes the picture worse while the number improves.
//
// What survives from that work and is worth keeping: solving on real pixels rather than a grey
// scalar, so the Magic colour move is accounted for. Pre-LUT that costs nothing, since the grade
// curve is per-channel and monotonic; it earns its keep only because Offset and Gain Temp move
// the channels after the floor is placed.
static inline void solve_black_px(const analysis::SampleSet& S, int cam, int enc,
                                  float P[analysis::kParamN], double blackTarget,
                                  const float* lut, int lutSize)
{
    const size_t n = S.rgb.size() / 3;
    if (n < 512) return;

    // Rank by the neutral render's min channel: what crushes is a CHANNEL, not a luminance, and
    // on a saturated shadow the channels sit far apart.
    float Pn[analysis::kParamN]; analysis::neutral_params(Pn);
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
    // THE SAME PERCENTILE THE SCALAR SOLVE PLACES -- p0.1 of the WHOLE frame, not of the dark
    // slice. The slice is only how the candidates are gathered; the quantity being placed has to
    // stay the one every constant was fitted against.
    //
    // Indexing into the slice by keep/100 quietly placed the frame's 0.02 percentile instead,
    // which is a darker pixel, so the solve lifted further to get it onto target: floors came out
    // at 0.13 rather than 0.05 and the landscapes flattened. A percentile changed by accident is
    // indistinguishable from a target changed on purpose, and it looks like a tuning problem
    // right up until you compare the two definitions.
    //
    // Not a minimum, either: one pixel of sensor noise would set the whole frame's lift.
    const size_t q = std::min(keep - 1, (size_t)(0.001 * (double)n));
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
    float Pn[analysis::kParamN]; analysis::neutral_params(Pn);
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
    // WHICH decline, not just that it declined. "No reference" is a sunset with no neutral
    // surface in it; "not neutral" is the reference itself looking wrong; "unreachable" is a cast
    // no sane colour temperature fixes. They call for completely different responses and a bare
    // false makes them indistinguishable -- which cost an hour of looking at the wrong check.
    const char* why = "";
    bool   ok     = false;
};

// thumbSrc: 512*512*3 camera log. regions: 512*512 region labels. enc: display-referred.
// POST-LUT, because "neutral" is a judgement about the picture on screen.
//
// This measured the reference through og::process alone, and so declared a frame already
// balanced while the graded result visibly was not. Across twelve frames the checkbox moved the
// render by under 4/255 on four shots and not at all on seven -- doing its job perfectly against
// an image nobody was looking at. Fourth instance in one day of a number computed in one space
// and judged in another.
//
// The print stock's own colour character is deliberately NOT what this corrects: it is a look and
// it belongs. What it corrects is a surface that should read neutral and does not, measured where
// the eye reads it.
static inline WhiteBalance solve_white_balance(const std::vector<float>& thumbSrc,
                                               const std::vector<unsigned char>& regions,
                                               int cam, int enc,
                                               const float* lut = nullptr, int lutSize = 0,
                                               float postExp = 0.f, float postCon = 1.f)
{
    const int dispEnc = (enc <= 2) ? enc : 1;   // for thresholds only; see the chroma check
    auto shade = [&](const float* P, size_t i, float& r, float& g, float& b) {
        og::process(cam, enc, P, thumbSrc[i*3], thumbSrc[i*3+1], thumbSrc[i*3+2], r, g, b);
        if (lut && lutSize >= 2) og::apply_lut(lut, lutSize, 1.f, r, g, b);
        og::apply_trim(postExp, postCon, r, g, b);
    };
    WhiteBalance out;
    const size_t N = (size_t)512 * 512;
    if (thumbSrc.size() != N * 3 || regions.size() != N) return out;

    float Pn[analysis::kParamN]; analysis::neutral_params(Pn);

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
        shade(Pn, i, dr, dg, db);
        const float mx = std::max(dr, std::max(dg, db));
        if (mx >= 0.90f || mx <= 0.05f) continue;
        ref.push_back(i);
    }
    // Coverage is of the USABLE reference, which is the quantity actually being trusted. A wall
    // that is 40% of frame but mostly blown window is not 40% of anything worth balancing on.
    out.cover = 100.f * (float)ref.size() / (float)N;
    if (out.cover < 15.f) { out.why = "no ref"; return out; }

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
            // AGAINST THE STOCK'S OWN NEUTRAL, not against zero.
            //
            // Targeting b* = 0 post-LUT asks Scene White Balance to cancel the print stock, and
            // the stock's colour character is the look -- it belongs. On one frame with 63% good
            // reference no temperature between 2500 K and 15000 K could reach zero, and the
            // estimator reported "unreachable" for a shot that needed almost no correction.
            //
            // So each reference pixel is compared with what a NEUTRAL surface of its own
            // luminance renders as through the same chain. What survives that subtraction is the
            // cast on the surface; what cancels is the stock being itself.
            // AGAINST ZERO, INCLUDING THE PRINT STOCK'S OWN CAST.
            //
            // A previous version subtracted what a neutral surface of the same luminance renders
            // as through the stock, so 2383's blue cancelled by construction and stayed in the
            // picture. That treats the cast as part of the look -- which it is -- but makes it a
            // DEFAULT rather than a choice.
            //
            // The user's call, and the better shape: start as neutral as the controls can get and
            // let Separation dial the blue back in or out deliberately. Magic Grade's asset is the
            // subject's tone; colour is offered, not imposed.
            float r, g, b;
            shade(P, i, r, g, b);
            float L, a, bb; analysis::display_to_Lab(1, r, g, b, L, a, bb);
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
            // PRE-LUT, DELIBERATELY, and it is the one measurement here that stays that way.
            //
            // This is a THRESHOLD -- "is a surface that claims to be neutral actually neutral" --
            // and 9 was fitted on eight frames without a print stock in the chain. The solve above
            // is post-LUT because it is judged on screen; this is compared against a constant, so
            // it needs the space that constant was chosen in. Running it post-LUT rejected a
            // frame with 63% good reference, because 2383 adds saturation and every fitted number
            // moved out from under it.
            //
            // Same distinction as the black point, written up in docs/AUTO-GRADE.md 2. It applies
            // WITHIN one function here, which is why it was easy to get wrong.
            float r, g, b;
            og::process(cam, dispEnc, Pn, thumbSrc[i*3], thumbSrc[i*3+1], thumbSrc[i*3+2], r, g, b);
            float L, a, bb; analysis::display_to_Lab(dispEnc, r, g, b, L, a, bb);
            as.push_back(a); bs.push_back(bb);
        }
        if (as.empty()) { out.why = "empty"; return out; }
        const size_t m = as.size() / 2;
        std::nth_element(as.begin(), as.begin() + m, as.end());
        std::nth_element(bs.begin(), bs.begin() + m, bs.end());
        const double ma = as[m], mb = bs[m];
        if (std::sqrt(ma*ma + mb*mb) > 9.0) { out.why = "ref not neutral"; return out; }
    }

    // A PLAUSIBLE WHITE BALANCE, not the whole legal range. 2500-15000 K is what the parameter
    // accepts; it is not what a starting point should ever be. With the wider bounds, a frame
    // whose reference stayed blue at every temperature clamped to 15000 K -- the maximum warm
    // shift the control has, spent on a residual of -2.1, over-warming everything else in the
    // picture while barely denting the cast it was aimed at. Clamping is only reasonable if the
    // ends are reasonable.
    double lo = 3500.0, hi = 9500.0;
    // AS NEUTRAL AS THE CONTROL CAN GET, rather than declining when it cannot reach zero.
    //
    // RAW Temp walks the Planckian locus and the stock's cast is not obliged to sit on it, so on
    // some frames no temperature in a sane range reaches b* = 0 -- one shot with 63% usable
    // reference declined outright on exactly that. Declining leaves the WHOLE cast in; clamping
    // to the nearest end removes most of it. "As neutral as we can" is literally a clamp, and the
    // slider covers what is left.
    const double bLo = refB(lo), bHi = refB(hi);
    out.b0 = (float)refB(6500.0);
    if (bLo > 0.0) { out.kelvin = lo; out.why = "clamped cool"; out.ok = true; return out; }
    if (bHi < 0.0) { out.kelvin = hi; out.why = "clamped warm"; out.ok = true; return out; }
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
    float sLo = 0.f, sMid = 0.f, sHi = 0.f, fHi = 0.f, fLo = 0.f;
    float rawExp = 0.f;   // scene-linear stops added to rescue an underexposed subject
    float ceil = 0.f;     // the frame highlight target actually solved against, after the search
    float frameLo = 0.f;  // the frame's own floor, which placing the subject must not wash out
    // THE FOURTH CONDITION, present only when Tone Separation asks for one. `con` is the post-LUT
    // Contrast the solve chose, `sSur` the surround's neutral midtone it was solved against and
    // `surr` what that midtone ended up at. Contrast rather than any of the other three because
    // it is the only tone control the subject conditions do not already own, and because it
    // pivots at 0.5 -- between a subject at 0.278 and a brighter surround, which is what lets the
    // two move APART instead of together.
    float con = 1.f, sSur = 0.f, surr = 0.f;
    float sMidNeutral = 0.f;   // the subject's midtone before any of this, for diagnosis
    const char* why = "";   // which decline, when ok is false -- see solve_white_balance
    // WHICH CONDITIONS BOUND, as a bitmask: 1 = the frame's floor took Lift off the subject,
    // 2 = the ceiling gave way and Gain carries the midtone. Reported because a change of
    // branch is a change of WHICH CONTROL DOES WHAT, and that is a step change in the picture
    // however smoothly the targets were moved -- Bias has to be able to see one coming.
    int   branch = 0;
    bool  ok = false;
};

// THE SOLVE ITSELF, over three cached scalars. Separated from the measurement so Bias can call
// it on every drag: Bias shifts the TARGETS and re-solves, rather than nudging the parameters the
// solve just chose. Nudging them breaks all three conditions at once -- which is exactly what
// happened, one slider move undoing the whole grade.
static inline MagicTone solve_magic_tone_from(double sLo, double sMid, double sHi, double fHi,
                                              const float P0[analysis::kParamN],
                                              const float* lut, int lutSize,
                                              double subjFloor, double subjMid, double frameCeiling,
                                              double fLo, double frameFloorMax,
                                              double frameFloorMin = -1.0,
                                              double fLoY = -1.0,
                                              double sSur = 0.0, double surrMid = -1.0)
{
    MagicTone out;
    const double pe = P0[8], roll = P0[12];

    // THE FOURTH CONDITION IS ABSENT AT REST, not fitted to a constant -- which is what makes this
    // safe to add to a solve every validated grade stands on. surrMid < 0 means "no surround
    // condition", the loops below stay exactly three passes, and a fresh Magic Grade is bit
    // identical to what it was. The condition only exists once Tone Separation is off zero, and
    // its target is then whatever the grade already achieves, so the slider's own origin is the
    // identity too.
    //
    // Contrast has to be a solved variable rather than a constant read from P0, so `cn` is what
    // render() closes over. Every existing call site is unchanged because it reads the same
    // variable; only the new pass writes it.
    const bool useSurr = (surrMid >= 0.0);
    double cn = P0[9];
    auto renderC = [&](double v, double lf, double gm, double gn, double c) {
        return tone_render(v, lf, gm, gn, pe, c, roll, lut, lutSize);
    };
    auto render = [&](double v, double lf, double gm, double gn) {
        return renderC(v, lf, gm, gn, cn);
    };
    // One more coordinate pass, in the same form as the other three: each condition is monotone in
    // its own control. Run LAST in each round so the three subject/frame conditions are solved
    // against the contrast the previous round settled on, rather than chasing a value that moves
    // underneath them mid-round.
    auto surrPass = [&](double lf, double gm, double gn) {
        if (!useSurr) return;
        cn = solve1d(0.05, 2.00, surrMid, [&](double c) { return renderC(sSur, lf, gm, gn, c); });
    };

    double lf = P0[3], gm = P0[4], gn = P0[5];
    auto frameLo = [&](double l, double g, double n) { return render(fLo, l, g, n); };
    for (int pass = 0; pass < kMagicTonePasses; ++pass) {
        lf = solve1d(-0.50, 0.50, subjFloor,    [&](double v) { return render(sLo,  v, gm, gn); });
        gm = solve1d( 0.30,  3.00, subjMid,     [&](double v) { return render(sMid, lf, v, gn); });
        gn = solve1d( 0.05,  3.00, frameCeiling,[&](double v) { return render(fHi,  lf, gm, v); });
        surrPass(lf, gm, gn);
    }
    // LOW KEY HAS TO SURVIVE BEING MADE LEGIBLE.
    //
    // Lift is global, so dragging a dark subject's shadows onto their target takes the whole
    // picture with it. On an underexposed frame that put the frame's own black at 0.151 where
    // every other shot in the set sits between 0.04 and 0.08: brighter, and in the user's words
    // it "lost a good bit of its original intent".
    //
    // The fix is to SWAP WHICH CONDITION BINDS rather than to back the subject floor off in
    // steps. Lift stops serving the subject's shadows and serves the frame's instead; Gamma still
    // places the subject's midtone and Gain still holds the ceiling, so it stays three conditions
    // on three controls and stays well posed. Legibility survives, because legibility lives in
    // the midtone -- what gives ground is only how far the subject's shadows come up.
    //
    // Stepping the floor down in a loop was tried first and was worse: it re-solved all three
    // conditions each pass, fought the fallback below, and ended up declining the frame outright
    // -- which handed the shot back to Creative at a black point of 0.002 and no shadow
    // separation at all, worse than the overshoot it was fixing.
    int branch = 0;
    if (frameLo(lf, gm, gn) > frameFloorMax) {
        branch |= 1;
        for (int pass = 0; pass < kMagicTonePasses; ++pass) {
            lf = solve1d(-0.50, 0.50, frameFloorMax, [&](double v) { return frameLo(v, gm, gn); });
            gm = solve1d( 0.30,  3.00, subjMid,      [&](double v) { return render(sMid, lf, v, gn); });
            gn = solve1d( 0.05,  3.00, frameCeiling, [&](double v) { return render(fHi,  lf, gm, v); });
            surrPass(lf, gm, gn);
        }
    }

    // ...AND THE SAME SWAP WHEN THE SUBJECT IS TOO BRIGHT.
    //
    // Exactly the branch above with the comparison reversed, because it is exactly the same
    // failure: Lift is global, so pulling a bright subject's shadows DOWN onto their target takes
    // the whole picture with it and the foreground goes to black. Lift serves the frame's floor,
    // Gamma keeps the subject's midtone, Gain keeps the ceiling -- still three conditions on three
    // controls.
    //
    // AND IT IS ANCHORED ON LUMA, NOT ON THE MIN CHANNEL -- which is the whole reason it was inert.
    //
    // fLo is the MIN channel of the pixel ranked p0.1 by MAX channel, and those are two different
    // pixels. A saturated pixel has a bright max and a min at zero, so it ranks nowhere near the
    // bottom on the ranking while sitting at the bottom of the value being read: the sample this
    // guard protected was not the darkest thing in the frame by any measure a viewer uses. Swept
    // 0.00/0.02/0.04/0.06 on the four sky clips and the first three changed nothing at all.
    //
    // Third instance of the same defect -- after the black-point encode bug and hot-versus-pin --
    // and it was found the same way, by the bench reporting a correct picture as 43.8% crushed
    // until that statistic was re-anchored on luma too. THE NUMBER COMPARED AGAINST A CONSTANT HAS
    // TO BE THE NUMBER THAT MATTERS.
    //
    // fLo keeps its min-channel reading for frameFloorMax above, deliberately: that cap is
    // load-bearing on every validated face grade, and re-anchoring it would move all of them. The
    // two guards want different statistics because they are asking different questions -- one is
    // "did a channel hit zero", the other "did the picture go black".
    //
    // Negative default means off, so every caller that does not ask for it behaves exactly as it
    // did. The two cannot both fire: a floor cannot be above the cap and below the minimum at once.
    const double loRead = (fLoY >= 0.0) ? fLoY : fLo;
    auto frameLoY = [&](double l, double g, double n) { return render(loRead, l, g, n); };
    if (frameFloorMin >= 0.0 && frameLoY(lf, gm, gn) < frameFloorMin) {
        branch |= 4;
        for (int pass = 0; pass < kMagicTonePasses; ++pass) {
            lf = solve1d(-0.50, 0.50, frameFloorMin, [&](double v) { return frameLoY(v, gm, gn); });
            gm = solve1d( 0.30,  3.00, subjMid,      [&](double v) { return render(sMid, lf, v, gn); });
            gn = solve1d( 0.05,  3.00, frameCeiling, [&](double v) { return render(fHi,  lf, gm, v); });
            surrPass(lf, gm, gn);
        }
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
        branch |= 2;
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
            surrPass(lf, gm, gn);
        }
    }

    // DECLINE RATHER THAN SHIP A FAILED SOLVE.
    //
    // On a frame 3.5 stops under, Gain ran to its 2.00 bound -- worth one stop -- and the subject
    // still came out with p10 and p50 both on 0.125 against a 0.278 midtone target: the face
    // squeezed into a single value, spread 0.133, and the frame's highlight blown to 1.000 at the
    // same time. Flat, crushed and clipped together, applied with no indication anything had gone
    // wrong.
    //
    // There is no arrangement of Lift, Gamma and Gain that makes that shot legible -- it wants
    // exposure the grade stage does not have -- so the honest answer is to leave Creative's tone
    // alone. Same shape as the non-skin gate and the 43%-coverage gate: the button's bad cases
    // have to be impossible rather than rare, and a solve that misses its target this far is a
    // bad case whatever produced it.
    //
    // Checked on the RESULT, not on whether a control sits at a bound. A bound can be reached
    // legitimately, and a solve can fail without reaching one.
    if (std::fabs(render(sMid, lf, gm, gn) - subjMid) > 0.02) { out.why = "subject unplaceable"; return out; }
    if (render(fHi, lf, gm, gn) >= kFrameBlown) { out.why = "highlight blown"; return out; }

    out.lift = (float)lf; out.gamma = (float)gm; out.gain = (float)gn;
    out.sLo = (float)sLo; out.sMid = (float)sMid; out.sHi = (float)sHi;
    out.fHi = (float)fHi; out.fLo = (float)fLo;
    out.frameLo = (float)frameLo(lf, gm, gn);
    out.subjLo  = (float)render(sLo,  lf, gm, gn);
    out.subjHi  = (float)render(sHi,  lf, gm, gn);
    out.frameHi = (float)render(fHi,  lf, gm, gn);
    out.mid     = (float)render(sMid, lf, gm, gn);
    out.con     = (float)cn;
    out.sSur    = (float)sSur;
    out.surr    = (float)render(sSur, lf, gm, gn);
    out.branch  = branch;
    out.ok = true;
    return out;
}


// BIAS: move the targets, re-solve, and HOLD AT THE LAST FEASIBLE BIAS.
//
// Lives here rather than in applyBias() because the bench has to be able to walk this exact
// curve -- a discontinuity in it is invisible from the outside until someone drags a slider in
// Resolve and the picture jumps, which is how it was found. Reimplementing it to test it is the
// mistake this header exists to prevent.
//
// Holding matters as much as the arithmetic. A declined solve used to fall through to a
// coefficient path, and the two do not meet: neighbouring slider positions gave Lift -0.134 and
// +0.162 on one frame, a 0.30 step that read as the image inverting. Bisecting to the limit
// rather than keeping the last value is deliberate -- keeping the last value makes the result
// depend on how fast the slider was dragged, so two users stop at two different grades. The
// limit is a property of the frame and should be found, not remembered.
//
// Returns !ok only when bias 0 itself does not solve, which means the node was never armed by
// Magic Tone; the caller's own fallback is right in that case and only that case.
static inline MagicTone solve_magic_tone_bias(double sLo, double sMid, double sHi, double fHi,
                                              const float P0[analysis::kParamN],
                                              const float* lut, int lutSize,
                                              const Tunables& t, double fLo, double bias,
                                              ToneTargets base = ToneTargets(),
                                              double sep = 0.0, double sepDir = 0.0,
                                              double sepPer = kToneSepMidPer,
                                              double sSur = -1.0)
{
    // BIAS LEANS AWAY FROM WHATEVER THE GRADE CURRENTLY MEETS, not from the constants the button
    // was solved to. Pass the conditions a hand edit achieved and the edit survives by
    // construction: at bias 0 the solve is asked for exactly what is already on screen, so it
    // returns it. Default-constructed (-1) means "use the fitted targets", which is what a fresh
    // Magic grade wants and what every existing caller got before.
    const double bFloor = (base.floor >= 0.0) ? base.floor : t.subjFloor;
    const double bMid   = (base.mid   >= 0.0) ? base.mid   : t.subjMid;
    const double bCeil  = (base.ceil  >= 0.0) ? base.ceil  : t.frameCeiling;
    // ...and the frame floor has to admit the grade it is being asked to reproduce, or the cap
    // reassigns Lift and the round trip fails. Never TIGHTER than the fitted value, so this can
    // only ever let an existing grade through -- it cannot license a new one to wash out.
    const double bFFMax = (base.floorMax >= 0.0) ? std::max(t.frameFloorMax, base.floorMax)
                                                 : t.frameFloorMax;
    const double bSurr  = base.surr;
    // ONE LINE FROM THE ANCHOR TO WHEREVER BOTH SLIDERS CURRENTLY SIT, walked by a single
    // parameter. With two sliders the feasible region is an area rather than an interval, and
    // bisecting each axis in turn would make the result depend on which was bisected first --
    // which is the same defect as keeping the last value when a drag runs out of road: the grade
    // would depend on the order the user touched the controls rather than on where they left them.
    //
    // A straight line from the origin depends only on the endpoint, so it is path-independent by
    // construction. With sep = 0 it reduces to exactly the interval the bias bisection used to
    // walk, point for point, which is why the existing behaviour is unchanged rather than
    // approximately preserved.
    // SEPARATION MOVES THE SURROUND, AWAY FROM THE SUBJECT. sepDir is the sign of the gap that
    // already exists (subject minus surround, from the region triple), so positive sep always
    // widens it whichever way round the frame is: a subject darker than its surround separates by
    // the surround going up, a brighter one by it coming down.
    //
    // The subject's own targets are untouched by this, which is the point. Moving them moves the
    // surround with them through the same curve -- measured at 1.9 L* across a whole slider, the
    // dead end this replaced. Moving the surround against a pinned subject is the only way the gap
    // opens.
    const bool hasSurr = (bSurr >= 0.0) && (sSur >= 0.0) && (sepDir != 0.0);
    auto at = [&](double u) {
        const double b = bias * u, s = sep * u;
        return solve_magic_tone_from(
            sLo, sMid, sHi, fHi, P0, lut, lutSize,
            std::min(0.40, std::max(0.00, bFloor + b * kBiasSubjFloorPer)),
            std::min(0.60, std::max(0.05, bMid + s * sepDir * sepPer)),
            std::min(kFrameCeilingMax, std::max(0.60, bCeil - b * kBiasCeilingPer)),
            fLo, bFFMax, kFrameFloorMinOff, kFloorReadUnset,
            sSur,
            hasSurr ? std::min(0.98, std::max(0.02, bSurr - s * sepDir * sepPer)) : -1.0);
    };
    MagicTone feasible = at(0.0);
    if (!feasible.ok) return at(1.0);   // never armed by Magic Tone -- let the caller fall back

    // THE CEILING GIVING WAY IS A STEP IN THE PICTURE, so it bounds the slider like a decline.
    //
    // Bit 2 is the fallback that DROPS the ceiling condition and lets Gain take the midtone off
    // Gamma. Crossing it changes which control does what, and that is discontinuous by nature:
    // on one frame it moved Lift 0.003 -> 0.081 and Gain 0.555 -> 0.282 between two neighbouring
    // slider positions. Rendered either side, the far side is not a different look -- it is a
    // washed-out picture with the blacks lifted off the floor, which is what the user saw and
    // called broken.
    //
    // ONLY BIT 2, and that distinction is the whole fix. Holding on ANY branch change was tried
    // first and was far too blunt: the frame-floor bit comes and goes SMOOTHLY -- one frame runs
    // Lift 0.122 -> 0.083 -> -0.019 straight through it -- so holding there capped that frame at
    // -0.06 and left the positive half inert on every tone-solved frame. Zero jumps because
    // nothing moved, which is a worse bug than the step it removed. The question is not "did the
    // branch change" but "did the assignment of conditions to controls change".
    //
    // Carrying the solved gamma into the fallback rather than restoring Creative's was also
    // tried; the step did not move, because the step IS the reassignment and not the value it
    // starts from. Smoothing it properly means blending the two branches, which wants its own
    // footage pass. Until then the slider runs out of road rather than driving off it.
    auto sameShape = [&](const MagicTone& s) {
        return s.ok && ((s.branch & 2) == (feasible.branch & 2));
    };

    MagicTone mt = at(1.0);
    if (sameShape(mt)) return mt;

    double lo = 0.0, hi = 1.0;           // lo keeps the assignment, hi does not
    for (int i = 0; i < 18; ++i) {
        const double mid = 0.5 * (lo + hi);
        const MagicTone s = at(mid);
        if (sameShape(s)) { lo = mid; feasible = s; } else hi = mid;
    }
    return feasible;
}

// ---------------------------------------------------------------------------------------
// WHICH SAMPLES A TONE TARGET IS MADE OF -- extracted so a reference still can be measured at the
// same five points the solve reads, rather than at five points that merely sound the same.
//
// solve_magic_tone() places its conditions at the subject's p10/p50/p90 ranked by Rec.709 LUMA and
// the frame's p99.9/p0.1 ranked by MAX CHANNEL. Both halves matter and neither is guessable: rank
// the frame by luma instead and a saturated practical stops being the ceiling; rank the subject by
// max channel and a red cheek outranks a lit forehead.
//
// A look fitted by measuring a reference at "the 50th percentile" without matching the ranking key
// would be a paraphrase, and on this feature every paraphrase produced plausible output while being
// wrong -- a neutral render for a graded one, a pre-LUT render for the real one, a threshold
// verified in Python and never ported. The rule lives here so there is one of it.
//
// It is the RULE that is shared and not a pixel buffer, because the two callers legitimately hold
// different things. The solve keeps SOURCE indices and re-renders them at every stage, since RAW
// Exposure acts before the measurement and moves the numbers it stands on; a reference still is
// already the finished picture and has nothing to re-render. So the callback hands back a rendered
// triple for sample i and the rule does not care where it came from.
struct TonePick {
    size_t iLo = 0, iMid = 0, iHi = 0;   // subject p10 / p50 / p90, ranked by luma
    size_t iTop = 0, iBot = 0;           // frame p99.9 / p0.1, ranked by max channel
    // The frame's darkest pixel BY LUMA, which is a different pixel from iBot and has to be.
    // Ranking by max channel finds the pixel with the dimmest brightest channel, and a saturated
    // pixel scores well on that while sitting at zero in its other two -- so the sample iBot
    // selects is not the darkest thing in the frame by the measure a viewer uses.
    size_t iBotY = 0;
    // The SURROUND's midtone: p50 by luma over everything the subject is not. The thing the
    // subject has to stand out from, and the only reading here that is about neither the subject
    // nor the frame as a whole.
    size_t iSur = 0;
    size_t subjN = 0;                    // how many samples carried the subject label
    bool   ok = false;
};

// The two readings a picked sample is turned into, kept next to the picker for the same reason:
// iTop is measured as its MAX channel and iBot as its MIN, which is not symmetric and not obvious.
// A ceiling is the brightest channel of the brightest pixel; a floor is the darkest channel of the
// darkest one, because that is the channel that hits zero first and takes the shadow detail with it.
static inline double tone_luma(float r, float g, float b) { return 0.2126*r + 0.7152*g + 0.0722*b; }
static inline double tone_hi(float r, float g, float b)   { return std::max(r, std::max(g, b)); }
static inline double tone_lo(float r, float g, float b)   { return std::min(r, std::min(g, b)); }

static inline TonePick pick_tone_samples(size_t n, const unsigned char* region, int subject,
                                         const std::function<void(size_t, float&, float&, float&)>& at)
{
    TonePick p;
    if (!region || n < 64) return p;

    std::vector<std::pair<float,size_t>> subjK, allK, allY, surK;
    subjK.reserve(n / 4 + 1); allK.reserve(n); allY.reserve(n); surK.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        float r, g, b;
        at(i, r, g, b);
        allK.push_back({ (float)tone_hi(r, g, b), i });
        allY.push_back({ (float)tone_luma(r, g, b), i });
        if ((int)region[i] == subject) subjK.push_back({ (float)tone_luma(r, g, b), i });
        else                           surK.push_back({ (float)tone_luma(r, g, b), i });
    }
    p.subjN = subjK.size();
    if (subjK.size() < 32) return p;

    auto pct = [](std::vector<std::pair<float,size_t>>& v, double q) {
        const size_t k = (size_t)(q * (v.size() - 1));
        std::nth_element(v.begin(), v.begin() + k, v.end(),
            [](const std::pair<float,size_t>& a, const std::pair<float,size_t>& b) { return a.first < b.first; });
        return v[k].second;
    };
    p.iLo  = pct(subjK, 0.10); p.iMid = pct(subjK, 0.50); p.iHi = pct(subjK, 0.90);
    p.iTop = pct(allK,  0.999); p.iBot = pct(allK,  0.001);
    p.iBotY = pct(allY, 0.001);
    // Falls back to the frame's own midtone when the subject fills the frame. A surround of
    // nothing has no midtone, and a separation target against an empty population would be a
    // number describing noise -- the same failure the 200-sample gate on the region triple exists
    // to prevent.
    p.iSur = surK.size() >= 200 ? pct(surK, 0.50) : pct(allY, 0.50);
    p.ok = true;
    return p;
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
    if (subject < 0 || subject >= analysis::kRegionN || !t.region[subject].has) {
        out.why = "no target for this subject";
        return out;
    }
    const double subjFloorT = t.region[subject].floor;
    const double subjMidT   = t.region[subject].mid;

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
        if ((double)cover > t.region[subject].maxCover * (double)n) {
            out.why = "region too large to be credible";
            return out;
        }
    }

    float Pn[analysis::kParamN]; analysis::neutral_params(Pn);
    Pn[11] = P0[11];   // keep the white balance; it is not a tone control

    // Neutral render once, in the encode the grade curve runs in. Everything below is a solve on
    // these numbers, so they must come from the render's own space (see docs/AUTO-GRADE.md 2).
    // REPRESENTATIVE SOURCE PIXELS, not percentile scalars.
    //
    // The percentiles used to be measured once at neutral and then treated as constants, which is
    // exact for Lift, Gamma and Gain because those act after the measurement. It is wrong for RAW
    // Exposure, which acts on scene light BEFORE the transform, so changing it changes the very
    // numbers the solve is standing on. Keeping the source triple that sits at each percentile
    // makes every stage a function of the parameters, exposure included.
    // The picking rule is shared with the reference-still measurement (pick_tone_samples), so a
    // look's targets and the solve that consumes them cannot disagree about what "the subject's
    // midtone" means. The callback re-renders from source, which is what lets RAW Exposure move
    // these numbers later instead of them being frozen scalars.
    const TonePick pk = pick_tone_samples(n, S.region.data(), subject,
        [&](size_t i, float& r, float& g, float& b) {
            og::process(cam, enc, Pn, S.rgb[i*3], S.rgb[i*3+1], S.rgb[i*3+2], r, g, b);
        });
    if (!pk.ok) { out.why = "subject too small"; return out; }
    const size_t iLo = pk.iLo, iMid = pk.iMid, iHi = pk.iHi, iTop = pk.iTop, iBot = pk.iBot;
    const size_t iBotY = pk.iBotY, iSur = pk.iSur;

    // UNDEREXPOSURE IS NOT LOW KEY, AND THE SUBJECT IS HOW YOU TELL THEM APART.
    //
    // Creative caps Gain at 0.80 so a deliberately dark shot is never pushed up -- the clamp that
    // made `key` descriptive rather than prescriptive. It is right about a frame median and blind
    // to the difference between a moody interior and a shot that simply missed exposure. A face
    // too dark to read settles it: that is not intent.
    //
    // RAW EXPOSURE, NOT GAIN, because 3.5 stops is an exposure problem. Gain is a multiply in
    // display space and buys about one stop before the highlights go; RAW Exposure is a linear
    // gain on scene light applied before the transform, which is what exposing the shot properly
    // would have done. On the failing frame Gain ran to 2.00 and still left the subject's p10 and
    // p50 both on 0.125 -- the face squeezed to a single value while the frame's top blew.
    //
    // Only ever upward, and only for a subject: this must not be able to pull a bright shot down,
    // and it must not fire on a landscape whose "subject" is a hillside.
    // A LIT FACE IS NOT BLACK. If the subject's midtone renders at the floor, the label is wrong
    // -- and on an underexposed shot that is exactly what the model has to work with, because it
    // is fed the graded thumbnail and a dark noisy picture is far outside what it was trained on.
    //
    // Worth separating from "unplaceable", which is where this landed before: exposure cannot fix
    // it. Scene-linear gain multiplies, and 0 times anything is 0 -- tested at 4, 6 and 8 stops,
    // all identical. A decline that names the cause stops the next person spending an hour on the
    // exposure path, which is what happened here.
    {
        float r0, g0, b0;
        og::process(cam, enc, Pn, S.rgb[iMid*3], S.rgb[iMid*3+1], S.rgb[iMid*3+2], r0, g0, b0);
        if (0.2126f*r0 + 0.7152f*g0 + 0.0722f*b0 < 0.01f) { out.why = "subject is black, not dark"; return out; }
    }

    float Pe[analysis::kParamN];
    for (int i = 0; i < analysis::kParamN; ++i) Pe[i] = P0[i];
    auto neutralMid = [&](double stops) {
        float Q[analysis::kParamN];
        for (int i = 0; i < analysis::kParamN; ++i) Q[i] = Pn[i];
        Q[10] = (float)stops;
        float r, g, b;
        og::process(cam, enc, Q, S.rgb[iMid*3], S.rgb[iMid*3+1], S.rgb[iMid*3+2], r, g, b);
        return (double)(0.2126f*r + 0.7152f*g + 0.0722f*b);
    };
    // Target the subject's NEUTRAL midtone, so the tone solve then starts from a shot that looks
    // correctly exposed rather than one it has to rescue with the grade curve.
    if (neutralMid(0.0) < t.subjNeutralMid) {
        Pe[10] = (float)std::min(t.rawExpMax,
                                 solve1d(0.0, t.rawExpMax, t.subjNeutralMid, neutralMid));
        Pn[10] = Pe[10];
    }

    // Same three readings the reference measurement takes, via the same helpers -- a subject point
    // is its luma, the frame's top is its max channel and the frame's bottom its min.
    auto shade = [&](size_t i) {
        float r, g, b;
        og::process(cam, enc, Pn, S.rgb[i*3], S.rgb[i*3+1], S.rgb[i*3+2], r, g, b);
        return tone_luma(r, g, b);
    };
    auto shadeMin = [&](size_t i) {
        float r, g, b;
        og::process(cam, enc, Pn, S.rgb[i*3], S.rgb[i*3+1], S.rgb[i*3+2], r, g, b);
        return tone_lo(r, g, b);
    };
    auto shadeMax = [&](size_t i) {
        float r, g, b;
        og::process(cam, enc, Pn, S.rgb[i*3], S.rgb[i*3+1], S.rgb[i*3+2], r, g, b);
        return tone_hi(r, g, b);
    };
    // The five readings are independent of the ceiling, so take them once and let the search vary
    // the one argument it is searching over. Recomputing them per probe would be harmless but
    // would make it look as though the measurement moved with the target, which is the confusion
    // that produced Magic Grade reading its own output.
    const double mLo = shade(iLo), mMid = shade(iMid), mHi = shade(iHi);
    const double mTop = shadeMax(iTop), mBot = shadeMin(iBot);
    // The frame floor read as LUMA, from the pixel that is darkest by luma -- a
    // different sample and a different reading from mBot, which is why it is measured
    // separately rather than derived from it.
    const double mBotY = shade(iBotY), mSur = shade(iSur);
    auto solve_at = [&](double c) {
        return solve_magic_tone_from(mLo, mMid, mHi, mTop, Pe, lut, lutSize,
                                     subjFloorT, subjMidT, c,
                                     mBot, t.frameFloorMax, t.frameFloorMin, mBotY,
                                     mSur, -1.0);
    };

    // TWO CANDIDATES, NOT A SEARCH -- and the difference is not economy, it is safety.
    //
    // Bisecting for the lowest feasible ceiling was written first and is wrong by construction: a
    // bisection converges TO the boundary, so the value it returns is always within its tolerance
    // of infeasible. On the underexposed clip it settled at 0.9339 while 0.9340 crosses into the
    // ceiling-gives-way branch and blows the face to 0.993 -- a grade balanced on a knife edge,
    // where the next frame of the same shot, or the first touch of Bias, falls off it. Fifth time
    // this project has met a discontinuity at a boundary, after Rolloff at 0, RAW Temp at 6500, the
    // halfway-armed anchor and the ceiling target that asked for what the acceptance test forbids.
    //
    // Both endpoints are validated values, which is the other half of the argument. 0.968 is the
    // hand-graded interview; 0.890 was checked on five clips. Anything between them is a number
    // nobody has looked at, so there is nothing to gain by landing on one.
    double ceil = t.frameCeilingLow;
    MagicTone r = solve_at(ceil);
    if (t.frameCeilingLow < t.frameCeiling && (!r.ok || (r.branch & 2))) {
        ceil = t.frameCeiling;
        r = solve_at(ceil);
    }
    r.ceil = (float)ceil;
    r.rawExp = Pe[10];
    r.sMidNeutral = (float)neutralMid(0.0);
    return r;
}

// ---------------------------------------------------------------------------------------
// THE WHOLE MAGIC SEQUENCE, so the plugin and the bench cannot run it in different orders.
//
// Every individual solve was already shared, and it was not enough. What stayed duplicated was
// the ORDER, written out once in applyMagicGrade and once in bench.cpp, and that is what drifted:
// the bench gained a re-solve after the colour move and the plugin did not, so the two produced
// different pictures from the same still -- the one failure an offline harness exists to prevent.
//
// Extracting the steps fixed the arithmetic and left the choreography to be kept in step by hand.
// This extracts the choreography. The callers no longer know the order; they supply pixels and a
// way to segment, and receive a filled parameter array.
//
// Writing it down immediately exposed a second live divergence nobody had noticed: the plugin
// balanced BEFORE Creative and the bench balanced after, so Creative solved its black point at
// 6500 K in one and at the corrected temperature in the other. Before is right -- white balance
// is a scene-referred correction and everything downstream should see the balanced picture -- and
// now there is only one answer to be right or wrong.
//
// Segmentation arrives as a callback rather than a dependency, so this header stays free of ncnn
// and the caller keeps ownership of the model.
using SegmentFn = std::function<bool(const unsigned char* rgb, int w, int h,
                                     std::vector<unsigned char>& regions512)>;

struct MagicResult {
    // Mirrors analysis::neutral_params(); a default member initialiser cannot call it.
    // static_assert there fires if the count moves, which is the reminder to update this.
    float P[analysis::kParamN] = {0.f,0.f,0.f, 0.f,1.f,1.f, 0.f,0.f, 0.f,1.f, 0.f,6500.f, 0.f,
                                  0.f,2.6f,1.f, 0.f,1.f,0.f, 1.f,1.f};
    // The grade as Creative left it, BEFORE a subject was chosen. Kept so switching subjects
    // starts from the same place every time -- re-running from the graded result would compound
    // one subject's colour move onto the next, and the answer would depend on the order they
    // were tried in.
    float Pcreative[analysis::kParamN] = {0.f,0.f,0.f, 0.f,1.f,1.f, 0.f,0.f, 0.f,1.f, 0.f,6500.f, 0.f,
                                  0.f,2.6f,1.f, 0.f,1.f,0.f, 1.f,1.f};
    analysis::MagicChoice choice;
    MagicTone     tone;
    WhiteBalance  wb;
    double        magicBase = 0.0;   // the colour move before Separation scales it
    bool          wbRan = false;
    bool          ok = false;
};

// thumbSrc: 512*512*3 camera log, top-down. S must already hold the frame's samples; its region
// labels are filled in here. `click` cycles the subject, `sep` scales the colour move.
// STEPS 4-7, GIVEN A SEGMENTATION THAT ALREADY EXISTS: decide, tone, colour, re-solve.
//
// Split out of solve_magic() so the choice of subject can be revisited for free. Segmentation is
// the whole cost of the button (~100 ms of inference); everything here is arithmetic over a
// SampleSet whose regions are already assigned, so offering every option the frame supports costs
// nothing next to offering one.
//
// `creativeP` is the grade as Creative left it -- BEFORE any subject was chosen. Re-running from
// the graded result instead would compound one subject's colour move onto the next, so switching
// options would depend on which order they were tried in.
static inline MagicResult solve_magic_from_regions(analysis::SampleSet& S,
                                                   const float creativeP[analysis::kParamN],
                                                   const Measurements& m, int cam, int enc,
                                                   const float* lut, int lutSize,
                                                   const Tunables& t, int click, double sep)
{
    (void)m;
    MagicResult out;
    for (int k = 0; k < analysis::kParamN; ++k) out.P[k] = creativeP[k];
    for (int k = 0; k < analysis::kParamN; ++k) out.Pcreative[k] = creativeP[k];

    const int dispEnc = (enc <= 2) ? enc : 1;
    analysis::RegionStat st[analysis::kRegionN];
    analysis::region_stats(S, cam, dispEnc, out.P, st);
    out.choice = analysis::magic_decide(st, click);
    if (!out.choice.ok) return out;

    // TONE, then COLOUR, then TONE AGAIN -- the last two because Offset Temp is additive and Gain
    // Temp multiplicative, so either shifts the channels the tone was placed on. A grade has to be
    // solved for the configuration it ends in, not one it passed through.
    auto applyTone = [&]() {
        const MagicTone tn = solve_magic_tone(S, out.choice.subject, cam, enc, lut, lutSize,
                                              out.P, t);
        if (tn.ok) {
            out.P[3] = tn.lift; out.P[4] = tn.gamma; out.P[5] = tn.gain;
            if (tn.rawExp > 0.f) out.P[10] = tn.rawExp;
        }
        out.tone = tn;
    };
    applyTone();

    out.magicBase = solve_magic_base(S, cam, dispEnc, out.choice, st, t);
    out.P[out.choice.param] = (float)std::min(1.0, std::max(-1.0,
        (double)out.P[out.choice.param] + out.magicBase * sep));

    solve_black_px(S, cam, enc, out.P, t.blackTarget, nullptr, 0);
    applyTone();

    out.ok = true;
    return out;
}

// HOW MANY DISTINCT SUBJECTS THIS FRAME OFFERS, and what they are. magic_decide() already builds
// the deduped candidate list and reports its size, so asking it for each index in turn enumerates
// them with no new logic to keep in step.
static inline int magic_option_count(const analysis::SampleSet& S, int cam, int enc,
                                     const float* P)
{
    analysis::RegionStat st[analysis::kRegionN];
    analysis::region_stats(S, cam, (enc <= 2) ? enc : 1, P, st);
    const analysis::MagicChoice c = analysis::magic_decide(st, 0);
    return c.ok ? c.options : 0;
}

static inline analysis::MagicChoice magic_option_at(const analysis::SampleSet& S, int cam, int enc,
                                                    const float* P, int k)
{
    analysis::RegionStat st[analysis::kRegionN];
    analysis::region_stats(S, cam, (enc <= 2) ? enc : 1, P, st);
    return analysis::magic_decide(st, k);
}

static inline MagicResult solve_magic(analysis::SampleSet& S,
                                      const std::vector<float>& thumbSrc,
                                      const Measurements& m, int cam, int enc,
                                      const float* lut, int lutSize,
                                      const Tunables& t, int click, double sep,
                                      bool wbFirst, const SegmentFn& segment)
{
    MagicResult out;
    creative_preset(out.P);

    // Render thumbSrc through the current parameters, for the model. Display-referred and 8-bit,
    // because the checkpoint was trained on photographs -- the same reason the analysis measures
    // display values rather than log.
    auto thumb = [&](const float* P, bool withLut, std::vector<unsigned char>& dst) {
        const size_t n = thumbSrc.size() / 3;
        dst.assign(n * 3, 0);
        for (size_t i = 0; i < n; ++i) {
            float r, g, b;
            og::process(cam, enc, P, thumbSrc[i*3], thumbSrc[i*3+1], thumbSrc[i*3+2], r, g, b);
            if (withLut && lut && lutSize >= 2) og::apply_lut(lut, lutSize, 1.f, r, g, b);
            og::apply_trim(P[8], P[9], r, g, b);
            dst[i*3+0] = (unsigned char)(og::clamp01(r) * 255.f + 0.5f);
            dst[i*3+1] = (unsigned char)(og::clamp01(g) * 255.f + 0.5f);
            dst[i*3+2] = (unsigned char)(og::clamp01(b) * 255.f + 0.5f);
        }
    };

    // 1. WHITE BALANCE, on a NEUTRAL render: the cast is what we are here to measure, so it has
    //    to still be in the picture. Before everything else, so the rest sees a balanced frame.
    if (wbFirst && thumbSrc.size() == (size_t)512 * 512 * 3) {
        float Pn[analysis::kParamN]; analysis::neutral_params(Pn);
        std::vector<unsigned char> t0, regions;
        thumb(Pn, /*withLut=*/false, t0);
        if (segment(t0.data(), 512, 512, regions)) {
            out.wb = solve_white_balance(thumbSrc, regions, cam, enc, lut, lutSize,
                                         out.P[8], out.P[9]);
            out.wbRan = true;
            if (out.wb.ok) out.P[11] = (float)out.wb.kelvin;
        }
    }

    // 2. CREATIVE: exposure from key, rolloff from clipping, black point on real pixels.
    solve_creative_px(S, cam, enc, m, t, out.P, nullptr, 0);

    // 3. SEGMENT THE PICTURE AS CREATIVE LEFT IT, not a neutral render -- the model reads the
    //    graded frame because that is the one that looks like a photograph.
    std::vector<unsigned char> th, mask;
    thumb(out.P, /*withLut=*/true, th);
    if (!segment(th.data(), 512, 512, mask)) return out;
    if (!analysis::assign_regions(S, mask, 512, 512)) return out;

    // 4-7 live in solve_magic_from_regions() so they can be re-run WITHOUT the model. Steps 1-3
    // are the ~100 ms; choosing a different subject out of the same segmentation is arithmetic,
    // which is what lets the panel offer the alternatives instead of making the user press the
    // button again and hope.
    for (int k = 0; k < analysis::kParamN; ++k) out.Pcreative[k] = out.P[k];
    const MagicResult tail = solve_magic_from_regions(S, out.P, m, cam, enc, lut, lutSize,
                                                     t, click, sep);
    for (int k = 0; k < analysis::kParamN; ++k) out.P[k] = tail.P[k];
    out.choice = tail.choice; out.tone = tail.tone; out.magicBase = tail.magicBase;
    out.ok = tail.ok;
    return out;
}

// ---------------------------------------------------------------------------------------
// RANGE BALANCE — where the bright population begins.
//
// The first version took p98 of the graded luminance. That assumes the highlight is a FIXED
// SHARE OF THE FRAME, and it is not: on a bedroom with one window p98 read 72.1 against the
// user's hand-dialled 63.5, and on a landscape whose top half is cloud it selected 1.97% of
// the frame — the same rule, two shot shapes, one of them absurd. A percentile answers "how
// much", and the question is "where is the gap".
//
// So: split the frame into two populations and put the edge between them (Otsu — maximise
// between-class variance, which is the same thing as minimising the spread within each side).
// It reads the shape of the histogram rather than a position in it, so a window that is 2% of
// frame and a sky that is 50% both land at their own boundary.
//
// The luma handed in must be the SAME luminance the mask reads at render — post grade curve,
// pre Range Balance. A threshold is only meaningful in the space it was chosen in, and this
// project has paid for that lesson on the black point already.
// OTSU ALWAYS ANSWERS, AND THAT IS ITS ONE TRAP. It finds the best of all possible splits, which
// on a frame with no bright population is still some split -- one corpus frame spans 27.9 to 35.3
// in display units and got a latch holding 87% of itself.
//
// SO THE TEST IS THE GAP, IN ABSOLUTE UNITS, NOT OTSU'S OWN SEPARABILITY. That was tried first
// and it is scale-invariant by construction: the flat frame above scored 0.65 and another one
// spanning 53 to 71 scored 0.80, both comfortably inside the range the bedroom window (0.77) and
// the sky (0.82) occupy. A ratio cannot see that one frame's two "populations" are seven code
// values apart. `gap` is the distance between the class means on the 0..100 display axis, which
// is the thing that decides whether holding one side off the other means anything.
//
// Third time this shape has come up here: hot versus pin, crushed% versus crushedY, and now this.
// A NUMBER COMPARED AGAINST A CONSTANT HAS TO BE THE NUMBER THAT MATTERS.
struct RangeLatch {
    double latch = 0.0;    // 0..100, where the mask crosses 0.5
    double cover = 0.0;    // % of the frame at or above it
    double gap   = 0.0;    // display units between the two class means
    bool   ok    = false;
};

// Below this there is nothing to hold apart. On the corpus the frames with a real window or sky
// sit far above it and the flat ones fall well under -- see docs/ROADMAP.md for the table. One
// corpus, so it is a bar rather than a constant of nature.
static const double kRangeGapMin = 20.0;

static inline RangeLatch range_latch(const std::vector<float>& y)
{
    RangeLatch out;
    if (y.size() < 64) return out;

    // 256 bins over [0,100]. Superwhite folds into the top bin: it is unambiguously highlight,
    // and letting it stretch the axis would push every threshold down with it.
    const int kB = 256;
    long long hist[kB] = {0};
    for (float v : y) {
        int b = (int)(v * (float)(kB - 1) + 0.5f);
        hist[b < 0 ? 0 : (b > kB - 1 ? kB - 1 : b)]++;
    }

    const double total = (double)y.size();
    double sum = 0.0;
    for (int i = 0; i < kB; ++i) sum += (double)i * (double)hist[i];

    double wB = 0.0, sumB = 0.0, best = -1.0, bestGap = 0.0;
    int bestT = 0;
    for (int t = 0; t < kB - 1; ++t) {
        wB += (double)hist[t];
        if (wB <= 0.0) continue;
        const double wF = total - wB;
        if (wF <= 0.0) break;
        sumB += (double)t * (double)hist[t];
        const double mB = sumB / wB, mF = (sum - sumB) / wF;
        const double between = wB * wF * (mB - mF) * (mB - mF);
        if (between > best) { best = between; bestT = t; bestGap = mF - mB; }
    }
    if (best < 0.0) return out;

    out.latch = 100.0 * ((double)bestT + 0.5) / (double)(kB - 1);
    long long above = 0;
    for (float v : y) if (100.0 * (double)v >= out.latch) ++above;
    out.cover = 100.0 * (double)above / total;

    out.gap = bestGap * 100.0 / (double)(kB - 1);   // bins -> the 0..100 display axis
    out.ok  = (out.gap >= kRangeGapMin);
    return out;
}

} // namespace grade
} // namespace og
