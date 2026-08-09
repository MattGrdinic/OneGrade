# Third-party notices

OneGrade itself is GPL-3.0-or-later (see [LICENSE](LICENSE)). It bundles the components below,
each of which keeps its own licence and is **not** relicensed by this project. All three are
GPL-compatible.

These notices are also copied into the installed plugin at
`OneGrade.ofx.bundle/Contents/Resources/`, because the bundle — not this repository — is what
most people actually receive, and both the Apache and BSD licences require the notice to travel
with the distribution rather than merely exist somewhere upstream.

---

## OpenFX SDK — BSD-3-Clause

`third_party/openfx/` · Copyright © The Open Effects Association.
Full text: [`third_party/openfx/LICENSE.md`](third_party/openfx/LICENSE.md).

The plugin API and support library. Compiled into the plugin binary.

---

## ncnn — BSD-3-Clause

`third_party/ncnn/` · Copyright © 2017 Tencent.
Full text: [`third_party/ncnn/LICENSE.txt`](third_party/ncnn/LICENSE.txt).
Vendored at upstream commit `5e66f09`; see [`third_party/ncnn/VENDORED.md`](third_party/ncnn/VENDORED.md)
for what was trimmed and why.

The neural-network runtime that executes Magic Grade's region model. Compiled into the plugin
binary. Built with Vulkan and OpenMP off, so it adds no runtime dependency of its own.

---

## PP-MobileSeg-Base (region model) — Apache-2.0

`models/ade20k.param`, `models/ade20k.bin` · Copyright © PaddlePaddle Authors.
Full text: [`models/LICENSE-Apache-2.0.txt`](models/LICENSE-Apache-2.0.txt).
Upstream: [PaddleSeg](https://github.com/PaddlePaddle/PaddleSeg), config
`pp_mobileseg_base_ade20k_512x512_80k.yml`, weights from
`paddleseg.bj.bcebos.com/dygraph/ade20k/pp_mobileseg_base/`. Converted to ncnn format;
provenance and the regeneration recipe are in [`models/README.md`](models/README.md).

**What it does, plainly.** 12 MB of weights that look at one frame when you press **Magic
Grade** and label roughly what is in it — sky, water, foliage, a person, ground, buildings.
Magic Grade uses those labels to pick which slider to move and in which direction.

**What it does not do.** It never touches the network. It does not run during playback or
render — only on a button press. It sends nothing anywhere, and it works with no internet
connection at all, on a render node that has never been online. It is 12 MB of arithmetic
running on one CPU thread for about a tenth of a second.

**Why this model.** Chosen on licence, and it happened to be better anyway. The obvious
alternative, NVIDIA's SegFormer, restricts use to "research or evaluation purposes only" — and
that restriction lands on **you**, not on this project. Grading a paid job is neither research
nor evaluation, so shipping it would have quietly made OneGrade unusable for the work most of
its users do. PP-MobileSeg is Apache-2.0 with no such restriction, and measured faster and more
accurate besides.
