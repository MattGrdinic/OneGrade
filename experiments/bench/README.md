# The bench

Grades log stills offline and prints what it did. **Use this, not Resolve, for anything numeric.**

Every constant in Auto Grade and Magic Grade was fitted at roughly one observation per minute —
build, install, restart Resolve, press a button, squint — which is why so few of them have much
evidence behind them. This turns that into a second, and lets you compare five settings side by
side instead of remembering what the last one looked like.

## Running it

```bash
cd ~/Documents/Development/Resolve/OneGrade
./experiments/bench/run.sh ~/Desktop/onegrade-training
```

Grades every `.png` in that folder and writes the results to `~/Desktop/onegrade-training/out/`.
It rebuilds itself if the source changed, so there is no separate build step.

## Changing values

**Anything after the folder is a flag**, and every constant the grade uses is exposed:

```bash
./experiments/bench/run.sh ~/Desktop/onegrade-training --unit=3
./experiments/bench/run.sh ~/Desktop/onegrade-training --black=0.08 --gain-per-key=0.12
./experiments/bench/run.sh ~/Desktop/onegrade-training --sep=1.5 --wb
```

| flag | default | what it does |
|---|---|---|
| `--gain-per-key` | 0.19 | how hard a bright shot gets pulled down. **The darkness suspect.** |
| `--gain-base` | 0.80 | Gain when the shot is already at mid-gray |
| `--gain-min` / `--gain-max` | 0.30 / 0.80 | limits on the above |
| `--black` | 0.050 | where the black point is placed, measured pre-LUT |
| `--unit` | 6.0 | how far Magic Grade pushes the chosen region, in Lab units |
| `--sep` | 1.0 | the Separation slider |
| `--wb` | off | White Balance First |
| `--camera` / `--encode` | 11 / 3 | Rec.2100 PQ decode, Cineon out (what the film LUT forces) |
| `--lut=` | Kodak 2383 D60 | print stock; `run.sh` fills this in |

### Comparing two settings

Each run overwrites `out/`, so to keep both, point them somewhere different by calling `bench`
directly rather than through the wrapper:

```bash
B=./experiments/bench/bench
M=OneGrade.ofx.bundle/Contents/Resources/Model
L="/Library/Application Support/Blackmagic Design/DaVinci Resolve/LUT/Film Looks/Rec709 Kodak 2383 D60.cube"
mkdir -p /tmp/u6 /tmp/u3
$B $M /tmp/u6 ~/Desktop/onegrade-training/*.png --lut="$L" --unit=6
$B $M /tmp/u3 ~/Desktop/onegrade-training/*.png --lut="$L" --unit=3
```

## Reading the table

```
frame                       key   gain   lift   roll    blk    mid  decision
Still …_220447_1.22.1.png  +1.32  0.800 +0.034  0.000  0.050  0.391  1/3 BUILT 52% -> OffTmp +0.063
                                         0.049   0.00%   0.021       post-LUT blk / crushed / shadow sep
```

- **key** — how far the shot sits from mid-gray, in stops. Negative means bright.
- **gain / lift / roll** — what the solve chose. `gain` at 0.800 means it hit its ceiling; `roll`
  above 0 means the frame had clipping at the sensor.
- **blk / mid** — what the grade *achieved*, measured **pre-LUT**, which is where the solve works.
- **decision** — which subject Magic Grade picked and how far it moved. A magnitude at ±0.35 is
  the clamp binding, which means the move wanted to be bigger than it is allowed to be.
- **post-LUT blk** — the black point of the picture you actually see, after the print stock's
  toe. Always lower than `blk`; if it is near zero the stock is eating the margin the solve left.
- **crushed** — share of pixels at or below 1/255. Detail that is *gone*.
- **shadow sep** — how much output range the darkest tenth of the frame still occupies. This is
  the one that matches what "crushed" looks like: shadows can be compressed into a narrow band
  without any pixel reaching zero, which reads as crushed to the eye and scores 0% on the count.
  Under about 0.02 is worth looking at.

## Input has to be camera log

Not a graded export — log is what the plugin receives from OFX.

**The tell that an export really is log: it never reaches 1.0.** Blackmagic peaks near 0.75. If
`max` is 1.0 you have exported something already transformed, and every number here will be
wrong in a way that still looks plausible.

**Export 16-bit from the Deliver page, not a Gallery still.** This is not a nicety, and it broke
a real investigation. Log packs the whole dynamic range into the code values available, so in 8
bits the shadows get very few of them — and a dark shot gets fewer still, because it does not use
the top of the range either. The first five stills were 8-bit, and the frame being investigated
for crushed blacks spanned 0.098 to 0.412, roughly **80 code values for the entire image**, with
its darkest tenth living in about ten of them.

So the shadow detail the grade was accused of destroying **was not in the file to destroy**. The
bench reported the frame as the healthiest of the five while Resolve visibly crushed it, and both
were right about their own input.

Log stills have two tells, and the floor is the one that bites:

- **max never reaches 1.0** — Blackmagic peaks near 0.75. If it is 1.0, the export was already
  transformed and every number here is wrong in a plausible-looking way.
- **min should be near 0** — if the floor is well above zero on a dark shot, the shadows have been
  quantised away. Check before trusting anything the bench says about blacks.

```bash
python3 -c "import sys;print(open(sys.argv[1],'rb').read()[24])" frame.png   # 8 or 16
```

## Why it shares the plugin's code

`src/OneGradeCreative.h` holds the grade solve and the Magic magnitude, and both the plugin and
this call them. That is deliberate: **every bug on Magic Grade that survived more than a few
minutes was a paraphrase** of something that already existed — a neutral render standing in for a
graded one, a pre-LUT render for the real one, a Python threshold never ported, and this bench
reporting a Magic decision it never applied. All four produced plausible output while being
wrong, and all four were caught only by comparing two implementations on the same frame.

If something has to be reimplemented to be tested, extract it to a header instead.
