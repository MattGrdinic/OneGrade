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
| `--cycle` | 0 | which press. Magic Grade offers a different subject each time, and this is the only way to reproduce a grade the user reached on press two |
| `--bias-sweep` | off | walk the Bias slider from +2 to -2 and print the tone re-solve at each stop. `held` marks where the targets stop being reachable and the slider stops moving — which is what a discontinuity looks like before it becomes "the picture jumped" |
| `--bias-step` | 0.5 | how often the sweep **writes a frame**, as `<name>-biasp0150.png` / `-biasm0150.png` (p/m for sign, hundredths, so they sort in slider order). `0` prints the table and writes nothing. Use one frame at a time — at 0.25 a 4K still becomes 17 PNGs |
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

## Exporting frames from Resolve

The bench needs what the plugin gets from OFX: **camera log, ungraded, 16-bit**. That is a
Deliver-page render of a single frame, not a Gallery still.

### On the Color page

1. Park the playhead on the frame you want. Single-frame analysis is the feature, not a
   limitation — you are choosing which moment the grade optimises for.
2. **Bypass all grades** — `Shift+D`, or the bypass toggle at the top of the node graph. This
   includes OneGrade itself. You want the footage as the plugin receives it.
3. Confirm there is no CST or LUT anywhere in the node tree, and that the clip's Camera RAW tab
   is on its project default. Whatever you leave enabled gets baked in and the bench will grade
   it a second time.

### Project settings, once

- **Color Science: DaVinci YRGB** (not Color Managed). Color Managed applies its own output
  transform on render and you will get display-referred pixels, not log.
- **Color Management → Output LUT: None.** An output LUT bakes into renders.

### On the Deliver page

4. Mark **in and out on the same frame** (`I` then `O`), and set Render to **Single Clip**.
5. Format **PNG**, and set **Bit Depth to 16** — this is the one that matters, and it is not the
   default.
6. Uncheck **Export Audio**.
7. Advanced Settings → **Data Levels: Full.** On Video the export is scaled to legal range and
   the shadows are clipped before you ever see them.
8. Add to Render Queue, Render All.

### Confirm it worked

Run the bench. It checks the input and complains if the frame cannot answer the question:

```
! 8-bit, 0.137..0.725 = ~150 levels, ~15 in the darkest tenth.  Export 16-bit from Deliver
```

A good 16-bit log frame produces no such line. If a 16-bit export still does, the range itself
is narrow and something upstream — Data Levels, or a transform left enabled — has compressed it.

## Why those two settings in particular

**Log**, because that is what the plugin receives from OFX. A display-referred export gets graded
a second time, and the tell is the ceiling: real log never reaches 1.0, and Blackmagic peaks near
0.75. If `max` is 1.0, something upstream already transformed it and every number here will be
wrong in a way that still looks entirely plausible.

**16-bit**, because the floor is where the grade is being judged and 8 bits does not have one.
This is not a nicety — it broke a real investigation. Log packs the whole dynamic range into the
code values available, so 8 bits leaves the shadows very few, and a dark shot fewer still because
it does not use the top of the range either. The first five stills were 8-bit, and the frame
under investigation for crushed blacks spanned 0.098 to 0.412 — about **80 code values for the
entire image**, its darkest tenth living in roughly ten of them.

The shadow detail the grade stood accused of destroying **was not in the file to destroy**. The
bench scored that frame the healthiest of the five while Resolve visibly crushed it, and both
were right about their own input. The ceiling tell passed the whole time; nobody had thought to
check the floor. That is why the bench now reports its own input range rather than leaving it to
a habit somebody has to remember.

## Why it shares the plugin's code

`src/OneGradeCreative.h` holds the grade solve and the Magic magnitude, and both the plugin and
this call them. That is deliberate: **every bug on Magic Grade that survived more than a few
minutes was a paraphrase** of something that already existed — a neutral render standing in for a
graded one, a pre-LUT render for the real one, a Python threshold never ported, and this bench
reporting a Magic decision it never applied. All four produced plausible output while being
wrong, and all four were caught only by comparing two implementations on the same frame.

If something has to be reimplemented to be tested, extract it to a header instead.
