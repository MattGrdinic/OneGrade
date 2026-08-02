#!/usr/bin/env python3
# OneGrade — generator for the built-in look LUTs (shipped in the bundle's
# Contents/Resources/LUTs). Pure Python, no dependencies. Regenerate after tuning:
#
#   python3 luts/generate_luts.py
#
# The LUTs run on the plugin's Custom Look path, i.e. input AND output are
# display-referred Rec.709 (Scene OETF) values in [0,1]. Tuning lives in the
# LOOKS dict at the bottom — tweak numbers, re-run, `make`, reinstall, eyeball.
# SPDX-License-Identifier: BSD-3-Clause
import colorsys
import math
import os

SIZE = 33  # 33-point 3D LUT — standard look-LUT resolution, trilinear-safe


def clamp01(v):
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


def lerp(a, b, t):
    return a + (b - a) * t


def smoothstep(e0, e1, v):
    t = clamp01((v - e0) / (e1 - e0))
    return t * t * (3.0 - 2.0 * t)


def softclip(v, amt):
    """Same shoulder as og::softclip — identity below the knee, asymptote 1.0."""
    if amt <= 0.0 or v <= 0.0:
        return v
    k = 1.0 - 0.6 * amt
    if v <= k:
        return v
    r = 1.0 - k
    return k + r * (1.0 - math.exp(-(v - k) / r))


def toe_lift(v, amt):
    """Lift the deepest shadows (creamy blacks); fades out by mid-shadow."""
    return v + amt * (1.0 - smoothstep(0.0, 0.30, v))


def s_curve(v, strength):
    """Gentle contrast: blend toward a smoothstep S about mid."""
    return lerp(v, v * v * (3.0 - 2.0 * v), strength)


def in_hue_arc(h_deg, lo, hi):
    """1.0 inside [lo,hi] degrees with soft 15-degree feather each side (wraps)."""
    f = 15.0
    h = h_deg % 360.0

    def dist_into(x, a, b):
        if a <= b:
            return None if not (a - f <= x <= b + f) else min(x - (a - f), (b + f) - x, f) / f
        # wrapped arc
        return dist_into(x, a, 360.0 + b) or dist_into(x + 360.0, a, 360.0 + b)

    d = dist_into(h, lo, hi)
    return 0.0 if d is None else clamp01(d)


def apply_look(r, g, b, p):
    # 1. white-balance bias (per-channel gain, display space — a look, not a CST)
    r *= 1.0 + p["warm"]
    b *= 1.0 - p["warm"]

    # 1b. optional luminance split-tone: warm(+)/cool(-) bias applied separately to
    #     shadows and highlights (the teal-and-orange machinery). Masks overlap gently.
    sp = p.get("split")
    if sp:
        lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
        w = sp.get("shadows", 0.0) * (1.0 - smoothstep(0.15, 0.55, lum)) \
          + sp.get("highlights", 0.0) * smoothstep(0.45, 0.85, lum)
        r *= 1.0 + w
        b *= 1.0 - w

    # 2. tone per channel: toe lift -> S-curve contrast -> highlight shoulder.
    #    Per-channel on purpose: bright saturated sources drift toward white.
    out = []
    for v in (r, g, b):
        v = toe_lift(v, p["toe"])
        v = s_curve(v, p["contrast"])
        v = softclip(v, p["shoulder"])
        out.append(clamp01(v))
    r, g, b = out

    # 3. color: global + hue-selective saturation, optional hue nudges, in HSV
    h, s, v = colorsys.rgb_to_hsv(r, g, b)
    hd = h * 360.0
    sat = p["sat"]
    hue_shift = 0.0
    for arc in p["hue_arcs"]:
        w = in_hue_arc(hd, arc["lo"], arc["hi"])
        sat += arc.get("sat", 0.0) * w
        hue_shift += arc.get("shift", 0.0) * w
    # shadows keep less saturation (creamy, avoids noisy chroma in the toe)
    sat = lerp(1.0, sat, smoothstep(0.0, p["sat_toe"], v)) if p["sat_toe"] > 0 else sat
    s = clamp01(s * sat)
    h = ((hd + hue_shift) % 360.0) / 360.0
    return colorsys.hsv_to_rgb(h, s, v)


def write_cube(path, title, p):
    n = SIZE
    with open(path, "w") as f:
        f.write(f'TITLE "{title}"\n')
        f.write(f"LUT_3D_SIZE {n}\n\n")
        for bi in range(n):
            for gi in range(n):
                for ri in range(n):
                    r, g, b = (ri / (n - 1), gi / (n - 1), bi / (n - 1))
                    r, g, b = apply_look(r, g, b, p)
                    f.write(f"{r:.6f} {g:.6f} {b:.6f}\n")
    print(f"wrote {path}")


# ---------------------------------------------------------------------------
# The looks. All tunables in one place.
LOOKS = {
    # Pale mid-day desert -> pop: warm bias, solid S-curve, rich global color,
    # deeper teal-leaning skies, richer oranges in the ground tones.
    "OneGrade Desert Day": dict(
        warm=0.03,
        toe=0.0,
        contrast=0.35,
        shoulder=0.25,
        sat=1.25,
        sat_toe=0.10,
        hue_arcs=[
            dict(lo=195, hi=255, sat=0.15, shift=-8.0),   # skies: deeper, toward teal
            dict(lo=20,  hi=60,  sat=0.10),               # sand / rock oranges
        ],
    ),
    # Creamy outdoor look: lifted soft shadows, early smooth shoulder, gentle
    # contrast, greens enriched, low-chroma toe. "Smooth, not punchy."
    "OneGrade Cinematic Landscape": dict(
        warm=0.015,
        toe=0.04,
        contrast=0.12,
        shoulder=0.45,
        sat=1.05,
        sat_toe=0.20,
        hue_arcs=[
            dict(lo=70,  hi=160, sat=0.20, shift=-4.0),   # foliage: richer, slightly emerald
            dict(lo=195, hi=250, sat=0.08),               # skies: a touch more presence
        ],
    ),
    # Low-sun amber glow: strong warmth into gently magenta-warm shadows, early soft
    # shoulder, golds enriched, skies calmed. Romantic, glowy — nothing like the
    # punchy Desert Day.
    "OneGrade Golden Hour": dict(
        warm=0.055,
        toe=0.02,
        contrast=0.10,
        shoulder=0.40,
        sat=1.10,
        sat_toe=0.15,
        split=dict(shadows=0.02, highlights=0.05),
        hue_arcs=[
            dict(lo=20,  hi=70,  sat=0.15),            # golds richer
            dict(lo=195, hi=255, sat=-0.10),           # skies recede
        ],
    ),
    # The blockbuster split: teal shadows vs warm highlights/skin, solid contrast,
    # pinned blacks. Punchy and modern.
    "OneGrade Teal Orange": dict(
        warm=0.0,
        toe=0.0,
        contrast=0.30,
        shoulder=0.20,
        sat=1.12,
        sat_toe=0.12,
        split=dict(shadows=-0.07, highlights=0.045),
        hue_arcs=[
            dict(lo=10,  hi=50,  sat=0.12),            # skin/orange presence
            dict(lo=170, hi=230, sat=0.10),            # teal enrich
        ],
    ),
    # Skip-bleach: silver and gritty — strong contrast, heavily muted color, faintly
    # cool cast, controlled whites. The desaturated corner of the look-space.
    "OneGrade Silver Bleach": dict(
        warm=-0.01,
        toe=0.0,
        contrast=0.45,
        shoulder=0.30,
        sat=0.60,
        sat_toe=0.25,
        hue_arcs=[],
    ),
    # Cool low-key mood: blue cast deepening in the shadows, muted warms, blues kept
    # alive. Night exteriors, thrillers, blue-hour city.
    "OneGrade Midnight Blue": dict(
        warm=-0.045,
        toe=0.0,
        contrast=0.25,
        shoulder=0.35,
        sat=0.85,
        sat_toe=0.30,
        split=dict(shadows=-0.03, highlights=0.0),
        hue_arcs=[
            dict(lo=200, hi=260, sat=0.15),            # blues stay alive
            dict(lo=20,  hi=60,  sat=-0.10),           # warms muted
        ],
    ),
}

if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    for name, params in LOOKS.items():
        write_cube(os.path.join(here, f"{name}.cube"), name, params)
