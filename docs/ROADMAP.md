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

## 2. Gamut compression, to make Export LUT near-exact

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

## 3. Declaring OFX 1.5 colour management

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

## 4. Standing items

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
