# Region-mask experiment

Offline. No plugin changes, no inference runtime, no C++ — this exists to find out whether real
semantic masks are worth building before any of that gets written.

## The question

**Do real region masks give separation numbers that track the hand grades, where the two cheap
region-finders in the plugin do not?**

Measured across three shots (see `docs/ROADMAP.md`):

| | bands (top/bottom third) | 2-means in (a*, b*) |
|---|---|---|
| beach (horizon) | **db\* +43** — found sky over water | two populations *both orange*, h29 / h44 |
| city (downward) | **db\* −0.6** — nothing | — |
| car (centred subject) | **db\* −1** — nothing | **h−158 / h+95** — distinct |

Each fails exactly where the other works.

## Why identity, not just structure

A luminance split can say *where* separation is available. It cannot say *which way to push*.
Cooling the shadows is right on a beach because the dark region is water and exaggerating water
reads as natural; the same move on a warm interior, or on skin sitting in shadow, is wrong.
Direction and permission come from knowing what the thing **is** — which is the whole argument
for a classifier, and the reason a purely tonal experiment would not have settled anything.

## Setup

```bash
python3.10 -m venv seg-env
./seg-env/bin/pip install numpy pillow torch torchvision transformers
```

## Run

```bash
./seg-env/bin/python regions.py beach.png city.png car.png --out out
```

Prints per-region coverage, mean `L*` / `a*` / `b*`, and vertical centroid, then the separation
triple between the two largest regions — the same three signed components the plugin steers on,
so the numbers sit directly alongside the panel's `Separation` row.

**Always look at `out/*-regions.png`.** A mask that is plausible in numbers and wrong in pixels
is the worst outcome here, so every run writes the frame beside its colour-coded regions.

## Model

Default is SegFormer-B0 on ADE20K — 150 classes covering sky / sea / water / sand / mountain /
tree / grass / person / building, which is close to the grading vocabulary already, at ~3.7M
params so it runs on CPU in about a second.

Swap freely with `--model`: the region map in `regions.py` matches on label **names**, not class
indices, so any semantic segmenter with sensible names drops in. Worth comparing against a
text-prompted model (CLIPSeg and similar), where the region vocabulary becomes a prompt list and
can be changed without retraining.

Checkpoint licences still want checking before anything ships, even though the plugin is GPL and
this is a research harness.

## What this deliberately does not do

No grading and no parameter solving. It reports regions and their colour statistics so the
heuristic table — *label → desired direction* — can be written against real numbers instead of a
guess. The solver that turns those targets into slider values already exists in
`src/OneGradeAnalysis.h`.
