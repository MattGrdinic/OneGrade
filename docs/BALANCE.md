# How the color balance controls work

OneGrade has three layers of white/color balance, applied at three different depths of
the pipeline — from "as close to the sensor as we can get" to "even tint across every
tone." They are deliberately different math, because casts have different causes.

| Control | Where it acts | Math | Best for |
|---|---|---|---|
| **Scene White Balance** (Kelvin) | XYZ, right after camera decode | Bradford chromatic adaptation | true white-balance errors (wrong WB at the shoot) |
| **Gain Temp / Gain Tint** | DWG linear, multiplicative | per-channel multiply | casts that live in the highlights; neutralizing lights |
| **Offset Temp / Offset Tint** | DWG linear, additive | per-channel add | stubborn casts that sit evenly across all tones |

All of this is steps 0 and 3 of `og::process()` in
[src/OneGradePipeline.h](../src/OneGradePipeline.h), mirrored in the kernels.

## 1. Scene White Balance — physically real white balance

This mirrors the Camera RAW tab's Temp control so that tab can stay untouched. It runs
in **CIE XYZ immediately after the camera decode** — the closest the plugin can get to
the sensor — and is a real chromatic adaptation, not an RGB slider:

1. `cct_to_xy()` converts the chosen Kelvin temperature to a chromaticity on the
   **Planckian (blackbody) locus** using the Kim et al. 2002 polynomial fit.
2. `white_balance()` treats the scene as if it were lit by that blackbody and adapts it
   to the D65 working white with a **Bradford transform**: XYZ → cone-ish LMS via the
   Bradford matrix, von Kries per-channel scale by (destination white ÷ source white),
   LMS → XYZ back.

Semantics match Resolve's knob: **raising** T tells the pipeline "the light was bluer
than assumed," so the image is *warmed*; lowering T cools it. 6500 K is exact identity
(the code short-circuits within ±1 K). It is not byte-identical to the RAW tab — no
sensor metadata reaches an OFX plugin — but it is the same physics.

## 2. Gain balance — multiplicative, pivots highlights neutral

In DWG linear, after the CST:

```c
w[0] *= (1.0f + temp*0.20f);    // R up  = warmer
w[2] *= (1.0f - temp*0.20f);    // B down
w[1] *= (1.0f + tint*0.20f);    // G up  = greener, down = magenta
```

A multiply scales a channel *in proportion to how much of it there is* — bright pixels
move a lot, near-blacks barely move. That's the behavior of the Gain wheel: it lets you
make **highlights neutral** (or deliberately cool/warm) without dragging the shadows
around. Temp works the R/B axis in opposition; Tint works G against the magenta that
R+B leave behind. The 0.20 factor maps the ±1 slider range to a usable grading range.

## 3. Offset balance — additive, even across all tones

Same place, different operator:

```c
w[0] += offTemp*0.10f;    // warm/cool
w[2] -= offTemp*0.10f;
w[1] += offTint*0.10f;    // green/magenta
```

An add moves **every pixel by the same absolute amount** — black, mid-gray, and white
all shift identically. That's the Offset wheel, and it's the right tool for casts that
don't scale with brightness: a lifted blue in the blacks, a uniform tint from a filter
or a stubborn light source. (Because it moves black off zero, big offsets visibly tint
the shadows — that's the point.)

## 4. Why in linear, and why this order

Both Balance stages run in **scene-linear DWG** — a multiply in linear is physically
"more/less light of this primary," which is what changing illumination actually does, so
corrections track across the tonal range the way real light does. Balance runs *before*
Density and the grade wheels so that everything downstream operates on
already-neutralized color.

Working practice (also in the plugin's Balance tip): keep the **vectorscope** open.
Offset centers the whole blob; Gain centers the highlight end. Neutral values: sliders
at 0 are exact identity.

Note that the Film Emulation presets deliberately use Balance as a *look* ingredient
(cool gain-balance against warm practicals), not just as correction — see
`docs/FILM-EMULATION.md`.
