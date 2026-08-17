# Roadmap — deferred work, with the reasoning kept

Things that are designed but not built. Each entry records **what was learned**, so the
work can restart from the conclusion rather than from the beginning.

The other docs in this folder explain what OneGrade *does*; this one explains what it
deliberately doesn't do yet, and why.

---

## 1. Match Clip — matching one shot to another

**Status:** designed, not built. A working probe existed briefly and was removed — recover
it with `git revert` / `git show` on commit **`0bbca39`** (`feat(match): probe whether
Resolve hands over frames from adjacent clips`), which also flipped
`setTemporalClipAccess` on.

**Credit:** Marc Wielage suggested matching a shot to the one before or after it.

### What was tried: reading adjacent frames

The first approach fetched the source clip at `time ± N` frames, hoping the host would hand
back the neighbouring clip. It was built behind a `kMatchProbeUI` switch, with a verdict
line that distinguished a genuine read from the host **clamping** to the current clip's
bounds and returning a frame we already had — a naive probe reports that as success.

**It was abandoned before validation, on shape rather than on result.** Even in the best
case the user has to supply a frame offset, which means doing arithmetic about where the
neighbouring clip starts. That's a bad control, and no answer from the probe would have
made it a good one.

### Why "just grab the adjacent clip" isn't available

Worth writing down so nobody re-litigates it:

- **OFX has no concept of a timeline.** It is a per-effect image-processing API. The host
  hands an effect its input clips and that is the entire model — there is no "next clip",
  no timeline query, no neighbour. Nothing in the API surfaces one.
- **Fetching at another time is the only mechanism** that can reach outside the current
  clip at all, and it requires `setTemporalClipAccess(true)` on both the effect and the
  clip. Whether Resolve honours it across a clip boundary was never established.
- **A second input clip** (`defineClip("Reference")`) looks promising and probably isn't.
  Resolve does show extra inputs on OFX nodes, but in the Color page every node input is
  fed from the *current clip's* graph; there is no route to bring a different timeline clip
  in. **Unverified** — treat as "probably not" rather than "definitely not".

### The design to build instead: Grab Reference

Sidesteps the problem completely.

1. Park the playhead on the shot to match **to**. Press **Grab Reference** — measure that
   frame and store the measurement.
2. Move to the clip to be matched. Press **Match to Reference** — measure that frame and
   solve for the params that carry it to the stored numbers.

Why this is strictly better than adjacency:

- **No API risk at all.** Works regardless of what the temporal question would have
  answered.
- **Any clip, not just neighbours** — a hero shot, another scene, another timeline, a
  session from last week.
- **Same interaction model as Auto Grade**, which already works: the user chooses the
  moment by parking the playhead. The known "clip whose exposure changes mid-take"
  limitation applies identically and is already documented as a feature, not a defect.
- **Two clicks and no frame arithmetic.**

### What it needs

- OFX params are **per-node-instance**, so the reference has to live in a **process-wide
  static** to travel between clips. Optionally persist to a small file in the plugin's
  config dir so it survives a Resolve restart. (Session-only is roughly ten lines; disk is
  the better product.)
- Reuse `probeAnalyze()` wholesale — the measurement already exists, is already in
  scene-linear XYZ, and is already grade-independent (confirmed: the same frame measures
  identically ungraded and with a Look LUT at Mix 1.0, see `docs/AUTO-GRADE.md`). That
  last property is what makes matching sound: the reference describes the *footage*, not
  whatever grade happens to be on it.

### Open questions

- **What should it match?** Exposure only is the evidenced version — `key` is validated
  across four shots at roughly four stops. **Start there.**
- **Balance is the interesting one.** The Auto Grade work concluded warmth is *not
  derivable* from image content: two shots measured skin `R/G` of 1.21 and 1.22 and the
  user warmed one to 9242 K and left the other at 6500. But matching to a **reference** is
  a different problem from deriving from nothing — there is a target rather than a guess,
  so the negative result does not automatically carry over. Worth a data pass.
- Resolve has a native **Shot Match**, so the bar isn't "do it at all". The case for doing
  it here is that the measurement is already in the right space and the output is visible
  slider values the user can then adjust — the same "shows its work" property that makes
  Auto Grade trustworthy.

---

## 2. A pleasing picture on drop — static defaults only

**Status:** the analysed version was built, **crashed Resolve**, and was removed the same
day (2026-08-03). Do not rebuild it in that shape.

**What was tried.** Run Base Grade once from the instance constructor, guarded by a saved
`autoInitDone` param so a project reload could not re-stamp a user's grade, and wrapped in
try/catch on the assumption that the worst case was "no image available this early".

**What actually happened.** `fetchImage()` from `createInstance` trips an assertion inside
Resolve, which calls `abort()`. From the crash report:

```
__assert_rtn -> abort
...
OFX::Clip::fetchImage(double)
OneGrade::probeAnalyze(double)
OneGrade::applyAutoGradeClean(double)
OneGrade::autoInitOnce()
OneGrade::OneGrade(OfxImageEffectStruct*)
OneGradeFactory::createInstance(...)
```

Three things worth keeping from that:

- **`try/catch` gave zero protection.** `abort()` is a process abort, not a C++ exception.
  "Wrap it and see" is not a safe experiment against a host assertion — the wrap was the
  reason it *felt* safe to try.
- **The blast radius was every existing project, not just new nodes.** `autoInitDone` did
  not exist in projects saved before that build, so it defaulted to `false` and the
  auto-grade fired on every OneGrade node at once. A "runs once" guard stored in a *new*
  param is not a guard for *old* files.
- **fetchImage is safe from `changedParam` and from `render`, and nowhere else.** Auto Grade
  step 1 validated the first; this validated the negative. Anything reading pixels must hang
  off a user action or a render, never off a lifecycle hook the host calls during load.

**The route that remains.** Better **static defaults** — pick shipped values that land well
on typical log footage. Footage-blind, so it helps the average clip and misses outliers, but
it cannot crash anything and needs no measurement. The two buttons (Base Grade / Creative
Grade) already cover the adaptive case with one click.

---

## 3. Gamut compression, to make Export LUT near-exact

**Status:** identified while measuring `Export .cube` (2026-08-03), deliberately not done.

The exported cube is exact **on lattice points** (worst 1.5e-08) but degrades between them
wherever the pipeline isn't smooth — and the output encode **hard-clips out-of-gamut
channels to zero**, which puts a step through the colour cube that no lattice can follow.
Measured, Gen 5 → Rec.709 2.2 at 33³:

| region | error |
|---|---|
| grey axis, log 0.10–0.70 | ~4/255 |
| median over the whole cube | 0/255 |
| mildly tinted bright colour (±15% of grey) | ~152/255 |

65³ roughly halves it and cannot remove it: **the limit is the discontinuity, not the
sampling.** Hence 65 is the export default.

A **soft gamut compression before the clip** would make the function continuous and the
bake near-exact. It is not done because it is a golden-rule pipeline change (4-file mirror
across `OneGradePipeline.h` + the three kernels) **and it changes the look** — every
validated preset and both film recipes would need re-checking on footage. That is a
deliberate release of its own, not something to smuggle in behind an export button.

---

## 4. Declaring OFX 1.5 colour management

**Status:** read-only probe shipped in v1.3.0; declaring support deliberately not done.

`Check Input` reports `kOfxImageEffectPropColourManagementStyle` and
`kOfxImageClipPropColourspace` **without** declaring a colour management style. If Resolve
populates them anyway, that is free information.

If they report `(absent)`, the next experiment is to declare
`kOfxImageEffectColourManagementBasic` and retest — as a **separate, deliberate step**.
The risk is specific: declaring support is what could let the host begin converting our
input, which would override the plugin's own camera transform. That is the one thing that
breaks the whole design, so it does not get switched on speculatively.

---

## 5. Standing items

Carried from `CLAUDE.md`, kept here so there is one place to look:

- **Rolloff smoothness on Gen 5** — the highlight softclip is not yet as smooth as
  Blackmagic's "Gen 5 Film to Video" LUT. Candidates: tune the knee, or a scene-linear
  shoulder before encode instead of (or blended with) the display-space clip.
- **CUDA colour A/B** — the CUDA path is fast on the user's 5090 but only *performance* was
  ever checked; its output has never been compared against the validated Metal/CPU result.
- **OpenCL inside Resolve on an AMD card** — the kernel agrees with the CPU on real
  hardware via a direct harness, but has never run through Resolve on an AMD GPU.
- **Per-camera gamut validation** — matrices other than Blackmagic's are published or
  approximate values, flagged for on-footage checking.
- **HDR tone-map** — HLG/PQ input is currently a normalize, not a tone-map, so highlights
  can clip. A real shoulder is future work.

---

## Separation slider (user's idea, 2026-08-06)

A taste knob that adds or reduces **separation** — the thing the user identified as what makes
a frame read as dynamic: *"not just increasing contrast in the normal sense, but seeing what's
in the frame and making choices that push those objects to be more separated from others of a
different hue or tone level."*

**Most of the machinery already exists.** `docs/AUTO-GRADE.md` §9:

- `describe()` measures the separation triple (`dL*`, `da*`, `db*`) — signed, so solvable
- `jacobian()` measures how each control moves them **on this shot**
- `solve_intent_iter()` lands a target rather than undershooting it

So the slider is: *raise the triple by N units* → solve → write ordinary slider values. Same
shape as Bias (a taste knob offsetting from an anchor), but **shot-adaptive** instead of fixed
coefficients — Bias multiplies constants, this one asks the footage what it takes.

**Its real value is as a fitting tool, which is the user's own point.** Drag it on real footage,
find where it looks right, and that number becomes the constant — the same loop that produced
every fit in this project. There is no way to guess the right amount of separation from first
principles, and no ground truth for it yet beyond a single hand grade.

**Blocked on one thing, and it is not what this entry used to claim.** It read "blocked on nothing
technical", on the reasoning that it only needed real region masks. Those shipped in 1.4.x — and
`describe()` was never rewired to use them. Checked 2026-08-14: `d.v[D_DL]` is still
`bandL[2]/bandN[2] - bandL[0]/bandN[0]`, the top third minus the bottom third via `S.band`, while
`S.region` sits unused a few lines away.

So the stand-in this entry warned about is still what the descriptor measures, and the failure it
predicted is the COMMON case rather than an edge one. A centred subject puts hair and background
in the top band and clothing in the bottom, so band `dL*` reports the gap between two parts of
**one** subject and calls it separation. Portraits are most of what has a face in them, and the
frame that prompted this work — a centred close-up with deep contrast and no clipping — is exactly
that shape.

**The prerequisite is a region-based separation triple**, subject against everything else, added as
NEW descriptors rather than by redefining `D_DL`, so the existing steerable set and the tests
pinning it stay untouched.

---

## Region masks for separation (the classifier) — the live blocker, 2026-08-06

**The tonal half is done and validated on footage** — the user's words: *"this work has made the
tonal look really close to what my hand-grades do."* Exposure comes from measurement, global
colour from the Jacobian, the black point from a solve. What remains is **colour separation**,
and it is blocked on one thing only.

### The job is narrow

Not exposure. Not colour. **Region identity** — which pixels belong to which thing, so the
separation descriptors have real objects to attach to. `docs/AUTO-GRADE.md` §9 has the design;
the descriptors only ever ask *region A minus region B*, so real masks drop in without changing
anything else.

### Both cheap region-finders are ruled out, by measurement

| | bands (top/bottom third) | 2-means in (a*, b*) |
|---|---|---|
| beach (horizon) | **db\* +43** — found sky over water | two populations *both orange*, h29 / h44 |
| city (downward) | **db\* −0.6** — found nothing | — |
| car (centred subject) | **db\* −1** — found nothing | **h−158 / h+95** — genuinely distinct |

**Each fails exactly where the other works.** That is the case for segmentation: not that it
would be nice, but that nothing cheaper covers the range of shots.

### Derisk it offline before writing any C++

The whole question — *do real masks produce separation numbers that track the user's hand grades
better than bands do?* — can be answered in Python, on exported frames, with no plugin changes,
no inference runtime, and no licensing commitment:

1. export the frames we already have measurements for (beach, city, car) plus a few more
2. run a candidate segmentation model offline
3. compute the separation triple from the real masks
4. compare against the band and cluster numbers already recorded

If the masks separate the shots better, build the C++ inference. If not, a day is spent instead
of two weeks.

### Constraints already established

- **It is a button, not a render** — ~1s budget, CPU, no GPU inference, no kernel work, no
  golden-rule mirror.
- **Feed it display-referred pixels**, never camera log — a net trained on sRGB sees garbage
  otherwise. Same trap as the `dispEnc` fallback in `probeAnalyze`.
- **Signed components only.** Separation must stay `dL*` / `da*` / `db*`; distances cannot be
  solved against (see §9).
- **Licensing is a real gate** on any pretrained weights — code licence ≠ weights licence ≠
  training-data licence. Does not block the offline experiment; does block shipping.
- **Runtime, when it comes to that:** hand-rolled inference with weights as a bundle resource
  (zero deps, ~800 lines) or vendored ncnn (~3MB, BSD-3). ONNX Runtime is too heavy for a 2.4MB
  plugin.

### Open question worth answering first

**Does every shot even want separation?** The city grade was purely tonal; the beach needed
colour separation. The classifier's first useful output may be *"are there separable regions
here at all"* rather than *"push these two apart"*.

---

## "White balance first" for Magic Grade (user's idea, 2026-08-06)

An optional checkbox: balance the frame before running the Magic Grade chain.

### The observation

Magic Grade on a slightly cool interior produced a dramatic result — usable, but a long way from
where the user would start. Resetting, warming Scene White Balance to 8301 by hand, and running
Magic Grade again produced "a much nicer starting place" from the same button.

### Why it works, which is the part worth keeping

**A global cast contaminates the comparison that picks the direction.** `magic_decide()` chooses
by asking whether the subject leans warm or cool *relative to the rest of the scene*. If the
whole frame is cool because the white balance is wrong, the algorithm reads a camera error as
scene content and pushes further along it. Correct the balance first and every remaining
difference is the room rather than the sensor.

That also explains why the effect is largest on interiors and mixed lighting, and smallest on a
sunset — where the cast IS the content.

### Grey-world is the obvious approach and it is wrong here

Averaging the frame to neutral would "correct" a beach sunset to grey, which is the opposite of
what anyone wants. The classic fixes (white-patch, neutral-pixel detection) all fail the same
way: they cannot tell a colour that is a mistake from a colour that is the point.

**The classifier already answers that.** Balance on the regions with a defensible neutral
expectation — BUILT, GROUND, TERRAIN, the man-made and underfoot surfaces — and ignore the ones
that are legitimately coloured: SKY, WATER, VEGETATION. SKIN is a strong secondary reference,
since skin chromaticity is far more consistent across people than intuition suggests.

On a frame with no trustworthy neutral reference, decline and say so, exactly like the rest of
the feature. A sunset over water has no neutral surface in it, and guessing one is how you get a
grey sunset.

### Shape

- Checkbox, default state to be decided on footage. Optional either way — some shots want the
  cast kept.
- Runs before the Creative Grade step, so everything downstream sees the balanced frame,
  including the segmentation itself.
- Writes an ordinary Scene White Balance value the user can drag, like everything else here.

### Explicitly out of scope

The user's own next moves from that starting point — reducing the contrast ratio on the face,
pushing further warm — are **not** something the plugin should chase. Their words: "this is
nothing our plugin should be concerned with... it's very easy to adjust via the Separation
slider, which is what we want."

---

## Deferred: axis confidence for Magic Grade (2026-08-06)

`magic_decide()` picks a direction from the subject's lean against the rest of the scene, with no
regard for how large that lean is. On a downward city view it chose from `L34 v 34` and
`b2 v 2` — differences that round to zero — and applied a full-strength move. The same logic on a
similar frame could go the other way and look wrong.

`experiments/segmentation/intent.py` already solves this, scaling the push by `gap / AXIS_FULL`
so a weak axis gets a weak move; it was never ported to C++. Two options when it is:

1. scale magnitude by confidence, so a weak axis produces a small move
2. as above, plus decline below a floor (~2 Lab units) with the Why row saying the two regions
   look the same

**Deferred deliberately.** The user liked the result on the frame where the reasoning was
weakest, and one shot is not enough to know how often the axis is that thin. Worth living with
first and seeing whether arbitrary directions actually bite.

---

## Multiple classifier passes: objects, then quality (user's idea, 2026-08-07)

> "we may want to run the image through the classifier multiple times, the first to grade
> objects, then further passes to determine quality and aesthetics."

The first pass exists — semantic regions, driving which slider moves and in which direction.
What is missing is any notion of whether the *result* is good, and that gap is exactly what the
"too dark on high-key scenes" report is: the grade lands where its constants say it should, and
nothing in the loop ever looks at the output and objects.

Two distinct things could sit in later passes, and they are worth keeping apart:

**Re-measure the graded frame.** Cheap and already possible — `describe()` is a pure function of
the parameters, so the graded state can be measured without re-rendering anything. A rule as
plain as "the midtone must not fall below X" would have caught the darkness before it was
noticed by eye. This is the one to do first: it needs no model, no data and no training.

**An aesthetic score.** The real version of the idea, and the harder one. A learned "is this a
good image" predictor could close the loop properly, letting the grade be searched rather than
computed. Two obstacles, both real: the small ones are trained on web-photo taste rather than
cinema, and running one inside a search loop costs a forward pass per candidate — CLIP-based
scorers are seconds each, which is a non-starter for a button. A tiny model over the descriptors
already computed is the tractable shape, and it needs the ground truth the bench is being built
to collect.

Sequence: measure the graded frame and add sanity rules (now) → collect graded/ungraded pairs
with the bench (ongoing) → consider a learned score once there is something to fit it to.

---

## The colour reasoning is done pre-LUT, on a picture the user never sees

Noticed 2026-08-07 while chasing an over-cool Magic result. `OneGradeAnalysis.h` contains no
reference to `apply_lut` at all: every region L\*/a\*/b\*, the Magic warm/cool decision, the
Jacobian and the separation triple are computed by `og::process()` alone. But Creative and Magic
Grade always select the Kodak 2383 print stock, and a print stock is emphatically not
hue-preserving — that is what it is for.

The split is already half-right and that is what makes it easy to miss: the model is fed the
**graded, post-LUT** thumbnail, because it was trained on photographs. Only the *colour
statistics* stayed pre-LUT. So the segmentation knows what it is looking at and the colorimetry
describes a different image.

Consistent with this, on one interview frame the pre-LUT descriptors ordered two presses
opposite to how they looked: the press reading `a*-4.1 b*-6.2` (cool) produced the neutral
picture, and the one reading `a*-1.3 b*+2.4` (near-neutral) produced the visibly teal one.
Suggestive, not proof — the frame was never captured as a log still, so it was never put through
the bench.

**This is the same defect class as the black-point encode bug** (`docs/AUTO-GRADE.md` §2): a
number measured in one space used to reason about another. Not acted on because the fix is not
obviously "add the LUT" — some of these consumers legitimately want the pre-LUT picture, since
that is the space the controls act in, and the Jacobian in particular is a derivative of
controls that operate before the LUT. Wanting the decision to be about the final picture and
wanting the derivative to be about the controls are different requirements and may need
different measurements.

**To restart:** capture the interview frame as 16-bit log, run it through the bench, and check
whether pre-LUT b\* predicts post-LUT b\* at all. If the ordering holds, this is a non-issue and
the note can be deleted. If it inverts, the decision-making descriptors need the LUT and the
Jacobian probably does not.

---

## Extract the whole Magic sequence, not just its steps

`src/OneGradeCreative.h` exists so the plugin and the bench cannot paraphrase each other, and
it works: `solve_creative`, `solve_creative_px`, `solve_magic_tone`, `solve_magic_base` and
`solve_white_balance` are each called by both. What is still written out **twice** is the
ORDER the steps run in — and on 2026-08-08 that is what drifted. The bench gained a re-solve
after the colour move; the plugin did not; the two produced different pictures from the same
still, which is the single failure the bench exists to prevent.

Extracting individual functions turned out to fix the arithmetic and leave the *choreography*
duplicated. The sequence is: creative → tone → decide → colour → creative → tone, and every
one of those edges is a place the two can diverge silently.

**The shape:** one call taking `(SampleSet, cam, enc, lut, lutSize, Tunables, int click)` and
returning a filled `P[kParamN]` plus the `MagicChoice` and `MagicTone` for the panel. The
plugin then writes `P[]` into OFX params and the bench renders it — neither knows the order.
Bias already works this way and is the proof it is tractable: it calls
`solve_magic_tone_from` over three cached scalars and cannot drift.

**Why it was not done at the time:** it lands in the middle of an active tuning session, and
the constants were still moving. Do it before the constants are refitted, not after, so the
refit happens against one implementation.

---

## Magic Tone beyond faces

`solve_magic_tone` declines every subject that is not skin, because all three of its targets came
from one hand-graded interview and a face is the one subject whose correct lightness is not a
matter of taste. Sky belongs near the top, foliage low, sand bright, and a beach frame whose
subject came back VEGETATION was destroyed by being driven to a face's midtone.

**It needs data, not code.** One hand-graded landscape gives the same three numbers for that
class the interview gave for faces; the machinery already takes them per-call. The measurement
recipe is in `docs/AUTO-GRADE.md` §10 and the probe that produced it is a few lines over the
bench's `SampleSet`.

Related and unhandled: an underexposed shot with **no** trustworthy face gets no exposure help at
all, since the RAW Exposure rescue sits behind the face gate. That is deliberate — it must not
fire on a landscape — but it means the underexposed-landscape case is simply not covered.

---

## Segment an exposure-normalised thumbnail

`dark-scene00086490` declines as `subject is black, not dark`: its SKIN mask lands on hair or
shadow, because the model is fed the **Creative-graded** thumbnail and an underexposed frame
graded by Creative is dark and noisy — far outside what an ADE20K checkpoint was trained on. The
segmentation degrades exactly when the shot most needs it.

The fix is the same ordering argument as the RAW Exposure rescue, applied one stage earlier:
normalise exposure *before* segmenting, so the model sees a picture it can read. Contained, and
probably the highest-value item left on this feature.

---

## Region masks in the RENDER, not just the analysis (user's idea, 2026-08-14)

> "professionals create contrast by smart use of lighting... they do so even by not crushing or
> clipping, but by using contrast ratios wisely. Of course this is the one key thing we can't do
> for footage — without masks we have global control over the image."

That is the correct diagnosis of the ceiling on everything in `docs/AUTO-GRADE.md` §10. Lift,
Gamma and Gain are global, so placing a subject moves the whole frame, and every guard in the tone
solve exists to limit that damage: `frameFloorMax` stops a dark subject dragging the frame's black
up, the ceiling-gives-way branch stops a bright window crushing a face, and the missing lower
guard is the same problem from the other side. **They are all workarounds for having one control
where the picture wants two.**

### The masks already exist

`og::seg::Segmenter` runs on every Magic Grade press and produces per-region labels, and
`assign_regions()` already maps them onto samples. Nothing new is needed to KNOW where the sky is.
What is missing is any way to act on it: `og::process()` takes one parameter vector for the whole
frame.

### Why it is a big change rather than a hook

- **Golden rule, in full.** Per-region parameters mean the mask has to reach the GPU and be sampled
  per pixel, in all four implementations (`OneGradePipeline.h`, Metal, OpenCL, CUDA). Param-count
  and buffer changes in every one.
- **A label mask is not a matte.** Segmentation output is nearest-neighbour class IDs at 256x256
  against a 4K frame. Applied directly it would produce visible blocky edges on every region
  boundary. It needs feathering, and probably guided filtering against the image, before it can
  drive anything continuous.
- **Temporal stability.** Analysis is a button pressed once, so a mask that differs slightly frame
  to frame never mattered. The moment a mask drives the render it must be stable across a whole
  clip or it will crawl — and per-frame inference in `render` is exactly what the ~1s button budget
  was designed to avoid.

### The cheap version worth trying first

Resolve already has qualifiers, windows and its own magic mask. The plugin does not need to become
a compositor to capture most of the value: **report what it found**. A read-only line naming the
regions and their coverage, or an exported matte, would let the user put a Resolve node on the sky
with the plugin's own segmentation informing where. That is a panel change, not a kernel change.

### The ceiling: measured, tested on footage, and SHIPPED as a range (2026-08-14)

**Outcome first: `frameCeilingLow = 0.890` with `frameCeiling = 0.968` as the fallback.** The
history below is why it is a pair of validated values and not a search between them.

Across 851 films the median frame ceiling is **0.890** and only **16% clip**, while OneGrade
targets `frameCeiling` **0.968** — above where 71% of professional films sit. That looked like the
cheapest change available: one constant, no masks, no model.

**Swept on footage at 0.968 / 0.930 / 0.890 / 0.850 and rejected.** Four of the five face clips
looked *identical* across the whole range despite Gain dropping about 20%, and the fifth got
worse: `00095358` clips on the face at every value below 0.968. Lowering the target makes the
ceiling harder to satisfy alongside the subject, so the solve takes the **ceiling-gives-way**
branch, abandons the ceiling condition entirely, and lands at **0.993** — nearly blown, and worse
than the 0.968 it started from. Lowering the ceiling to reduce clipping produced more of it.

**The corpus median is not transferable.** Films sit at 0.890 because they are *lit* for it. The
statistic describes the reference footage, not what this footage wants, and nothing in the
measurement could have revealed that — it took the pictures.

Worth keeping as a methodological note. Three constants now come from one hand-graded interview,
and all three have been independently checked: `subjMid` and the lift target held under the
preference test (`training-data/labeling2`), and `frameCeiling` holds here. **One shot the user
graded by hand has out-predicted 851 films every time it has been tested.**

### What shipped instead: a range, resolved per frame

The user's read of the sweep is what unlocked it — *"the difference between all values was mostly
invisible \[and] the one shot that needed 0.968 is simply not properly lit, so I don't want to
overfit for bad footage."* That is not an argument for either constant. It is an argument for
**asking for 0.890 and accepting 0.968 when the frame insists**, which is a different feature from
a different number: the fallback is per-shot, so the choice has to be too.

`solve_magic_tone` now solves at `frameCeilingLow` and re-solves at `frameCeiling` if the first
attempt declines or takes the ceiling-gives-way branch. On the five face clips: four hold 0.890,
and `00095358` walks up to 0.968 and reproduces its validated grade exactly
(L −0.069 · G 1.809 · g 0.511, subject high 0.566). Bias is unaffected — `tone_targets_of` reads
the ceiling a grade *achieves* off its live parameters, so it picks up whichever was used without
being told, which is the defect-4 fix paying for itself. Zero jumps on both clips at
`--bias-inc=0.002`.

**A bisection was written first and was wrong by construction — worth keeping, because it looked
strictly better.** Searching for the *lowest feasible* ceiling converges TO the feasibility
boundary, so its answer is always within tolerance of infeasible. On `00095358` it settled at
**0.9339**, and **0.9340** crosses into the ceiling-gives-way branch and blows the face from 0.566
to **0.993**. The grade it produced was correct and sat one ten-thousandth from a cliff, where the
next frame of the same shot or the first touch of Bias falls off it. **Fifth discontinuity at a
boundary in this project**, after Rolloff at 0, RAW Temp at 6500, the halfway-armed anchor, and the
ceiling target that asked for what the acceptance test forbids.

The two-point form has no edge to sit on and no interpolation to justify: both endpoints are
values someone has looked at, and everything between them is not. Pinned by test 33, which asserts
the *shape* rather than the numbers — re-fitting an endpoint is expected, re-introducing a search
is not.

### Tone Separation: the target axis is chosen, and the first choice was wrong (2026-08-14)

The slider re-solves rather than writing parameters, and that part is settled by measurement. The
`d(rdL*)/dp` row of the descriptor Jacobian on the user's footage is dominated by `lft`, `gam`,
`gan` and `pCn` — every control the tone solve already owns — with signs that flip between frames
depending on which side of the contrast pivot the subject sits. Writing any of them directly would
undo the three conditions that place the subject, which is the failure Bias was rebuilt to avoid.
So the slider moves a target and re-solves, exactly as Bias does.

**Which target is the open question, and `subjMid` is not it.** Walked across its full range on a
face clip with the achieved `rdL*` printed beside the parameters: `rdL*` moved −18.05 → −19.96,
about **1.9 L\* units or 10%**, while Lift went 0.001 → 0.173 and Gamma 1.914 → 0.890. A large,
obvious change in the picture buying almost nothing in the quantity the control is named after.

Structural, and predictable in hindsight: subject and surround are placed by **one curve**, so
moving the subject's target drags the surround with it and the gap barely opens. Nothing that
moves both together can separate them.

**The design that follows: a fourth condition on a fourth control.** Three conditions currently
place the subject and hold the frame's ceiling; separation needs one on the *surround*. Post
contrast is the candidate — it pivots at 0.5, which sits between a subject at 0.278 and a brighter
surround, and it carries the largest consistent `d(rdL*)/dp` in the Jacobian (roughly 20–38 L\* per
unit against the 1.9 the whole `subjMid` slider managed). Four conditions on four controls stays
well posed and is the existing coordinate-pass structure with one more pass.

**The risk is real and is the reason this is written down rather than built.** It changes the shape
of `solve_magic_tone`, which every validated face grade stands on: the pass ordering, the
convergence argument, the two branch fallbacks and the two acceptance tests would all need
re-checking against the five clips. That is a deliberate decision, not a drive-by.

Scaffolding that stays either way: `D_RDL/D_RDA/D_RDB`, the `sep`/`sepDir` arguments, and the
bias/sep line walk (both sliders travel one line from the armed anchor, so the result depends on
where they are left rather than on the order they were touched). Bench: `--sep-report --sep-jac
--tone-sep-sweep --tone-sep-per`.

**A bench-side trap worth keeping.** The first sweep passed a default `ToneTargets`, so sep 0
returned a different grade than the one on screen — the fitted ceiling is 0.968 and the frame had
been solved at 0.890, so the solve was asked to reproduce a grade it had never made. `applyBias()`
derives its base correctly; only the new bench path did not. Any caller of
`solve_magic_tone_bias()` that does not pass `tone_targets_of()` is asking the wrong question.

### SKY enabled, and the guard that was blocking it (2026-08-14)

`frameFloorMin` — the guard that stops a BRIGHT subject crushing the frame's floor, the mirror of
`frameFloorMax` — existed but was **inert**, and the value was never the problem. It read `fLo`,
the MIN channel of the pixel ranked p0.1 by MAX channel, and those are two different pixels: a
saturated pixel has a bright max and a min at zero, so it ranks nowhere near the bottom on the
ranking while sitting at the bottom of the value being read. The sample the guard protected was not
the darkest thing in the frame by any measure a viewer uses.

Re-anchored on **luma**, with its own sample (`TonePick::iBotY`, the frame's p0.1 by luma) rather
than reusing `fLo`. `fLo` deliberately keeps its min-channel reading for `frameFloorMax`, which is
load-bearing on every validated face grade — the two guards ask different questions, "did a channel
hit zero" and "did the picture go black", and want different statistics for it.

Third instance of that defect after the black-point encode bug and hot-versus-pin, and found the
same way: by the bench reporting a correct picture as 43.8% crushed until `crushed%` was itself
re-anchored on luma.

**Verified live and left at 0.020.** Swept 0.020 / 0.080 / 0.150 on the four sky clips: branch 4
appears at 0.080 on three of them and at 0.150 on all four (where Lift runs to its 0.500 bound on
two, which is over-constraining). So the guard works and simply does not fire at its default on
this footage — and `crushY` is 0.00% on all four, so by the luma measure there is nothing for it to
fire about. **The earlier "3 of 4 clips improve" reading came from `crushMin`**, the statistic now
known to be measuring saturation, and it is not a reason to raise the guard.

Face grades are unchanged to every printed digit, which is the regression check that matters: the
guard was inert on them before and is inert on them now, for a different and correct reason.

### The fourth condition: built, measured, and it is post contrast that is wrong (2026-08-14)

Following the `subjMid` dead end above, the surround's midtone was added as a fourth condition on a
fourth control, post contrast, chosen because it carried the largest `d(rdL*)/dp` in the Jacobian
and pivots at 0.5. Built so it is **absent at rest** — `surrMid < 0` means no fourth pass, so a
fresh Magic Grade stays three conditions and every validated grade is bit-identical. Confirmed: all
nine clips print identical L/G/g/branch/ceiling with the machinery in place.

**It does not work, and the reason is exact.** The surround's midtone renders at **0.552** and
**0.540** on the two face clips — essentially *on* the contrast pivot of 0.500. Contrast is
`(v − 0.5)·con + 0.5`, so its leverage on the surround is `0.052` per unit: moving the surround by
0.05 demands a full unit of contrast. Measured, contrast ran to its 2.0 bound by sep +0.25 and
`rdL*` went the **wrong way**, −18.80 → −16.61, with Lift dragged 0.055 → 0.323 and Gamma
1.381 → 0.926 compensating.

**The Jacobian was read wrong, not measured wrong.** `pCn` really does carry the largest
`d(rdL*)/dp` — because contrast moves the SUBJECT, which sits at 0.278 and far from the pivot. The
subject is pinned by two of its own conditions, so the solve spends the whole move undoing it.
Attributing a separation derivative to the surround when it belongs to the subject is the same
class of error as `hot` versus `pin`: the number was real and described something else.

**The structural finding, which outlives the attempt.** Subject floor, subject midtone and frame
ceiling pin the tone curve at three points. The surround's position is then a CONSEQUENCE of those
three, not a free variable, and no fourth control can move it without a control whose leverage is
independent of all three — which post contrast is not, since it overlaps Gamma almost exactly.

**Where a third attempt should start: relax one of the three, do not add a fourth.** The code's own
reasoning already says legibility lives in the midtone and the floor is what gives ground first —
that is exactly what the `frameFloorMax` branch does. So Tone Separation should SWAP which
condition Lift serves, trading the subject's floor against the surround's position, reusing the
branch-swap pattern that already exists twice rather than inventing a fourth condition. Untested.

Kept in the tree because it is inert and any future law needs it: `TonePick::iSur` (surround p50 by
luma, falling back to the frame median when the subject fills the frame), `MagicTone::sSur/surr/con`,
`ToneTargets::surr`, and the fourth coordinate pass behind its sentinel.

### The re-solve is fitted for a subject near the BOTTOM of the range (2026-08-14)

Reported on footage: Tone Separation and the colour Separation slider both "snap to over-saturated
or blown out" on SKY, while both work well on SKIN. Reproduced offline, and it is not a Tone
Separation bug — **Bias alone shows it**:

```
armed:   L-0.044  G1.367  g0.656
bias 0:  L-0.500  G1.367  g0.885     <- Lift pinned at its bound
bias -0.5 .. -2.0:  flat, held there
```

Bias 0 is required to be the identity and is not: it steps 0.456 of Lift the instant the slider is
touched. Everything that re-solves inherits it.

**Cause.** Lift is `lift*(1 - min(v,1))`, so its authority falls off toward white. The three
conditions were fitted to a face whose floor sits at **0.125**, where Lift has full leverage; a
sky's floor sits at **0.486**, where it has barely half. The solve runs Lift to its −0.500 bound,
the subject then misses its midtone, the ceiling-gives-way branch fires, and Gain is allowed past
Creative's clamp — which is the blown, over-saturated picture.

**Grading once is unaffected**, and that is the whole reason the sky grades looked good: that path
solves from neutral, where Lift still has room. Only the re-solve, starting from an already-bright
grade, has nowhere to go.

**Shipped: the anchors are not armed for a subject the re-solve is not fitted for.** That is the
existing mechanism for "the tone solve declined", so Bias drops to its OFFSET law — which predates
all of this and works on any subject — and Tone Separation goes inert. Sky keeps a working Bias
rather than losing one, and no grade changes. Bad cases impossible rather than rare, the same bar
as the non-face gate on the tone solve itself.

**What a real fix needs: a control assignment that follows the subject's position.** Lift/Gamma/Gain
for a subject near black is a fitted choice, not a law — for a subject near white the natural
assignment is different (Gain has the leverage there that Lift has at the bottom). That is the same
"swap which condition each control serves" pattern as the two existing branches, applied to the
subject's height in the frame rather than to a guard being crossed. Untested, and it wants the sky
grades hand-graded first the way the interview was: the targets exist but nothing says what a sky
grade should do as it is LEANED.

Third design in a row on this feature to fail on structure rather than on tuning, and all three
found by the bench rather than by reasoning.

## Range Balance: the latch, and the two things that were measured wrong (2026-08-16)

Both shipped. Kept here because each was a plausible-looking number that described the wrong
thing, which is the failure mode this project keeps meeting.

**The latch was a percentile, and a percentile assumes the highlight is a fixed share of frame.**
p98 on a bedroom interior put the edge above the window entirely; on a landscape whose top half is
cloud it selected 1.97%. `og::grade::range_latch()` splits the histogram in two (Otsu) instead —
reading its shape rather than a position in it. Same rule, both frames: **58.2 → the window at
7.5%**, **46.9 → the sky at 52.5%**.

**Otsu always answers, and that is its one trap.** It returns the best of all possible splits even
when there is only one population: a corpus frame spanning 27.9 to 35.3 got a latch holding 87% of
itself. The decline test needed is **the absolute gap between the class means**, not Otsu's own
separability — which was tried first and is scale-invariant by construction:

| frame | Otsu η | class gap | what it is |
|---|---|---|---|
| bedroom + window | 0.77 | **56.9** | the window, 7.5% |
| beach sky | 0.82 | **42.8** | the sky, 52.5% |
| flat interior (27.9–35.3) | 0.65 | **10.1** | nothing — held 87% |
| flat exterior (53–71) | 0.80 | **29.0** | a real sky, 82% |

η cannot see that one frame's two "populations" are seven code values apart. The gap can. Over the
17-frame corpus the declines top out at **16.9** and the accepts start at **21.5**, so
`kRangeGapMin = 20.0` sits inside a real gap in the data rather than on a guess. Third instance of
this shape after `hot` vs `pin` and `crushed%` vs `crushedY`: **a number compared against a
constant has to be the number that matters.**

**The mask had an upper edge, which put the brightest pixels back outside it.** Measured: 8.08% of
the bedroom held with the ceiling open against 5.92% at 100, and the 27% it dropped was the blown
middle of the window — the exact thing "hold the highlights" is about. Resolve gets away with the
same shape because its qualifier axis stops at 100; ours is float and a practical at 121 is
highlight by any definition. `highlight_mask()` now rises once and stays up, so there is no High
control and none is wanted.

**The latch re-measures itself when the grade under it changes.** The mask reads the picture after
the grade curve, so Magic Grade, Auto Grade, the Subject dropdown and presets all leave a latch
describing a picture that no longer exists — the manual answer was "press Set From Frame again",
which is a step nobody should have to know about. Two deliberate limits: it only fires when a
latch is **already set** (a stage you are not using must not have values stamped into it), and
only on the **buttons that replace the whole grade**, never on slider edits or Bias. Re-measuring
under the hand would make the latch move while you worked, which is what made this a button rather
than an automatic reading in the first place; Bias additionally re-solves live during a drag,
where a frame measurement per tick is not real time. The refresh reuses the samples the grade just
took (no image fetch), leaves the viewer alone rather than switching the matte on, and says
"Re-latched" so a number that moved on its own names what moved it.

**Still open:** spatial feathering. The softness is in luminance, not in space, so where the
threshold lands on a steep edge (a window frame) the mask is stable, and where it lands inside
noise the mask shimmers frame to frame. A per-pixel kernel cannot have a neighbourhood; this is
the blur architecture, and it is the next thing this feature needs.

## A mode selector for the panel (user's idea, 2026-08-17)

Twelve groups and growing. `setOpen()` per group (2026-08-17) decides what a colorist sees on
drop, and that is a stopgap: one static answer for every kind of work. The user's plan is a **mode
selector that switches the control surface by work type** — a "just make it look right" mode
showing the two buttons and a look, a shot-matching mode, a delivery mode, a full manual mode.

Groups are already the unit this would switch, and `setEnabledness()` already runs every param's
visibility from one place on load and on change, so the mechanism is mostly present: a mode choice
param plus `setIsSecret()` per group instead of a fixed `setOpen()`. **What needs deciding is not
the plumbing but the modes** — which controls belong to which kind of work — and that is a
question about how the plugin is used, not about the code. Worth doing after the panel stops
changing shape every release.

## Range Balance: locking the mask against the grade under it (2026-08-17)

Shipped as **Lock Mask**. The mask reads the picture AFTER the grade curve, which is what lets it
tell a window from a bright pillow — and is also what makes it slide when exposure moves. Measured
on the bedroom frame with the latch pinned at 58.2, as the solved Gain rises:

| solved gain | unlocked | locked |
|---|---|---|
| low  | 7.45% held | 7.96% |
| mid  | 8.56% | 7.89% |
| high | **19.39%** | 7.89% |

The held region nearly triples under a control that was supposed to be adjusting it, so "pull the
window down" quietly becomes "change what the window is".

**The fix is a reference grade, not a branch.** `P[21..23]` carry the Lift/Gamma/Gain the mask is
evaluated against; `resolveConfig()` writes the live grade there when unlocked and the captured
grade when locked. The four render paths see three floats and have no idea a lock exists — a
branch out there would be a second definition of the mask with nothing to say which one a frame
used. Cost is two extra `lgg_core` calls per pixel, because `d` is already in hand.

**It locks against the grade curve only.** RAW Exposure and Density sit upstream of `d`, so
anchoring them would mean running the whole chain a second time per pixel. They still move the
mask, the hint says so, and they are not what a mask gets nudged by while you dial it.

**A locked mask is never auto-refreshed** — `refreshRangeLatch()` returns early. Lock means the
selection holds while the grade moves, and Magic Grade is the largest instance of the grade
moving; re-measuring would also not preserve it, since Otsu re-run on the new grade can land on a
different population.

**What this does NOT solve, and the user named it in the same breath:** a window contains content
as dark as things in the room, and a luminance qualifier cannot tell those apart at any latch,
locked or not. Lowering the latch to include the dark parts of the window also grabs the room.
Locking makes that workflow *possible* — you can now drop the latch and push hard without the
selection running away — but separating "dark, and outside the window" from "dark, and in the
room" is spatial, and lands with the blur architecture.
