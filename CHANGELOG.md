# Changelog

All notable changes to OneGrade. Versions follow [SemVer](https://semver.org).

---

## v1.5.0 — Range Balance, and highlights that stay in range

### New: Highlight Tone Map (Output)

Rolls bright detail smoothly into white instead of cutting off whatever will not fit. **On by
default** — a shot that used to arrive with a flat white sky now keeps the detail in it.

Everything below the shoulder's start point is left exactly as it was, so mid-tones do not move.

- **Fit From Frame** — measures the current shot and shapes the shoulder to it. Better than the
  default on both ends: a shot that already fits gets no shoulder at all, a shot far over white
  gets the room it needs. Worth pressing on anything you care about.
- **Start** — where the shoulder begins. Lower gives highlights more range and compresses the
  upper mid-tones; higher leaves more of the picture untouched.
- **White Point** — the value that becomes white. Raise it to pack more highlight range in.
- The status line reports what it found: `peak 2.75 contained, sensor clean`,
  `3.0% clipped at sensor` (the camera lost that part — nothing recovers it), or
  `nothing to contain` (this shot already fits).

Untick it to render exactly as the plugin did before.

### New: Range Balance — a bright window and a dark room, in one node

Holds the bright part of the picture still while you open up the rest. Replaces a qualifier, an
invert and a second node.

**To use it:**

1. **Set From Frame** — finds the bright region and sets the Latch to it, and turns on Show Mask.
2. **Show Mask** — white is held, black is opened up, grey is the soft edge. Adjust **Latch** and
   **Softness** until the matte selects what you want, then turn it off.
3. Work the two control sets: **Held: Brightness / Midtones** on what the mask holds, and
   **Rest: Shadows / Midtones / Brightness** on everything else.

Typical window shot: pull *Held: Brightness* down to bring the view back, *Held: Midtones* up to
restore the detail that flattens, then *Rest: Midtones* up to open the room.

- **Lock Mask** — freezes the selection so it stops following the exposure underneath it. Without
  it, pulling the highlights down changes *what* the highlights are. With it, you can pull hard and
  the shape stays put.
- **Shape** (ellipse or rectangle) — restricts Range Balance to part of the frame. What gets held
  is whatever is both above the Latch and inside the shape. Use it when something elsewhere in the
  picture is the same brightness as your subject — a bright pillow across a room from a window,
  which no threshold can separate because it is the same brightness *and* the same colour.
  **Fit To Frame** puts the shape around whatever the Latch is already holding.
- **Bypass** mutes the stage without losing your values.

The Latch stays put while you work, and re-measures itself if Auto Grade or Magic Grade changes
the grade underneath it. Press **Set From Frame** again on a different shot.

**Note:** the mask's softness is in brightness, not in space. On a hard edge like a window frame it
is rock solid; where the edge falls inside noise or a fine gradient it can shimmer. Spatial
feathering is not in this release.

### New: Face Tone Separation (Magic Grade)

Opens or closes the distance between a face and everything around it. It re-solves the grade rather
than nudging one control, so the face stays where Magic Grade placed it while the surround moves.

Only works where the grade was solved around a face. On other shots it hides itself, and the line
above it says why — try Magic Grade on a frame with a clearer face.

### The panel follows the workflow

Reordered into the order you work in: Role / Preset, Magic Grade, Auto Grade, Input Transform,
Balance & Density, Exposure & White Balance, Range Balance, Look / Film LUT, Trim, Output, Setup.

- Balance and Density are one section now.
- Scene Exposure and Scene White Balance moved out of Input Transform to Exposure.
- Highlight Rolloff moved to Exposure, beside the other exposure controls.
- Sections start open except Role / Preset, Export LUT and Setup / Help.

### Fixes

- **Fixed a crash.** Using Range Balance and then pressing Magic Grade crashed Resolve every time.
- **Fixed grades that could vary between runs** on the same frame, including through the Bias
  slider.
- **Panel status lines** now say what is actually in effect rather than leaving a greyed control
  showing a stale value.

---

## v1.4.3 — the Bias slider behaves

- **Bias is predictable now.** It could previously jump: the picture inverting partway through a
  drag, alternating between two looks every other step, or switching permanently to a washed-out
  version with the blacks lifted off the floor and staying there. Four separate causes, all
  fixed. Dragging it now moves the grade smoothly across its whole range on every shot we have,
  and where a shot genuinely runs out of room the slider simply stops rather than falling off a
  step.
- **Your own adjustments survive it.** Nudging Lift, Gamma or Gain by hand after a Magic Grade
  used to be undone the moment you touched Bias. Your edit now becomes the thing Bias leans away
  from. The exception is an edit that blows the frame's highlight — Bias needs somewhere to go,
  so it will not adopt a picture that is already clipped.
- **Bias tells you which way it is working.** After a Magic Grade it re-solves the grade around
  your subject, so Lift, Gamma and Gain move by different amounts and in different directions to
  keep that subject where it was put — the numbers look busy while the picture stays coherent,
  because they are results rather than settings. Without a Magic grade it is a plain offset and
  all three move together. A line under the slider now says which.

### New: pick the subject, instead of pressing until you get it

Magic Grade used to cycle — press again for a different subject, with no way to see what the
alternatives were or to get back to one you liked. It now **looks once and lists what it found**
in a **Subject** dropdown. Selecting a different subject re-grades immediately: the expensive part
is looking at the frame, and that is already done.

This matters more than it sounds, because the subject decides how much room the rest of the grade
has. On a beach shot of a child, one option is the child (**skin, 13% of frame**) and the other is
the sand (**terrain, 43%**). The terrain version is looser and takes Bias across a wide range; the
skin version holds the face in place and allows less movement before it runs out of road.

Neither is wrong, and **which one is more pleasing is a judgement the plugin does not make** — on
that shot the skin version has the better skin tones and an unblown sky, and nothing measurable
says so. That is exactly why the alternatives are now offered rather than hidden behind another
press.

The list is rebuilt each time you press Magic Grade, and is empty until you do — it describes the
frame that was analysed, so a reopened project asks you to press the button rather than showing a
subject it can no longer stand behind.

---

## v1.4.2 — Magic Grade picks the face

- **Magic Grade could land visibly too dark on a shot with a face in it.** Where a frame held
  a modest face against a large wall or interior, the subject was chosen by weighing how much
  of the frame each covered — and the two came out within 1.5% of each other. Landing on the
  wall meant the grade declined to place a subject at all, so a dark shot kept no exposure
  correction and came out around half as bright as it should have. A believable face now takes
  the subject slot outright rather than competing on square footage; a skin region too large to
  be a face (sand, foliage, a whole beach) still falls through to the old ranking.
- **The measurement no longer depends on frame size.** The analysis pass sampled the frame more
  coarsely than the offline tools it is checked against, which is what let the above sit
  undetected: the two disagreed about region coverage by about half a percentage point, and on
  a close call that was the whole decision. Both now read the same samples.
- **Licences ship with the plugin.** The release builds omitted the ncnn and model licence
  texts that the bundle is supposed to carry. They are in the bundle again, under
  `Contents/Resources`.

---

## v1.4.0 — Magic Grade

One button that reads the frame, finds what the shot is *of*, and grades for that.

### Magic Grade

- **A segmentation model finds the subject.** PP-MobileSeg-Base (Apache-2.0) runs on the CPU
  in about 100 ms, sorting the frame into sky, water, skin, vegetation, terrain, ground,
  built and other. It ships inside the plugin bundle — nothing to download, works on any
  render machine.
- **The grade places that subject to be legible**, rather than stamping a preset: its
  shadows, its midtone and the frame's highlight are solved together, after the film LUT,
  because that is the picture you are looking at.
- **It knows underexposed from deliberately low-key**, which nothing measuring the whole
  frame can do — a moody interior and a missed exposure have almost the same median. A face
  too dark to read settles it, and the fix is scene exposure, applied before everything
  else, the way exposing the shot correctly would have.
- **It declines more often than it acts, and says why.** The targets were fitted on faces,
  so a landscape gets Creative Grade and a note explaining that rather than a confident
  wrong answer.
- **The first press is the right one.** The grade is solved against the film LUT, so the LUT
  has to be in memory before the solve rather than after the first render — otherwise a press
  on a node that had not yet rendered could come out dark and crushed, snapping correct as
  soon as any slider moved.
- **Press again for a different subject.** Each press offers a distinct move, and the panel
  shows which region it chose, how much of the frame it covers, and why that control and
  that direction.

### White Balance First (optional)

- Balances on the surfaces that ought to be neutral — walls, floors, ground — and **declines
  when there are none**, which on a sunset is the right answer. Light sources are excluded:
  a blown window is not a grey card, and using one as a reference put a blue cast over
  whole interiors.

### Bias

- Now moves the grade's **targets** and re-solves, instead of nudging sliders. Contrast can
  be added or taken away from a neutral starting point, the subject's brightness never
  moves, and crushing the blacks is no longer possible rather than merely guarded against.

### Fixes

- **Creative Grade no longer crushes the blacks.** The black point was solved in one colour
  space and rendered in another, so it hit its target exactly while the picture on screen
  went to zero. Shadow separation on a test frame went from 0.024 to 0.070.
- **The first press is now the right one.** Magic Grade used to read the node's state before
  the preset that changes it, so a fresh node crushed on press one, recovered on press two,
  and found its white balance on press three.

### For developers

- An offline bench (`experiments/bench`) grades log stills and prints every decision, so
  constants get fitted in seconds instead of one Resolve restart at a time. It calls the
  plugin's own code — including the order the steps run in — so the two cannot disagree.
- The repo is 140 MB lighter: model-conversion intermediates and stray binaries are gone,
  regenerable from `experiments/segmentation/convert_paddle.py`.

### Installing on macOS

The installer clears three macOS protections that otherwise sit between a downloaded zip and a
loaded plugin, none of which announces itself:

- **Quarantine.** A bundle downloaded through a browser is flagged, and Gatekeeper will not let
  Resolve load it — with no error and no plugin in the Effects list.
- **File access.** The installer's privileged step runs as root, and root does not inherit your
  permission to read `~/Downloads`, so copying from there fails with `Operation not permitted`.
- **Resolve's plugin cache.** Resolve records a verdict per plugin, and writes a failed load with
  no size or timestamp — so it cannot tell the plugin has changed and never tries again. Without
  clearing that entry, one failed install makes the plugin invisible no matter how many times you
  reinstall.

The installer also checks the plugin really is on disk before reporting success, and explains
each failure rather than stopping on a raw system error. `docs/RESOLVE-PATHS.md` has the paths
and the order to diagnose a plugin that will not appear.

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

### Added — Base Grade (and Auto Grade becomes two buttons)

- **Two one-click buttons, named for what they do.** **Base Grade** measures the frame and
  places its range so nothing is crushed at 0 or clipped at 1023 — a neutral, gradable
  starting point with **no LUT, no film tint, no density**, letting the smooth decode do the
  work. **Creative Grade** is the previous Auto Grade, unchanged: same measurement, film
  emulation look on top.
- **Base solves rather than fits, and that is the point.** Creative had to be fitted to four
  hand-graded shots, because "what Gain would a colorist choose" is a taste question. "Is
  anything clipped" is not — it is objective and true on anyone's footage. So Base places
  the top with **Gain**, the bottom with **Lift** and the middle with **post exposure**, each
  control owning the end it pivots away from, in three coordinate passes.
- **It measures per channel, not luma.** A waveform shows R, G and B independently and that
  is what clips. Containing *luma* at 0.95 put blue past 1023 on a real interview frame while
  the luma reading insisted the target had been hit exactly.
- **The black point is p0.1, not p1.** Placing the 1st percentile on target leaves a full 1%
  of the frame below it, and on any shot with shadow area that 1% is a visibly crushed region
  on 0. Using p0.1 flips Lift from negative to positive on real shots — it stops pushing
  shadows down — while a genuinely milky flat-log frame still gets its floor corrected.
- **The midtone rides on post exposure, not gamma.** This is the structural lesson from
  Creative, which looks right on bright shots because of the *order* it works in: Gain pulled
  hard, the print LUT shoulders the top, then postExp brings the mids back. Gain down,
  shoulder, exposure back up — an S-curve assembled from three stages. postExp sits after the
  encode and before the highlight softclip, so it lifts mids while the shoulder catches what
  that pushes up. Gamma pivots black *and* white and structurally cannot make that trade.
- **Base gets a shoulder.** Lift/Gamma/Gain can't make an S-curve, so Highlight Rolloff is
  the only film-like response available and it was switched off on most footage — the formula
  was inherited from Creative, where the print LUT was already doing the shouldering. Rolloff
  is now `max(pin × 0.090, overshoot × 0.216)`, where overshoot is how far the channels run
  past display white. Fitted to a hand-dialled value; `hot` was ruled out for the second time
  because it runs backwards.
- **Two asymmetric caps, and they are the important half.** Gain never exceeds its ceiling and
  post exposure never brightens by more than 0.85 stops; darkening is unlimited, because
  pulling a blown frame down is always safe while pushing a dark one up destroys a
  deliberately low-key shot. A car interior at `key +2.90` asked for **+1.74 stops** without
  the cap; the same shot graded by hand used **+0.55**.
- **Mid Strength moves the target, not the step.** It blends the shot's *own* midtone toward
  the target, so 0 keeps the shot as exposed and 1 forces the target. Damping the correction
  instead — as it did at first — is a no-op in a solver that converges: it only changes how
  fast you arrive at the identical place, which is why every shot was being flattened toward
  the same median regardless of the setting.
- Defaults are the values validated on footage, not the originals: Target High 0.94, Target
  Low 0.05, Target Mid 0.70, Max Gain 2.0, Max Exposure 0.85, Shoulder 0.216, Mid Strength
  0.838.
- The readout reports what the solve **achieved**, not what it aimed at, so an unreachable
  target is visible rather than a quietly pinned slider.
- **Bias now works everywhere.** It is an *offset* from the grade a button produced, held in
  hidden saved params — so it survives a project reload, works with either button, and can be
  used on a hand-built grade (the first move adopts the current settings as its zero point).
  Previously it wrote absolute values taken from the film preset, which meant it stamped that
  recipe over whatever grade was actually on the node, and went inert after a restart.
  Creative's response is identical at every Bias value.
- **"Grade on drop" was built, crashed Resolve, and was removed** before release. Calling
  `fetchImage` from the instance constructor trips an assertion inside Resolve that calls
  `abort()` — and since that is a process abort rather than an exception, the `try/catch` it
  was wrapped in gave no protection at all. It also fired on every node in every pre-existing
  project, because the "runs once" flag didn't exist in files saved earlier. Written up in
  [docs/ROADMAP.md](docs/ROADMAP.md); the remaining route is better static defaults.
- Internal: `lgg_core()` extracted into `OneGradePipeline.h` so the solver and the pipeline
  share one definition of the grade curve. Test 14 proves the premise the whole solve rests
  on — that evaluating `lgg_core` on a measured display percentile predicts what the pipeline
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
