# Ground-truth log curves — measuring Resolve instead of trusting a spec sheet

Extracts the scene-linear transfer function of every log curve Resolve implements, so
`og::decode_log` can be checked against a second implementation of the same transform
rather than against a whitepaper someone retyped.

**It found two bugs that had shipped since the camera list was written**, neither of
which was visible by reading the code:

| | our mid-gray | Resolve | error | |
|---|---|---|---|---|
| Canon Log 3 | 0.43377 | 0.33095 | **1.33 EV** | a missing input remap |
| DJI D-Log | 0.14004 | 0.39876 | **2.00 EV** | mangled constants, plus a **47× step across its own knee** |

Canon's own published figure for 18% gray is 32.8 IRE, which is Resolve's 0.331 and not
our 0.434 — so this was not a matter of taste. The other ten cameras matched Resolve to a
**median of 0.000 EV**, which is the result that makes the two failures trustworthy: the
method is not biased toward finding problems.

## The method, and the one step you must not skip

A 4096-wide neutral 16-bit ramp is rendered through Resolve's RCM with the **input gamut
pinned equal to the timeline and output gamut**, so the 3×3 is identity and only the
transfer function acts. Output gamma is **DaVinci Intermediate**, chosen because it holds
~9 stops over mid-gray inside [0,1] so nothing clips on the way out, and because we
already own its exact inverse as camera 1.

**Run the identity pass first.** It renders DaVinci Intermediate → DaVinci Intermediate
and the ramp must come back unchanged. If it does not, every curve measured afterwards is
describing the harness. Here it measured **0.498 of a 16-bit LSB**, with R=G=B exact.
This is the same discipline as verifying a test bites: prove the instrument before
trusting the reading.

## What the measurement cannot see

- **Negatives.** A 16-bit PNG clamps at 0, so the toe below each curve's zero crossing
  reads a flat 0.0. That is not evidence. Fit the log branch to this data, take the toe
  from the published form, and check the two for continuity at the knee — which is exactly
  how the D-Log step was found and fixed.
- **The top of the widest curves.** DaVinci Intermediate clips at linear 100, so anything
  reaching +9.12 EV is flat up there. Nine stops over mid-gray, so it does not matter.

## Running it

```bash
make                                  # builds makeramp and genheader
mkdir -p /tmp/ogcurves
./makeramp /tmp/ogcurves/ramp.png     # 4096x256 neutral 16-bit ramp
# then, with Resolve Studio open, run extract.py in its scripting environment
./genheader /tmp/ogcurves ../../test/reference/resolve_log_curves.h
make -C ../.. test
```

`extract.py` creates its own scratch project and deletes it afterwards, so an open project
is not touched.

## Enumerating what Resolve supports

`SetSetting("colorSpaceInputGamma", name)` returns false for a name Resolve does not know,
which makes it a usable enumerator: probe candidates and keep what sticks. Worth knowing:

- **`GoPro GP-Log2` exists**, and is a strikingly low-range curve — mid-gray at code
  **0.54169** with only **+4.27 EV** of headroom, against +7.87 for D-Log and +8.26 for
  LogC3. Decoding it with a cinema log curve overstates exposure by ~2.3 stops at the
  median and ~3.6 stops at p99, which is what "overblown in both directions" looks like.
- **`DJI D-Log M` does not exist** in Resolve 21.1 — only `DJI D-Log` and `DJI D-Log2`.
  D-Log M has to be sourced from DJI's own LUT instead, and that LUT is display-referred
  with a tone curve baked in, so it is a weaker reference than anything here.

## Rec.2100 HLG is still open

PQ differs from Resolve by a **dead-constant ×0.4926**, which is our deliberate 203-nit
reference-white normalisation and is asserted as a constant by the test. HLG does not: its
ratio spans **0.377…2.475**, so part of that is shape, not normalisation. Recorded in
`docs/ROADMAP.md`; the test prints it every run rather than asserting it, so it cannot
quietly become accepted.
