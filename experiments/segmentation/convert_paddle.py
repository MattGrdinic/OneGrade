#!/usr/bin/env python3
"""
OneGrade — export PP-MobileSeg (PaddleSeg, Apache-2.0) to ONNX, with a reference to verify against.

WHY THIS MODEL

    The shipping blocker was licensing, not quality. NVIDIA's SegFormer licence restricts use to
    "research or evaluation purposes only" and that restriction lands on the END USER, not the
    distributor -- a colourist on a paid job is neither, so no amount of relicensing OneGrade
    would have helped.

    PP-MobileSeg-Base is Apache-2.0 with no commercial restriction, 5.62M params, and 41.57%
    mIoU on ADE20K against SegFormer-B0's ~37.4%. Better, and shippable. It also removes the
    need for the distillation fallback entirely, which would have cost a training set and baked
    in the teacher's own flaws.

    Config reproduced from configs/pp_mobileseg/pp_mobileseg_base_ade20k_512x512_80k.yml.
    Normalisation is ImageNet mean/std, identical to SegFormer, so nothing changes in the plugin.

WHY A SEPARATE VIRTUALENV

    paddleseg pulls opencv, which is built against numpy 1.x, while the torch/transformers stack
    wants 2.x. They cannot share an environment. Both are conversion-time only and neither ever
    ships -- what ships is two files that ncnn reads.

USAGE
    pdl-env/bin/python convert_paddle.py [--variant base|tiny] [--ref FRAME.png]
"""
import argparse
import os
import sys
import urllib.request

import numpy as np
import paddle

VARIANTS = {
    # variant: (backbone builder name, weights URL, out_feat_chs)
    "base": ("MobileSeg_Base",
             "https://paddleseg.bj.bcebos.com/dygraph/ade20k/pp_mobileseg_base/model.pdparams",
             [64, 128, 192]),
    "tiny": ("MobileSeg_Tiny",
             "https://paddleseg.bj.bcebos.com/dygraph/ade20k/pp_mobileseg_tiny/model.pdparams",
             [32, 64, 128]),
}


def fetch(url, path):
    if os.path.exists(path):
        print(f"cached {path}")
        return path
    print(f"downloading {url}")
    urllib.request.urlretrieve(url, path)
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--variant", default="base", choices=list(VARIANTS))
    ap.add_argument("--size", type=int, default=512)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "converted-paddle"))
    ap.add_argument("--ref", default=os.path.expanduser(
        "~/Desktop/onegrade-frames/beach-sunset_1.3.1.png"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    import paddleseg.models as M
    bb_name, url, chs = VARIANTS[a.variant]

    # Built from the released config rather than from defaults: the backbone's channel widths
    # have to match the checkpoint exactly or the weights load into the wrong shapes, which
    # paddle will report as a warning and then happily run, producing nonsense.
    backbone = getattr(M, bb_name)(inj_type="AAMSx8", out_feat_chs=chs)
    model = M.PPMobileSeg(num_classes=150, backbone=backbone, upsample="intepolate")

    w = fetch(url, os.path.join(a.out, f"{a.variant}.pdparams"))
    sd = paddle.load(w)
    missing = model.set_state_dict(sd)
    if missing:
        print("state dict report:", missing)
    model.eval()
    n = sum(int(np.prod(p.shape)) for p in model.parameters())
    print(f"{a.variant}: {n/1e6:.2f}M params")

    # A REAL FRAME, NOT NOISE. A reference built from noise came back as a single class for
    # every cell when this was done for SegFormer -- a broken conversion emitting a constant
    # would have matched it perfectly. A reference has to be able to fail.
    from PIL import Image
    img = Image.open(a.ref).convert("RGB").resize((a.size, a.size), Image.BILINEAR)
    arr = np.asarray(img).astype(np.float32) / 255.0
    mean = np.array([0.485, 0.456, 0.406], np.float32)
    std = np.array([0.229, 0.224, 0.225], np.float32)
    x = ((arr - mean) / std).transpose(2, 0, 1)[None].copy()

    with paddle.no_grad():
        out = model(paddle.to_tensor(x))
    logits = out[0] if isinstance(out, (list, tuple)) else out
    logits = np.asarray(logits)
    print("logits", logits.shape)

    ref = logits.argmax(axis=1)[0].astype(np.uint8)
    u, c = np.unique(ref, return_counts=True)
    print(f"reference {ref.shape}: {len(u)} distinct classes,",
          "top:", sorted(zip(c.tolist(), u.tolist()), reverse=True)[:5])
    if len(u) < 3:
        sys.exit("reference too flat to catch a bad conversion")
    x.astype(np.float32).tofile(os.path.join(a.out, "reference_input.f32"))
    ref.tofile(os.path.join(a.out, "reference_classes.u8"))

    onnx_path = os.path.join(a.out, f"pp_mobileseg_{a.variant}.onnx")
    paddle.onnx.export(
        model, os.path.splitext(onnx_path)[0],
        input_spec=[paddle.static.InputSpec([1, 3, a.size, a.size], "float32", "x")],
        opset_version=12)
    print("onnx ->", onnx_path, os.path.getsize(onnx_path) if os.path.exists(onnx_path) else "MISSING")
    return 0


if __name__ == "__main__":
    sys.exit(main())
