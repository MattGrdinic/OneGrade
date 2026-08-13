# looks — measuring reference stills, to find out what a Look is made of

Graded reference stills in; tone targets out, plus the number that decides whether the Look
feature is worth building at all.

```bash
cd experiments/looks && make
./looks ../../OneGrade.ofx.bundle/Contents/Resources/Model ~/looks/cinematic ~/looks/corporate
```

Each folder is one look and its name is the label. JPEG, PNG, WebP and TIFF all load.

---

## What it is for

The Look feature is: press **Magic Grade**, then cycle looks until one is right. Magic Grade
already finds the subject and solves three conditions on three controls — subject shadows,
subject midtone, frame highlight. A *look* is the same three conditions aimed somewhere else.

So a look is four numbers, and they already exist as arguments:

```c
solve_magic_tone_from(..., subjFloor, subjMid, frameCeiling, ..., frameFloorMax)
```

Today there is exactly one set of them — `0.125 / 0.278 / 0.968 / 0.085` — measured from **one**
hand-graded interview. This tool reads those same four numbers off any finished picture, so a
folder of references becomes a candidate look.

## Why the input is the opposite of the bench's

`experiments/bench` is fed **camera log**, because that is what OFX hands the plugin.

This tool is fed **finished pictures**, because a look is a property of the output. There is no
look in a log frame — every shot is the same flat grey going in. A reference still is already the
post-LUT display picture the targets live in, so its code values *are* the numbers the solve
consumes. Nothing is decoded, rendered or graded here.

Keeping them separate is deliberate. One image standing in for two different questions is exactly
the shape of the black-point encode bug (`docs/AUTO-GRADE.md` §2) and of the bench's own copy of
it.

## It does not reimplement the measurement

The five points come from `og::grade::pick_tone_samples()` — the same function
`solve_magic_tone()` picks with. That function was **extracted** from the solve for this tool
rather than copied, because the ranking keys are not guessable and getting one wrong produces
plausible numbers:

- the subject's p10 / p50 / p90 ranked by **Rec.709 luma**
- the frame's p99.9 / p0.1 ranked by **max channel**, then read as **max** and **min** respectively

Rank the frame by luma instead and a saturated practical stops being the ceiling. Rank the subject
by max channel and a red cheek outranks a lit forehead. Regions come from the shipped
`og::seg::Segmenter` and the subject from `og::analysis::magic_decide()`, so a reference is
measured with the subject the plugin would actually pick on it.

**Sanity check that this is wired up right:** run it over `experiments/bench`'s own output. SKIN
midtone came back **0.296 with a MAD of 0.026** — the tightest spread of any region by 4× — against
the solve's target of 0.278. The measurement agrees with what the solve pins.

## Reading the output

Per still, one line per region above `--min-cover` (default 2%), with `*` marking the subject
`magic_decide()` would choose:

```
   SKIN      13.1% *  floor 0.092  mid 0.284  hi 0.707 | ceil 0.988  fLo 0.039 | dL -33.6 da +4.3 db -11.7
```

`floor/mid/hi` are the subject triple, `ceil/fLo` the frame's two, and `dL/da/db` this region
against everything else as **signed** Lab components — signed because a distance cannot be solved
against and predicted the wrong sign outright when it was tried (`docs/AUTO-GRADE.md` §9).

Then a per-`(look, region)` median table — the target table the plugin would carry — and the
separability report.

## Separability is the go/no-go, and the metric had to be fixed once

Cycling looks only works if the looks are meaningfully different. The report gives, per axis, the
**common-language effect size**: the share of cross pairs where a still from A sits above one from
B, rescaled so 0.00 is indistinguishable and 1.00 never overlaps. `DISTINCT` needs ≥ 0.60 on some
axis, which is one look above the other in 80% of pairs.

**The first version of this metric was wrong and the null test caught it.** It divided the gap by
the MAD, and splitting *one* look at random into two halves reported **3.6 sigma on SKIN** — with
n=3 the MAD collapses toward zero and the ratio explodes. A go/no-go number that says yes to noise
is worse than not having one. The rank statistic has no denominator, and there is now a hard floor
of **8 stills per look per region** below which no verdict is printed at all.

Validated three ways:

| test | expected | got |
|---|---|---|
| one look split in half | no verdict / overlapping | refuses at n<8 ✓ |
| same-look halves, BUILT | overlapping | overlapping ✓ |
| log stills vs their graded output | strongly distinct | **0.85 → DISTINCT** ✓ |

If real looks come back **overlapping on every axis**, that is an argument for more *axes* — the
colour/separation triple, or a different print stock per look — not for more stills.

## A still only counts where the plugin would have graded to it

`--all-rows` off by default. A region is excluded from the aggregate unless it passes the two
tests `solve_magic_tone()` already applies to a subject — coverage ≤ 35%, and a midtone at or
above 0.01. Not a new rule invented here: a still contributes to a look exactly when the plugin
would have accepted its subject, so the corpus and the solve cannot drift apart. Exclusions are
printed per still with the reason, and named again in the aggregate when they empty a region.

**The first pass over real film stills is what forced this.** ADE20K class 12 is **"person"** —
whole body, wardrobe and hair — not "face". One still came back SKIN 25.7% with p10, p50 and p90
**all at 0.008**: a flat silhouette with no tonal range, reported as a subject midtone. Averaging
that into a look would fit `subjMid` to how films frame people rather than to where a face
belongs.

### The framing mismatch the filter does not fully fix

Even after filtering, person-region midtones from wide narrative framing sit far below the
interview-derived target (first pass, n=5–8):

| look | subjMid, SKIN |
|---|---|
| moody | 0.062 |
| cinematic | 0.065 |
| bright | 0.161 |
| *plugin's current target* | *0.278* |

A person occupying 10% of a 2.39:1 frame is mostly body, and no threshold recovers a face from
that. So **film-grab numbers are not usable as absolute subject targets.** What they do appear to
carry is the *ratio* — bright runs ~2.5× moody — which is a look-to-look relationship that
survives the framing difference.

The shape that follows: take relative offsets from the reference corpus and keep the absolute
anchor on hand-graded footage. Treat the table above as a hypothesis at n=5–8, not a result; the
prediction to test on a full corpus is that the ordering and rough ratio hold while the absolute
level stays low.

## Gathering the references

- **~30 stills per look.** The floor is 8 per region, and a folder only speaks for the regions it
  actually contains: an interview folder says nothing about TERRAIN, and guessing there is what
  destroyed the beach frame (`docs/ROADMAP.md`, "Magic Tone beyond faces").
- **Letterbox bars are cropped automatically** (`--crop`, default 0.02; `--no-crop` to disable),
  and the crop is reported per still. This is not tidying: matte bars are hard zeros, so the
  frame's p0.1 would read 0.000 on every matted still and the measured floor would describe the
  container instead of the picture.
- **8-bit web JPEG is workable but marginal.** A 0.01 difference in `subjMid` is 2.5 code values,
  which is why the aggregate uses medians over many stills rather than a handful.

## Flags

| flag | default | what |
|---|---|---|
| `--encode=N` | 1 | display encode for Lab: 0 Scene, 1 Gamma 2.2, 2 Gamma 2.4 |
| `--min-cover=P` | 2.0 | ignore regions below this % of frame |
| `--crop=T` | 0.02 | letterbox detection threshold |
| `--no-crop` | off | leave bars in |
| `--region=NAME` | all | only this region (`SKIN`, `SKY`, `TERRAIN`, …) |
| `--csv` | off | machine-readable rows, no aggregate |

## What this does not do

It does not choose looks, name them, or write anything into the plugin. It measures. Turning a
measured quadruple into a shipped Look is the next step and depends on what the separability
report says.
