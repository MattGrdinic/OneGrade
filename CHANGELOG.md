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

### Experimental — Match Clip probe

- **A probe, not a feature yet.** Marc Wielage suggested matching a shot to the one before
  or after it. Everything downstream of that is tractable — we already measure a frame and
  already turn measurements into slider values — but one question gates it and only Resolve
  can answer it: *can an OFX plugin see pixels belonging to a different clip at all?*
- The **Match Clip (probe)** group asks the host for a frame N before and after the
  playhead and reports what came back, including a verdict line. The outcome that matters
  is telling a genuine read apart from the host **clamping** to this clip's own bounds and
  handing back a frame we already had — a naive probe would call that success.
- `setTemporalClipAccess` is now on for the effect and the source clip, which the OFX spec
  requires before fetching any frame other than the render time.
- Hidden behind `kMatchProbeUI` in `OneGrade.cpp`, currently on for testing.

### Internal

- Plugin version bumped to **1.3** (`kPluginVersionMajor`/`Minor`). It had been left at
  1.1 through the v1.2.0 release.

<!-- Entries below land as the work does; see the branch feat/forum-feedback. -->

---

## Acknowledgements

**justin_daniels** — for the detailed critique on the Blackmagic forum that prompted this
entire release: the RAW naming, the honest label on the smooth decode, per-operation
bypass, and the LUT export idea are all theirs. Several of the points were ones we'd have
defended rather than fixed if they hadn't been made so precisely. Disagreeing well is a
contribution, and this one improved the plugin more than any feature request has.

**Marc Wielage** — for the match-clip idea (matching a shot to the one before or after it),
currently under exploration. If it ships, it ships because they suggested it.

---

## Earlier releases

| Version | Highlights |
|---|---|
| **v1.2.0** | Auto Grade ("magic button") — measures the frame and sets a cinematic starting point; live Bias slider; Node Role group split (Pre-Clip / Post-Clip). |
| **v1.1.1** | LUT encode-override made visible in the panel; "DaVinci Wide Gamut / Intermediate" naming unified across Camera and Output Encode. |
| **v1.1.0** | Renamed PowerGrade → OneGrade (the old name collides with Resolve's Gallery feature). Breaking: saved grades do not carry over. |
| **v1.0.x** | Six built-in look LUTs shipped inside the bundle; per-platform Resolve LUT directory; CUDA build (sm_120) and the OpenCL black-frame fix; Rec.709 Gamma 2.2 default. |
| **v0.x** | Initial OpenFX plugin: camera CST, balance, density, exposure, output encode, LUT, trim. |
