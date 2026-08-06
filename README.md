# OneGrade

A single-node **OpenFX** color grade plugin for DaVinci Resolve. It collapses the
classic scene-linear grading chain — **input CST → balance → density → exposure →
output transform → look / film LUT** — into one node with a clean, grouped interface,
running on the GPU (Metal / CUDA / OpenCL) with a CPU fallback.

```
[ OneGrade ]  ==  CST → Balance → Density → Exposure → Output → LUT → Trim   (one node)
```

- **macOS** (Apple Silicon + Intel, universal) — Metal + OpenCL
- **Windows / Linux** — CUDA + OpenCL
- Cross-platform 3D `.cube` LUT support, HDR (HLG / PQ) input, and camera log/gamut transforms for Blackmagic, Sony, ARRI, Canon, RED, DJI, Fuji, Panasonic.

---

# Part 1 — For Colorists

## Install (end users)

1. Download the latest release for your OS from **[Releases](../../releases)**.
2. Unzip and run the installer:
   - **macOS**: double-click `install-macos.command` (copies the plugin to `/Library/OFX/Plugins/`; you'll be asked for your password).
   - **Windows**: right-click `install-windows.bat` → **Run as administrator**.
   - Or copy `OneGrade.ofx.bundle` manually to:
     - macOS: `/Library/OFX/Plugins/`
     - Windows: `C:\Program Files\Common Files\OFX\Plugins\`
3. Restart DaVinci Resolve.

## Project setup (do this once)

OneGrade does the camera transform itself, so Resolve must **not** be color-managing
your clips. In **Project Settings → Color Management**:

| Setting | Value |
|---|---|
| Color Science | **DaVinci YRGB** (not Color Managed / not ACES) |
| Timeline Color Space | **Rec.709 (Scene)** — see the macOS note below. This is *not* tied to the plugin's Output Encode |
| Output Color Space | Same as Timeline |

Leave your clips at their **camera raw / log defaults** — don't put a CST or input LUT
before this node. (This guidance is also in the plugin's **Setup / Help** section.)

### macOS: what you see vs. what you deliver

Also turn on **DaVinci Resolve → Preferences → General → "Use Mac display color profiles
for viewers"**, which enables its sub-option *"Viewers match QuickTime player when using
Rec.709 Scene"*.

macOS interprets a Rec.709 tag using the **scene OETF**, not a pure power curve —
QuickTime, Safari/Chrome/Firefox and YouTube playback all do this. **Rec.709 (Scene) is
the only Timeline Color Space under which Resolve's viewer adopts that same
interpretation.** Set the timeline to anything else and the viewer shows you a picture no
other application on the machine will reproduce: your export looks different in
QuickTime, and different again on YouTube, while the file itself is perfectly correct.

With both settings in place the chain matches end to end — Resolve viewer → ProRes/H.264
export → QuickTime → YouTube — verified by measurement on macOS, 2026-07-29.

**This is independent of Output Encode.** Timeline Color Space governs how Resolve
*interprets* the picture for the viewer; Output Encode governs the curve the plugin
*bakes into the render*. Timeline **Rec.709 (Scene)** with Output Encode **Rec.709
(Gamma 2.2)** is the correct, intended combination — it looks like a contradiction and
isn't. Leave Output Encode on its default unless you're delivering for broadcast.

*Windows/Linux:* the viewer-matching preference is macOS-only and this chain has not been
verified there. Match Timeline Color Space to your Output Encode as a starting point, and
check an export against your player before trusting the viewer.

## Add it

Color page → **Effects** (Library) → **OpenFX → OneGrade** (the category) **→ OneGrade**
(the plugin) → drag it onto a node. The controls appear top-to-bottom in the order
they're applied.

> **Renamed in v1.1.0.** This plugin was previously called *PowerGrade* — renamed because
> "PowerGrade" already means something specific in Resolve (the stills album in the
> Gallery). The OFX plugin ID changed with it, so **grades saved with PowerGrade will not
> carry over**; the installers remove the old bundle so you don't end up with a dead
> duplicate in the Effects list.

## About the model in Magic Grade

**Magic Grade** looks at the frame and works out roughly what is in it — sky, water, foliage, a
person, ground, buildings — then uses that to pick which slider to move and in which direction.
That recognition is done by a small neural network bundled with the plugin, and it is worth
being straight about what that means.

- **It never touches the network.** Nothing is uploaded, nothing phones home, no account, no
  key. It works on a render node that has never been online.
- **It runs on a button press only.** Not during playback, not during render. Press the button,
  it thinks for about a tenth of a second on one CPU thread, and writes ordinary slider values
  you can then drag.
- **It is 12 MB.** [PP-MobileSeg-Base](models/README.md), Apache-2.0 licensed, running on the
  CPU. No GPU is involved and none is needed.
- **It is not required.** If the model is missing the rest of the plugin works exactly as
  before, and Magic Grade says so rather than pretending.

It is also **not a fail-safe tool and is not meant to be.** It offers one opinionated move,
tells you in plain words why it chose it, and gives you a slider to push it further or take it
back. Press the button again and it picks a different subject. On some shots — a flat aerial, a
macro of leaves — it will correctly tell you there is nothing to separate and leave you with
Creative Grade.

---

## This plugin is opinionated

Worth knowing before you compare it to anything: **OneGrade is a look-first tool, not a
colorimetry reference.** A neutral OneGrade node is not the same picture as a neutral CST
node, and it isn't meant to be.

- **The camera transforms are published math, applied uniformly.** Resolve's own decode
  for a specific camera can go further — the Camera RAW tab's default color science path
  applies a **camera-specific technical LUT** (e.g. for a Pyxis 6K), which bakes in
  per-model tuning on top of the log/gamut transform. OneGrade can't reproduce that: no
  sensor metadata reaches an OFX plugin, so all it has is the camera's published log curve
  and gamut. Expect a family resemblance to the RAW-tab default, not a match.
- **The default Camera isn't a camera match at all, and says so.** The entry is named
  **Rec.2100 PQ - Smooth Decode**: a deliberate creative choice, a compressive curve that
  flatters log footage with a near-perfect highlight rolloff. It's the happy path the
  presets are built on. Every *other* entry in the list is a faithful decode — if you want
  a colorimetric starting point, pick your actual camera.
- **Don't A/B it against "Gen 5 Film to Video" either.** That LUT bakes in Blackmagic's
  contrast and tone curve; it isn't a plain colorimetric conversion. The fair neutral
  reference is a CST node with tone mapping off.
- **The grade wheels follow the output curve, and the LUT paths pin it.** Both are
  deliberate (see Output Encode below) and both are things a raw CST chain doesn't do.

None of this is a limitation you need to work around — it's the trade. You get one node,
one set of controls, and a look that lands close on the first move. If your job is to
match a reference transform exactly, use a CST node for that part.

## The controls

**Auto Grade (experimental)** — one click that reads the frame and sets a starting point.
It measures the shot's exposure and its highlight clipping, applies the Cinematic Film
Emulation look, and writes **Gain** and **Highlight Rolloff** from those measurements.

- **Auto Grade** — measures the frame, then writes the look and the derived values.
- **Bias** — leans the result across the whole tonal range, live: negative protects the
  highlights (shoulder up, floor down, mids darker, Gain pulled), positive opens the image
  up (floor and mids up, shoulder off). Zero is the measured result.

Everything it writes is an ordinary slider value you can drag afterwards, so a starting
point you don't like costs one undo. It deliberately leaves **white balance and Density
alone**: exposure and clipping are measurable, warmth is taste, and a button that guesses at
taste is worse than one that doesn't touch it. Full method, the fits, and what they were
fitted to: [docs/AUTO-GRADE.md](docs/AUTO-GRADE.md).

**0 · Node Role** — which part of the pipeline this node does. Leave it on **Full Grade
(single node)** unless you group your clips; that's the default and it's the whole plugin
in one node. The other two roles split it across Resolve's group grading levels so a whole
group shares one setup:

- **Input Transform (Group Pre-Clip)** — camera decode only, handed off in DaVinci
  Intermediate. The panel drops to three live controls: Camera, Scene Exposure, Scene
  White Balance.
- **Output Transform (Group Post-Clip)** — takes that hand-off and applies the look, LUT,
  trim and delivery encode.

Chained, the two match a single Full Grade node (measured to within a quarter of an 8-bit
code value). Controls a role doesn't own are greyed out *and* forced neutral at render, so
the look can never be applied twice. See **Workflows** below, and
[docs/GROUPS.md](docs/GROUPS.md) for the full explanation.

**0 · Preset** — one-click starting points on the happy path. Every preset sets
**Camera → Rec.2100 PQ - Smooth Decode** (also the plugin default) plus Balance,
Density, Lift/Gamma/Gain, LUT and Trim; every slider stays live to tweak per clip. The
scene stage and Output Encode are never touched. The name tells you which LUT path it drives:
- **Cinematic Film Emulation (Kodak 2383 D60)** — cooled highlights against warm
  practicals, shadows lifted off video-black, gain pulled so highlights roll into the
  print stock, brightness brought back after. Swap stocks in **Film Look LUT**.
- **Cinematic Film Emulation (Fujifilm 3513DI D60)** — the same recipe on Fuji's print
  stock (falls back to Kodak if that stock isn't in Resolve's Film Looks).
- **Custom LUT – Cinematic Landscape** — the built-in creamy outdoor look through the PQ
  decode, with a gentle cool offset. Swap looks in **Look LUT** (six built-ins ship).
- **Custom LUT – Teal Orange** — the built-in blockbuster split with its own on-footage
  recipe: density eased so the split-tone doesn't oversaturate, shadows lifted and the
  image brightened into the look.
- **None / Reset Look** — returns the look params to neutral (Camera stays put).

The built-in LUTs live at `OneGrade.ofx.bundle/Contents/Resources/LUTs` and appear in
the **Look LUT Group** dropdown as **OneGrade (built-in)** — you can use them directly
at any mix, on any machine the plugin is installed on. Six looks ship, each deliberately
distinct so one of the defaults is likely close to what your footage wants (pick it,
then trim with **LUT Mix** and the sliders):

| Built-in look | Character |
|---|---|
| **Cinematic Landscape** | creamy outdoor: lifted soft shadows, smooth shoulder, enriched greens |
| **Desert Day** | pale mid-day scenery → warm pop, deeper teal skies, rich ground oranges |
| **Golden Hour** | amber low-sun glow: strong warmth, soft shoulder, rich golds, calmed skies |
| **Teal Orange** | the blockbuster split — teal shadows vs. warm highlights/skin, punchy contrast |
| **Silver Bleach** | skip-bleach: heavily muted color, strong contrast, silvery and gritty |
| **Midnight Blue** | cool low-key mood: blue-cast shadows, muted warms, blues kept alive |

## Creating built-in looks

The shipped looks are generated, not hand-painted: each one is a small set of tunable
numbers in [luts/generate_luts.py](luts/generate_luts.py). The full authoring process —
design constraints, every parameter, and the step-by-step for adding a new look and
preset — is documented in [docs/CREATING-LUTS.md](docs/CREATING-LUTS.md).

**1 · Input Transform**
- **Camera** — how the clip is decoded into the working space. The default,
  **Rec.2100 PQ - Smooth Decode**, is not a camera match: it's a deliberately compressive
  *smooth decode* that flatters log footage (near-perfect highlight rolloff, smooth color, rich
  texture) — the happy path the presets build on. For a colorimetric transform instead,
  pick the real camera: Blackmagic Gen 5 Film (Pocket 4K/6K, URSA, Pyxis), DaVinci Wide
  Gamut / Intermediate, Sony S-Log3, ARRI LogC3/LogC4, Canon Log3, RED Log3G10, DJI D-Log,
  Fuji F-Log2, Panasonic V-Log, or **Rec.2100 HLG / PQ** for genuine HDR clips.

**2 · Balance** — white balance, in linear. *Open the Vectorscope while adjusting.*
- **Offset Temp / Tint** — additive; shifts every tone's chroma **evenly**. Best for a
  stubborn cast across the whole image.
- **Gain Temp / Tint** — multiplicative; keeps **highlights neutral**.
- Use whichever suits the shot (or both).

**3 · Density**
- **Density** — color density via an HSV saturation gain (the "green channel of Gain in
  HSV" trick). Deepens saturated colors. −1 = grayscale, +1 = double saturation.

**4 · Exposure (Lift / Gamma / Gain)**
- Classic wheels behavior on the master: **Gain** pivots black, **Lift** pivots white,
  **Gamma** pivots both. Matches Resolve's primary wheels.

**5 · Output**
- **Output Encode** — the curve baked into the render, i.e. your **delivery** target.
  Leave it on **Rec.709 (Gamma 2.2)** (the default — what web/streaming delivery like
  YouTube assumes); use **Rec.709 (Gamma 2.4)** for broadcast/reference delivery, or
  **Rec.709 (Scene)** for a scene-referred hand-off. (Also: Cineon Log, DaVinci Wide
  Gamut / Intermediate, Linear.) The Lift/Gamma/Gain wheels grade in whichever Rec.709
  curve you pick, so a wheel move reads linearly in that curve.
  **An active LUT takes this control over** and greys it out — a LUT can only be fed the
  curve it was authored for (Film Look → Cineon, Custom Look → Rec.709 Scene). The
  *In effect* line underneath always names what is actually being rendered. LUT Mix does
  not hand it back (see below); set LUT Mode to None for that.
  **Do not change this to match Timeline Color Space** — on macOS the timeline is set to
  Rec.709 (Scene) for viewer-matching reasons that have nothing to do with the encode
  (see [Project setup](#macos-what-you-see-vs-what-you-deliver)). How gamma works, end to
  end: [docs/GAMMA.md](docs/GAMMA.md).

**6 · Look / Film LUT** — a LUT applied inside the node. The two paths are mutually
exclusive (they use different transforms):
- **Film Look** → set **LUT Mode = Film Look**, pick from **Film Look LUT** (Resolve's
  built-in print emulations: Kodak 2383, Fuji 3513DI…). Output auto-switches to Cineon.
- **Custom Look** → set **LUT Mode = Custom Look**, choose a **Look LUT Group** then a
  **Look LUT** (any `.cube` from Resolve's LUT folder). Output switches to Rec.709 (Scene).
  Note this path always feeds the LUT **Rec.709 (Scene)**, which is what OneGrade's own
  built-in looks are authored for. A third-party `.cube` expecting Rec.709 Gamma 2.4 or a
  log input will render, but not quite as its author intended.
- **LUT Mix** — strength / output level, like Key Output (0 = off, 1 = full). Mix blends
  the LUT in and out *inside the LUT's own encode*, so a selected LUT still owns Output
  Encode at Mix 0 — what you see at 0 is the curve the blend happens in. (Tying the encode
  to Mix instead would put a contrast cliff between 0.000 and 0.001.)

**7 · Trim (after LUT)** — *finishing touches; most grades need nothing here.*
- **Exposure Trim / Contrast** — small final adjustments applied *after* the LUT. Film
  emulations darken the image by design; raise **Exposure Trim** to bring it back. This is
  **not** the exposure control — set exposure with **Gain** in group 4, which works in the
  grade curve. The slider spans ±1 stop because that is the intended range.
- **Highlight Rolloff** — per-channel soft clip so lamps and speculars roll off to white
  instead of clipping into a flat "neon" patch. Higher = earlier, stronger shoulder.
  Only engages on display-referred output (Rec.709 encodes or any LUT path) — never on
  Cineon / DI / Linear feeds to downstream nodes.

**Bypass** — Balance, Density, Exposure, Look/Film LUT and Trim each carry a **Bypass**
checkbox, so a stage can be auditioned in and out in one click. The sliders keep their
values while muted, so switching back restores the grade exactly. Bypassing the LUT also
hands **Output Encode** back to you, since a selected LUT otherwise pins it.

**Export LUT** — bakes the entire node (camera transform, balance, density, grade, output
encode, any LUT, trim) into a single `.cube` at 17/33/65³, honouring Node Role and any
Bypass. This is how you archive or hand on a project **without** needing OneGrade
installed to open it correctly.

> **Accuracy.** The bake is exact on lattice points. Between them it is as good as the
> pipeline is smooth, and ours isn't everywhere: the output encode hard-clips out-of-gamut
> channels to zero, and no lattice can follow a step. In practice it matches the node
> through the normal tonal range (~4/255 on the grey axis at 33³, median error 0 across the
> whole cube) and **can differ on blown, saturated highlights**, where mildly tinted bright
> colour reached ~150/255 in testing. 65³ roughly halves that — it's the default here for
> that reason — but can't remove it. Treat it as an excellent stand-in, not a bit-exact one.

## Workflows

- **Clean Rec.709 grade** — set Camera, balance on the vectorscope, set Density and
  Exposure, done.
- **Film emulation** — grade first, then LUT Mode → Film Look → pick a stock, then
  **Trim → Exposure** to taste.
- **Custom look** — LUT Mode → Custom Look → pick your `.cube`, dial **LUT Mix**.
- **HDR clip (e.g. DJI drone)** — set Camera = Rec.2100 HLG (or PQ), then trim exposure.
- **Grouped timeline (pre-clip / post-clip)** — for shot-per-minute work where a whole
  group should share one transform and one look:
  1. Color page → select the clips → right-click → *Add into a New Group*.
  2. Node Editor mode selector → **Group Pre-Clip**. Add OneGrade,
     **Node Role → Input Transform**, set **Camera**.
  3. Mode selector → **Clip**. Grade shots normally — curves, primaries, secondaries,
     windows, panel. OneGrade isn't involved at this level.
  4. Mode selector → **Group Post-Clip**. Add OneGrade,
     **Node Role → Output Transform**, pick a Preset or set **Output Encode**.

  Change the delivery curve or the look once and it applies to every clip in the group.
  Costs two GPU passes per clip instead of one. Details: [docs/GROUPS.md](docs/GROUPS.md).

---

# Part 2 — For Developers & Agents

## What it is

A standard **OpenFX image effect** (`OFX::ImageEffect`) built on the OpenFX 1.4 C++
Support library (vendored). One node, float RGBA, GPU render with a CPU fallback. All
color math lives in **one header** used by the CPU path; the three GPU kernels mirror it.

## Layout

```
src/
  OneGrade.cpp        OFX plugin: params, grouped UI, render dispatch, LUT scan
  OneGrade.h          factory declaration
  OneGradePipeline.h  the color pipeline — SINGLE SOURCE OF TRUTH (CPU path)
  CubeLUT.h             minimal .cube 3D-LUT parser (host side)
  MetalKernel.mm        Metal kernel      (mirrors OneGradePipeline.h)
  OpenCLKernel.cpp      OpenCL kernel     (mirrors OneGradePipeline.h)
  CudaKernel.cu         CUDA kernel       (mirrors OneGradePipeline.h)
  Info.plist            bundle plist
Makefile                macOS/Linux build
CMakeLists.txt          cross-platform build (macOS/Windows/Linux)
test/pipeline_test.cpp  CPU unit tests for the color math
third_party/openfx/     vendored OpenFX 1.4 SDK
```

## The color pipeline (order and spaces)

Per pixel, in `og::process()` (`OneGradePipeline.h`). The **space each step runs in is
deliberate** — this is where most of the correctness lives:

| # | Step | Space | Why |
|---|------|-------|-----|
| 1 | camera decode | camera log → scene-linear | per-camera `decode_log()` |
| 2 | gamut | camera → XYZ → **DaVinci Wide Gamut linear** | wide working space, like the reference node tree |
| 3 | **Balance** | linear (DWG) | gain = multiply, offset = additive; even vs. highlight-weighted |
| 4 | **Density** | **DI-log** HSV | saturating in log enriches highlights instead of blowing them out |
| 5 | gamut out | DWG → Rec.709 linear (or keep DWG for DI/Linear) | output primaries |
| 6 | **Lift/Gamma/Gain** | **Rec.709 display curve** — Scene OETF *or* pure 2.2/2.4, **follows the output encode** | matches Resolve's timeline wheels; blacks stay pinned; lift clamped at white so superwhites aren't amplified |
| 7 | output encode | Rec.709 Scene / Rec.709 Gamma 2.2 / Rec.709 Gamma 2.4 / Cineon / DI / linear | `encode()` |
| 8 | **LUT + mix** | output space | trilinear 3D-LUT sample, then lerp by mix (done in the processor / kernels) |
| 9 | **Trim** | output (display) space | post-LUT exposure (stops) + contrast about 0.5 + per-channel highlight roll-off (display-referred only) |

Parameter vector `P[13]` = `{temp, tint, density, lift, gamma, gain, offTemp, offTint,
postExp, postCon, rawExp, rawTemp, rolloff}`; `camera` and `outEncode` are passed
separately as ints. `postExp` / `postCon` / `rolloff` are applied by the caller in the
trim step (after the LUT), not inside `og::process()`.

## Golden rule: the CPU header is the source of truth

`OneGradePipeline.h` is authoritative. The Metal/OpenCL/CUDA kernels **must mirror it
exactly**. Any change to the math is a **4-file change** (CPU + 3 kernels). Keep the
helper names and formulas identical so they're easy to diff.

## How to extend

**Add a camera** (e.g. a new log format):
1. `OneGradePipeline.h` → add a branch to `decode_log()` (log→linear) and, if the
   gamut isn't already covered, a matrix branch in `to_XYZ()`.
2. Mirror both in `MetalKernel.mm`, `OpenCLKernel.cpp`, `CudaKernel.cu` (same `cam`
   index).
3. `OneGrade.cpp` → `cam->appendOption("…")` in the same order.

**Add an output encode:** add a branch to `encode()` (CPU) + `og_enc()` (3 kernels), and
an `enc->appendOption("…")`. If it changes the grade space, update the LGG accordingly.

**Add a control:** bump `kParamCount`, define the param in `describeInContext`, fetch it,
push into `params[]`, and read `P[n]` in `og::process()` + the 3 kernels (mind the Metal
`setBytes` length, the OpenCL arg list/count, and the CUDA `cudaMalloc` size).

**LUTs:** `CubeLUT.h` parses 3D `.cube` files; the host scans Resolve's LUT folder
(`filmLutDir()`, per-platform) into a Film list and a grouped Look cascade, loads the selected `.cube`
(cached by path), and passes `(data, size, mix)` to the processor. Sampling is trilinear
in `apply_lut()` / `og_sampleLUT()`.

## Build from source

```bash
# macOS (universal arm64+x86_64) / Linux
make                 # -> OneGrade.ofx.bundle
sudo make install         # -> /Library/OFX/Plugins   (needs sudo)

# any platform, via CMake
cmake -S . -B build-cmake
cmake --build build-cmake --config Release
```

### Windows

Needs Visual Studio 2022 (or its Build Tools) and a **CUDA 13.x** toolkit. The CUDA
toolkit also supplies OpenCL, so vcpkg isn't required. If `cmake` isn't on your PATH, use
the copy bundled with Visual Studio, as below.

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build-cuda -G "Visual Studio 17 2022" -A x64 -DBUILD_CUDA=ON
& $cmake --build build-cuda --config Release      # -> build-cuda\OneGrade.ofx.bundle
```

Install (**close Resolve first** — the plugin is locked while it runs):

```powershell
Copy-Item install\install-windows.bat build-cuda\
```

then right-click `build-cuda\install-windows.bat` → **Run as administrator**. It copies the
bundle to `%CommonProgramFiles%\OFX\Plugins`. Restart Resolve → Color page → Effects →
OpenFX → OneGrade.

Confirm the GPU kernel actually shipped — a plugin built without it looks fine and just
renders on the CPU:

```powershell
& "$env:CUDA_PATH\bin\cuobjdump.exe" --list-elf "$env:CommonProgramFiles\OFX\Plugins\OneGrade.ofx.bundle\Contents\Win64\OneGrade.ofx"
```

Expect a list of `sm_*` cubins covering your GPU (`sm_120` for Blackwell / RTX 50-series).
`does not contain device code` means CUDA was left out and Resolve will fall back to the
CPU.

**GPU backends:** Metal + CPU are validated on Apple Silicon; CUDA is validated on an
RTX 5090. OpenCL — the path AMD and Intel GPUs use — has been checked against the CPU
pipeline on both NVIDIA and AMD hardware (worst deviation well under one 8-bit code
value), but not yet inside Resolve on an AMD card.

CUDA needs `-DBUILD_CUDA=ON` **and** a CUDA 13.x toolkit (12.x emits no Blackwell/sm_120
code). Windows CI ships it on by default. Without it the plugin still loads and renders —
Resolve just silently falls back to the CPU, which on an NVIDIA card is dramatically
slower, so build the Windows plugin with CUDA unless you have a reason not to.

## Tests

`test/pipeline_test.cpp` compiles `OneGradePipeline.h` on the CPU and asserts pipeline
invariants (neutral pass-through sanity, Lift pins white, Gain pins black, Gamma pins
both, LUT identity, HDR decodes finite). Run locally:

```bash
make test            # or: cmake --build build-cmake --target pipeline_test && ./build-cmake/pipeline_test
```

CI (GitHub Actions) builds the bundle and runs these tests on macOS and Windows for every
push, and attaches release artifacts on tags.

## Cutting a release

Releases are **git-tag driven** — pushing a `v*` tag is the only trigger. There is no
manual upload step.

**When to run it:** after your changes are merged to `main`. CI runs on the *PR* into
`main`, not on the merge commit, so "known-good `main`" means that PR's checks were green
before you merged it.

**How to run it:**

```bash

git checkout main && git pull          # be on the merged, green main
git tag v1.1.0                         # semantic version, must start with "v"
git push origin v1.1.0                 # this push is what triggers the release
```

**What it does** (`.github/workflows/ci.yml`, the `release` job — it's skipped on normal
pushes and only runs for `refs/tags/v*`):

1. Builds and **tests** on macOS **and** Windows (the same `build` matrix every PR runs).
2. Packages a zip per OS — each contains `OneGrade.ofx.bundle` **plus its installer**
   (`install-macos.command` / `install-windows.bat`).
3. Publishes a **GitHub Release** named for the tag, attaches both zips, and
   auto-generates release notes from the merged commits.

So the whole flow is: **merge → green `main` → tag `vX.Y.Z` → push tag → Release appears
under [Releases](../../releases)** with ready-to-install downloads.

**Versioning:** use [SemVer](https://semver.org) — `vMAJOR.MINOR.PATCH`. The plugin's own
internal version is `kPluginVersionMajor` / `kPluginVersionMinor` in `src/OneGrade.cpp`;
bump it to match when you cut a release so Resolve reports the same number. If a tag was
wrong, delete it (`git push origin :refs/tags/vX.Y.Z`) and re-tag.

## License

OneGrade is **free software** under the **GNU General Public License v3.0 or later** —
see [LICENSE](LICENSE). Copyright © 2026 Matthew Grdinic.

**What that means in practice:**

- **Use it for anything, including paid work.** Grade client jobs with it, deploy it across
  a facility, ship the render. The GPL puts no restriction on the *output* of the software —
  your grades and your deliverables are yours, with no obligation of any kind.
- **Fork it, patch it, contribute back.** Pull requests welcome.
- **If you redistribute it — modified or not — you must ship the complete source under the
  GPL too.** That's the whole point: nobody can take OneGrade, reskin it, and sell it as a
  closed product, because their customers would be entitled to the source and free to pass
  it on.

**A note on the history:** versions up to and including **v1.2.0** were published under
BSD-3-Clause. That grant is perpetual and cannot be withdrawn, so anyone who obtained the
code under those terms keeps them for those versions. The GPL applies from **v1.3.0**
onward.

**Third-party code and the bundled model** — all GPL-compatible, each keeping its own
licence, none relicensed by this project. Full detail in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), which is also copied into the installed
bundle, since that is what most people actually receive:

| component | licence | what it is |
|---|---|---|
| [OpenFX SDK](third_party/openfx/LICENSE.md) | BSD-3-Clause | the plugin API and support library |
| [ncnn](third_party/ncnn/LICENSE.txt) | BSD-3-Clause | the neural-network runtime Magic Grade's model runs on |
| [PP-MobileSeg-Base](models/README.md) | Apache-2.0 | the region model itself — 12 MB of weights |

The model was **chosen on licence and turned out to be better anyway.** The obvious
alternative, NVIDIA's SegFormer, restricts use to "research or evaluation purposes only", and
that restriction lands on *the user*, not on this project — grading a paid job is neither
research nor evaluation, so shipping it would have quietly made OneGrade unusable for the work
most of its users do. PP-MobileSeg is Apache-2.0 with no such restriction, and measured faster
and more accurate besides.
