# ncnn — vendored

Upstream: https://github.com/Tencent/ncnn — BSD-3-Clause.
Pinned at commit **5e66f09** (2026-08-05).

Vendored rather than submoduled to match `third_party/openfx`, and because the Windows
development box has no convenient tooling for submodule init. Builds are hermetic: no network,
no package manager, `make` and CMake both build it from this source.

## What was removed from upstream

Trimmed to 25 MB / 981 files (upstream is 77 MB). The resulting `libncnn.a` is **byte-for-byte
the same size** as a build from the untrimmed tree, so none of this is reachable on the
platforms OneGrade targets:

| removed | why |
|---|---|
| `src/layer/loongarch`, `mips`, `riscv` | architectures OneGrade does not build for |
| `src/layer/vulkan` | built with `NCNN_VULKAN=OFF`; inference here is a button press on the CPU, not a render |
| `tools/`, `examples/`, `tests/`, `benchmark/`, `docs/`, `python/` | not part of the library |

## Build options

`NCNN_VULKAN=OFF NCNN_OPENMP=OFF NCNN_SHARED_LIB=OFF NCNN_SIMPLEOCV=OFF`, plus tools/examples/
benchmark/tests off. OpenMP is off deliberately: it would add a libomp dependency to the
bundle, and the model runs once per button press, where a second of single-threaded CPU is
already well inside budget.

Measured on an M3 Max: 22 s to build universal (arm64 + x86_64), 7.8 MB fat static library.
