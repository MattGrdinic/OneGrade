# Conversion intermediates — deliberately not checked in

This directory held about 140 MB of PaddleSeg → ONNX → pnnx → ncnn conversion output. All of it
was regenerable, and most of it was a second copy of a file the plugin already ships.

## What was removed, and how to get it back

```bash
python3 ../convert_paddle.py          # writes everything below back into this directory
```

- `base.pdparams` — the upstream PP-MobileSeg-Base checkpoint, a download rather than an artifact
  of this project.
- `pp_mobileseg_base.onnx`, `.pnnx.onnx`, `.pnnxsim.onnx`, `.pnnx.bin`, `.pnnx.param`,
  `*_pnnx.py` — pnnx's working files.
- `pp_mobileseg_base.ncnn.*`, `model.ncnn.*` — the conversion result, **byte-identical** to the
  shipped `models/ade20k.{param,bin}`. Verified by sha256 before deletion; keeping three copies
  of one 11 MB model bought nothing.

## What stayed, and why it had to

- `reference_input.f32` and `reference_classes.u8` — the fixed input and the exact class map the
  **source** model produced for it, consumed by `../verify.cpp`.

They are the reason this directory still exists. A conversion that "works" is not a conversion
that is correct: a traced graph can bake in the wrong input size, an op can be approximated,
normalisation can be applied twice or not at all, and every one of those yields a model that
loads and infers and produces plausible masks that are not what the source said. The reference is
what makes that falsifiable.

They are also **no longer regenerable**, which is the point. Producing them needs the source
checkpoint and a working PaddlePaddle environment; with those gone, this pair is the only
remaining evidence that the model OneGrade ships matches the model it claims to be. Three
megabytes to keep a claim checkable is a bargain, and deleting them would have quietly converted
a verified model into a trusted one.
