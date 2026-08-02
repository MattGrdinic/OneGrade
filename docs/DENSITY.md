# How the Density control works

Density is OneGrade's one-slider answer to "make the colors richer without making the
picture brighter or shifting hue." It is a **saturation gain applied in HSV, computed in
DaVinci Intermediate log** — and the log part is the whole trick.

## 1. What "density" means

The name comes from film: on a print, more dye density means deeper, more saturated
color at the same luminance — the look people describe as "rich" or "dense." A plain
saturation knob in video tends to do something subtly different: it pushes colors toward
the gamut edge and, in linear light, drags bright saturated areas toward clipping. The
goal of this control is film-style depth: saturation that *enriches* highlights instead
of blowing them out.

The approach is the classic colorist trick sometimes described as "raise the green
channel of Gain in HSV mode" — generalized here to a direct saturation multiply in HSV
space.

## 2. The math

Step 4 of `og::process()` in
[src/OneGradePipeline.h](../src/OneGradePipeline.h) (mirrored in the three kernels):

```c
if (density != 0.0f) {
    float l0 = di_encode(w[0]), l1 = di_encode(w[1]), l2 = di_encode(w[2]);
    float h, s, v;  rgb2hsv(l0, l1, l2, h, s, v);
    s = clamp01(s * (1.0f + density));
    hsv2rgb(h, s, v, l0, l1, l2);
    w[0] = di_decode(l0);  w[1] = di_decode(l1);  w[2] = di_decode(l2);
}
```

Reading it inside-out:

1. **Encode to DI-log.** The working pixel is DaVinci Wide Gamut *linear* at this point;
   `di_encode()` takes each channel into DaVinci Intermediate log.
2. **Convert to HSV.** `rgb2hsv()` uses the hexcone model: `V = max(R,G,B)`,
   `S = (max−min)/max`. Because V is the max channel, scaling S never changes V — the
   brightest channel is left alone and the *other* channels move toward or away from it.
3. **Scale saturation.** `s * (1 + density)`, clamped to [0,1]. The slider runs −1…+1:
   −1 zeroes S (grayscale), 0 is identity, +1 doubles it.
4. **Back out.** HSV → RGB → `di_decode()` → linear DWG, and the pipeline continues.

Hue is passed through untouched, so colors deepen along their own hue vector — nothing
drifts toward a different color, it just gets more or less of what it already was.

## 3. Why log and not linear (a real bug we fixed)

The operation originally ran in linear, and it looked wrong: saturated highlights —
neon, taillights, warm practicals — blew out. In linear light the max channel of a
bright saturated red is already huge; pulling the green/blue channels further *down*
relative to it (which is what a saturation gain does when V = max) makes the displayed
result poorer and harsher, and any subsequent encode slams the dominant channel into
clip while the others crater.

In DI-log the channel values are compressed into a perceptually even scale before the
HSV math happens. The same relative saturation push now moves the subordinate channels
by *perceptual* steps rather than linear-light steps, so a bright red gets **richer** —
deeper, still bright, still red — instead of tearing toward clipped neon. This is the
same reason Resolve's own wheels operate in a log-like space rather than linear.

## 4. Where it sits in the pipeline (and why)

Density runs **after Balance, before the output primaries conversion**:

- After Balance so you neutralize/stylize the white point first and the density push
  works on the corrected color.
- Before the DWG → Rec.709 conversion and the grade curve, so the enrichment happens in
  the wide working gamut where saturated values still have headroom, not after they've
  been squeezed into output primaries.

Neutral value: `density = 0` skips the block entirely (exact identity — no HSV
round-trip error).
