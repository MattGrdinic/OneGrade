# Auto Grade — measuring a frame and turning it into a starting point

*Experimental. Everything here was fitted to a small set of hand-graded shots and is
expected to move as more footage goes through it.*

One button reads the frame at the playhead, measures it, and writes ordinary slider
values: the Cinematic Film Emulation look, with Gain set from the shot's exposure and
Highlight Rolloff set from its clipping. A **Bias** slider then leans the whole tonal
range warmer-to-the-highlights or open-to-the-shadows, live.

The design rule that everything else follows from: **the button writes slider values, not
pixels.** It sets the same controls you would have set by hand, so you can drag any of
them afterwards and a bad analysis costs one undo rather than your trust in the tool.

---

## 1. Why this is a param-layer feature

`og::process()` and the three GPU kernels are untouched by any of this. That matters more
than it sounds:

- **No golden-rule mirror.** Analysis lives entirely in `OneGrade.cpp`, so there is no
  fourth implementation to keep in step and no CPU-vs-GPU reduction to reconcile.
- **No temporal instability.** The measurement happens once, on click. Nothing changes
  frame to frame, so there is no flicker across a cut and no frame that renders differently
  the second time Resolve asks for it.
- **Nothing to undo but sliders.** The output is visible state in the panel.

The one thing that had to be proven before any of it could be designed was whether a host
hands over pixels from *outside* a render call. Resolve does — validated 2026-08-02, a 4K
frame returned from `fetchImage()` inside `changedParam`.

---

## 2. What is measured

`probeAnalyze()` walks a coarse grid — step chosen so no frame yields more than ~200k
samples, which is far more than percentiles need and keeps the button off the UI thread's
back on an 8K plate. Each sample goes through the **real pipeline**, not a lookalike:

| Quantity | How | Why that way |
|---|---|---|
| **Scene luminance** | `decode_log` → `to_XYZ`, take **Y** | Exact and gamut-agnostic. Rec.709 luma weights are simply wrong against DWG primaries. |
| **Display values** | `og::process()` at neutral params | What the node would actually render with the grade zeroed. |
| **Source ceiling** | max input code, then the share sitting on it | Detects clipping without assuming where the ceiling is (see §4). |
| **Skin** | hue 0.01–0.11, sat 0.10–0.65, in display RGB | Chromaticity only — see the trap in §5. |

Percentiles come from `nth_element` over the kept samples rather than a histogram: at this
sample count the memory is under a megabyte and there is no bin width to argue about.

Two derived numbers do the real work:

```
key = log2(0.18 / Y50)          exposure distance from mid-gray, in stops
pin = share of samples sitting on the source ceiling, in %
```

### The Scene row is grade-independent

`Y50`, `key` and `DR` are measured before any grade or encode, so **clicking Analyze on an
already-graded node still describes the footage.** The same frame read `Y50 0.6236 /
key -1.79 / DR 6.2` both ungraded and with a Custom Look at full mix, while the Display row
moved as the LUT lifted blacks. No tail-chasing, and no "only use this on a fresh node"
caveat.

### The Display row always lands in a display-referred space

It starts from the *effective* encode — the same LUT override the render applies — but
falls back to Gamma 2.2 when that is not display-referred, and prints which it used
(`@Scene` / `@2.2` / `@2.4`).

This is not fussiness. A Film Look forces **Cineon**, and analysing in Cineon silently
breaks two things at once: Cineon clamps to [0,1], so nothing can ever read as above white,
and it compresses chroma, so the skin mask stops matching faces. Both were observed on one
shot when only its LUT mode changed — `hot` fell 22.9% → 0.0% and skin coverage collapsed
to 1.6%, with no change to the picture at all.

> **Percentile and hue thresholds are only meaningful in the space they were chosen for.**

---

## 3. Gain, from exposure

Four hand-graded shots turned out to be the Cinematic Film Emulation preset with exactly
one value moved per shot: **Gain**. Lift, gamma, density, trim, and the
`Gain Temp -0.220 / Gain Tint 0.090` tint that gives the look its character were identical
across all four. One of the four grades *is* the untouched preset.

| shot | key | Gain (hand-graded) | `0.80 + 0.19·key` |
|---|---|---|---|
| car interior | +2.60 | 0.800 | 0.800 (clamped) |
| desert flat | −0.79 | 0.642 | 0.650 |
| interview | −1.04 | 0.655 | 0.602 |
| cactus | −1.96 | 0.407 | 0.428 |

```
gain = clamp(0.80 + 0.19 · key, 0.30, 0.80)
```

Three of four land within 0.02. The interview is the outlier and explains itself: the only
shot on a different camera, with RAW Exposure already at −0.50, so part of its correction
happened upstream of Gain.

**The clamp at the preset value for `key ≥ 0` is the important half.** A dark shot is never
pushed up. `key` describes where a shot sits; it does not prescribe where it should sit — a
low-key interior is *supposed* to sit low, and chasing 18% grey would flatten every moody
shot into mid-gray mush. Refusing to act in that direction handles it without a special
case. The 0.30 floor exists because the fit is only evidenced out to about −2 EV, and the
bare line reaches zero near −4 EV.

---

## 4. Highlight Rolloff, from source clipping

Rolloff is **not** a function of how much of the frame is bright:

| shot | `hot` | **`pin`** | p99.9/p99 | Rolloff (hand-graded) |
|---|---|---|---|---|
| cactus | 33.7% | **0.00%** | ×1.12 | **0** |
| car | 6.4% | **0.00%** | ×1.05 | **0** |
| desert dirt | 0.0% | **0.00%** | ×1.05 | **0** |
| interview | 17.8% | **6.18%** | ×1.00 | **0.557** |

```
rolloff = min(0.80, 0.090 · pin)
```

`hot` runs backwards — the 33.7% shot got no rolloff and the 17.8% shot got the most. So
does `p99.9/p99`: a large blown window puts p99 and p99.9 on the *same plateau*, so the one
shot that needed rolloff scores the *lowest* multiplier of the four.

`pin` is the whole signal, and it is the physically right answer. Rolloff exists to soften
flat, detail-free patches, and clipped-at-source *is* flat and detail-free. A merely bright
frame keeps its texture and needs nothing.

### Do not test source clipping against 1.0

The first attempt used `input ≥ 0.995`. A raw waveform with the node disabled showed
**Blackmagic log peaking around 768/1023 ≈ 0.75**, with a textured, unpinned top — the range
really was captured. A fixed 1.0-ish threshold therefore reports 0% on *every* Blackmagic
shot, blown ones included: accidentally right on that frame, broken in general.

What identifies clipping is a **pile-up at whatever this clip's own maximum is**. A real
highlight rolls off with falling density; a clipped one stacks samples on the ceiling. So
`pin` finds the observed max and counts the share within a small epsilon of it, reported as
`pin %@max`. Self-calibrating, so it generalises across cameras and log formats without a
per-format table.

---

## 5. What is deliberately *not* set

**Warmth.** Only two of the four test shots contain an actual face (skin coverage 3.4% and
10.3%; the other two, at 46.5% and 72.2%, are the mask eating desert sand). Those two
measure skin `R/G` of **1.21** and **1.22** — indistinguishable — yet one was warmed to RAW
Temperature 9242 and the other left at 6500. The warmed one is also the only shot on a
different camera, which points at a property of the shoot rather than of the image.

Gain and Rolloff each have a hard physical anchor: distance from mid-gray, and pixels dead
at the sensor ceiling. Warmth has none. **Some of what a colorist does is correction and
some is taste, and a button that guesses at taste is worse than one that leaves it alone**,
because you have to undo it every time instead of just making the move yourself.

Density is untouched for the same reason.

### The skin mask, and a trap worth remembering

The mask selects on **chromaticity only**. The first version also gated on display luma
0.15–0.95, which is self-fulfilling: it picks mid-tone pixels by construction, so their
median lands near mid-gray and the subject key reads about zero on any shot. It was caught
on a desert frame reporting masked `Y 0.2100` against a frame median of `0.6236` — a filter
artefact presented as a measurement.

> **A selection rule that constrains the quantity being measured produces a number that
> describes the filter, not the footage.**

The mask cannot tell skin from sand, and is not asked to. Its **coverage %** is printed next
to the key precisely so a nonsense read announces itself: 3–10% is a plausible face, 60%+ is
a landscape.

---

## 6. Bias

One slider, applied live once Auto Grade has run. It moves four controls together so the
result stays a coherent picture rather than a lifted floor on an otherwise unchanged image.

```
rolloff = clamp(0.090·pin − bias·0.35, 0.00, 0.80)
lift    = clamp(0.11    + bias·0.06, −0.50, 0.50)
gamma   = clamp(1.00    + bias·0.12,  0.20, 3.00)
gain    = clamp(measured_gain + gain_delta, 0.20, 1.20)

headroom   = max(0, 1 − hot/40)
gain_delta = bias ≥ 0 ? bias·0.08·headroom : bias·0.08
```

- **Negative — protect.** Shoulder the highlights, deepen the floor, darken the mids, pull
  Gain down. For blown windows and hot speculars.
- **Zero.** The measured result, untouched.
- **Positive — open.** Drop the shoulder, raise the floor, brighten the mids and Gain.

Gain's response is the measurement-modulated one: brightening a frame that already has a
third of itself above display white only pushes more of it past clipping, so the positive
direction is scaled by remaining headroom and fades to nothing by about 40% `hot`. The
negative direction is never scaled — pulling Gain down is always safe.

Bias re-derives from the **cached** measurement, so a drag is arithmetic on two stored
numbers and keeps up in real time. It is gated on Auto Grade having actually run: dragging
it on a node that was never auto-graded must not silently stamp values. That state does not
persist, so after a project reload the slider is inert until Auto Grade is pressed again —
deliberately inert rather than acting on a stale measurement.

---

## 7. Known limitations

- **A shot whose exposure changes mid-take.** A clip that cranes from a bright exterior into
  a dark interior gets an excellent exterior and too dark an interior from one click, and an
  excellent interior from a second click inside. This is single-frame analysis meeting a
  multi-stop change; splitting the clip is the right answer. Being frame-based is a feature
  here — park the playhead on the beat that matters and the button optimises for it.
- **Fitted to four shots from one shooter.** The constants describe one person's taste,
  which is the right target for a button in their own plugin, but this is not a general
  auto-grade and should not be presented as one.
- **The rolloff constant rests on a single non-zero point.** `0.090` is a line through the
  origin with three controls correctly at zero. A `pin` case at a different clip percentage —
  a practical lamp rather than a window — is the test that would confirm or break it.
- **The film stock is always Kodak 2383.** Choosing a *look* from statistics is a far weaker
  inference than setting a *level* from a measurement, so it isn't attempted.

---

## 8. Reading the panel

Tick **Show analysis** to reveal the measurements. Off by default so the panel stays a
grading panel; on when a shot behaves oddly and the numbers matter.

| Row | Reads |
|---|---|
| **Result** | frame size, sampling step, sample count. `ALL ZERO` means the host returned an empty buffer — a different thing from a black shot. |
| **Scene** | `Y50`, `key` in stops, `DR` (p1→p99). Encode- and grade-independent. |
| **Display** | p1 / p50 / p99, and which encode they were measured in. |
| **Peak** | p99.9 and how far it runs past p99. |
| **Shape** | `hot` (above display white), `pin %@ceiling` (clipped at source), mid-tone saturation. |
| **Subject** | skin coverage %, skin-masked key, skin `R/G` and `B/G`. High coverage means the mask matched the scene, not a face. |
| **Applied** | what Auto Grade last wrote and the measurement behind it. |

**Analyze Frame** only measures and reports; it never changes the picture. Only **Auto
Grade** writes values.
