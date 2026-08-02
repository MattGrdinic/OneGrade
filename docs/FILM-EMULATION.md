# How the film emulation pipeline works

The Cinematic Film Emulation presets are OneGrade's "shot on film" path: the grade is
prepared the way a film scan would be, pushed through one of Resolve's print-film LUTs,
and trimmed back to level afterward. This doc explains the chain and why each stage is
shaped the way it is.

## 1. What a print-film LUT actually is

Resolve ships "Film Looks" LUTs (Kodak 2383, Fujifilm 3513DI, …) that model a **print
stock**: the color and contrast a projected release print adds at the end of the
photochemical chain. Crucially, they are built to receive **Cineon log** — the classic
scan encoding of camera *negative* — and emit a display-referred Rec.709 picture. Feed
them anything else (display gamma, linear) and the curve lands on the wrong part of the
S-curve: crushed mids, wrong colors.

Cineon is a printing-density log format: 10-bit code values where mid-gray sits around
code 445 and diffuse white around 685. OneGrade's encode is the standard mapping,
normalized to 0–1:

```c
code = 685 + 300 * log10(x);      // x = linear, floored at 1e-4
out  = clamp(code / 1023, 0, 1);
```

(`encode()` index 3 / `og_enc()` — see `docs/GAMMA.md` for where encodes sit in the
pixel path.)

## 2. The chain, end to end

With `LUT Mode = Film Look` the node renders, per pixel:

```
camera decode → DWG linear → Balance → Density → DWG→Rec.709 linear
   → Lift/Gamma/Gain (Scene-OETF grade curve) → Cineon log encode
   → print-film LUT (trilinear, LUT Mix)
   → Trim: post-exposure / contrast → Highlight Rolloff
```

Key mechanics:

- **The encode is forced.** Selecting Film Look overrides Output Encode to Cineon at
  render time (`setupAndProcess()`), so the LUT always receives what it was built for —
  the user cannot mismatch them (see `docs/LUTS.md`).
- **The grade happens *before* the encode**, playing the role of the negative: Balance
  and the wheels shape what "the negative held," then the print LUT interprets it.
  Grading upstream of a print LUT behaves like exposure/timing decisions on film —
  moves ride through the print curve's shoulder and toe instead of fighting the finished
  picture.
- **Trim happens *after* the LUT.** Print stocks darken and compress by design; the
  post-LUT Exposure (stops) and Contrast (about 0.5) bring the level back without
  disturbing what the grade fed the stock. This mirrors print-light adjustments.
- **Rolloff is available, not required.** The per-channel soft clip
  (`og::softclip`) runs last for practicals that still clip; any active LUT counts as
  display-referred output, so the control stays live on this path.

## 3. The preset recipe

Both Film Emulation presets apply the same user-validated recipe
(`applyPreset()` in [src/OneGrade.cpp](../src/OneGrade.cpp)) — they differ only in
the stock they select:

| Param | Value | Role in the look |
|---|---|---|
| Camera | Rec.2100 PQ | the "smooth decode" — compressive curve with built-in highlight shoulder (`docs/CAMERAS.md`) |
| Gain Temp / Tint | −0.22 / +0.09 | cool highlights against warm practicals — a look ingredient, not correction |
| Offset Temp / Tint | −0.02 / +0.01 | slight even cool bias |
| Density | +0.10 | richer color into the print (`docs/DENSITY.md`) |
| Lift | +0.11 | shadows lifted off video-black before the print curve re-compresses them |
| Gain | 0.80 | pulled hard so highlights land on the stock's shoulder instead of past it |
| LUT Mode / Mix | Film Look / 1.0 | the print stock itself |
| Post-Exposure | +0.55 | brightness brought back *after* the darkening print LUT |
| Post-Contrast | 1.0 | neutral |
| Rolloff | 0 | PQ decode already provides the shoulder |

The stock is found by name, not index: `filmLutIndex("kodak 2383 d60")` /
`filmLutIndex("fujifilm 3513di d60")` — case-insensitive fragment match over the
scanned Film Looks list, preferring a Rec.709 variant (the film path outputs Rec.709).
The Fuji fragment names the exact D60 stock because Resolve ships it only as DCI-P3
variants (D55/D60/D65) and the tie-break must not drift. If the requested stock is
missing from the machine, the preset falls back to Kodak 2383; if that's missing too,
index 0.

Everything stays a starting point: presets only stamp values (guarded to real user
edits, so project loads don't re-stamp), and every slider remains live. Swapping stocks
afterward is just the Film Look LUT dropdown — the recipe holds.

## 4. Why this beats "LUT on a node"

Dropping 2383 on a plain node makes the LUT interpret whatever happens to be on the
timeline. This path guarantees the three things a print emulation needs: a *known input
encoding* (Cineon, forced), a *grade upstream* of the stock (the negative), and *level
control downstream* of it (the print light). That's the photochemical order, which is
why the result reads as "shot on film" rather than "filter applied."
