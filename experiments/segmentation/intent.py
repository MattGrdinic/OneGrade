#!/usr/bin/env python3
"""
OneGrade — heuristic table: region label -> grading intent.

This is the layer between "what is in the frame" and "which sliders move". It turns a set of
labelled regions into DESCRIPTOR TARGETS -- how far each region's a* and b* should move -- and
nothing further. Turning targets into slider values is already solved: solve_intent_iter() in
src/OneGradeAnalysis.h measures how each control moves each descriptor on this shot and inverts.

WHY IT IS A TABLE AND NOT A MODEL
    There is one shot of ground truth for colour separation. A table of per-label directions can
    be read, argued with, and corrected one line at a time; a fitted model cannot, and there is
    nothing like enough data to fit one anyway. Every constant here is a starting point expected
    to move once it has been looked at on footage -- which is how every working number in this
    project was found.

THE CONTROLS ARE GLOBAL, AND THAT IS THE WHOLE PROBLEM
    OneGrade has no per-region controls, so "cool the water and leave the sky" is only reachable
    to the extent the two regions respond DIFFERENTLY to a global move. They do: Offset Temp is
    additive, so on a dark region it is a large relative shift and on a bright one it is small.
    That is why the user's own beach grade reached for Offset Temp, and why the damped solve can
    honour a per-region target set it cannot possibly satisfy exactly. It finds the best
    compromise, which is what a colourist does by eye.

NOT EVERY SHOT HAS SEPARATION AVAILABLE
    The downward city view comes back as one undifferentiated region, and the user's own grade
    of that shot was purely tonal. So the honest output is sometimes "there is nothing here to
    separate" -- at which point Magic Grade is just Creative Grade, and says so. Silence is the
    failure mode to avoid, not the absence of a move.

USAGE
    python intent.py FRAME [FRAME ...] [--strength S]
"""
import argparse
import os
import sys

import numpy as np

from regions import ORDER, segment, srgb_to_lab

# ---------------------------------------------------------------------------------------
# THE TABLE.
#
#   push      unit direction the region WANTS to move, as (da*, db*).
#             a* is green(-) / magenta(+);  b* is cool(-) / warm(+).
#   protect   0 = free to move, 1 = must not move. Enters the solve as a weight on holding
#             the region still, so it competes with the pushes rather than overriding them.
#   headroom  how far this region can be pushed before it stops reading as itself. Water
#             tolerates a great deal of exaggeration; skin tolerates almost none.
#
# Directions are memory colours, not measurements: what a viewer accepts a thing looking like.
# That is exactly the knowledge no amount of pixel statistics recovers, and the reason this
# needs a classifier at all.
INTENT = {
    # Where separation is won. Large, and the eye forgives exaggeration on both.
    "WATER":      dict(push=(-0.2, -1.0), protect=0.0, headroom=1.0,
                       why="cool it; bluer-than-life water reads as natural and it is usually "
                           "the darker half of a horizon shot, where an additive move has most grip"),
    "SKY":        dict(push=(+0.0, +0.0), protect=0.2, headroom=0.5,
                       why="usually already extreme (sunset warm or daylight cool) and usually "
                           "the thing everything else separates FROM, so hold it and move the rest"),
    # Memory colours of their own. Vegetation goes green, not cyan.
    "VEGETATION": dict(push=(-1.0, +0.2), protect=0.2, headroom=0.7,
                       why="foliage pushes toward green; cyan foliage reads as a colour cast"),
    # The one that must not move. A cyan-shadow move also cyans skin in shadow, which is the
    # most visible way to wreck a frame. The plugin's chromaticity mask could not find skin on
    # a beach (46% coverage = sand); a semantic mask reads 0.7% there, which is the actual people.
    "SKIN":       dict(push=(0.0, 0.0), protect=1.0, headroom=0.0,
                       why="PROTECTED. Never pushed, and actively held against everything else"),
    # References. They are what the pushed regions separate away from.
    "TERRAIN":    dict(push=(0.0, 0.0), protect=0.2, headroom=0.4, why="reference"),
    "BUILT":      dict(push=(0.0, 0.0), protect=0.2, headroom=0.4, why="reference"),
    "OTHER":      dict(push=(0.0, 0.0), protect=0.1, headroom=0.3, why="reference"),
}

MIN_COVER = 8.0     # below this a region is scenery, not something a grade should chase
DOMINANT  = 88.0    # above this one region IS the frame, and there is nothing to separate from


MIN_GAP   = 3.0     # below this the two regions are the same colour and there is no axis to widen
AXIS_FULL = 20.0    # gap at which the axis direction is trusted completely


def targets(rows, strength):
    """Region rows -> per-region (da*, db*) targets, plus a verdict on whether to act at all.

    THE FIRST VERSION OF THIS ONLY KNEW HOW TO MOVE WATER, and it is worth recording why,
    because the mistake is an easy one to repeat. Each label carried an absolute push direction
    — water cool, foliage green — and everything else was "reference", meaning zero. Run on
    eight frames it produced a move on two of them. It vetoed the sky-over-mountain shot, which
    is the single clearest case for having a classifier at all: two regions three L* apart in
    tone and fourteen b* apart in colour, which nothing but a mask can separate. A table of
    absolute directions can only ever act on labels somebody thought to write a direction for.

    What the frame actually offers is a PAIR and an axis between them. So the move is: push the
    two dominant regions apart along the axis they ALREADY differ on, and let the table decide
    only how far each of them is allowed to travel. Skin has zero headroom and therefore never
    moves — the whole push goes to whatever it is sitting against, which is also how a colourist
    separates a face: cool the surround and let the skin come forward.
    """
    big = [r for r in rows if r["cover"] >= MIN_COVER]
    if not big:
        return [], "no region above the coverage floor"
    if len(big) < 2:
        return [], f"one region is the whole frame ({big[0]['label']} {big[0]['cover']:.0f}%)"
    if big[0]["cover"] >= DOMINANT:
        return [], f"{big[0]['label']} covers {big[0]['cover']:.0f}% — nothing to separate from"

    A, B = big[0], big[1]
    da, db = A["a"] - B["a"], A["b"] - B["b"]
    mag = float(np.hypot(da, db))
    if mag < MIN_GAP:
        return [], (f"{A['label']} and {B['label']} are the same colour "
                    f"(gap {mag:.1f}) — no axis to widen")
    ua, ub = da / mag, db / mag

    # TRUST THE AXIS IN PROPORTION TO HOW WELL DEFINED IT IS. The direction comes from the
    # difference between two region means, so when that difference is small the direction is
    # mostly noise — and pushing hard along a noisy axis does not separate anything, it just
    # applies an arbitrary cast. A beach sunset with sand and sky 3.3 apart was getting the same
    # push as a shot with a gap of 54.
    #
    # Ramped rather than thresholded: a hard cutoff makes the behaviour jump either side of it,
    # which is the shape of trap this project has hit twice already (rolloff at 0, RAW Temp at
    # 6500). A frame near the boundary should get a small move, not a coin flip.
    confidence = min(1.0, mag / AXIS_FULL)
    strength *= confidence

    # How far each end may travel. headroom is how much exaggeration the thing tolerates before
    # it stops reading as itself; protect is how hard we hold it still. Skin lands on zero from
    # both directions, which is the point.
    def allowance(r):
        it = INTENT[r["label"]]
        return it["headroom"] * (1.0 - it["protect"])

    wA, wB = allowance(A), allowance(B)
    if wA + wB < 1e-6:
        return [], f"neither {A['label']} nor {B['label']} may be pushed"

    out = []
    for r, sign, w in ((A, +1.0, wA), (B, -1.0, wB)):
        s = strength * sign * w / (wA + wB)
        out.append(dict(r, da=ua * s, db=ub * s, protect=INTENT[r["label"]]["protect"]))
    for r in big[2:]:                       # present, but not what the frame is built from
        out.append(dict(r, da=0.0, db=0.0, protect=INTENT[r["label"]]["protect"]))

    # Memory-colour sanity check. Widening the gap is the goal, but not at the cost of a thing
    # looking wrong: water driven warm or foliage driven magenta is a widened axis and a worse
    # picture. Flagged rather than vetoed — one shot of ground truth is not enough to know how
    # often the two genuinely conflict.
    warn = []
    for t in out[:2]:
        pa, pb = INTENT[t["label"]]["push"]
        if (pa or pb) and (pa * t["da"] + pb * t["db"]) < 0:
            warn.append(f"{t['label']} is being pushed against its memory colour")
    return out, ("; ".join(warn) + " (widening anyway)") if warn else None


def report(path, model, strength):
    rgb, regions, ids, id2label = segment(path, model)
    lab = srgb_to_lab(rgb)
    total = regions.size
    rows = []
    for r in ORDER:
        m = regions == r
        n = int(m.sum())
        if n < total * 0.005:
            continue
        rows.append(dict(label=r, cover=100.0 * n / total,
                         L=lab[..., 0][m].mean(), a=lab[..., 1][m].mean(), b=lab[..., 2][m].mean()))
    rows.sort(key=lambda r: -r["cover"])

    print(f"\n=== {os.path.splitext(os.path.basename(path))[0]} ===")
    tg, veto = targets(rows, strength)
    if veto and tg:
        print(f"  WARNING: {veto}")
        veto = None
    if veto:
        print(f"  NO SEPARATION MOVE: {veto}")
        print("  -> Magic Grade falls back to Creative Grade, and should say so.")
        for r in rows:
            print(f"     {r['label']:<11}{r['cover']:5.1f}%  a*{r['a']:+6.1f} b*{r['b']:+6.1f}")
        return

    print(f"  {'region':<11}{'cover':>7}{'a*':>8}{'b*':>8}{'->da*':>8}{'->db*':>8}{'hold':>7}")
    for t in tg:
        print(f"  {t['label']:<11}{t['cover']:6.1f}%{t['a']:8.1f}{t['b']:8.1f}"
              f"{t['da']:+8.2f}{t['db']:+8.2f}{t['protect']:7.2f}")

    # What the move is FOR: the pair the frame is actually built from, and whether the targets
    # open the gap or close it. A heuristic that quietly reduces separation is worse than none.
    a, b = tg[0], tg[1]
    gap = float(np.hypot(a["a"] - b["a"], a["b"] - b["b"]))
    print(f"  axis {a['label']}->{b['label']} gap {gap:.1f}"
          f"  confidence {min(1.0, gap/AXIS_FULL):.2f}")
    for ax, key in (("a*", "a"), ("b*", "b")):
        d0 = a[key] - b[key]
        d1 = (a[key] + a["d" + key]) - (b[key] + b["d" + key])
        if abs(d0) < 0.5 and abs(d1) < 0.5:
            continue
        verb = "widens" if abs(d1) > abs(d0) else ("narrows" if abs(d1) < abs(d0) else "holds")
        print(f"  {a['label']} vs {b['label']} on {ax}: {d0:+.1f} -> {d1:+.1f}  ({verb})")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("frames", nargs="+")
    ap.add_argument("--model", default="nvidia/segformer-b0-finetuned-ade-512-512")
    # One unit is "a firm but not transformative push", in Lab units, before the solve has to
    # find global controls that approximate it. Fit on footage.
    ap.add_argument("--strength", type=float, default=6.0)
    a = ap.parse_args()
    for f in a.frames:
        if not os.path.exists(f):
            print(f"missing: {f}", file=sys.stderr); continue
        report(f, a.model, a.strength)


if __name__ == "__main__":
    main()
