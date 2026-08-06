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
shot on a different camera, with Scene Exposure already at −0.50, so part of its correction
happened upstream of Gain.

**The clamp at the preset value for `key ≥ 0` is the important half.** A dark shot is never
pushed up. `key` describes where a shot sits; it does not prescribe where it should sit — a
low-key interior is *supposed* to sit low, and chasing 18% grey would flatten every moody
shot into mid-gray mush. Refusing to act in that direction handles it without a special
case. The 0.30 floor exists because the fit is only evidenced out to about −2 EV, and the
bare line reaches zero near −4 EV.

### The black point is solved, not stamped

The preset writes **Lift 0.11** on every shot, and a fixed lift lands a *different* black point
depending on where the footage's own floor already sits:

| shot | Creative lift | user's lift | Creative black point |
|---|---|---|---|
| beach | 0.050 | −0.011 | 0.229 |
| city | 0.110 | 0.066 | 0.161 |
| car | — | *"lifts shadows a bit too much"* | |

Three hand grades, all corrected downward. **Base has always solved its floor to a target;
Creative stamped a constant, and that difference is the whole defect.** It is now bisected on
the same monotonic curve, against the `Creative Black` tunable (default 0.050).

Note the beach and city need Lift moved in **opposite directions** — −0.019 and +0.034 — to
land the same black point. That is exactly what a constant cannot do.

> **To fit `Creative Black`:** grade a shot by hand until it looks right, hit Analyze, and read
> the **Tone** row's graded `blk`. That number *is* the target.

### The anti-crush floor, and a bug the change created

Bias's −0.06-per-unit lift offset was calibrated against the fixed 0.11, which left room
underneath. With Creative solving to −0.019 on the beach, **Bias −1 took lift to −0.079 and the
black point to −0.03 — crushed**, from two individually correct changes.

Fixed as a floor in `applyBias` rather than by retuning the coefficient: the coefficient is
taste, the floor is a fact. Bias keeps its full range and stops removing shadows once there are
none left.

Bias also runs **±2** now. Measured black point: 0.050 at 0, ~0.18 at +1, ~0.25 at +1.5, ~0.33
at +2, against the old stamped 0.229 / 0.161 — so the extra travel is new headroom rather than a
restoration. The negative half now does much less to shadows (−1 and −2 both land on the floor),
which is correct but makes the slider asymmetric about zero.

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
measure skin `R/G` of **1.21** and **1.22** — indistinguishable — yet one was warmed to Scene
White Balance 9242 and the other left at 6500. The warmed one is also the only shot on a
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

**In a shipping build the panel shows two controls: Auto Grade and Bias.** That is
deliberate — it is what a colorist needs, and the measurements are of interest to whoever
is *tuning* the thing, not to whoever is using it.

### Turning the analysis UI back on

The debug surface — the **Show analysis** checkbox, the **Analyze Frame** button, the nine
measurement rows and the **Applied** readout — is hidden behind a compile-time switch in
`src/OneGrade.cpp`:

```c
static const bool kAnalysisDebugUI = false;   // -> true, rebuild
```

> **On the `feat/scene-descriptors` branch this is currently `true`**, because the Colour /
> Regions / Response rows exist to be read on footage. It must go back to `false` before the
> branch merges into a release.

Flip it, rebuild, and the checkbox reappears and toggles the rest at runtime. The params
always exist and always work; only their visibility changes, so saved projects and the
measurement itself are unaffected either way.

**This is the mode to be in when fitting new constants or working out why a shot analysed
oddly.** Every fit in this document was found by staring at those rows across real footage —
`pin` versus `hot` for rolloff, `key` versus hand-set Gain, the skin coverage % that exposed
a mask measuring its own filter. There is no way to extend this feature responsibly without
them.

| Row | Reads |
|---|---|
| **Result** | frame size, sampling step, sample count. `ALL ZERO` means the host returned an empty buffer — a different thing from a black shot. |
| **Scene** | `Y50`, `key` in stops, `DR` (p1→p99). Encode- and grade-independent. |
| **Display** | p1 / p50 / p99, and which encode they were measured in. |
| **Peak** | p99.9 and how far it runs past p99. |
| **Shape** | `hot` (above display white), `pin %@ceiling` (clipped at source), mid-tone saturation. |
| **Subject** | skin coverage %, skin-masked key, skin `R/G` and `B/G`. High coverage means the mask matched the scene, not a face. |
| **Colour** | mid-tone `a*` / `b*` / `C` / `sep`, at NEUTRAL — describes the footage, not the grade on it. |
| **Graded** | the same, for the grade actually on the node. The one that moves. Measured pre-LUT. |
| **Regions** | the two colour populations (share + hue, cooler first) and `db*`. |
| **Separation** | the triple, neutral → graded: `dL*` tone, `da*` / `db*` hue. |
| **Drives b\*** · **Drives dL\*** · **Drives db\*** | which controls produced each change: measured, linear-predicted, and the top three contributors. A large act/lin gap means the grade sits outside the linear range. |
| **Response** | measured Jacobian rows: how far `b*` moves per nudge of each balance control on *this* shot. |
| **Applied** | what Auto Grade last wrote and the measurement behind it. |

**Analyze Frame** only measures and reports; it never changes the picture. Only **Auto
Grade** writes values.

---

## 9. Scene descriptors and the control Jacobian

Everything above answers *how is this frame exposed*. `src/OneGradeAnalysis.h` answers **what
colour is it, and what would each control do about that** — the half that was missing when a
sunset-over-ocean grade reached for **Offset Temp** to separate water from sky and no measured
number could have asked for it.

It is a separate header in its own namespace (`og::analysis`, aliased `oga`) because **it is
not part of the golden rule**. `OneGradePipeline.h` is the single source of truth that the
three GPU kernels mirror; nothing here is mirrored and nothing here may be called from a
kernel. It runs once per button press, on the CPU, over ~40k samples, and produces parameter
values rather than pixels.

### Descriptors

Thirteen numbers, in CIELAB rather than HSV — `b*` **is** the warm/cool axis and `a*` **is**
green/magenta, which lines them up one-for-one with the Temp and Tint controls and keeps the
Jacobian well conditioned instead of smearing one control across several rows.

**Signed axes steer. Magnitudes do not.** Measured on the beach sunset, neutral → grade,
linear prediction against measurement:

| descriptor | kind | error |
|---|---|---|
| `b*` | signed axis | **5%** |
| `C*` | magnitude, `√(a²+b²)` | 37–57% |
| `sep` | distance between centroids | **wrong sign** (+1.1 predicted, −3.8 measured) |

A distance is a positive quantity built from squares, so a linear model cannot express "apart
in `a`" cancelling "together in `b`". `C*` and `sep` are therefore **report-only** — they
measure honestly and only fail as solve targets. `kSteerableDescN` makes that structural, and
test 24 pins it so a future edit can't let a distance back into a solve.

### The separation triple

The user's definition of what makes a frame dynamic: *"push those objects to be more separated
from others of a different **hue or tone level**."* Two axes. The original `sep` was a distance
in `(a*, b*)` — hue and chroma only, with **no tone axis in it at all**.

It is now the three signed components of the Lab difference between the frame's two regions:

- **`dL*`** — tone separation
- **`da*` / `db*`** — hue separation

Band means are taken in `L*` rather than display luma so all three live in one space and are
comparable with each other.

**Regions are the top and bottom third, and that is a stand-in.** It works on landscape footage
because a landscape separates its objects by height — `db*` came back **+43** and found
sky-over-water cleanly, where PCA-seeded 2-means in `(a*, b*)` returned two populations *both*
at orange (h29 and h44). It fails the moment the subjects aren't stacked vertically: two people
side by side, a car against a wall, a face against a window.

**This is the seam where segmentation plugs in.** Supplying real region masks changes nothing
else in this file, because the descriptors only ever ask for *region A minus region B*. It is
also the first concrete argument for a classifier in this project — not for exposure, which the
numbers already handle, but for **region identity**.

**Membership is fixed at neutral, and this is the whole ballgame.** The mid-tone window, the
skin mask, and which cluster a pixel belongs to are decided **once**, by `classify()`, from
the ungraded render. `describe()` only ever recomputes statistics over those fixed masks.
This is the same trap the skin mask already fell into once — a selection rule that constrains
the quantity being measured describes the filter, not the footage. If the mid-tone window
were re-selected after every perturbation, "the mids got brighter" would be unmeasurable by
construction, because the window would slide along with them. Cluster membership is a
property of the *footage*; the grade is what gets differentiated.

### The Jacobian

**We do not write down what the controls mean.** `jacobian()` perturbs each control by one
natural step, central-differenced, and measures which descriptors moved. That the system
knows negative Offset Temp adds blue is a *measurement*, not a table — so it cannot drift when
the pipeline changes (same reasoning as `og_solve()` using bisection over a closed form), it
is testable, and it is shot-dependent for free.

`solve_intent()` inverts it by damped least squares. The damping earns its place: twelve
descriptors against thirteen controls, several nearly redundant (Gain and Post Exposure both
raise the midtone), so an undamped solve finds an enormous cancelling pair that is correct to
first order and absurd on the picture. Damping buys the *smallest* move that gets close —
which is also the one a colourist would make.

Verified in `test/pipeline_test.cpp` (tests 15–21): the error falls ~4× per halving of the
move, which is the signature of a real derivative rather than a plausible-looking table.

### Two controls are not differentiable at their defaults

Both were found *by* the Jacobian, not looked for, and both are properties of the shipping
pipeline. `steer_mask()` excludes them.

| Control | What happens |
|---|---|
| **Highlight Rolloff** at 0 | `softclip()` early-outs at `amt <= 0` but asymptotes hard at 1.0 for any `amt > 0`. `softclip(1.26)` goes **1.26 → 1.00000** between `amt` 0 and 0.0001; measured mean overshoot goes 0.031 → 0.000 across the same gap. The control has two regimes — *is there a ceiling at all* (a step at 0) and *how far down does the knee reach* (the rest, smooth). |
| **RAW Temperature** at 6500 K | `white_balance()` forces identity on `6499 < T < 6501`, but the Kim et al. **Planckian** locus at 6500 K is (0.31349, 0.32366) while D65 is (0.31270, 0.32900) — `dy = -0.0053`, because D65 is a *daylight* illuminant and sits above the blackbody locus. The skipped adaptation is therefore not an identity, and stepping 1 K off the default jumps a neutral mid-gray by `a* +2.06`: rgb `0.55397 0.55397 0.55397` → `0.54311 0.55807 0.54535`. A green cast appears out of nowhere on the first nudge. |

The rolloff behaviour may well be intended — it is a soft clip *to* 1.0 by definition. The RAW
Temp one looks like a plain defect: adapting to `blackbody(6500)` instead of to D65 would make
the stated "identity at 6500 K" contract true by construction and remove the early-out
entirely. **That is a colour-math change and therefore a four-file kernel edit**, and it moves
every saved grade with RAW Temp ≠ 6500, so it is a deliberate decision rather than a fix to
slip in. Test 20 pins both, so if either is ever changed that test fails first and says so.
