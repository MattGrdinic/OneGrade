# Node Role: PowerGrade across Resolve's group grading levels

PowerGrade normally does the whole job in one node: camera decode, balance, density,
grade, output encode, LUT, trim. That's the **Full Grade** role, and it's the default.

But Resolve gives every clip four grading levels, and colorists working at volume lean on
them heavily. **Node Role** lets one PowerGrade node be split in two so it fits that
structure instead of fighting it. This doc covers how the levels work, what the split
does, why DaVinci Intermediate is the hand-off, and the clipping bug the design exposed.

## 1. Resolve's four grading levels

Every clip in a group is rendered through four node graphs, in this order:

```
Group Pre-Clip  →  Clip  →  Group Post-Clip  →  Timeline
```

- **Group Pre-Clip** and **Group Post-Clip** are *one graph each per group*, shared by
  every clip in it. Edit a node there and it changes for all of them at once.
- **Clip** is the ordinary per-shot graph — curves, primaries, secondaries, windows.
- **Timeline** is a single graph for the whole timeline, after everything else.

A group is made on the Color page: select clips in the thumbnail bar, right-click → *Add
into a New Group*. The Node Editor's mode selector (top-left of the Nodes panel) chooses
which of the four graphs you're editing.

The conventional colorist setup is an input transform in Pre-Clip, per-shot work at Clip
level, and the output transform in Post-Clip. One place to change the delivery curve for
a whole group; per-shot grading untouched.

**One constraint matters:** a clip can belong to only **one** group at a time. So you
group by camera (to share an input transform) *or* by scene/look (to share a look), not
both. That tension is inherent to the structure, not to any plugin.

## 2. What the three roles do

`nodeRole` — the first control in group **0 Role / Preset**.

| Role | Owns | Camera | Output Encode |
|---|---|---|---|
| **0 Full Grade** (default) | everything | your pick | your pick |
| **1 Input Transform** | camera decode + RAW exposure/temp | your pick | pinned to **DaVinci Intermediate** |
| **2 Output Transform** | balance, density, LGG, LUT, trim, delivery | pinned to **Blackmagic (DWG/DI)** | your pick |

Chained, **role 1 → role 2 reproduces role 0.** Put an Input Transform node in Group
Pre-Clip, grade shots normally at Clip level, put an Output Transform node in Group
Post-Clip, and the result matches a single Full Grade node.

Controls a role doesn't own are greyed out in the panel **and forced neutral at render**
(`PowerGrade::setupAndProcess`). The render-time enforcement is the important half: it
means switching roles, or loading a project saved under a different role, can never
double-apply the look or apply the RAW stage twice. The UI greying is a convenience; the
render path is the guarantee.

`changedParam` also stamps the values a role implies (role 1 sets the encode to DI and
resets the look; role 2 sets Camera to DWG/DI and zeroes RAW), but only on
`eChangeUserEdit` — the same guard the presets use, so a project load never re-stamps
over values you've since tweaked.

## 3. Why DaVinci Intermediate is the hand-off

The two nodes have to meet in some encoding, and DI is the right one for three reasons.

**It's an exact inverse pair.** Camera index 1 (`Blackmagic DWG/DI`) decodes with

```
A=0.0075  B=7.0  C=0.07329248  M=10.44426855
```

and output encode 4 (DaVinci Intermediate) encodes with *the same constants*
([PowerGradePipeline.h](../src/PowerGradePipeline.h), `decode_log` and `encode`). One is
the algebraic inverse of the other, so the round trip costs only float rounding.

**Primaries don't move.** Encodes 0–3 (Scene, 2.2, 2.4, Cineon) convert to Rec.709
primaries; DI and Linear keep the DaVinci Wide Gamut working primaries. So the pre-clip
node hands off in DWG and the post-clip node picks it up in DWG — no gamut round trip, no
out-of-gamut excursion introduced at the boundary.

**It has enormous headroom.** `di_encode` maps linear **100 → code 1.0**, with mid-gray
0.18 landing at **0.336**. That's roughly **nine stops above mid-gray inside [0,1]**.
Real footage essentially never exceeds it, so the hand-off carries the full highlight
range comfortably.

That last point reframes where the risk in *any* hand-off encode actually lies. The
fragile end isn't the highlights — it's the **negatives**. Out-of-gamut colors sit just
below zero, and anything that clamps at 0 destroys them silently.

### Measured accuracy

`test/pipeline_test.cpp` test 11 chains role 1 into role 2 and compares against a single
full node, across 12 cameras × 3 delivery encodes, both neutral and with a real look on
the output node. Worst case:

| Delivery encode | Worst \|split − single\| |
|---|---|
| Rec.709 (Scene) | 0.23 × one 8-bit code value |
| Rec.709 (Gamma 2.2) | 0.003 × one 8-bit code value |
| Rec.709 (Gamma 2.4) | 0.003 × one 8-bit code value |

The test fails the build above 1 LSB.

### Validated in Resolve

macOS / Metal / M3 Max / DaVinci YRGB, 2026-08-02. The split is visually identical to a
single node on footage, and — the open question at design time — **Resolve does not clamp
float between group grading levels.** Out-of-gamut negatives survive the boundary,
confirmed on the RGB Parade with the Post-Clip node disabled: the trace sits visibly below
the 0 line. No offset or Cineon fallback hand-off is needed.

## 4. The clipping bug this exposed

The split failed on its first test by about **30 8-bit code values**, and the cause turned
out to be a defect that had nothing to do with groups.

[PowerGradePipeline.h](../src/PowerGradePipeline.h) defines

```c
static inline float safe_pow(float b, float e) { return powf(b < 0.f ? 0.f : b, e); }
```

The floor at zero is there because `powf` of a negative base with a fractional exponent is
NaN. But the Lift/Gamma/Gain loop called it unconditionally:

```c
v = safe_pow(v, 1.0f/gamma);
```

With gamma at its neutral 1.0 that is still a hard clamp. So **a completely neutral
PowerGrade node clipped every negative value to zero.** Out-of-gamut negatives are normal
and expected after a camera-gamut → DWG conversion; they were being destroyed on every
frame.

Diagnostics ruled everything else out: white balance at 6500 K is a bit-exact identity,
the DWG↔XYZ round trip is accurate to 1.5e-8, and the neutral LGG curve round-trips to
3.2e-7. The clamp was the sole cause.

The fix is to leave negatives alone through the gamma step:

```c
v = (v < 0.f) ? v : safe_pow(v, 1.0f/gamma);
```

Mirrored, per the golden rule, into `MetalKernel.mm`, `OpenCLKernel.cpp` and
`CudaKernel.cu`.

### Why this was safe to change

Measured across 12 cameras on both a neutral grade and the Cinematic Film preset's recipe:

| Encode | Output change from the fix |
|---|---|
| Rec.709 (Gamma 2.2) — *the default* | **exactly zero** |
| Rec.709 (Gamma 2.4) | **exactly zero** |
| Cineon — *the film preset path* | **exactly zero** |
| Rec.709 (Scene) / DaVinci Intermediate / Linear | negatives preserved instead of clamped |

Zero on 2.2 and 2.4 because `r709_g_enc` already clamps negatives at the *entry* to the
LGG loop, upstream of the gamma step; zero on Cineon because that encode clamps to [0,1]
regardless. So every user-validated look and both film presets render bit-identically.

The three encodes that *do* change are exactly the ones documented in the source as
scene-referred feeds to downstream nodes — where clipping was never correct. The comment
above `softclip()` says the highlight roll-off is deliberately gated off for
"Cineon/DI/Linear feeds to downstream nodes"; the LGG clamp was quietly violating that
same principle one step earlier.

Test 12 in `test/pipeline_test.cpp` guards it: a neutral node must produce negatives on
the DI hand-off for saturated out-of-gamut input.

## 5. Using it

**Group setup, start to finish:**

1. Color page → select the clips → right-click → *Add into a New Group*.
2. Node Editor mode selector → **Group Pre-Clip**. Add PowerGrade, set
   **Node Role → Input Transform**, set **Camera** to your source.
3. Mode selector → **Clip**. Grade shots normally. PowerGrade isn't involved.
4. Mode selector → **Group Post-Clip**. Add PowerGrade, set
   **Node Role → Output Transform**, then pick a Preset or set **Output Encode** to your
   delivery curve.

In the Input Transform role the panel shows **three live controls** — Camera, RAW
Exposure, RAW Temperature. Everything else is greyed.

**When to use Full Grade instead.** If you aren't grouping, or you want the whole
transform-and-look chain to travel as a single node (one still, one copy-paste, portable
across projects), Full Grade is simpler and it's why it stays the default.

**Cost.** The split means two OFX instances per clip rather than one — two GPU passes.

## See also

- [GAMMA.md](GAMMA.md) — transfer functions, the grade curve, and the output encodes
- [CAMERAS.md](CAMERAS.md) — input transforms and the DWG working space
- [LUTS.md](LUTS.md) — LUT discovery, sampling, and how the LUT path forces the encode
