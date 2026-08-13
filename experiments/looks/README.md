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

## RESULT (2026-08-13): film-grab does not separate these looks

123 stills, four looks, seven axes. **Every pair overlaps on every axis.** The largest effect
anywhere is 0.59 (`dL*`, cinematic vs moody) against a 0.60 bar; the best tone axis is `subjMid`
at 0.51.

The tone signal is directionally right and too small to use. `subjMid` on SKIN orders exactly as
the labels predict — bright 0.215 > high-contrast 0.180 > cinematic 0.115 > moody 0.103 — but the
within-look spread swamps it: cinematic's MAD is 0.075 on a median of 0.115, ±65%, while the
look-to-look gaps are 0.03–0.11. **Individual films vary more than the look categories differ.**

`frameCeiling` and `frameFloor` carry almost nothing (0.01–0.29 throughout). Everything blows to
~0.9 and crushes to ~0.0 regardless of look, so those two are dead as look carriers.

### Why, and why more stills will not fix it

Two structural problems with this source, neither solved by gathering more:

1. **One frame per film samples a random scene, not the film's look.** The choice avoided
   correlated frames, and the cost was not appreciated at the time: a moody film has bright scenes.
   Scene variance goes straight into the within-look spread that separability is measured against.
2. **The subject region is framing-dependent.** ADE20K "person" is whole body, so `subjMid` moves
   with how much frame a body occupies — noise with nothing to do with the look.

So *overlapping* here is a statement about **this measurement**, not proof that the looks are the
same. The measurement is too noisy to see a difference that may well be there.

### What would actually test it

**Change the unit of analysis from the frame to the film.** Pull many frames per film from the
gallery, take a median per film first, then compare films across looks. That collapses scene
variance into the per-film estimate instead of letting it inflate the spread, and n becomes 30
films each estimated from ~30 frames rather than 30 lone frames. It reuses everything here; only
the fetcher changes. Cost is real — roughly 300–900 requests per look — so rate limits matter.

If that still overlaps, the conclusion is that a look is not a tone target and is carried by the
print stock / colour transform instead, which is a different feature.

### What the pass DID produce

The per-region target table is independently useful, and for a reason the look comparison is not:
it is a central tendency rather than a between-group difference, and SKY / FOLIAGE / TERRAIN /
GROUND are large natural regions that do not suffer the framing problem the way "person" does.
That is real data for **`docs/ROADMAP.md` "Magic Tone beyond faces"**, which has been waiting on
exactly these numbers. Wants on-footage validation before anything ships.

## The reframe: not "look like a film", but "have what film frames have"

The separability pass above tested whether cinematic separates from moody from bright. That is the
wrong question, and its answer is weak evidence for the right one: **all 123 stills are film, and
they failed to separate from each other.** A common signature is what that looks like.

The question that matters is film against everything else — a frame with a dull subject that reads
as film anyway. That needs a non-film control, and it needs **frame-level** descriptors, because
region axes describe where a subject sits and a small control set fragments into n=2-5 once split
by region.

### First pass, and it is not a result

121 film stills against 14 OneGrade outputs. Nothing crosses the 0.60 bar (max 0.47), but three
related measures agree:

| axis | film | OneGrade | effect |
|---|---|---|---|
| `loC` shadow chroma | **1.51** | **6.56** | 0.47 |
| `loRel` shadow ÷ midtone chroma | **0.13** | **0.50** | 0.47 |
| `hiRel` highlight ÷ midtone chroma | **1.15** | **0.76** | 0.45 |
| `spread` p90 − p10 luma | 0.431 | 0.578 | 0.30 |

**The print-stock hypothesis was wrong in its direction.** The prediction was that film
desaturates highlights hard, the way a print shoulder does. It does not: film *holds* highlight
chroma slightly above its midtone, and OneGrade is the one desaturating. The large consistent
difference is in the **shadows** — film goes nearly neutral down there, OneGrade keeps half its
midtone chroma.

### CONFOUND CONFIRMED (2026-08-13): the finding was the confound

Re-measured at matched luminance, against a control regenerated by a **default** bench run so it
is a picture of ordinary output rather than of bias sweeps:

| axis | film | OneGrade | effect |
|---|---|---|---|
| `loC` by percentile | 1.51 | 6.56 | 0.47 |
| `loC@L` at matched luminance | 3.02 | 2.82 | **0.15**, sign flipped |
| `loRel` -> `loRel@L` | | | **0.47 -> 0.18** |
| `hiRel` -> `hiRel@L` | | | **0.45 -> 0.00** |

Every chroma effect collapses. `hiRel` vanishes entirely. The mechanism is in the coverage row:
film puts **12.7%** of the frame inside the 0.02-0.10 luma band against OneGrade's **4.2%**. The
percentile measure was reading *a film frame's darkest tenth is much darker* -- a tone difference
-- and reporting it as colour.

**So the shadow-colour hypothesis is dead**, killed in an hour rather than after building a colour
feature on it. What survives is tonal and untested: film commits ~3x more of the frame to deep
shadow and uses less of the range (`spread` 0.431 vs 0.578, effect 0.30).

### Nothing has crossed the bar, on any axis, in two passes

Seven region axes and eleven frame axes, and the largest effect anywhere is 0.59. Worth stating
plainly because the temptation from here is to keep adding axes: **with this many comparisons one
will eventually reach 0.60 by chance.** Any new axis needs a reason before it is measured, not
after.

Two readings remain open and they want different responses:

- **The control is thin.** Fourteen frames. Clean now, but fourteen.
- **The signature may not be in summary statistics at all** — and may not be gradeable. Set
  decoration and lighting do most of the work in the reference frames, which is the user's own
  observation and is consistent with everything measured here.

### Two reasons not to act on this yet

1. **The control is bad data.** Fourteen frames, and they come from mixed bench runs including
   bias sweeps and deliberately extreme settings — the same folder that reads `frameCeiling 1.000`
   with `MAD 0.000`. It is not a picture of normal OneGrade output. **~30 clean exports from
   ordinary use is the blocker on this whole line.**
2. **`loC` has a confound that would change the fix.** Film frames crush to `frameFloor 0.000`
   almost universally, and a pixel at zero has no chroma to measure. So low `loC` may be reporting
   *film crushes its blacks* rather than *film neutralises shadow colour*. Those are different
   claims: the first is a tone control, the second is a colour one. Distinguishing them means
   measuring chroma at matched luminance rather than at matched percentile — the same shape as the
   `hot` versus `pin` distinction, where a threshold on the wrong quantity described the filter
   instead of the footage.

Chroma here is a **diagnostic, not a solve target**. `docs/AUTO-GRADE.md` §9 bans magnitudes from
the steerable set; if any of these becomes something to aim at, it has to be re-expressed as
signed components first.

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
