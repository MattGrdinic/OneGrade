# Changelog

All notable changes to OneGrade. Versions follow [SemVer](https://semver.org).

---

## v1.3.0 — the feedback release

Almost everything here came from other people telling us what was wrong with the plugin,
which is the best possible reason to cut a release. See **Acknowledgements** below.

### Licensing

- **OneGrade is now GPL-3.0-or-later** (previously BSD-3-Clause). Use it for any work,
  paid or not — the licence puts no condition on your grades or your deliverables. Fork it
  and contribute freely. But anyone who *redistributes* it, modified or not, has to ship
  the complete source under the GPL as well, so it can't be reskinned and sold as a closed
  product. A `LICENSE` file finally exists; the repo had none.
- Versions up to **v1.2.0** stay BSD-3-Clause. That grant can't be withdrawn — it applies
  to those versions forever. The GPL starts here.

### Renamed — labels only, no saved grade is affected

- **"RAW Exposure" → "Scene Exposure"** and **"RAW Temperature" → "Scene White Balance"**.
  The old names promised a relationship to Resolve's Camera RAW tab that cannot exist: no
  sensor metadata reaches an OpenFX plugin. Scene Exposure genuinely *is* the same
  operation the RAW tab performs (a linear gain on scene light); Scene White Balance is a
  Bradford chromatic adaptation in XYZ — a physically real white balance, but not a raw
  decoder's. The hints now say both things plainly.
- **Camera entry 11: "Rec.2100 PQ / ST.2084 (HDR)" → "Rec.2100 PQ - Smooth Decode".**
  That entry is a deliberately compressive curve that flatters log footage — a look, not a
  camera — and it was sitting in the one slot of the list every other entry reserves for a
  faithful decode. The objection was never to the arithmetic (a decode curve and a look
  curve are the same class of operation) but to the label, because a label is a claim other
  people read.
- Both were **renamed, not reordered**. Choice params save by index and double params save
  by name, so every existing grade loads exactly as before.
- The Camera hint now states outright that *every other entry in the list is a faithful
  camera decode*. The input transform is one of the most useful things the plugin does and
  the old wording undersold it.

### Changed — Trim reads as finishing, not as a second grade

- **Trim > "Exposure" is now "Exposure Trim"**, and its slider spans **±1 stop** instead of
  ±3. It was being read as a second, competing exposure control — two places to set
  brightness, one of them after the LUT — which is a workflow trap rather than a feature.
  Exposure belongs to **Gain** in group 4, where it acts in the grade curve.
- The **hard** range stays ±3 on purpose. `setRange` is a clamp the host applies to saved
  values, so narrowing it would quietly rewrite existing grades — and the film emulation
  presets legitimately sit at +0.55, bringing level back after a print stock crushes it.
  Narrow what the slider shows, never what a project can hold.
- A tip line now says what the group is for: *"Finishing touches. Most grades need nothing
  here."*

### Added — per-stage Bypass

- **A "Bypass" checkbox on Balance, Density, Exposure, Look/Film LUT and Trim.** Auditioning
  a stage used to mean zeroing its sliders and putting the numbers back from memory. Now
  it's one click, the values are untouched, and clicking back restores the grade exactly.
- Bypass is enforced **at render** by holding the stage's params neutral — the same
  mechanism Node Role uses. A bypassed stage is therefore *precisely* a neutral stage: no
  second code path, and nothing new for the three GPU kernels to mirror.
- **Bypassing the LUT hands Output Encode back to you.** A selected LUT normally pins the
  encode to the curve it was authored for, so a bypass that left it pinned would still be
  changing the picture — which would not be a bypass. The "In effect" line says so while
  it's on.
- No bypass on the Input Transform: the camera decode is structural, not an effect.
  "Bypassing" it would emit raw log, which is never what the checkbox would mean.

### Added — Export LUT

- **A new "Export LUT" group bakes the whole node into a `.cube`.** Camera transform,
  balance, density, grade, output encode, any selected LUT and the trim, in one file. This
  is the answer to the strongest professional objection to a plugin like this: a project
  graded with OneGrade otherwise needs OneGrade archived beside it, and because this node is
  the entire pipeline, substituting it later would mean starting over rather than replacing
  one effect. Bake it and the dependency is gone.
- Sizes 17 / 33 / **65 (default)**. Node Role and every Bypass are honoured, so what you
  export is what you see — both read through the same `resolveConfig()` the renderer uses,
  rather than a second implementation that would drift.
- **Accuracy, measured rather than assumed.** The bake is **exact on lattice points**
  (worst 1.5e-08). Off-lattice it is as good as the pipeline is smooth, and ours is not
  smooth everywhere: the output encode **hard-clips out-of-gamut channels to zero**, which
  puts a step through the colour cube that no lattice can follow. On Gen 5 → Rec.709 2.2 at
  33³ the grey axis is within ~4/255, the median over the whole cube is 0, but mildly
  tinted bright colour can reach ~150/255. 65³ roughly halves that and cannot remove it,
  because the limit is the discontinuity and not the sampling.
- So: **an excellent archival stand-in, not a bit-exact one.** It matches the node through
  the normal tonal range and can differ on blown, saturated highlights. The hint and the
  docs say exactly that. A soft gamut compression before the clip would make the bake
  near-exact and is noted as future work — it changes the look, so it isn't being smuggled
  in behind an export button.

### Added — Base Grade, and a sane picture on drop

- **Two buttons now, named for what they do: "Base Grade" and "Creative Grade".** Base
  measures the frame and places its range so nothing is crushed at 0 or clipped at 1023 —
  a neutral, gradable starting point with **no LUT and no film tint**, letting the smooth
  decode do the work. Creative is the previous Auto Grade: same measurement, film emulation
  look on top.
- **Base solves rather than fits, and that's the point.** Creative Grade had to be fitted to
  four hand-graded shots because "what gain did they choose" is a taste question. "Is
  anything clipped" is not — it is objective, and true on anyone's footage. So Base places
  p99 with Gain, p1 with Lift and p50 with Gamma, each control owning the end it pivots away
  from, via three 1-D solves on the measured percentiles.
- **The black point is p0.1, not p1.** Placing the 1st percentile on the target leaves a
  full 1% of the frame *below* it, and on any shot with real shadow area that 1% is a
  visibly crushed region sitting on 0 — which is what an interview frame showed on the first
  build. Using p0.1 puts the actual bottom of the picture on the target instead of the bottom
  of the bulk. On measured shots this flips Lift from negative to positive: it stops pushing
  shadows down, while a genuinely milky flat-log frame still gets its floor corrected.
- **The solve now predicts through Highlight Rolloff.** Rolloff is applied after the grade,
  so it squashed whatever the solve had placed: an interview frame aimed at 0.90 landed near
  0.83 because a 6% pin drove Rolloff to ~0.55. The target means something now.
- Target High raised to **0.95**, so the picture fills the range rather than stopping short.
- **It applies, measures, edits, and repeats** — up to 20 passes, typically settling in two
  or three. The closed-form solve gets a good starting point, then each pass runs the *real*
  pipeline over the cached source samples and reads the finished picture back: density,
  rolloff, encode, all of it. No model to be wrong about.
  - This exists because a one-shot solve is only ever as right as its model of everything
    downstream, and the history of this feature is discovering another stage the model
    didn't know about — first the encode, then rolloff, then luma-vs-channel, then Density
    (which runs *before* the grade and moves the very percentiles being placed). Measuring
    the finished picture ends that whole class of bug.
  - The **best** result is kept rather than the last, so a late overshooting step can never
    make the outcome worse than an earlier one. It's allowed to fail to improve, never to
    regress. It also stops early when two passes bring no real improvement, since the
    remaining error is then structural — a clamped control, or targets that can't all be met
    at once — rather than something more passes would fix.
  - Costs ~39 ms per pass at 220k samples; the readout reports the pass count.
- **It never brightens.** Gain is capped at 1.0 by default. A shot whose highlights sit
  below the target isn't clipping — it's dark, which is usually deliberate. Without the cap
  the solver dragged a moody interior's p99 from 0.55 up to 0.90 and blew it out; the user's
  own grade on that shot pulled Gain *down* to 0.714. Same clamp, same reason, as Creative.
- **The midtone is only half-applied** (`Mid Strength`, default 0.5). Driving every shot's
  median to a fixed target is exactly the mistake Creative had to unlearn — it flattens
  deliberately dark shots into mid-gray. Containment at the ends is safe to enforce because
  clipping is a defect; the midtone is intent.
- **"Grade on drop" was built, crashed Resolve, and was removed** before release. Calling
  `fetchImage` from the instance constructor trips an assertion inside Resolve that calls
  `abort()` — and since that is a process abort rather than an exception, the `try/catch` it
  was wrapped in gave no protection whatsoever. It also fired on every node in every
  pre-existing project, because the "runs once" flag didn't exist in files saved earlier.
  Written up in [docs/ROADMAP.md](docs/ROADMAP.md); the remaining route is better static
  defaults. Press Base Grade instead — it's one click.
- The readout reports what the solve **achieved**, not just what it set: a target can be
  unreachable, and silently pinning a slider while reporting success is a bug shape this
  project keeps finding.
- Internal: `lgg_core()` extracted into `OneGradePipeline.h` so the solver and the pipeline
  share one definition of the grade curve. Test 14 proves the premise the solver rests on —
  that evaluating `lgg_core` on a measured display percentile predicts what the pipeline
  actually renders — across 3 encodes, 3 cameras and 3 grades.

### Added — Check Input (setup sanity check)

- **A "Check Input" button in Setup / Help** reads the frame and says whether this node is
  being fed camera log, which is what it expects.
- **What it can't do, and why:** it cannot read your Timeline Color Space. That is a
  *monitoring* setting applied downstream of the node graph — it changes how Resolve
  interprets our output for the viewer, and nothing about it is visible from inside an OFX
  plugin. No property carries it.
- **What it does instead, which is the useful half:** every setup mistake that actually
  ruins a grade — a color-managed timeline, a CST node in front of this one, an input LUT on
  the clip — changes the **input**, and the input is measurable. Camera log has a narrow,
  lifted footprint (Blackmagic log peaks around 0.75 on real footage); display-referred
  material uses the full range, crushing to 0 and clipping at 1.
- Deliberately **conservative**: it calls a verdict only when the frame is clearly one thing
  or the other, reports "inconclusive" otherwise, and always prints the percentiles it
  judged on. A false alarm on a correct setup would be worse than staying quiet — the same
  reasoning that stopped Auto Grade guessing at white balance.
- It also reports whatever Resolve volunteers through the **OFX 1.5 colour management API**
  (`ofxColour.h`). These are read **without declaring a colour management style**, on
  purpose: declaring support is exactly what could invite the host to start converting our
  input and override the plugin's own camera transform. If it reports "(absent)", the next
  experiment is a deliberate one, not a speculative switch.

### Deferred — Match Clip

- **Not shipped, and the UI has been removed.** Marc Wielage suggested matching a shot to
  the one before or after it. A working probe was built and taken back out: the only
  mechanism OFX offers is fetching the source clip at another *time*, which even at best
  makes the user do arithmetic about where the neighbouring clip starts. That's a bad
  control, and no probe result would have made it a good one.
- OFX has **no concept of a timeline** — the host hands an effect its input clips and that
  is the whole model. There is no "adjacent clip" to ask for.
- The design that replaces it, **Grab Reference** (measure any shot, then match another one
  to it), needs no temporal access at all, works across any two clips rather than
  neighbours, and reuses the existing measurement. It is written up in full, with the open
  questions, in **[docs/ROADMAP.md](docs/ROADMAP.md)**.
- `setTemporalClipAccess` is back **off**: advertising a capability we don't use is the
  same class of mistake as the OpenCL black frame.

### Internal

- Plugin version bumped to **1.3** (`kPluginVersionMajor`/`Minor`). It had been left at
  1.1 through the v1.2.0 release.

---

## Acknowledgements

**justin_daniels** — for the detailed critique on the Blackmagic forum that prompted this
entire release: the RAW naming, the honest label on the smooth decode, per-operation
bypass, and the LUT export idea are all theirs. Several of the points were ones we'd have
defended rather than fixed if they hadn't been made so precisely. Disagreeing well is a
contribution, and this one improved the plugin more than any feature request has.

**Marc Wielage** — for the match-clip idea (matching a shot to the one before or after it).
It isn't in this release: the obvious implementation turned out to be the wrong shape, and
working out why produced a better design than the one we started with. It's specced in
[docs/ROADMAP.md](docs/ROADMAP.md) and it ships because they suggested it.

---

## Earlier releases

| Version | Highlights |
|---|---|
| **v1.2.0** | Auto Grade ("magic button") — measures the frame and sets a cinematic starting point; live Bias slider; Node Role group split (Pre-Clip / Post-Clip). |
| **v1.1.1** | LUT encode-override made visible in the panel; "DaVinci Wide Gamut / Intermediate" naming unified across Camera and Output Encode. |
| **v1.1.0** | Renamed PowerGrade → OneGrade (the old name collides with Resolve's Gallery feature). Breaking: saved grades do not carry over. |
| **v1.0.x** | Six built-in look LUTs shipped inside the bundle; per-platform Resolve LUT directory; CUDA build (sm_120) and the OpenCL black-frame fix; Rec.709 Gamma 2.2 default. |
| **v0.x** | Initial OpenFX plugin: camera CST, balance, density, exposure, output encode, LUT, trim. |
