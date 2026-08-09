// OneGrade — does the converted ncnn model agree with the PyTorch one it came from?
//
// A conversion that produces files is not a conversion that is correct. Tracing can bake in the
// wrong shape, the converter can approximate an op, weights can be read in the wrong layout --
// all of which yield a model that loads, infers, and returns a plausible-looking mask that is
// simply not what Python said. The first symptom would be a Magic Grade decision that looks
// slightly off on footage, which is unfalsifiable and would get blamed on the heuristics.
//
// So this feeds the EXACT tensor Python used -- already normalised, so preprocessing is out of
// the picture and only the model is under test -- and compares class maps cell by cell.
//
// Build:
//   c++ -std=c++17 -O2 -I ../../third_party/ncnn/src -I ../../build/ncnn/src \
//       verify.cpp ../../build/ncnn/src/libncnn.a -o verify
#include "net.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

static std::vector<unsigned char> slurp(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> v((size_t)n);
    if (fread(v.data(), 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(2); }
    fclose(f); return v;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "converted";
    char pp[512], bp[512], ip[512], cp[512];
    snprintf(pp, sizeof pp, "%s/model.ncnn.param", dir);
    snprintf(bp, sizeof bp, "%s/model.ncnn.bin", dir);
    snprintf(ip, sizeof ip, "%s/reference_input.f32", dir);
    snprintf(cp, sizeof cp, "%s/reference_classes.u8", dir);

    ncnn::Net net;
    net.opt.use_vulkan_compute = false;
    net.opt.num_threads = 1;
    net.opt.lightmode = true;
    if (net.load_param(pp) != 0) { fprintf(stderr, "load_param failed\n"); return 2; }
    if (net.load_model(bp) != 0) { fprintf(stderr, "load_model failed\n"); return 2; }

    std::vector<unsigned char> raw = slurp(ip), ref = slurp(cp);
    const int S = 512;
    if (raw.size() != (size_t)3 * S * S * sizeof(float)) {
        fprintf(stderr, "input is %zu bytes, expected %zu\n",
                raw.size(), (size_t)3 * S * S * sizeof(float));
        return 2;
    }
    const float* src = (const float*)raw.data();

    // Already normalised in Python, so it goes in untouched. That is the point: preprocessing
    // is verified separately, and mixing the two would leave a mismatch ambiguous.
    ncnn::Mat in(S, S, 3);
    for (int c = 0; c < 3; ++c)
        memcpy(in.channel(c), src + (size_t)c * S * S, (size_t)S * S * sizeof(float));

    auto t0 = std::chrono::steady_clock::now();
    ncnn::Extractor ex = net.create_extractor();
    if (ex.input("in0", in) != 0) { fprintf(stderr, "input blob rejected\n"); return 2; }
    ncnn::Mat out;
    if (ex.extract("out0", out) != 0) { fprintf(stderr, "extract failed\n"); return 2; }
    auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    printf("output %dx%dx%d   inference %.0f ms (1 thread)\n", out.w, out.h, out.c, ms);
    if ((size_t)out.w * out.h != ref.size()) {
        fprintf(stderr, "output %dx%d does not match reference of %zu cells\n",
                out.w, out.h, ref.size());
        return 1;
    }

    size_t same = 0;
    long long hist[256] = {0};
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            int best = 0; float bv = -1e30f;
            for (int c = 0; c < out.c; ++c) {
                const float v = out.channel(c).row(y)[x];
                if (v > bv) { bv = v; best = c; }
            }
            ++hist[best & 255];
            if ((unsigned char)best == ref[(size_t)y * out.w + x]) ++same;
        }
    const double pct = 100.0 * (double)same / (double)ref.size();
    printf("agreement with PyTorch: %.2f%%  (%zu / %zu cells)\n", pct, same, ref.size());
    for (int i = 0; i < 256; ++i)
        if (hist[i] * 200 > (long long)ref.size())
            printf("   class %3d  %5.1f%%\n", i, 100.0 * hist[i] / ref.size());
    // Argmax over near-equal logits can legitimately flip on a boundary cell, so the bar is
    // "essentially identical" rather than "bit-exact".
    if (pct < 99.0) { printf("FAIL: conversion does not reproduce the model\n"); return 1; }
    printf("PASS\n");
    return 0;
}
