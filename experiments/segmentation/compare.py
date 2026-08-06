#!/usr/bin/env python3
"""
OneGrade — did the hand grade widen the region separation the heuristic aims at?

The heuristic in intent.py claims a frame's two dominant regions should be pushed apart along
the axis they already differ on. This checks that claim against grades the user actually made:
same frame, two states, and the SAME MASKS applied to both.

Masks come from the Creative version and are reused on the hand-graded one. They are the same
pixels at the same resolution, so the masks transfer exactly — which also means any measured
difference is the GRADE and nothing else. Segmenting each version separately would let the mask
move too, and a change in the numbers would no longer say which of the two caused it.

WHAT IS BEING TESTED
    1. Did separation widen at all? If the user's own grades narrow it, the premise is wrong and
       everything built on top of it is wrong with it.
    2. Does the predicted direction agree with the observed one, per region?

Common-mode shifts are reported separately. A grade that lifts the whole frame moves every
region's L* together and separates nothing; only the DIFFERENCE between regions is separation.

USAGE
    python compare.py BEFORE AFTER [BEFORE AFTER ...]
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

from regions import ORDER, segment, srgb_to_lab
from intent import INTENT, MIN_COVER, targets


def stats(lab, regions, label):
    m = regions == label
    return dict(L=lab[..., 0][m].mean(), a=lab[..., 1][m].mean(), b=lab[..., 2][m].mean())


def compare(before_path, after_path, model, strength):
    rgb0, regions, _, _ = segment(before_path, model)
    rgb1 = np.asarray(Image.open(after_path).convert("RGB"))
    if rgb1.shape != rgb0.shape:
        print(f"  size mismatch: {rgb0.shape} vs {rgb1.shape}", file=sys.stderr)
        return
    lab0, lab1 = srgb_to_lab(rgb0), srgb_to_lab(rgb1)
    total = regions.size

    rows = []
    for r in ORDER:
        n = int((regions == r).sum())
        if n < total * 0.005:
            continue
        rows.append(dict(label=r, cover=100.0 * n / total, **stats(lab0, regions, r)))
    rows.sort(key=lambda r: -r["cover"])

    name = os.path.basename(before_path).split(".png")[0]
    print(f"\n=== {name} ===")
    tg, veto = targets(rows, strength)
    if veto and not tg:
        print(f"  heuristic: NO MOVE — {veto}")

    big = [r for r in rows if r["cover"] >= MIN_COVER]
    if len(big) < 2:
        # Even with no pair, report what the hand grade did, since "the user graded it anyway"
        # is the interesting fact about a frame the heuristic declines to touch.
        for r in big:
            s1 = stats(lab1, regions, r["label"])
            print(f"     {r['label']:<11}{r['cover']:5.1f}%  "
                  f"L* {r['L']:6.1f}->{s1['L']:6.1f}   a* {r['a']:+6.1f}->{s1['a']:+6.1f}   "
                  f"b* {r['b']:+6.1f}->{s1['b']:+6.1f}")
        return

    A, B = big[0], big[1]
    a1, b1 = stats(lab1, regions, A["label"]), stats(lab1, regions, B["label"])

    # Separation is the DIFFERENCE between the two regions. Everything common to both is
    # exposure or a global cast, and separates nothing.
    def gap(x, y):
        return np.array([x["L"] - y["L"], x["a"] - y["a"], x["b"] - y["b"]])
    g0, g1 = gap(A, B), gap(a1, b1)
    m0, m1 = float(np.linalg.norm(g0)), float(np.linalg.norm(g1))

    print(f"  {A['label']} vs {B['label']}   separation {m0:.1f} -> {m1:.1f} "
          f"({'WIDER' if m1 > m0 else 'narrower'} by {abs(m1-m0):.1f})")
    for i, ax in enumerate(("L*", "a*", "b*")):
        print(f"     {ax}  {g0[i]:+7.1f} -> {g1[i]:+7.1f}")

    # Common mode: what moved on BOTH regions together. Large here and small above means the
    # grade was an exposure or cast change rather than a separation one.
    cm = np.array([(a1["L"] + b1["L"]) / 2 - (A["L"] + B["L"]) / 2,
                   (a1["a"] + b1["a"]) / 2 - (A["a"] + B["a"]) / 2,
                   (a1["b"] + b1["b"]) / 2 - (A["b"] + B["b"]) / 2])
    print(f"     common-mode shift  L* {cm[0]:+.1f}  a* {cm[1]:+.1f}  b* {cm[2]:+.1f}")

    if not tg:
        return
    # Predicted vs observed, per region. Cosine because what matters is whether the heuristic
    # points the right way; the magnitude is a strength constant nobody has fitted yet.
    print(f"     {'region':<11}{'predicted (dL,da,db)':>26}{'observed':>26}{'agree':>8}")
    for t, obs in ((tg[0], a1), (tg[1], b1)):
        p = np.array([t["dL"], t["da"], t["db"]])
        o = np.array([obs["L"] - t["L"], obs["a"] - t["a"], obs["b"] - t["b"]])
        np_, no = np.linalg.norm(p), np.linalg.norm(o)
        cos = float(p @ o / (np_ * no)) if np_ > 1e-9 and no > 1e-9 else float("nan")
        print(f"     {t['label']:<11}"
              f"{f'{p[0]:+.1f} {p[1]:+.1f} {p[2]:+.1f}':>26}"
              f"{f'{o[0]:+.1f} {o[1]:+.1f} {o[2]:+.1f}':>26}"
              f"{cos:+8.2f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pairs", nargs="+", help="BEFORE AFTER [BEFORE AFTER ...]")
    ap.add_argument("--model", default="nvidia/segformer-b0-finetuned-ade-512-512")
    ap.add_argument("--strength", type=float, default=6.0)
    a = ap.parse_args()
    if len(a.pairs) % 2:
        sys.exit("pairs must come in twos: BEFORE AFTER")
    for i in range(0, len(a.pairs), 2):
        compare(a.pairs[i], a.pairs[i + 1], a.model, a.strength)


if __name__ == "__main__":
    main()
