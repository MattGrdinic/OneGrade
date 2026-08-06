#!/usr/bin/env python3
"""
OneGrade — Magic Grade decision: which slider, which way, and why.

THE DESIGN (the user's, and it is a better product than what preceded it)

    identify the objects -> rank them by how PRESENT they are -> pick a subject -> look at the
    rest of the scene -> choose Offset Temp or Gain Temp -> choose a direction -> render it.

    The slider then scales that one decision up or down. If the user does not like it they press
    the button again, a DIFFERENT subject is picked, and the same process runs.

WHY THAT SHAPE IS RIGHT, AND WHY THE PREVIOUS ATTEMPT WAS NOT

    The version before this tried to maximise a separation number across all regions at once. It
    was measured against four of the user's own hand grades and FAILED: two of three narrowed
    region separation rather than widening it, and the changes were 2-11% riding on common-mode
    global moves several times larger. The metric was not measuring what the grades were doing.

    This does not need a metric. It makes a CHOICE -- one subject, one control, one direction --
    and the user accepts it, tunes it, or cycles to the next candidate. Being wrong on the first
    pick costs one more click instead of being wrong silently, which means the decision rule only
    has to be reasonable, never right.

CHOOSING BETWEEN OFFSET TEMP AND GAIN TEMP IS NOT A GUESS

    It falls out of the pipeline. In og::process():

        offset:  w[0] += offTemp*0.10        additive -- a large RELATIVE shift on a dark region,
                                             a small one on a bright region
        gain:    w[0] *= (1 + temp*0.20)     multiplicative -- scales with the value, so it grips
                                             the bright end

    So the control follows the subject's luminance against the rest of the scene: a dark subject
    is reached with Offset Temp, a bright one with Gain Temp. Nothing fitted.

    On the beach that yields Offset Temp, negative -- which is exactly the control and direction
    the user reached for by hand (-0.167), arrived at from the pipeline's own arithmetic.

PROTECTED SUBJECTS INVERT THE RULE

    Skin is never pushed. When skin is the subject, the move goes to the SURROUND instead: pick
    the control that grips the surround, and push AWAY from skin's hue. Cool the room, let the
    face come forward -- the same operation, addressed from the other side.

USAGE
    python magic.py FRAME [FRAME ...] [--clicks N]
"""
import argparse
import os
import sys

import numpy as np

from regions import ORDER, segment, srgb_to_lab

# How much a region matters BEYOND its share of the frame. Coverage is most of importance, which
# is the user's rule, but a face is the subject of a shot at 15% and a wall is not at 70%.
SALIENCE = {
    "SKIN": 3.0,        # a person is what the shot is about, nearly regardless of size
    "WATER": 1.2,       # large, and tolerates being pushed further than anything else
    "VEGETATION": 1.0,
    "SKY": 0.7,         # usually the backdrop others are read against, not the subject
    "TERRAIN": 0.7,
    "BUILT": 0.6,
    "OTHER": 0.4,
}
PROTECTED = {"SKIN"}
MIN_COVER = 6.0


def scene(rows):
    """Coverage-weighted mean of everything, i.e. what the frame looks like overall."""
    w = np.array([r["cover"] for r in rows], float)
    w /= w.sum()
    return {k: float(w @ np.array([r[k] for r in rows], float)) for k in ("L", "a", "b")}


def decide(rows, click):
    """-> (subject, control, direction, why). `click` cycles the subject on repeat presses."""
    big = [r for r in rows if r["cover"] >= MIN_COVER]
    if len(big) < 2:
        return None, None, 0, ("only one region in frame — nothing to read this against; "
                               "Magic Grade is Creative Grade here, and should say so")

    ranked = sorted(big, key=lambda r: -r["cover"] * SALIENCE[r["label"]])

    # DEDUPE BY THE MOVE, NOT BY THE SUBJECT. Two different subjects can resolve to the same
    # control and the same sign — on the car portrait, "protect the face" and "push the interior"
    # both come out as Gain Temp negative — and offering that twice makes the second press do
    # nothing visible. A button whose whole affordance is "press again for a different answer"
    # has to actually give one.
    options, seen = [], set()
    for s in ranked:
        d = _decide_for(s, [r for r in big if r is not s])
        key = (d[1], d[2])
        if key in seen:
            continue
        seen.add(key)
        options.append(d)
    if not options:
        return None, None, 0, "no distinct move available"
    return options[click % len(options)]


def _decide_for(subj, rest):
    sc = scene(rest)                       # the scene the subject is read AGAINST, not including it
    dL = subj["L"] - sc["L"]
    db = subj["b"] - sc["b"]
    if abs(db) < 0.5:
        # No lean to enhance. Push away from wherever the scene sits, so the frame still gains a
        # colour relationship rather than the press doing nothing.
        db = -sc["b"] if abs(sc["b"]) > 0.5 else 1.0

    if subj["label"] in PROTECTED:
        # Never push the subject. Move the surround instead: grip whatever the SURROUND is, and
        # push away from the subject's hue.
        control = "Gain Temp" if sc["L"] > subj["L"] else "Offset Temp"
        direction = -1 if db > 0 else +1
        why = (f"{subj['label']} is protected, so the surround moves instead — "
               f"surround is {'brighter' if sc['L'] > subj['L'] else 'darker'} "
               f"(L* {sc['L']:.0f} vs {subj['L']:.0f}) so {control} has the grip on it, "
               f"pushed {'cooler' if direction < 0 else 'warmer'} away from the subject")
    else:
        control = "Gain Temp" if dL > 0 else "Offset Temp"
        direction = +1 if db > 0 else -1
        why = (f"{subj['label']} is {'brighter' if dL > 0 else 'darker'} than the rest "
               f"(L* {subj['L']:.0f} vs {sc['L']:.0f}) so {control} has the grip on it; "
               f"it already leans {'warm' if db > 0 else 'cool'} "
               f"(b* {subj['b']:.0f} vs {sc['b']:.0f}) so push it further that way")
    return subj, control, direction, why


def report(path, model, clicks):
    rgb, regions, _, _ = segment(path, model)
    lab = srgb_to_lab(rgb)
    total = regions.size
    rows = []
    for r in ORDER:
        n = int((regions == r).sum())
        if n < total * 0.005:
            continue
        m = regions == r
        rows.append(dict(label=r, cover=100.0 * n / total,
                         L=lab[..., 0][m].mean(), a=lab[..., 1][m].mean(), b=lab[..., 2][m].mean()))
    rows.sort(key=lambda r: -r["cover"])

    print(f"\n=== {os.path.splitext(os.path.basename(path))[0]} ===")
    for r in rows:
        if r["cover"] >= MIN_COVER:
            print(f"   {r['label']:<11}{r['cover']:5.1f}%  L*{r['L']:6.1f} b*{r['b']:+6.1f}"
                  f"   importance {r['cover']*SALIENCE[r['label']]:6.1f}")
    for c in range(clicks):
        subj, control, direction, why = decide(rows, c)
        if subj is None:
            print(f"   press {c+1}: NO MOVE — {why}")
            break
        print(f"   press {c+1}: {subj['label']:<11} -> {control} "
              f"{'+' if direction > 0 else '-'}ve")
        print(f"             {why}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("frames", nargs="+")
    ap.add_argument("--model", default="nvidia/segformer-b0-finetuned-ade-512-512")
    ap.add_argument("--clicks", type=int, default=3, help="how many presses to simulate")
    a = ap.parse_args()
    for f in a.frames:
        if not os.path.exists(f):
            print(f"missing: {f}", file=sys.stderr); continue
        report(f, a.model, a.clicks)


if __name__ == "__main__":
    main()
