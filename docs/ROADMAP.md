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

**Blocked on nothing technical.** The open question is what it should move when the regions are
wrong — top/bottom thirds are a stand-in that only holds for landscape-shaped frames, so on a
shot where the subjects aren't stacked vertically the slider would push apart two things that
aren't the subjects. Either ship it with that caveat and let the panel show the regions it
found, or wait for real region masks.

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
