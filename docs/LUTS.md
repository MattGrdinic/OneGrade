# How the LUT integration works

OneGrade applies one 3D LUT inside the node, after the output encode. This doc covers
the two LUT paths and their encode coupling, how LUTs are discovered and parsed, how
sampling and mixing work, and where the built-in LUTs come from. (For *authoring* new
built-in looks, see `docs/CREATING-LUTS.md`.)

## 1. Two paths, coupled to the encode

`LUT Mode` selects between mutually exclusive paths, because the two kinds of LUT expect
different input spaces:

| Mode | Picker | LUT expects | Encode forced at render |
|---|---|---|---|
| **Film Look** | Film Look LUT (flat list) | **Cineon log** input | 3 (Cineon Log) |
| **Custom Look** | Look LUT Group → Look LUT (cascade) | **Rec.709** input | 0 (Rec.709 Scene) |
| None | — | — | user's Output Encode used unchanged |

The coupling lives in `setupAndProcess()` ([src/OneGrade.cpp](../src/OneGrade.cpp)):
the *rendered* encode is overridden from `lutMode` every frame, so the pre-LUT encoding
and the LUT can never mismatch — picking a film stock without remembering to switch the
encode to Cineon simply cannot produce the wrong pipeline. The Output Encode dropdown
still shows the user's choice; it just isn't consulted while a LUT is active.

## 2. Discovery: where the lists come from

`scanLuts()` (host side, run once at describe time) builds two lists:

- **`s_FilmLuts`** — every `.cube` under Resolve's LUT folder, preferring the
  `Film Looks` subfolder when it exists. These are Resolve's print-film emulations
  (Kodak 2383, Fujifilm 3513DI, …). Resolve installs that folder in a different place per
  platform, so `filmLutDir()` picks it at runtime — it was hardcoded to the macOS path
  until 2026-07-16, which left Windows with an **empty Film list and no error**, so the
  Film Emulation presets silently rendered with no print LUT:
  - **Windows** — `%PROGRAMDATA%\Blackmagic Design\DaVinci Resolve\Support\LUT`
    (note the extra `Support` level that macOS doesn't have)
  - **macOS** — `/Library/Application Support/Blackmagic Design/DaVinci Resolve/LUT`
  - **Linux** — `/opt/resolve/LUT`
- **`s_LookGroups`** — a grouped cascade for the Custom path:
  1. **"OneGrade (built-in)"** is always the *first* group: the `.cube` files shipped
     inside the bundle itself (see §5).
  2. Then the whole Resolve LUT folder, grouped by top-level subfolder (files in the
     root land in a "General" group).

Scanning is recursive, case-insensitive on the `.cube` extension, permission-safe
(`skip_permission_denied`), capped at 1000 files per list, and sorted. Labels are the
filename stem; values are `(label, absolute path)` pairs.

Two fragment-match helpers sit on top for the presets: `filmLutIndex(fragment)`
(case-insensitive substring over the film list, preferring a `rec709` variant, −1 when
absent) and `findLookLut(fragment, group, lut)` across all look groups.

## 3. Parsing: CubeLUT.h

[src/CubeLUT.h](../src/CubeLUT.h) is a minimal `.cube` reader:

- Understands `LUT_3D_SIZE`, `DOMAIN_MIN`/`DOMAIN_MAX`, `TITLE`, comments, CRLF.
- **3D only** — `LUT_1D_SIZE` files are rejected.
- Data is stored as `N*N*N*3` floats with the **red index varying fastest** (the .cube
  convention), and the file is accepted only if the row count matches `N³` exactly.
- **Caching:** `load(path)` is a no-op when `path` is already loaded, so the render
  thread can call it per frame; a LUT is only re-read from disk when the selection
  changes. One `CubeLUT` instance lives on the effect (`m_Lut`).

The host resolves the active path from the mode + pickers each render, loads it, and
passes `(data pointer, size N, mix)` to the processor — the kernels never touch files.

## 4. Sampling and mixing

`apply_lut()` (CPU) / `og_sampleLUT()` (kernels) do classic **trilinear interpolation**:
the input RGB (already in the LUT's expected space — Cineon or Rec.709, both 0–1) is
clamped to [0,1], scaled to the `N−1` grid, and the surrounding 8 lattice entries are
blended per channel. Then **LUT Mix** linearly interpolates between the un-LUTted and
LUTted pixel:

```c
out = in + (lut(in) - in) * mix;     // mix 0 = LUT off, 1 = full strength
```

so Mix behaves like Key Output on a LUT node. `N < 2` or `mix <= 0` short-circuits to
identity. Because input is clamped, anything the encode left above 1.0 samples the LUT's
edge — one reason the Highlight Rolloff runs *after* the LUT and why any active LUT
counts as display-referred for its gating.

After the LUT come the Trim controls (post-exposure/contrast) and the rolloff — see the
pipeline table in the README.

## 5. Built-in LUTs: shipped inside the bundle

Six looks ship in `Contents/Resources/LUTs` of the bundle so any render machine has
them with zero external installs. `bundleLutDir()` finds them **relative to the plugin
binary's own path** — `dladdr()` on macOS/Linux, `GetModuleFileName()` on Windows —
walking up from `Contents/<arch>/OneGrade.ofx` to `Contents/Resources/LUTs`. That's
why they work wherever the bundle is copied, including network render nodes.

They're generated by `luts/generate_luts.py` (all tunables in its `LOOKS` dict), checked
into `luts/`, and copied into the bundle by both the Makefile (`bundle-luts`, glob) and
the CMake bundle step (**explicit file list — a new LUT must be added there by name**).
Authoring process and parameter reference: `docs/CREATING-LUTS.md`.
