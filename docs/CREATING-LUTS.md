# Creating built-in look LUTs

How OneGrade's shipped looks (Desert Day, Cinematic Landscape) were made, and the
process for adding more. The short version: every look is a handful of numbers in one
Python dict — edit, regenerate, rebuild, eyeball in Resolve, repeat.

## How the built-in LUTs work

- The `.cube` files live in `luts/` (checked in) and are copied into
  `OneGrade.ofx.bundle/Contents/Resources/LUTs` at build time — by the `bundle-luts`
  target in the Makefile (globs `luts/*.cube`) and by the bundle-assembly step in
  `CMakeLists.txt` (**explicit file list — new LUTs must be added there by name**).
- At runtime the plugin resolves that folder from its own binary path
  (`bundleLutDir()` in `src/OneGrade.cpp`) and lists it as the first **Look LUT
  Group**, named **OneGrade (built-in)**. Nothing is installed into Resolve's LUT
  folder; the looks travel with the plugin.
- LUTs run on the **Custom Look** path: input *and* output are display-referred
  **Rec.709 (Scene OETF)** values in [0,1]. Design for that space — no log, no linear.
- 33-point 3D LUTs, sampled trilinearly. Keep transforms smooth; hard hue boundaries
  band at 33 points (the generator feathers hue arcs by 15° for exactly this reason).

## The generator

`luts/generate_luts.py` — pure Python, no dependencies. Each look is an entry in the
`LOOKS` dict at the bottom of the file:

| Param | What it does |
|---|---|
| `warm` | white-balance bias: `+` warms (R up / B down), display-space gain |
| `toe` | lifts the deepest shadows, fading out by mid-shadow (creamy blacks) |
| `contrast` | gentle S-curve about mid (blend toward smoothstep) |
| `shoulder` | highlight soft clip — the same curve as the plugin's Highlight Rolloff, so looks stay consistent with that control |
| `sat` | global saturation multiplier |
| `sat_toe` | shadows below this value keep progressively less saturation (kills chroma noise in the toe, reads "creamy") |
| `hue_arcs` | list of hue-selective tweaks: `lo`/`hi` in degrees (0=red, 120=green, 240=blue; wraps), optional `sat` (added saturation, may be negative) and `shift` (hue rotation in degrees), feathered 15° per side |
| `split` | optional luminance split-tone: `dict(shadows=…, highlights=…)`, each a warm(+)/cool(−) bias applied through soft luminance masks — the teal-and-orange machinery |

Tone ops run **per channel** on purpose: bright saturated sources drift toward white
instead of holding full chroma — the film-like behavior we want everywhere.

## Adding a new look

1. Add an entry to `LOOKS` in `luts/generate_luts.py`. Name it `OneGrade <Name>` —
   the file stem is both the dropdown label and what presets search for.
2. Regenerate and sanity-probe before ever opening Resolve:
   ```bash
   python3 luts/generate_luts.py
   ```
   Probe key colors numerically (black stays pinned unless `toe` says otherwise,
   mid-gray shifts only as intended, white lands where the shoulder puts it):
   ```bash
   python3 -c "
   import sys; sys.path.insert(0,'luts')
   from generate_luts import apply_look, LOOKS
   p = LOOKS['OneGrade <Name>']
   for probe in [(0,0,0),(0.5,0.5,0.5),(1,1,1),(0.3,0.5,0.2),(0.5,0.6,0.8)]:
       print(probe, '->', tuple(round(c,3) for c in apply_look(*probe, p)))"
   ```
3. **Add the new filename to the CMake copy list** in `CMakeLists.txt` (the Makefile
   picks it up automatically via glob; CMake will not).
4. `make && make test`, install the bundle, restart Resolve, and delete the render
   cache (Playback → Delete Render Cache) so stale frames don't lie to you.
5. Eyeball on real footage via **LUT Mode = Custom Look → OneGrade (built-in)**.
   Iterate: tweak the dict, re-run the generator, `make`, reinstall. Screenshot
   comparisons against a reference grade are the fastest way to converge (that's how
   Desert Day and Cinematic Landscape were tuned).
6. Optional preset: in `src/OneGrade.cpp` add a `preset->appendOption(...)` entry and
   an `applyPreset()` branch that locates the LUT with
   `findLookLut("onegrade <name>", gi, li)` — see the Desert Day branch for the
   pattern, including the LUT-free fallback. Preset option order defines the stored
   index, so append rather than reorder.
7. Commit the generator change **and** the regenerated `.cube` files (they're checked
   in, ~1 MB each), on a feature branch.

## History

Desert Day and Cinematic Landscape were authored numerically with this exact loop and
validated on footage first try (2026-07-14). They replaced an earlier dependency on a
third-party downloadable LUT — asking users to install LUTs separately was judged too
much friction; built-ins travel with the plugin.
