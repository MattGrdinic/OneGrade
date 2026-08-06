#!/usr/bin/env python3
"""
OneGrade — offline region-mask experiment.

WHAT THIS ANSWERS
    Do real semantic region masks give separation numbers that track the user's hand grades,
    where the two cheap region-finders in the plugin do not?

    Measured in the plugin, across three shots (docs/ROADMAP.md):

        shot                 bands (top/bottom third)        2-means in (a*, b*)
        beach (horizon)      db* +43  found sky over water   two populations BOTH orange
        city  (downward)     db* -0.6 found nothing          -
        car   (centred)      db* -1   found nothing          h-158 / h+95, distinct

    Each fails exactly where the other works. That is the case for segmentation: not that it
    would be nicer, but that nothing cheaper covers the range of shots.

WHY IDENTITY AND NOT JUST STRUCTURE
    A luminance split can say where the separation is AVAILABLE. It cannot say which way to
    push. Cooling the shadows is right on a beach because the dark region is water and
    exaggerating water reads as natural; the same move on a warm interior, or on skin sitting
    in shadow, is wrong. Direction and permission come from knowing what the thing IS.

WHAT IT DOES NOT DO
    No grading, no parameter solving. It reports regions and their colour statistics so a
    heuristic table (label -> desired direction) can be written against real numbers instead
    of against a guess. The solver that turns those targets into slider values already exists
    in src/OneGradeAnalysis.h.

USAGE
    python regions.py FRAME [FRAME ...] [--model NAME] [--out DIR]
"""
import argparse
import os
import re
import sys

import numpy as np
from PIL import Image

# ---------------------------------------------------------------------------------------
# Grading vocabulary. ADE20K ships 150 classes; almost none of the distinctions matter to a
# colourist. What matters is the handful of things a grade treats DIFFERENTLY:
#
#   SKY / WATER     large, and we accept exaggeration on them — this is where separation is won
#   SKIN            protected, never pushed. A cyan-shadow move also cyans skin in shadow, and
#                   that is the single most visible way to wreck a frame.
#   VEGETATION      has a memory colour of its own, pushes green rather than cyan
#   TERRAIN / BUILT the rest of the frame; usually what everything else separates FROM
#
# Matched on whole words of the model's own label names, so the map survives a model swap.
# Whole words and not substrings -- see region_of() for the 36%-of-frame bug that cost.
REGIONS = {
    "SKY":        ("sky",),
    "WATER":      ("water", "sea", "river", "lake", "waterfall", "swimming pool", "pool"),
    "SKIN":       ("person", "people"),
    "VEGETATION": ("tree", "plant", "grass", "palm", "flower", "field", "bush", "leaves"),
    "TERRAIN":    ("earth", "sand", "dirt", "land", "hill", "mountain", "rock", "snow", "path"),
    "BUILT":      ("building", "house", "skyscraper", "wall", "ceiling", "floor", "road",
                   "sidewalk", "bridge", "tower", "fence", "car", "truck", "bus", "windowpane",
                   "seat", "cushion", "chair", "sofa", "table", "bed", "curtain",
                   "screen", "door", "signboard", "box", "pole", "streetlight"),
}
ORDER = ["SKY", "WATER", "SKIN", "VEGETATION", "TERRAIN", "BUILT", "OTHER"]
SWATCH = {  # for the mask preview, so a wrong mask is obvious at a glance
    "SKY": (90, 160, 235), "WATER": (30, 90, 160), "SKIN": (235, 165, 130),
    "VEGETATION": (70, 150, 70), "TERRAIN": (170, 140, 95), "BUILT": (140, 140, 150),
    "OTHER": (60, 60, 60),
}


def region_of(label: str) -> str:
    """Label -> grading region, matched on WHOLE WORDS.

    Substring matching looks fine and is quietly wrong. The first run of this mapped the car
    interior of a portrait shot to WATER, because "seat" contains "sea" — 36% of the frame in
    the wrong region, with numbers that looked perfectly plausible in the table. Two more were
    waiting: "street" contains "tree", and "carpet" contains "car".

    The mask preview is what caught it, which is why every run writes one.
    """
    l = label.lower()
    toks = {t for t in re.split(r"[^a-z]+", l) if t}
    for name, keys in REGIONS.items():
        for k in keys:
            if " " in k:
                if k in l:          # multi-word key, e.g. "swimming pool"
                    return name
            elif k in toks:
                return name
    return "OTHER"


# ---------------------------------------------------------------------------------------
# sRGB -> CIELAB (D65). Mirrors display_to_Lab() in src/OneGradeAnalysis.h so the numbers here
# are directly comparable with the plugin's Colour / Separation rows. b* IS warm/cool and a* IS
# green/magenta, which is what lines the descriptors up one-for-one with the Temp and Tint
# controls.
def srgb_to_lab(img: np.ndarray) -> np.ndarray:
    v = img.astype(np.float64) / 255.0
    lin = np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)
    m = np.array([[0.4123908, 0.3575843, 0.1804808],
                  [0.2126390, 0.7151687, 0.0721923],
                  [0.0193308, 0.1191948, 0.9505322]])
    xyz = lin @ m.T
    xyz /= np.array([0.95047, 1.0, 1.08883])
    d = 6.0 / 29.0
    f = np.where(xyz > d ** 3, np.cbrt(xyz), xyz / (3 * d * d) + 4.0 / 29.0)
    return np.stack([116 * f[..., 1] - 16,
                     500 * (f[..., 0] - f[..., 1]),
                     200 * (f[..., 1] - f[..., 2])], axis=-1)


def segment(path, model_name):
    """Frame -> per-pixel grading-region names, at the frame's own resolution."""
    import torch
    from transformers import AutoImageProcessor, AutoModelForSemanticSegmentation

    img = Image.open(path).convert("RGB")
    proc = AutoImageProcessor.from_pretrained(model_name)
    model = AutoModelForSemanticSegmentation.from_pretrained(model_name)
    with torch.no_grad():
        logits = model(**proc(images=img, return_tensors="pt")).logits
    up = torch.nn.functional.interpolate(
        logits, size=img.size[::-1], mode="bilinear", align_corners=False)
    ids = up.argmax(dim=1)[0].numpy()

    id2label = model.config.id2label
    names = np.array(ORDER)
    lut = np.array([ORDER.index(region_of(id2label.get(i, ""))) for i in range(len(id2label))])
    return np.asarray(img), names[lut[ids]], ids, id2label


def report(path, model_name, outdir):
    rgb, regions, ids, id2label = segment(path, model_name)
    lab = srgb_to_lab(rgb)
    h, w = regions.shape
    total = h * w
    name = os.path.splitext(os.path.basename(path))[0]

    # Per-region statistics. `y` is the vertical centroid, 0 = bottom, so it can be checked
    # against the plugin's top/bottom-third bands directly.
    rows = []
    for r in ORDER:
        m = regions == r
        n = int(m.sum())
        if n < total * 0.005:          # under half a percent is not a region, it is speckle
            continue
        L, a, b = (lab[..., i][m].mean() for i in range(3))
        ys = np.nonzero(m)[0]
        rows.append((r, 100.0 * n / total, L, a, b, 1.0 - ys.mean() / h))

    print(f"\n=== {name} ({w}x{h}) ===")
    print(f"  {'region':<11}{'cover':>7}{'L*':>8}{'a*':>8}{'b*':>8}{'height':>8}")
    for r, pct, L, a, b, y in sorted(rows, key=lambda t: -t[1]):
        print(f"  {r:<11}{pct:6.1f}%{L:8.1f}{a:8.1f}{b:8.1f}{y:8.2f}")

    # The separation triple, between the two largest regions. Same three signed components the
    # plugin steers on -- dL* tone, da*/db* hue -- so these numbers sit alongside the panel's.
    if len(rows) >= 2:
        big = sorted(rows, key=lambda t: -t[1])[:2]
        (r1, _, L1, a1, b1, _), (r2, _, L2, a2, b2, _) = big
        print(f"\n  separation  {r1} vs {r2}:  "
              f"dL* {L1-L2:+.1f}   da* {a1-a2:+.1f}   db* {b1-b2:+.1f}")
        print(f"  (plugin band split for comparison is in docs/ROADMAP.md)")

    # A MASK THAT LOOKS PLAUSIBLE IN NUMBERS AND IS WRONG IN PIXELS is the worst outcome here,
    # so always write something to eyeball.
    if outdir:
        os.makedirs(outdir, exist_ok=True)
        vis = np.zeros_like(rgb)
        for r in ORDER:
            vis[regions == r] = SWATCH[r]
        blend = (0.45 * rgb + 0.55 * vis).astype(np.uint8)
        Image.fromarray(np.concatenate([rgb, blend], axis=1)).save(
            os.path.join(outdir, f"{name}-regions.png"))
        print(f"  -> {outdir}/{name}-regions.png")

    # What the model itself saw, before the grading remap — the tell when a region comes back
    # as OTHER and the reason is a class we simply did not map.
    uniq, cnt = np.unique(ids, return_counts=True)
    top = sorted(zip(cnt, uniq), reverse=True)[:6]
    print("  raw classes: " + ", ".join(
        f"{id2label.get(int(i), '?')} {100.0*c/total:.0f}%" for c, i in top))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("frames", nargs="+")
    # SegFormer-B0 on ADE20K: 150 classes covering sky / sea / water / sand / mountain / tree /
    # grass / person / building, which is exactly the grading vocabulary, at ~3.7M params so it
    # runs on CPU in about a second. Swap freely -- the region map matches on label NAMES, not
    # on class indices, so any semantic segmenter with sensible names drops in.
    ap.add_argument("--model", default="nvidia/segformer-b0-finetuned-ade-512-512")
    ap.add_argument("--out", default="out")
    a = ap.parse_args()
    for f in a.frames:
        if not os.path.exists(f):
            print(f"missing: {f}", file=sys.stderr); continue
        report(f, a.model, a.out)


if __name__ == "__main__":
    main()
