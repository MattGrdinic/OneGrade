# Magic Grade region model

`ade20k.param` / `ade20k.bin` — **PP-MobileSeg-Base**, converted to ncnn. Copied into the bundle
at `Contents/Resources/Model/` by both build systems, exactly like the built-in LUTs, and
resolved at runtime from the plugin binary's own path so it works on any render machine.

## Provenance and licence

| | |
|---|---|
| upstream | [PaddleSeg](https://github.com/PaddlePaddle/PaddleSeg) `configs/pp_mobileseg/pp_mobileseg_base_ade20k_512x512_80k.yml` |
| weights | `https://paddleseg.bj.bcebos.com/dygraph/ade20k/pp_mobileseg_base/model.pdparams` |
| **licence** | **Apache-2.0** — no commercial restriction, no weight-specific clauses |
| params | 5.62 M |
| ADE20K mIoU | 41.57% |

**The licence is the reason this model and not another one.** The obvious candidate,
NVIDIA's SegFormer-B0, is smaller and converts more easily, but its licence restricts use to
"research or evaluation purposes only" — and that restriction lands on the END USER rather than
the distributor. A colourist grading a paid job is doing neither, so no amount of relicensing
OneGrade could have fixed it. PP-MobileSeg is both cleanly licensed and *better*: 41.57% mIoU
against SegFormer-B0's ~37.4%, and 96 ms against 251 ms.

## Regenerating

```bash
python3.10 -m venv pdl-env                 # paddleseg pulls opencv, built against numpy 1.x,
pdl-env/bin/pip install "numpy<2" \         # which cannot share an env with the torch stack
    paddlepaddle paddleseg paddle2onnx pillow
pdl-env/bin/python experiments/segmentation/convert_paddle.py      # -> ONNX + reference
seg-env/bin/pnnx converted-paddle/pp_mobileseg_base.onnx "inputshape=[1,3,512,512]"
```

Neither environment ever ships. What ships is the two files in this directory.

**Verify before trusting a rebuild.** `experiments/segmentation/verify.cpp` feeds the exact
tensor Paddle used — already normalised, so preprocessing is out of the picture and only the
model is under test — and compares class maps. The current pair scores **99.85%** against
Paddle (261757 / 262144 cells); the disagreements are boundary cells where two logits are
near-identical.

The reference frame must be a real photograph. An early version used random noise, and the model
returned a single class for all 16384 cells — a conversion that emitted a constant would have
matched it perfectly. A reference has to be able to fail.
