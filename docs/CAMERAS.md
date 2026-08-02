# How the camera input transforms work

The Camera dropdown is OneGrade's built-in CST: it takes the clip from its native
log/gamut into the plugin's working space, replacing the Color Space Transform node (or
color management) you'd otherwise need in front of the grade. This doc explains the two
halves of that transform and the working space it lands in.

## 1. The two halves: curve, then gamut

Every camera entry is a pair applied in `og::process()` steps 1–2
([src/OneGradePipeline.h](../src/OneGradePipeline.h)):

1. **Transfer decode — `decode_log(cam, x)`.** Inverts the camera's log curve (or HDR
   EOTF) per channel, producing **scene-linear** values normalized so 18% mid-gray sits
   at 0.18 and diffuse white near 1.0, with highlights extending far above 1.0. Each
   branch is the published inverse function with the manufacturer's constants (e.g.
   Blackmagic Gen 5 Film from the Gen 5 Color Science white paper: exponential segment
   above the linear toe junction at code 0.1339).
2. **Gamut matrix — `to_XYZ(cam, v)`.** A 3×3 matrix from the camera's native primaries
   to CIE XYZ (D65). Then `XYZ_to_DWG()` takes XYZ into the working space. Two matrices
   rather than one camera→DWG matrix so the RAW white balance can act in XYZ between
   them (see `docs/BALANCE.md`).

RAW Exposure (a linear multiply in stops) is applied to the scene-linear values right
after the curve decode, *before* the gamut matrix — the same place the Camera RAW tab's
Exposure acts, which is why it matches that control nearly exactly.

## 2. The camera list

| Index | Camera | Curve | Gamut matrix |
|---|---|---|---|
| 0 | Blackmagic Gen 5 Film | Gen 5 Film log (white paper) | BMD Wide Gamut Gen 4/5 |
| 1 | DaVinci Wide Gamut / Intermediate | DaVinci Intermediate | DaVinci Wide Gamut |
| 2 | Sony S-Log3 | S-Log3 | S-Gamut3 |
| 3 | ARRI LogC3 (EI800) | LogC3 | ARRI Wide Gamut 3 |
| 4 | ARRI LogC4 | LogC4 | ARRI Wide Gamut 4 |
| 5 | Canon Log3 | Canon Log 3 | Rec.2020 stand-in |
| 6 | RED Log3G10 | Log3G10 | REDWideGamutRGB |
| 7 | DJI D-Log | D-Log | Rec.2020 stand-in |
| 8 | Fuji F-Log2 | F-Log2 | Rec.2020 stand-in |
| 9 | Panasonic V-Log | V-Log | V-Gamut |
| 10 | Rec.2100 HLG | HLG inverse OETF | Rec.2020 |
| 11 | **Rec.2100 PQ / ST.2084** — default | PQ inverse EOTF | Rec.2020 |

Notes:

- **Stand-ins:** Canon, DJI, and Fuji currently use the Rec.2020 matrix as a
  reasonable wide-gamut approximation of their native gamuts; matrices other than
  Blackmagic's are published/approximate values pending on-footage validation.
- **Blackmagic footage:** Pocket/URSA/Pyxis clips in a DaVinci YRGB project are
  **Gen 5 Film** (index 0) — that's the colorimetric match. DWG/DI (index 1) is only
  correct for material already rendered into DaVinci Wide Gamut / Intermediate.

## 3. The HDR entries and the "smooth decode"

HLG and PQ are display EOTFs, not camera curves, so their decodes are **normalized to
reference white** rather than mid-gray: HLG's inverse OETF is scaled by 3.774 (75% HLG
signal → ~1.0) and PQ's inverse EOTF by 49.26 (203-nit reference white → ~1.0). Applied
to genuine HDR material this is a normalize, not a tone-map — highlights above reference
white can still clip downstream.

The PQ entry doubles as the plugin's **default and the presets' base**, used
deliberately "wrong": feeding *log camera footage* through the PQ inverse EOTF is not a
colorimetric decode, it's a strongly **compressive** curve that lands log material with
a near-perfect built-in highlight rolloff and smooth color. Treat index 11 as a creative
decode ("smooth decode") and index 0–9 as the measurement-faithful ones.

## 4. The working space: DaVinci Wide Gamut linear

Everything after the CST — balance, density, the grade — operates in **DWG linear**:

- **Wide**: DWG contains every camera gamut in the list, so no camera's color is
  clipped by the working space itself.
- **Linear**: physically meaningful multiplies/adds for balance (see
  `docs/BALANCE.md`); the steps that *want* log (Density, the grade curve) encode into
  their own space and come back.
- It's also what a reference Resolve node tree would use (DWG/Intermediate), which keeps
  the plugin's behavior comparable to a hand-built CST → grade → CST chain.

On output, `enc <= 3` (the Rec.709 encodes and Cineon) converts DWG → XYZ → Rec.709
linear before the grade curve and encode; DaVinci Intermediate and Linear outputs stay
in DWG primaries (see `docs/GAMMA.md`).

## 5. Adding a camera

1. `OneGradePipeline.h` — a branch in `decode_log()` (published inverse curve) and,
   if the gamut isn't covered, a matrix branch in `to_XYZ()` (camera → XYZ D65).
2. Mirror both in `MetalKernel.mm`, `OpenCLKernel.cpp`, `CudaKernel.cu` — same index.
3. `OneGrade.cpp` — `cam->appendOption("…")` in the same position. Appending at the
   end avoids renumbering existing saved grades.
4. Extend the camera loop in `test/pipeline_test.cpp` and, ideally, add a mid-gray
   anchor test like the Gen 5 one.
