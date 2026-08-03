# CLAUDE.md — OneGrade working notes

Operational memory for resuming work. User-facing usage + architecture live in
`README.md`; this file is the "how we work on it and why it's built this way" layer.

## What it is
A **compiled OpenFX plugin** (`.ofx.bundle`) for DaVinci Resolve — one node that does
camera CST → balance → density → exposure → output encode → look/film LUT → trim, on the
GPU (Metal/OpenCL/CUDA) with a CPU fallback. Repo: `github.com/MattGrdinic/OneGrade`.

**History / dead ends (don't retry):** we started as a **DCTL**, then a scripting panel —
both abandoned. DCTL can't do CST/`.cube` LUTs/multi-node; the Resolve scripting API
can't set node params. OFX is the only path that does everything. Not going back.

**Deep-dive explainers live in `docs/`** (how each subsystem works — keep them current
when touching the matching code): `GAMMA.md` (transfer functions, grade curve, encodes) ·
`CAMERAS.md` (input transforms, working space) · `BALANCE.md` (RAW WB + gain/offset
balance) · `DENSITY.md` (HSV-in-DI-log saturation) · `LUTS.md` (discovery, parsing,
sampling, built-ins) · `FILM-EMULATION.md` (Cineon → print-stock path + preset recipe) ·
`CREATING-LUTS.md` (authoring new built-in looks) · `GROUPS.md` (Node Role, the
pre-clip/post-clip split, the DI hand-off + the negative-clip bug it exposed) ·
`AUTO-GRADE.md` (frame measurement, the Gain/Rolloff fits and the footage behind them,
what is deliberately not set, and the traps found on the way).

## The golden rule
`src/OneGradePipeline.h` (namespace `pg`, CPU) is the **single source of truth** for all
color math. The three GPU kernels **mirror it exactly**:
- `src/MetalKernel.mm`  (Apple — the one path validated in Resolve)
- `src/OpenCLKernel.cpp` (kernel is a C string; CI-green on Windows, not correctness-tested on HW)
- `src/CudaKernel.cu`   (`-DBUILD_CUDA=ON`; ON in Windows CI, validated on an RTX 5090)

**Any math change is a 4-file edit.** Keep helper names/formulas identical so they diff
cleanly. Param-count changes also need: Metal `setBytes` length, OpenCL `clSetKernelArg`
loop count + kernel signature, CUDA `cudaMalloc`/`cudaMemcpy` size.

## Pipeline order + SPACES (the crux — took many iterations, do NOT regress)
Per pixel, in `og::process()`:
0. **RAW** (Camera-RAW-tab analogs, so that tab can be left alone):
   - **RAW Exposure** = linear gain in stops on scene light, right after `decode_log`, before
     the CST. This *is* what RAW exposure does (sensor-linear multiply) → near-exact match.
   - **RAW Temp** = Kelvin white balance via a **Bradford chromatic adaptation** in XYZ (right
     after `to_XYZ`, closest to sensor). Blackbody(T) source white → D65; raise T = warmer.
     Identity at 6500 K. NOT byte-exact to the RAW tab (no sensor metadata reaches OFX), but a
     physically-real WB. `white_balance()` / `cct_to_xy()` (Kim et al. locus) helpers.
     (Explainer: `docs/BALANCE.md`.)
1. camera log → scene-linear (`decode_log`)
2. camera gamut → XYZ → **DaVinci Wide Gamut linear** (working space; `docs/CAMERAS.md`)
3. **Balance** in linear: Gain (multiplicative, pivots highlights) + Offset (additive, even)
   (`docs/BALANCE.md`)
4. **Density** = HSV saturation gain in **DI-log** — NOT linear. Linear blows out saturated
   reds; log enriches them. This was a real bug we fixed. (`docs/DENSITY.md`)
5. output primaries: DWG → Rec.709 linear (or keep DWG for DI/Linear encodes)
6. **Lift/Gamma/Gain in the Rec.709 display curve** (linear toe). This is the big one —
   we tried linear and DI-log first and both were wrong. The grade curve **follows the
   output encode**: Scene OETF for the Scene encode, **pure gamma 2.2/2.4 for those
   encodes** (`dg` float, 0 = Scene OETF; `r709_g_enc/dec(x, g)` helpers — replaced the
   old `g24` flag/`r709_24_enc/dec` when 2.2 landed). Either way:
   - **Gain** = multiply, pivots **black**
   - **Lift** = `lift*(1 - min(v,1))`, pivots **white**, clamped so **superwhites aren't amplified**
   - **Gamma** = power, pivots **black & white**
   Matches Resolve's timeline primary wheels. `og_lgg(...,dg)` helper.
7. output encode: **Rec.709 (Scene)** = scene OETF (linear toe), so Lift's taper reads
   linearly on a Rec.709 (Scene) timeline. **Rec.709 (Gamma 2.2)** = pure 2.2 power for
   web/YouTube delivery — the **param default** since 2026-07-16 (user call: that's where
   most exports land). **Rec.709 (Gamma 2.4)** = pure 2.4 power for broadcast/BT.1886;
   the grade curve in step 6 follows whichever is picked. (`encode`/`og_enc`)
   Explainer: `docs/GAMMA.md` (how-it-works only — the why-2.2 rationale lives HERE).
8. LUT + trilinear sample + mix (done in processor/kernels, after encode)
9. post-LUT **Trim**: exposure (stops) + contrast about 0.5 + **Highlight Rolloff**
   (`og::softclip`, per-channel display-space soft clip, asymptote 1.0 — saturated
   practicals converge to white instead of clipping "neon"; gated to display-referred
   output only: `enc <= 2 || active LUT` — never distorts Cineon/DI/Linear feeds)

`P[13] = {temp, tint, density, lift, gamma, gain, offTemp, offTint, postExp, postCon, rawExp,
rawTemp, rolloff}` (postExp/postCon/rolloff applied by the caller in the trim step, not
inside `process()`; rawTemp defaults to 6500 = neutral); `camera` + `outEncode` passed
separately as ints.

Cameras (index): 0 Blackmagic Gen 5 Film (Gen 5 Film log + BMD Wide Gamut Gen 4/5, from
the Gen 5 Color Science white paper; the colorimetric match for Pocket/URSA/Pyxis clips
in a YRGB project — DWG/DI is NOT correct for them) · 1 BMD DWG/DI · 2 Sony S-Log3 ·
3 ARRI LogC3 · 4 LogC4 · 5 Canon Log3 · 6 RED Log3G10 · 7 DJI D-Log · 8 Fuji F-Log2 ·
9 Panasonic V-Log · 10 Rec.2100 HLG · 11 Rec.2100 PQ — **the param default** since the
happy-path redesign: NOT a camera match but the user's preferred creative "smooth
decode" for log footage (compressive inverse-EOTF = near-perfect rolloff, smooth color;
"best results in the most situations"). (Indices were renumbered when Gen 5 moved to
slot 0 — pre-renumber saved grades will show the wrong camera.) Encodes: 0 Rec.709 (Scene) ·
1 Rec.709 (Gamma 2.2) — **the param default** (web/YouTube delivery, 2026-07-16) · 2 Rec.709
(Gamma 2.4) · 3 Cineon Log · 4 DaVinci Intermediate · 5 Linear. (2.2 was INSERTED at slot 1,
shifting 2.4/Cineon/DI/Linear up one — grades saved before 2026-07-16 load one encode off;
old default-2.4 grades become 2.2, film-look grades still render right because lutMode
re-forces Cineon at render.) **709 primaries for enc ≤ 3** (Scene/2.2/2.4/Cineon); DI &
Linear keep DWG primaries. Film Look LUT auto-sets enc=3 (Cineon); Custom Look sets enc=0.
Explainers: `docs/GAMMA.md` (encodes/grade curve) · `docs/CAMERAS.md` (camera list, PQ
smooth decode, stand-in gamuts).

## Node Role — splitting across Resolve's group grading levels (2026-08-02)
`nodeRole` choice param (group "0 Role / Preset"): 0 **Full Grade** (default, the original
one-node behavior) · 1 **Input Transform (Group Pre-Clip)** · 2 **Output Transform (Group
Post-Clip)**. Resolve applies Group Pre-Clip → Clip → Group Post-Clip → Timeline, and the
pre/post graphs are shared by every clip in the group. Role 1 does camera decode only and
pins the encode to **DaVinci Intermediate**; role 2 pins Camera to **1 (DWG/DI)** and owns
the look + LUT + trim + delivery encode. Chained, 1→2 reproduces role 0.

**Why DI is the hand-off:** `decode_log(cam=1,…)` and `encode(enc=4,…)` use identical
constants (exact inverse pair) and both keep DWG primaries, so the round trip is lossless
apart from float error. Measured worst |split − single| = **0.23 8-bit LSB** (0.003 on the
gamma encodes) across 12 cameras × 3 delivery encodes, neutral and graded — test 11 in
`test/pipeline_test.cpp` guards it at < 1 LSB.

**This only works because of the negative-clip fix** (same commit): `safe_pow()` floors at
0, and the LGG loop called `safe_pow(v, 1/gamma)` unconditionally — so **a neutral node
hard-clipped every out-of-gamut negative to zero**, breaking the split by ~30 LSB. Now
`v = (v < 0) ? v : safe_pow(...)`. Regression risk is nil: output is **bit-identical** on
Rec.709 2.2 / 2.4 / Cineon (negatives are already clamped upstream by `r709_g_enc`, and
Cineon clamps to [0,1]), so every validated look and both film presets are untouched. It
changes only Scene / DI / Linear — the scene-referred feeds where clipping was wrong.
Test 12 guards it. **4-file edit** per the golden rule — all four mirror.

Role is enforced **at render** (`setupAndProcess`), not just greyed in the UI: params the
role doesn't own are forced neutral, so a role switch or an old project can't double-apply
the look. `changedParam` stamps the implied values but only on `eChangeUserEdit`, same
guard as presets.

**VALIDATED in Resolve (macOS/Metal, 2026-08-02).** Pre-Clip Input Transform + Post-Clip
Output Transform is visually identical to a single Full Grade node on footage, and
**Resolve does NOT clamp float between group grading levels** — out-of-gamut negatives
survive the boundary (checked on the RGB Parade with the Post-Clip node disabled, trace
visibly below the 0 line). So DI is a safe hand-off and no offset/Cineon fallback is
needed. Panel greying and the forced-encode behavior both confirmed.

**Why DI has so much headroom:** `di_encode` maps linear **100 → code 1.0** and mid-gray
0.18 → **0.336**, i.e. ~9 stops over mid-gray fit inside [0,1]. Even a hypothetical [0,1]
clamp at a group boundary would barely touch real footage — the exposed end was always the
negatives, not the highlights. Worth remembering when picking any future hand-off encode.

## The LUT encode override — visible, not conditional (2026-08-02, github issue)
A LUT pins the pre-LUT encode (Film -> Cineon, Custom Look -> Rec.709 Scene) in
`setupAndProcess`. A user on a **color-managed DWG/I timeline** (not our supported setup —
we need DaVinci YRGB) with Output Encode = DI picked a Look LUT and got a huge contrast
jump. The override was doing its job; **the panel was lying about it** — Output Encode
stayed enabled showing the user's pick while the render used something else.

**Dead end, tried and reverted the same day: do NOT gate the override on `lutMix > 0`.**
It looks like the obvious fix ("Mix 0 should be a bypass") and it wrecks the slider: the
encode then jumps between 0.000 and 0.001, so the first nudge off zero is a contrast
cliff. Mix blends the un-LUTted and LUTted picture **within** the LUT's encode — the encode
is the *domain* of the blend, not a term in it. A selected LUT owns Output Encode at every
Mix value. (Caught on footage by the user, not by tests — the CPU tests don't touch
`OneGrade.cpp` param logic at all.)

What shipped instead:
- render gate is `lutOk` (LUT resolved **and** loaded), not `lutMode` — a missing/corrupt
  `.cube` no longer re-encodes with no LUT (the old silent-degradation shape, cf. the
  Windows Film-list bug: that combination rendered flat Cineon with no print LUT).
- `lutSelected()` (UI mirror of `lutOk`, path-resolves only — no file I/O from a param
  callback) + `setEnabledness()` greys Output Encode whenever a LUT is selected;
  `lutMode`/`lookGroup`/`lookLut`/`filmLut` re-run it from `changedParam`, `lutMix` doesn't.
- **`encodeNote`** string-label param ("In effect") under Output Encode names what's
  actually being rendered. Greying alone is only half the truth — a greyed dropdown still
  shows the stale value. Keep these strings **short (~45 chars) and ASCII**, same panel
  truncation rule as the `helpLine` block.
  **VALIDATED in Resolve (macOS/Metal, 2026-08-02):** a `setValue` on an
  `eStringTypeLabel` param from `setEnabledness()` **does** update the panel live — the
  line switches between "Look LUT owns this: Rec.709 (Scene)" and "Film LUT owns this:
  Cineon in, 709 out" as LUT Mode changes, no reload. So a read-only status line driven
  from `changedParam` is a working pattern here; reuse it rather than re-deriving it.

**General rule, third instance: a silent override is a bug even when the math is right.**
(CUDA CPU-fallback, Windows LUT dir, this.)

Same issue also flagged **two names for one space**: camera option 1 was "Blackmagic
(DWG/DI)", encode option 4 "DaVinci Intermediate". Both are now
**"DaVinci Wide Gamut / Intermediate"** (labels only — choice params save by index, so no
project breakage). `DWG/DI` survives as prose shorthand in docs/comments, not in the UI.

## Presets (param layer only — no pipeline/kernel involvement)
`preset` choice param (group "0 Preset"): 0 None/Reset · 1 Cinematic Film Emulation
(Kodak 2383 D60) · 2 Cinematic Film Emulation (Fujifilm 3513DI D60) · 3 Custom LUT -
Cinematic Landscape · 4 Custom LUT - Teal Orange. **Happy-path redesign (2026-07-14,
user's design):** EVERY preset sets Camera → 11 Rec.2100 PQ (same as the new param
default) so there's no more "some presets change Camera, some don't" confusion; preset
names call out which LUT path they drive (Film Emulation = Resolve print stocks, Cineon
path; Custom LUT = our built-in looks, Rec.709 path). Film Emulation presets share the
user-validated Cinematic Film recipe (see below); Fuji falls back to Kodak when the
stock is missing (`filmLutIndex()` by name fragment, -1 when absent). Custom LUT preset 3
(Cinematic Landscape) is neutral except Offset Temp -0.112 — the user's measured "happy
medium" cool offset for the PQ path — plus a light trim (post-exp +0.023, contrast 0.965),
retuned on footage 2026-07-21 (the offset had been left at a much stronger -0.7 in code). Preset 4 (Teal Orange) has its own on-footage recipe
(user-tuned in Resolve, 2026-07-16): Offset Temp -0.073, Density -0.15, Lift 0.059,
Gamma 1.222, Gain 1.691 — density backed off so the split-tone doesn't oversaturate,
grade lifted + brightened into the look. The PQ smooth-decode trick was discovered when the camera renumber made
an old node decode Gen 5 as PQ. Presets set Rolloff 0 (PQ is already the shoulder).
None/Reset does NOT restore Camera. The former Desert Day / Cinematic Smooth presets are
gone (Smooth is now literally preset 1+default camera; Desert Day lives on as a built-in
LUT only). Explainer for the whole film path (Cineon, print stocks, recipe rationale):
`docs/FILM-EMULATION.md`; LUT machinery (scan/parse/sample/mix, encode coupling):
`docs/LUTS.md`.

**Built-in LUTs** (six ship; Cinematic Landscape + Teal Orange are preset-backed):
generated by
`luts/generate_luts.py` (pure Python, all tunables in the LOOKS dict — edit, re-run,
`make`, reinstall). The .cube files are checked in AND copied into
`Contents/Resources/LUTs` by both Makefile (`bundle-luts`, globs) and CMake bundle
assembly (**explicit file list — new LUTs must be added there by name**).
`bundleLutDir()` resolves them from the plugin binary's own path (dladdr /
GetModuleFileName) and `scanLuts()` surfaces them as the FIRST Look group,
"OneGrade (built-in)" — zero external installs, works on any render machine. The old
Sedona-LUT dependency is gone (was a third-party download — too much to ask of users).
Desert Day + Cinematic Landscape were authored numerically and **user-validated on
footage first try** (2026-07-14). The other four (Golden Hour · Teal Orange (uses the generator's
`split` luminance split-tone) · Silver Bleach · Midnight Blue) are deliberately spread
across the look-space so a default lands close — **NOT yet user-validated on footage**,
except Teal Orange, validated as a preset on footage 2026-07-16 (with its own preset
recipe compensating around the LUT — see Preset mechanics above). Full authoring process + parameter
reference: `docs/CREATING-LUTS.md` (LOOKS entry → regenerate → CMake copy list →
optional preset via `findLookLut()`; user plans to add more looks over time).

Preset mechanics: applied in `OneGrade::applyPreset()` from `changedParam`, **guarded
on `eChangeUserEdit`** so project loads don't re-stamp the preset over user tweaks.
Presets set Camera (→ PQ, see above) + look params (balance temp/tint + offsets, density,
LGG, lutMode/filmLut/lookGroup/lookLut/lutMix, postExp/postCon, rolloff) — never RAW or
Output Encode. Balance IS part of presets (user decision: cool highlights are part of the
cinematic look, not just WB) and None/Reset clears it. The Film Emulation recipe is
user-validated (lift 0.11, gain 0.80, print LUT, +0.55 post-exp; on the old Gen 5 decode
it needed Rolloff 0.5 to stop practicals clipping neon — under PQ decode the shoulder is
built in, so presets set rolloff 0). Values are starting points — expect on-footage
tuning requests.

## Build / test / install (macOS, the dev machine)
```bash
make                 # -> OneGrade.ofx.bundle (universal arm64+x86_64)
make test            # CPU unit tests (test/pipeline_test.cpp) — must stay green
cmake -S . -B build-cmake && cmake --build build-cmake   # cross-platform path
# install for testing in Resolve (needs sudo; Resolve does NOT follow symlinks — copy real files):
sudo cp -fr OneGrade.ofx.bundle /Library/OFX/Plugins/
```
After installing, the user restarts Resolve and checks Color page → Effects → OpenFX →
OneGrade. Only the user can visually verify in Resolve; we can't from here.

**The user's Windows box** (Ryzen + RTX 5090) has no cmake/vcpkg on PATH — use VS Build
Tools' bundled copy, and note the CUDA toolkit supplies OpenCL, so vcpkg isn't needed
locally (CI still uses it). Resolve must be closed to overwrite the plugin; the copy needs
elevation, which we don't have — shell out via `Start-Process -Verb RunAs` and the user
clicks the UAC prompt.
```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S c:\src\OneGrade -B c:\src\OneGrade\build-cuda -G "Visual Studio 17 2022" -A x64 -DBUILD_CUDA=ON
& $cmake --build c:\src\OneGrade\build-cuda --config Release   # -> build-cuda\OneGrade.ofx.bundle
# install target: %CommonProgramFiles%\OFX\Plugins\  (NOT the macOS /Library/OFX/Plugins)
```

## Deploy / release (tag-driven)
CI = `.github/workflows/ci.yml`. Builds+tests macOS + Windows on exactly two events
(user's call, 2026-08-02): **PRs into `main`**, and **`v*` tags**. Nothing else — not
branch pushes, not PRs into `feature/<version>`, not the merge commit on `main` (it's
green because its PR was). `push: ["**"]` plus an unfiltered `pull_request` used to fire
twice for one commit; v1.1.1 produced 7 runs where 3 were wanted. **Consequence: a
sub-feature is only CI-verified when its release branch PRs into `main`** — run
`make test` locally while working. In-flight runs auto-cancel for PR events only, never
for a `v*` tag (a cancelled tag run = no release artifacts). Pushing a
**`v*` tag** additionally runs the `release` job → packages per-OS zip (bundle + installer
from `install/`) → publishes a GitHub Release. Shipping since v0.1.0; tags so far v0.1.0,
v0.2.0, v1.0.0, v1.0.1, v1.0.2, v1.0.3, v1.1.0 (the OneGrade rename), **v1.1.1** (current —
the LUT encode-override visibility fix + the DWG/I naming unification).
Plugin internal version is `kPluginVersionMajor/Minor` in `src/OneGrade.cpp` — OFX carries
only major/minor, so 1.1 covers the whole v1.1.x line (no bump for v1.1.1); bump it when
major/minor moves.

## The rename (2026-08-02) — PowerGrade → OneGrade
Renamed because **"PowerGrade" already means something else in Resolve**: the stills album
in the Gallery. In a room full of colorists "I added it to PowerGrade" parses as Resolve's
feature before it parses as ours — a real problem for a plugin about to be discussed on the
Blackmagic forum.

**It is a deliberate breaking change.** `kPluginIdentifier` went from
`com.mattgrdinic.PowerGrade` to `com.mattgrdinic.OneGrade`, which is the string Resolve
uses to find the plugin in saved projects — so **grades saved with PowerGrade do not carry
over**. User's call, made because adoption was still low enough that a clean break beat
carrying a stale ID forever. Both installers now delete a leftover `PowerGrade.ofx.bundle`
so nobody ends up with a dead duplicate in the Effects list.

Scope: 9 files renamed (`src/OneGrade.{cpp,h}`, `src/OneGradePipeline.h`, six
`luts/OneGrade *.cube`), and the internal namespace/prefix went `pg` → `og` throughout
(`og::process`, `og_lgg`, …). Occurrence counts were checked before and after and matched
exactly (41 `og::`, 235 `og_`, 1 `namespace og`). **The three GPU kernels are compiled at
runtime from strings**, so a prefix typo there would only surface inside Resolve, not at
build time — that's why the rename needs a GPU smoke test, not just a green `make test`.

## Git workflow
**The branch flow (user's, follow it exactly):**
1. Every version gets a branch off `main` named **`feature/<version_number>`** — e.g.
   `feature/1.2.0`. This is the integration branch for the whole release.
2. Each sub-feature gets its **own branch off that** — never off `main`.
3. When a sub-feature is done, **ASK whether to roll it into the release branch.** Do not
   assume. Sometimes yes, sometimes not — it depends on testing, on-footage validation,
   whether the user wants to sit on it. The answer is theirs, not ours.
4. On a yes, it merges back into `feature/<version>`. Repeat from 2 for the next
   sub-feature. The release branch PRs into `main` when the version is done.

The user opens PRs and merges on GitHub (they do the merge, not us; `gh` is NOT installed
on this machine, so hand them a `compare/` URL instead of trying to automate it).
**ALWAYS check `git branch --show-current` before committing** — the user merges PRs
mid-session, so the local checkout can silently be sitting on `main` (this bit us once:
d8ef1d8 went straight to main; user OK'd it that time, pre-release, but never again).
`main` is protected-in-practice; don't commit to it directly. Commits end with a
`Co-Authored-By:` trailer naming the Claude model that did the work. Merged so far: #1 look-first-and-hdr
(all the color fixes), #2 docs-and-distribution, #3 docs-release-process.

## Validation status / gotchas
- **Validated in Resolve:** Metal + CPU on the user's M3 Max, Rec.709 (Scene) / DaVinci YRGB project.
  Node Role group split (Pre-Clip + Post-Clip) validated 2026-08-02, incl. no float clamp
  between group levels — see the Node Role section above.
  CUDA perf on the user's Windows box (Ryzen + RTX 5090, 2026-07-16) — real-time; colour
  output not yet A/B'd against the Metal path.
- **OpenCL:** kernel checked against `og::process` on real HW (2026-07-16) on both an
  RTX 5090 and an AMD gfx1036 iGPU, all 12 cameras x 6 encodes: worst deviation
  ~1.7e-3 in display space (under half an 8-bit code value), which is float rounding, not
  a mirror bug — see the Log3G10 note below. **Still not validated inside Resolve on an
  AMD card** (no AMD dGPU here); the harness drives `RunOpenCLKernelBuffers` directly.
- **Don't chase small GPU-vs-CPU deltas with an absolute tolerance.** Use relative, or the
  wide-range cameras and enc=5 (Linear, values ~50-100) drown out everything else. Where
  both vendors agree with each other but differ from the CPU, suspect precision, not math:
  e.g. camera 6 (RED Log3G10) decodes `(pow(10,x/0.224282)-1)/155.975327 - 0.01`, and at
  small x that `-1` cancels two near-equal numbers, so host `powf` (more internal
  precision) and OpenCL single-precision `pow` legitimately diverge.
- **Advertise only backends that are actually compiled.** `describeInContext` flags are
  what the host picks a GPU path from, and once it picks there is **no CPU fallback**
  (`ofxsProcessing.h` `process()`). Until 2026-07-16 the plugin called
  `setSupportsOpenCLBuffersRender(true)` unconditionally while the body of
  `processImagesOpenCL()` sat behind `OFX_SUPPORTS_OPENCLRENDER` — a symbol **nothing ever
  defined** (the Makefile/CMake define the unrelated `OFX_SUPPORTS_OPENGL RENDER`; the
  near-identical name is the trap). It compiled to an empty function, so any AMD/Intel GPU
  — or anyone setting Resolve's GPU mode to OpenCL — got an unwritten destination buffer:
  a black frame. macOS never hit it because Resolve drives Metal there. Each
  `setSupports*Render` call is now behind the same `#ifdef` that guards its implementation.
- **CUDA is a silent-fallback trap.** A plugin built without `-DBUILD_CUDA=ON`, or with a
  CUDA 12.x toolkit (tops out at sm_90, so nothing for Blackwell/sm_120), still loads and
  renders fine — Resolve just quietly renders the node on the CPU. That shipped once and
  read as "the plugin is incredibly slow on Windows" (2026-07-16). Guards now: Windows CI
  passes `-DBUILD_CUDA=ON` with CUDA 13.2, and a `cuobjdump --list-elf` step fails the
  build if sm_120 is missing from the bundle. Diagnose with
  `cuobjdump --list-elf <plugin>.ofx` — "does not contain device code" means CPU fallback.
  Keep `CUDA_ARCHITECTURES` as the bare `all-major` keyword (nvcc expands it against the
  real toolkit); do NOT expand it from `CMAKE_CUDA_ARCHITECTURES_ALL_MAJOR`, which is baked
  into CMake and lags it (3.31 still lists CUDA 13's removed `compute_50`, stops at 90).
  Separable compilation must stay OFF or the `-dlink` strips the fatbinary's PTX.
- **Camera matrices** other than Blackmagic are published/approx — flagged for on-footage validation.
- **Resolve's LUT folder is per-platform** (`filmLutDir()`): Windows adds a `Support` level
  (`%PROGRAMDATA%\Blackmagic Design\DaVinci Resolve\Support\LUT`), macOS doesn't
  (`/Library/Application Support/…/DaVinci Resolve/LUT`), Linux is `/opt/resolve/LUT`. It
  was hardcoded to the macOS path until 2026-07-16 → empty Film list on Windows, no error,
  Film Emulation presets silently rendered with no print LUT. Same class of bug as the CUDA
  fallback: **a missing resource degrades silently instead of failing.** Any new
  host-path/resource lookup gets the per-platform treatment + a way to tell it found nothing.
- **Can't auto-read the timeline colorspace** from OFX without becoming color-managed (which
  would make Resolve override our CST). So Output Encode is manual; default Rec.709 (Gamma 2.2).
- HDR (HLG/PQ) is a **normalize, not a tone-map** — highlights can clip; a real shoulder is future work.
- **Timeline Color Space is a MONITORING setting, not an encode setting** (macOS, measured
  2026-07-29 — the docs said the opposite until then and it was the cause of a week-long
  "YouTube looks wrong" hunt). macOS resolves a Rec.709 tag via the **scene OETF** (≈1.96
  effective) — QuickTime, Safari/Chrome/Firefox, YouTube all do. Resolve's
  Preferences → General → *"Use Mac display color profiles for viewers"* has a sub-option
  *"Viewers match QuickTime player when using Rec.709 Scene"* that **only engages on a
  Rec.709 Scene timeline**. With the timeline on Gamma 2.2, playback measured a clean
  **^0.815 ≈ 1.96/2.4** power law vs the viewer, uniform from 0.40 to 0.94, identical
  across three browser engines and invariant to the display ICC profile. Timeline →
  Rec.709 (Scene) + that preference ON makes viewer/QuickTime/YouTube agree to ~0.01.
  **The file was never wrong — the viewer was**: clips at 2.2, 2.4 and the film path all
  converged at once, which an encode fix could not do. So Output Encode default stays 2.2;
  the two params are orthogonal and the docs must say so (users will otherwise "fix" the
  encode to match the timeline — the trap the old wording built). Windows/Linux: no such
  preference, **unverified**. Explainer: `docs/GAMMA.md` §4.
- Required project setup (also in the plugin's Setup/Help group): DaVinci YRGB, Timeline
  **Rec.709 (Scene)** on macOS (+ the viewer preference above) regardless of Output Encode,
  clips left at camera log, no CST/LUT before this node.
- **Don't expect a pixel match against Resolve's "Gen 5 Film to Video" LUT** — "to Video"
  bakes in Blackmagic's contrast/tone curve, not a plain colorimetric conversion. The right
  neutral reference is a CST node (Gen 5 Film → Rec.709 / Gamma 2.4, tone mapping off).
  Nor against the **Camera RAW tab's default color-science path**, which applies a
  **camera-specific technical LUT** (per-model tuning, e.g. Pyxis 6K) on top of the log/gamut
  transform — we can't reproduce it, no sensor metadata reaches an OFX plugin. README §"This
  plugin is opinionated" says this to users out loud (added 2026-08-02, user's call): it's a
  look-first tool, family resemblance to a CST/RAW-tab neutral, not a match. Say this when
  users report "it doesn't look like a plain CST".

## Auto Grade ("magic button") — user's idea, in progress from 2026-08-02
One click that reads the frame, measures it, and sets the sliders to a pleasing cinematic
starting point (creamy lifted lows, exposure on the key, smooth shoulder). **Why it's
tractable where general image analysis isn't: the output is PARAM VALUES, not pixels.**
`og::process()` and the three kernels are untouched — no golden-rule 4-file mirror, no
CPU/GPU reduction agreement, no per-frame temporal instability (click once, values freeze).
It is `applyPreset()` with the numbers measured instead of hardcoded, same
`eChangeUserEdit` guard.

**Staged, because two questions decide whether it's real:**
1. **Probe — can a button read pixels outside `render`?** `probeAnalyze()` + the
   "9 Auto Grade (experimental)" group. Fetches the source image in `changedParam`, walks a
   coarse grid, reports size/percentiles into two label params. Wrapped in try/catch:
   fetchImage outside render may throw, return null, or hand back zeros, and all three are
   answers. `anyNonZero` is tracked separately so "empty buffer" stays distinguishable from
   "black shot". **ANSWERED YES — validated in Resolve 2026-08-02:** a 4K frame came back
   from `fetchImage` inside `changedParam`, 230400 samples, plausible log percentiles
   (p1 0.240 / p50 0.465 / p99 0.725). The button approach is viable; the feature stays in
   the param layer.
2. **Analysis + readout only — BUILT, awaiting on-footage sanity check.** Measures through
   the *real* pipeline, not a parallel copy: scene luminance is **XYZ Y from `to_XYZ`**
   (exact and gamut-agnostic — Rec.709 luma weights are wrong against DWG primaries), and
   display values come from **`og::process()` itself at neutral params**, using the user's
   Camera + Output Encode. Measuring the *neutral* node is deliberate: analysing the graded
   result would make a second click chase its own tail. Percentiles via `nth_element` over
   the kept samples (~200k, under a megabyte) rather than a histogram — no binning error.
   Reports Y50 / key EV / DR stops, display p1-p50-p99, hot/src/sat, and a subject row.

   **`key` VALIDATED on footage (4 shots, 2026-08-02).** It orders exposure correctly
   across ~4 stops: overexposed cactus **-1.79 EV** (Y50 0.62) · bright interview **-1.05**
   · golden-hour desert **-0.79** · dark car interior **+2.37** (Y50 0.035). Direction and
   magnitude both read the way a colorist would call it, so it's a sound basis for step 3.

   **Two things that footage taught us, both now measured:**
   - **`hot` (bright in display) is NOT `pin` (clipped at the sensor).** The user's cactus
     shot is 36% hot but "we had the range on camera" — pulling exposure down recovers it.
     A shot pinned at the top of its log range does not recover at any exposure. Any
     auto-exposure that ignores this will happily "fix" unrecoverable frames.
     **Do NOT test source clipping against 1.0.** First attempt used `>= 0.995` and was
     wrong: a raw waveform with the node disabled showed **Blackmagic log peaking at
     ~768/1023 ≈ 0.75** with a textured, unpinned top. A fixed 1.0-ish threshold therefore
     reports 0% on *every* Blackmagic shot, blown ones included. What identifies clipping is
     a **pile-up at whatever this clip's own maximum is** — a real highlight rolls off with
     falling density, a clipped one stacks samples on the ceiling. So `pin` = share within
     `max(0.002, srcMax*0.004)` of the observed max, reported as `pin %@srcMax`.
     Generalises across cameras and log formats for free.
   - **Frame-median exposure is subject-blind.** The car-interior shot asks for **+2.37 EV**
     because a dark interior dominates the frame — applying it would blow the windows and
     overexpose a face that was already fine. Hence the Subject row: the same key asked of
     skin-toned pixels only, with its coverage % alongside. **The mask cannot tell skin from
     sand** — on a desert shot it matches most of the frame, and a high coverage % is the
     tell that the number means nothing (desert frame: **39.7%** coverage = sand, not a face
     — the guard working as designed). Where the two keys disagree, frame median is wrong.
     **Select the mask on CHROMATICITY ONLY.** v1 also gated on display luma 0.15-0.95 and
     that is self-fulfilling: it picks mid-tone pixels by construction, so their median
     lands near mid-gray and the key reads ~0 on every shot. Caught on the desert frame —
     masked Y 0.2100 against a frame median of 0.6236, i.e. a filter artefact reported as a
     measurement. Only a minimal luma guard remains (too dark / too blown for hue to mean
     anything). **General lesson: a selection rule that constrains the quantity being
     measured produces a number that describes the filter, not the footage.**

   **The Scene row is grade-independent — confirmed by accident and worth relying on.** The
   same cactus frame read `Y50 0.6236 / key -1.79 / DR 6.5` both ungraded and with a Custom
   Look at Mix 1.0, while the Display row moved (p1 0.126 -> 0.201, the LUT lifting blacks).
   So Analyze can be clicked on an already-graded node and still describe the *footage* — no
   tail-chasing, no "only use on a fresh node" caveat needed in the UI. Exposure, DR and the
   subject key come from the Scene row; only black-point and rolloff targets need Display.

   Display stats start from the **effective** encode (same LUT override as the render) but
   **fall back to Gamma 2.2 when that isn't display-referred** — a Film Look forces Cineon,
   and analysing in Cineon silently breaks two things: it clamps to [0,1] so `hot` reads a
   flat 0% on a blown frame, and it compresses chroma so the skin mask's saturation window
   stops matching faces. Both were observed on one shot when only the LUT mode changed —
   `hot` 22.9% -> 0.0%, skin coverage -> 1.6%. **Percentile and hue thresholds are only
   meaningful in the space they were chosen for.** The Display row now prints which encode
   it used (`@Scene` / `@2.2` / `@2.4`). The Scene row is encode-independent and is the
   robust one to build heuristics on.

   **`key` IS DESCRIPTIVE, NOT PRESCRIPTIVE — the single most important finding so far.**
   The dark car-interior shot asks for **+2.37 EV** (frame) and **+2.25 EV** (skin-masked),
   i.e. the subject mask did *not* rescue it. But the user's own grade on that shot is trim
   exposure **+0.55** with Gain pulled to 0.714 — roughly a quarter of what `key` demands.
   A moody low-key interior is *supposed* to have a low median; "move the median to 0.18"
   would flatten every deliberately dark shot into mid-gray mush. So step 3 must NOT map
   key -> exposure directly. Options to try: apply a fraction of key, clamp hard (±1 EV),
   or only correct when the shot falls outside a plausible band. Also note 18% grey is the
   wrong *target* for skin specifically — lit skin sits roughly a stop above mid-gray.
3. **BUILT — and it turned out to be one slider, not five.** Four hand-graded shots
   (2026-08-02) were the **Cinematic Film Emulation preset with exactly one value moved:
   Gain**. Lift, gamma, density, trim, and the `Gain Temp -0.220 / Gain Tint 0.090` tint
   were **identical across all four** — the user's words: "the filmic look has a bit of a
   tint, an opinionated look if you will". The car-interior grade *is* the untouched preset.
   And Gain tracks the measured key:

   | shot | key | gain | fit `0.80 + 0.19*key` |
   |---|---|---|---|
   | car interior | +2.60 | 0.800 | 0.800 (clamped) |
   | desert | -0.79 | 0.642 | 0.650 |
   | interview | -1.04 | 0.655 | 0.602 |
   | cactus | -1.96 | 0.407 | 0.428 |

   Three of four within 0.02. The interview is the outlier and explains itself: the only
   shot on a different camera (F-Log2) with **RAW Exposure already at -0.50**, so part of
   its correction happened upstream of Gain. `applyAutoGrade()` = `applyPreset(1)` then
   `gain = clamp(0.80 + 0.19*key, 0.30, 0.80)`.

   **The clamp at the preset value for key >= 0 is the important half.** It's how "key is
   descriptive, not prescriptive" gets resolved: a dark shot is never pushed up, so a
   deliberately low-key interior keeps its intent. That came out of a clamp, not a special
   case. Floor at 0.30 because the fit is only evidenced to about -2 EV; the bare line
   reaches zero near -4 EV.
4. **Warmth + rolloff — the two gaps the user named after trying the button** ("the auto
   grade is quite cool and highlights are quite harsh", interview shot, 2026-08-02). Their
   grade fixed both with controls the button doesn't touch: **RAW Temperature 6500 -> 9242**
   and **Density 0.436**, plus **Rolloff 0.557**. Measurements added for these, not yet
   fitted — need a data pass on the fixed build first:
   - **ROLLOFF SOLVED — it's `pin` (source clipping), and nothing else.** Measured across
     four shots on the fixed build:

     | shot | hot | **pin** | peak | user's rolloff |
     |---|---|---|---|---|
     | cactus | 33.7% | **0.00%** | x1.12 | **0** |
     | car | 6.4% | **0.00%** | x1.05 | **0** |
     | desert dirt | 0.0% | **0.00%** | x1.05 | **0** |
     | interview | 17.8% | **6.18%** | x1.00 | **0.557** |

     `rolloff = min(0.80, 0.090 * pin%)`. Physically right: rolloff softens flat
     detail-free patches, and clipped-at-source *is* flat and detail-free, while a merely
     bright frame keeps its texture and needs nothing. **Two candidates are ruled out by
     that table** — `hot` runs backwards (33.7% -> 0, 17.8% -> 0.557), and so does
     `p99.9/p99`, my own hypothesis: a big blown window puts p99 and p99.9 on the *same
     plateau*, so the interview scores the *lowest* multiplier. Evidenced by one non-zero
     point; three controls sit correctly at zero.
   - **WARMTH IS NOT DERIVABLE FROM `R/G`** — and may not be derivable at all. Only two of
     the four shots have a real face (interview skin 3.4%, car 10.3%; cactus 46.5% and
     desert 72.2% are sand). Those two measure **R/G 1.21 and 1.22** — indistinguishable —
     yet the user warmed one to **RAW Temp 9242** and left the other at 6500. The warmed
     shot is also the only one on a different camera (Fuji F-Log2), which points at a
     *shoot* property rather than an image-content one. B/G was truncated in the panel and
     is now visible; if it doesn't separate them either, **the button should leave warmth
     alone rather than guess.** Not every control the user touches is a correction — some
     are taste, and taste has no measurement to fit.

5. **Bias slider (`autoBias`, -1..+1, default 0) — LIVE, moves FOUR params, group sits FIRST
   in the panel.** Driving Lift + Rolloff only wasn't enough (user: "all that seems to happen
   now is we change lift") — Rolloff clamps at 0 for positive bias, so opening a shot up did
   nothing but raise the floor. Now Lift/Gamma/Gain/Rolloff move together. **Gain's response
   is measurement-modulated**: the positive direction scales by `max(0, 1 - hot/40)` so
   brightening fades out on a frame that's already a third above white; the negative
   direction is never scaled, since pulling gain down is always safe. `m_LastGain` holds the
   measured value so a Bias drag stays anchored to it instead of drifting. **The whole analysis UI is
   hidden in shipping builds** behind `static const bool kAnalysisDebugUI = false` in
   `OneGrade.cpp` — the `showAnalysis` checkbox, the Analyze Frame button, the six readout
   rows and the Applied line. A colorist sees only Auto Grade + Bias. **FUTURE WORK: flip
   that constant and rebuild to get the debug panel back** — it's the mode to be in when
   fitting new constants, since every current fit was found by reading those rows across
   real footage. Visibility only: the params exist and work either way, so nothing about
   saved projects depends on it. Secrets are applied in `setEnabledness()` so they survive a
   project load.** (both at the user's request, 2026-08-02: "the bias adjustment is great, the only
   thing is to make it real time"). `applyBias()` is split out of `applyAutoGrade()` so a
   drag re-derives Rolloff and Lift from the **cached** measurement — pure arithmetic on two
   stored numbers, no re-analysis, so it keeps up with the drag. Gated on `m_AutoApplied`:
   dragging Bias on a node that was never auto-graded must not silently stamp values, and
   since that flag and the cached measurement are instance state, the slider goes **inert
   after a project reload** until Auto Grade is pressed again — deliberately inert rather
   than acting on a stale number. The group is **unnumbered** while experimental: 0-8 is the
   pipeline in application order and this isn't a pipeline stage, plus it keeps the numbering
   in the README and users' heads from shifting for a feature that may still change shape.
   Number it 0 and renumber the rest if it graduates.

   User's request after using the button Negative tames the top (rolloff up, lift down), positive opens the bottom
   (lift up, rolloff down), 0 = the fitted result. `rolloff = clamp(0.090*pin - bias*0.35)`,
   `lift = clamp(0.11 + bias*0.06)`. It moves **Rolloff and Lift only** — those are the two
   the user reached for in exactly this situation ("add a touch of highlight rolloff until
   we bring the highlights below 1023", "lift darker images a bit"). **Gain stays on its
   measurement**: it's the one parameter with a hard physical anchor, and letting a taste
   control drag it would undo the part that works. It's an *input to the button*, read when
   pressed — not a live control.

**KNOWN LIMITATION — a shot whose exposure changes mid-take.** The user's car clip cranes
from a bright exterior into a dark interior. Auto Grade on the exterior frame produces an
excellent exterior and too dark an interior; clicking again inside produces an excellent
interior. This is single-frame analysis meeting a multi-stop change, and the right answer
is to split the clip, which the user reached independently. **Frame-based is a feature
here, not a defect** — the user picks which moment it optimises for by parking the
playhead. Say that rather than trying to engineer around it.

**Fit to the USER's grades, not to a convention.** Every textbook target tried before this
(median -> 18% grey) contradicted what the user actually does. Four shots of ground truth
beat the convention immediately. Re-fit the constants if the style shifts; they're two
numbers in `applyAutoGrade()`.

**Design rules agreed up front:** percentiles, never means (one blown practical wrecks a
mean) · subsample to ~200k samples (a button that stalls on 8K is its own failure) ·
**the button writes visible slider values the user can then adjust** — a starting point
that shows its work, not a black box, so a bad analysis costs one undo rather than trust ·
**no LUT selection in v1** ("warm scene therefore Golden Hour" is a guess; "median 0.31,
target 0.42" is a measurement) · skin is most of what "pleasing" means and a luma histogram
can't find it — a hue-window mask is the biggest quality lever, design for it early.

## Likely next tasks
**Rolloff smoothness on Gen 5 (user's active thread):** the Highlight Rolloff softclip is
not yet as smooth as the "Blackmagic Gen 5 Film to Video" LUT, which is the stated target
for the default Gen 5 path (Cinematic Film preset). Candidates: tune softclip knee/curve,
or a scene-linear shoulder before encode instead of (or blended with) the display-space clip.

**CUDA colour A/B (opened 2026-07-16):** the CUDA path is now live and fast on the user's
5090, but only *perf* was checked — its output has never been compared against the
validated Metal/CPU result. `CudaKernel.cu` was written blind and had never even been
compiled before this. Worth a same-frame A/B (mac vs Windows) — still open at v1.0.3.

Validate OpenCL inside Resolve on an AMD card; per-camera gamut validation; HDR tone-map
(highlight roll-off). (Done: Rec.709 Gamma 2.4 output with grade-space-following LGG,
branch `feature/rec709-gamma24` — validated in Resolve. In progress: camera 0 Blackmagic
Gen 5 Film — now the default camera, list reordered so both Blackmagic entries lead —
+ default encode → Gamma 2.4, branch `feature/gen5-camera-g24-default` — needs visual
verify on Pyxis footage vs a CST node. Superseded 2026-07-16: default encode is now
Rec.709 Gamma 2.2 for web/YouTube delivery, branch `feature/gamma22-default`, encode
indices renumbered — see `docs/GAMMA.md`.)
