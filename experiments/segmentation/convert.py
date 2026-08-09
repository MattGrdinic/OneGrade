#!/usr/bin/env python3
"""
OneGrade — convert an ADE20K segmentation checkpoint to ncnn, and save a reference to check
the converted model against.

WHY A REFERENCE MATTERS MORE THAN THE CONVERSION SUCCEEDING

    A conversion that "works" is not a conversion that is correct. Every stage here can go
    subtly wrong while producing a file that loads and infers: a traced graph can bake in the
    wrong input size, an op can be approximated by the converter, normalisation constants can be
    applied twice or not at all. All of those give plausible-looking masks that are simply not
    what the Python model said.

    So this writes `reference.npz` -- the exact class map the PyTorch model produces for a fixed
    input -- and the C++ side is expected to reproduce it. Without that, the first sign of a
    broken conversion is a Magic Grade decision that looks a bit odd on footage, which is
    unfalsifiable and could be blamed on the heuristics for weeks.

WHICH CHECKPOINT

    The default is the NVIDIA SegFormer-B0 already measured on the user's own frames, so
    conversion is proved against a known-good result before anything else changes. Its licence
    is not suitable for shipping and it is expected to be swapped -- but swapping the checkpoint
    is one flag here, whereas debugging conversion and quality at the same time is two unknowns
    at once.

USAGE
    python convert.py [--model NAME] [--size 512] [--out ../../models]
"""
import argparse
import os
import subprocess
import sys

import numpy as np
import torch


class Wrapper(torch.nn.Module):
    """Plain tensor in, plain tensor out.

    HuggingFace models return a dataclass, and tracing one produces a graph the converters
    cannot follow. Unwrapping to a bare logits tensor is the difference between a conversion
    that works and one that fails with an error about the output type.
    """
    def __init__(self, inner):
        super().__init__()
        self.inner = inner

    def forward(self, x):
        return self.inner(pixel_values=x).logits


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", default="nvidia/segformer-b0-finetuned-ade-512-512")
    ap.add_argument("--size", type=int, default=512)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "converted"))
    ap.add_argument("--ref", default=os.path.expanduser(
        "~/Desktop/onegrade-frames/beach-sunset_1.3.1.png"),
        help="photograph to build the reference from — must not be noise, see the note in main()")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    from transformers import AutoModelForSemanticSegmentation
    model = Wrapper(AutoModelForSemanticSegmentation.from_pretrained(a.model)).eval()

    # A REAL FRAME, NOT NOISE. The first version of this used random noise and produced a
    # reference where all 16384 cells were the same class -- the model sees no structure in
    # noise, so it emits one label everywhere. A converted model that returned a constant would
    # have matched it perfectly. A reference has to be able to FAIL.
    #
    # A photograph gives a rich multi-class map (sea, sky, mountain, person on the beach frame),
    # so a conversion that drops a layer, mangles an op or misreads a weight shows up as a
    # different map rather than as an identical flat one.
    if a.ref and os.path.exists(a.ref):
        from PIL import Image
        img = Image.open(a.ref).convert("RGB").resize((a.size, a.size), Image.BILINEAR)
        arr = np.asarray(img).astype(np.float32) / 255.0
        mean = np.array([0.485, 0.456, 0.406], np.float32)
        std = np.array([0.229, 0.224, 0.225], np.float32)
        x = torch.from_numpy(((arr - mean) / std).transpose(2, 0, 1)[None].copy())
        print(f"reference input: {a.ref}")
    else:
        raise SystemExit("--ref is required and must be a real photograph; see the note above")

    with torch.no_grad():
        logits = model(x)
    print(f"logits {tuple(logits.shape)}  (1, classes, H/4, W/4 for SegFormer)")

    ref = logits.argmax(dim=1)[0].numpy().astype(np.uint8)
    np.savez_compressed(os.path.join(a.out, "reference.npz"),
                        input=x.numpy().astype(np.float32), classes=ref)
    # Raw dumps too: the C++ verifier reads these, and .npz is zip-wrapped .npy, which is a
    # silly amount of machinery to reimplement for two fixed-shape arrays.
    x.numpy().astype(np.float32).tofile(os.path.join(a.out, "reference_input.f32"))
    ref.tofile(os.path.join(a.out, "reference_classes.u8"))
    u, c = np.unique(ref, return_counts=True)
    print("reference class map:", ref.shape, f"{len(u)} distinct classes,",
          "top:", sorted(zip(c.tolist(), u.tolist()), reverse=True)[:4])
    if len(u) < 3:
        raise SystemExit(f"reference has only {len(u)} class(es) — too flat to catch a bad "
                         f"conversion. Use a frame with more in it.")

    ts = os.path.join(a.out, "model.pt")
    torch.jit.trace(model, x).save(ts)
    print(f"traced -> {ts}")

    # pnnx rather than onnx2ncnn: it takes TorchScript directly, so ONNX never enters the
    # picture as a third place for the graph to be misrepresented, and it handles more of the
    # transformer ops that an ADE20K SegFormer is made of.
    pnnx_bin = os.path.join(os.path.dirname(sys.executable), "pnnx")
    cmd = [pnnx_bin, ts, f"inputshape=[1,3,{a.size},{a.size}]"]
    print("$", " ".join(cmd))
    r = subprocess.run(cmd, cwd=a.out, capture_output=True, text=True)
    tail = (r.stdout + r.stderr).strip().splitlines()
    for line in tail[-25:]:
        print("   ", line)
    made = [f for f in sorted(os.listdir(a.out)) if f.endswith((".param", ".bin"))]
    print("produced:", made or "NOTHING — see the log above")
    return 0 if made else 1


if __name__ == "__main__":
    sys.exit(main())
