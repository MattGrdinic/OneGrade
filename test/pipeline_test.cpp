// OneGrade — CPU unit tests for the color pipeline (OneGradePipeline.h).
// Builds with any C++17 compiler; no OFX/GPU needed. Returns non-zero on failure.
// Copyright (C) 2026 Matthew Grdinic
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/OneGradePipeline.h"
#include "../src/OneGradeAnalysis.h"
#include "../src/OneGradeCreative.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <array>

static int g_fail = 0;
static void check(bool ok, const std::string& name) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) g_fail++;
}
static bool close(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
static bool finite3(float r, float g, float b) { return std::isfinite(r) && std::isfinite(g) && std::isfinite(b); }

// neutral parameter vector: temp,tint,density,lift,gamma,gain,offTemp,offTint,postExp,postCon,rawExp,rawTemp
// Neutral for the WHOLE array, Range Balance included: latch 0 is what turns it off, and a
// fixture that left it uninitialised would enable the stage with a garbage threshold. Defers to
// the shipping definition so a new parameter cannot be neutral in the plugin and garbage here.
static void neutral13(float P[og::analysis::kParamN]) { og::analysis::neutral_params(P); }
static void neutral(float P[og::analysis::kParamN]) { neutral13(P); }

// ---- synthetic frames for the scene-descriptor / Jacobian tests (OneGradeAnalysis.h) ----
// Deterministic noise, so percentiles are non-degenerate but every run is identical.
static uint32_t g_seed = 12345u;
static float srnd() { g_seed = g_seed*1664525u + 1013904223u; return (float)(g_seed >> 8) / (float)(1u<<24); }

// kind 0: two-tone "sunset over water" — warm bright top third over a cooler bottom.
// kind 1: achromatic, for the control case where the two populations should collapse.
// kind 2: two-tone with a skin-hued patch, to exercise the masked descriptors. The patch
//         value renders to display h 0.051 / s 0.395, mid-window in both.
static oga::SampleSet make_frame(int kind, int W = 64, int H = 64)
{
    oga::SampleSet S; g_seed = 12345u;
    S.rgb.reserve((size_t)W*H*3); S.band.reserve((size_t)W*H);
    for (int y = 0; y < H; ++y) {
        const uint8_t band = (uint8_t)std::min(2, (y * 3) / H);   // OFX is bottom-up: 2 = top
        for (int x = 0; x < W; ++x) {
            float r, g, b;
            const bool face = (kind == 2) && (x > 20 && x < 44 && y > 8 && y < 32);
            if (face)              { r = 0.460f; g = 0.420f; b = 0.384f; }   // skin
            else if (kind == 1)    { r = 0.440f; g = 0.440f; b = 0.440f; }   // achromatic
            else if (band == 2)    { r = 0.560f; g = 0.480f; b = 0.380f; }   // warm sky
            else                   { r = 0.380f; g = 0.390f; b = 0.430f; }   // cool water
            const float n = (srnd() - 0.5f) * 0.10f;
            S.rgb.push_back(r+n); S.rgb.push_back(g+n); S.rgb.push_back(b+n);
            S.band.push_back(band);
        }
    }
    return S;
}
// Each descriptor's natural scale: the largest one-step response in its own row. Judging a
// prediction against this rather than against the size of the move is what keeps a 0.0003
// wobble in a quantity whose value is 78 from being reported as a modelling failure.
static void desc_scales(const oga::Jac& J, float* out) {
    for (int d = 0; d < oga::kDescN; ++d) {
        float mx = 0.f;
        for (int p = 0; p < oga::kParamN; ++p) mx = std::max(mx, std::fabs(J.at(d, p)));
        out[d] = std::max(mx, 1e-4f);
    }
}

int main() {
    printf("OneGrade pipeline tests\n");

    // 1. Transfer-function round trips (must be exact inverses)
    {
        bool ok = true;
        for (float v = 0.f; v <= 1.5f; v += 0.05f) {
            ok &= close(og::r709_dec(og::r709_enc(v)), v, 2e-3f);
            ok &= close(og::r709_g_dec(og::r709_g_enc(v, 2.2f), 2.2f), v, 2e-3f);
            ok &= close(og::r709_g_dec(og::r709_g_enc(v, 2.4f), 2.4f), v, 2e-3f);
            ok &= close(og::di_decode(og::di_encode(v)),  v, 2e-3f);
        }
        check(ok, "r709 scene / r709 g2.2 / r709 g2.4 / DI encode/decode round-trip");
    }

    // 1b. Rec.709 Gamma 2.2 (encode 1) and Gamma 2.4 (encode 2) are pure power curves, no linear toe
    {
        bool ok = close(og::encode(1, 0.5f), std::pow(0.5f, 1.0f/2.2f), 1e-4f)
               && close(og::encode(1, 1.0f), 1.0f, 1e-4f)
               && close(og::encode(1, 0.0f), 0.0f, 1e-4f)
               && close(og::encode(2, 0.5f), std::pow(0.5f, 1.0f/2.4f), 1e-4f)
               && close(og::encode(2, 1.0f), 1.0f, 1e-4f)
               && close(og::encode(2, 0.0f), 0.0f, 1e-4f);
        check(ok, "Rec.709 Gamma 2.2 / 2.4 encodes are pure power");
    }

    // 2. HDR decodes finite and monotonic over the signal range
    {
        bool ok = true;
        for (int cam = 10; cam <= 11; ++cam) {          // 10=HLG, 11=PQ
            float prev = -1e9f;
            for (float x = 0.f; x <= 1.0f; x += 0.05f) {
                float y = og::decode_log(cam, x);
                ok &= std::isfinite(y);
                ok &= (y >= prev - 1e-4f);               // non-decreasing
                prev = y;
            }
        }
        check(ok, "HLG/PQ decode finite and monotonic");
    }

    // 2b. Blackmagic Gen 5 Film decode: published mid-gray code, continuous at the
    //     toe junction, monotonic over the signal range
    {
        bool ok = close(og::decode_log(0, 0.38355f), 0.18f, 1e-3f);    // 0.18 -> 0.38355 (white paper)
        ok &= close(og::decode_log(0, 0.13388378f), 0.005f, 1e-4f);    // linear/log junction
        float prev = -1e9f;
        for (float x = 0.f; x <= 1.0f; x += 0.02f) {
            float y = og::decode_log(0, x);
            ok &= std::isfinite(y) && (y >= prev - 1e-5f);
            prev = y;
        }
        check(ok, "Blackmagic Gen 5 Film decode (mid-gray, junction, monotonic)");
    }

    // 3. Identity 3D LUT leaves pixels unchanged
    {
        const int N = 9;
        std::vector<float> lut((size_t)N*N*N*3);
        for (int b=0;b<N;b++) for (int g=0;g<N;g++) for (int r=0;r<N;r++) {
            size_t i = (((size_t)b*N + g)*N + r)*3;
            lut[i+0] = (float)r/(N-1); lut[i+1] = (float)g/(N-1); lut[i+2] = (float)b/(N-1);
        }
        bool ok = true;
        for (float t = 0.f; t <= 1.f; t += 0.1f) {
            float r=t, g=t*0.5f, b=1.f-t;
            og::apply_lut(lut.data(), N, 1.0f, r, g, b);
            ok &= close(r, t, 3e-3f) && close(g, t*0.5f, 3e-3f) && close(b, 1.f-t, 3e-3f);
        }
        check(ok, "identity 3D LUT is a pass-through");
    }

    // 4. Neutral trim is identity; exposure/contrast move predictably
    {
        float r=0.4f,g=0.5f,b=0.6f;
        og::apply_trim(0.f, 1.f, r, g, b);
        check(close(r,0.4f)&&close(g,0.5f)&&close(b,0.6f), "neutral trim is identity");
    }

    // 4b. Highlight roll-off (softclip): amt 0 = identity, identity below the knee,
    //     continuous at the knee, monotonic and bounded by 1.0 above it
    {
        bool ok = true;
        for (float v = 0.f; v <= 3.f; v += 0.1f) ok &= close(og::softclip(v, 0.f), v, 1e-6f);
        ok &= close(og::softclip(0.3f, 0.5f), 0.3f, 1e-6f);          // below knee (k = 0.7)
        ok &= close(og::softclip(0.7001f, 0.5f), 0.7f, 1e-3f);       // continuity at the knee
        float prev = -1e9f;
        for (float v = 0.f; v <= 20.f; v += 0.25f) {
            float y = og::softclip(v, 0.5f);
            ok &= (y >= prev - 1e-6f) && (y <= 1.0f + 1e-4f);
            prev = y;
        }
        check(ok, "highlight roll-off: identity below knee, monotonic, bounded at 1");
    }

    // 5. Full pipeline is finite for every camera x encode x sample input
    {
        float P[og::analysis::kParamN]; neutral(P);
        bool ok = true;
        for (int cam = 0; cam <= 11; ++cam)
          for (int enc = 0; enc <= 5; ++enc)
            for (float x = 0.02f; x <= 0.98f; x += 0.12f) {
                float or_,og,ob; og::process(cam, enc, P, x, x*0.9f, x*1.1f, or_, og, ob);
                ok &= finite3(or_,og,ob);
            }
        check(ok, "process() finite for all cameras/encodes/inputs");
    }

    // 6. Gain pivots black: a black input stays black under gain
    {
        float P[og::analysis::kParamN]; neutral(P); P[5] = 2.0f;            // gain = 2
        float r,g,b; og::process(1, 0, P, 0.f, 0.f, 0.f, r, g, b);
        check(close(r,0.f,2e-3f)&&close(g,0.f,2e-3f)&&close(b,0.f,2e-3f), "gain pins black");
    }

    // 7. Lift pivots white: diffuse white (BMD/DI code ~0.5139 -> linear 1.0) unchanged by lift
    {
        const float whiteCode = og::di_encode(1.0f);     // camera code that decodes to linear 1.0
        float P0[og::analysis::kParamN]; neutral(P0);
        float P1[og::analysis::kParamN]; neutral(P1); P1[3] = -0.25f;        // lift down
        float a0,b0,c0, a1,b1,c1;
        og::process(1, 0, P0, whiteCode, whiteCode, whiteCode, a0, b0, c0);
        og::process(1, 0, P1, whiteCode, whiteCode, whiteCode, a1, b1, c1);
        check(close(a0,a1,5e-3f)&&close(b0,b1,5e-3f)&&close(c0,c1,5e-3f), "lift pins white (diffuse white unchanged)");
    }

    // 8. Lift does not amplify superwhites (BMD/DI code 1.0 -> linear ~100)
    {
        float P0[og::analysis::kParamN]; neutral(P0);
        float P1[og::analysis::kParamN]; neutral(P1); P1[3] = -0.25f;
        float a0,b0,c0, a1,b1,c1;
        og::process(1, 5, P0, 1.0f, 1.0f, 1.0f, a0, b0, c0);   // enc=5 linear so we compare raw
        og::process(1, 5, P1, 1.0f, 1.0f, 1.0f, a1, b1, c1);
        check(close(a0,a1,1e-2f), "lift leaves superwhites untouched");
    }

    // 9. RAW exposure: +1 stop doubles scene-linear (linear output path)
    {
        float P0[og::analysis::kParamN]; neutral(P0);
        float P1[og::analysis::kParamN]; neutral(P1); P1[10] = 1.0f;    // +1 stop
        float a0,b0,c0, a1,b1,c1;
        og::process(1, 5, P0, 0.5f, 0.5f, 0.5f, a0, b0, c0);   // enc=5 = linear output
        og::process(1, 5, P1, 0.5f, 0.5f, 0.5f, a1, b1, c1);
        check(close(a1,2.0f*a0,2e-2f)&&close(b1,2.0f*b0,2e-2f)&&close(c1,2.0f*c0,2e-2f),
              "Scene Exposure +1 stop doubles linear output");
    }

    // 10. RAW temperature: neutral at 6500; warmer raises R / lowers B, cooler the reverse
    {
        float P6[og::analysis::kParamN]; neutral(P6);                    // rawTemp = 6500 (neutral)
        float Pw[og::analysis::kParamN]; neutral(Pw); Pw[11] = 9000.f;   // warmer
        float Pc[og::analysis::kParamN]; neutral(Pc); Pc[11] = 4000.f;   // cooler
        float r6,g6,b6, rw,gw,bw, rc,gc,bc;
        og::process(1, 0, P6, 0.5f, 0.5f, 0.5f, r6, g6, b6);
        og::process(1, 0, Pw, 0.5f, 0.5f, 0.5f, rw, gw, bw);
        og::process(1, 0, Pc, 0.5f, 0.5f, 0.5f, rc, gc, bc);
        check(finite3(r6,g6,b6) && rw>r6 && r6>rc && bw<b6 && b6<bc,
              "Scene White Balance: warmer raises R / lowers B, 6500 sits between");
    }

    // 11. Node Role split: Input Transform (cam -> DaVinci Intermediate) chained into
    //     Output Transform (DWG/DI -> delivery) must reproduce a single Full Grade node.
    //     This is what lets one group share a Pre-Clip decode and a Post-Clip look.
    {
        float Pn[og::analysis::kParamN]; neutral13(Pn);
        float Pg[og::analysis::kParamN]; neutral13(Pg);                 // a real look on the output node
        Pg[2]=0.25f; Pg[3]=0.06f; Pg[4]=1.10f; Pg[5]=0.92f; Pg[6]=-0.09f;
        float worst = 0.f;
        for (int look = 0; look < 2; ++look) {
            const float* P = look ? Pg : Pn;
            for (int enc = 0; enc <= 2; ++enc)
                for (int cam = 0; cam < 12; ++cam)
                    for (int i = 0; i <= 40; ++i) {
                        float x = i/40.0f, y = x*0.85f, z = x*0.62f;
                        float s0,s1,s2;  og::process(cam, enc, P, x, y, z, s0, s1, s2);
                        float a0,a1,a2;  og::process(cam, 4, Pn, x, y, z, a0, a1, a2);
                        float b0,b1,b2;  og::process(1, enc, P, a0, a1, a2, b0, b1, b2);
                        worst = std::fmax(worst, std::fmax(std::fabs(s0-b0),
                                          std::fmax(std::fabs(s1-b1), std::fabs(s2-b2))));
                    }
        }
        // 1/255 = one 8-bit code value; we land ~4x under that even at the worst camera.
        check(worst < 1.0f/255.0f, "Node Role split (pre-clip + post-clip) == single node");
        if (worst >= 1.0f/255.0f) printf("        worst delta %.4f (%.2f x 8-bit LSB)\n", worst, worst*255.f);
    }

    // 12. A neutral node must not clip out-of-gamut negatives on the scene-referred
    //     hand-off encodes — that clip is what broke the role split (safe_pow floors at 0).
    {
        float P[og::analysis::kParamN]; neutral13(P);
        bool sawNegative = false, ok = true;
        for (int cam = 0; cam < 12 && ok; ++cam)
            for (int i = 1; i <= 40; ++i) {
                float x = i/40.0f;
                float r,g,b;
                og::process(cam, 4, P, x, x*0.05f, x, r, g, b);   // saturated magenta
                if (!finite3(r,g,b)) { ok = false; break; }
                if (r < -1e-4f || g < -1e-4f || b < -1e-4f) sawNegative = true;
            }
        check(ok && sawNegative, "neutral node preserves out-of-gamut negatives (DI hand-off)");
    }

    // 13. LUT EXPORT — guards the bake used by `Export .cube`.
    //
    //     Two claims, and only the ones measurement actually supports:
    //
    //     (a) ON-LATTICE the bake is exact. This is the strong test: it is a perfect
    //         detector for the index-order transpose that is the easy bug here (a .cube
    //         stores red varying fastest, ((b*N + g)*N + r)*3, and a transposed bake loads
    //         fine, looks like a plausible grade, and has its red and blue axes swapped).
    //         Sampling exactly at lattice points has zero interpolation error, so any
    //         deviation above float noise is a real structural bug.
    //
    //     (b) OFF-LATTICE, only along the GREY AXIS, and only loosely. The pipeline hard-
    //         clips out-of-gamut channels at the output encode, which puts a discontinuity
    //         through the colour cube; trilinear interpolation cannot follow a step, so a
    //         tight global tolerance here would be asserting something false. Measured on
    //         Gen 5 -> 709 2.2 at 33^3: grey axis ~4 LSB, mildly tinted bright colour ~152
    //         LSB, median over the whole cube 0 LSB. See exportCube() for the full note.
    {
        const int N = 33;
        float P[og::analysis::kParamN]; neutral13(P);
        P[3] = 0.08f; P[4] = 1.1f; P[5] = 0.85f; P[2] = 0.2f; P[0] = -0.15f;   // a real grade

        bool ok = true;
        for (int cam : {0, 2, 11}) {
            const int enc = 1;                                   // Rec.709 Gamma 2.2
            std::vector<float> lat((size_t)N*N*N*3);
            const float d = 1.0f / (float)(N - 1);
            for (int bi = 0; bi < N; ++bi)
                for (int gi = 0; gi < N; ++gi)
                    for (int ri = 0; ri < N; ++ri) {
                        float ro, go, bo;
                        og::process(cam, enc, P, ri*d, gi*d, bi*d, ro, go, bo);
                        const size_t idx = (((size_t)bi*N + gi)*N + ri)*3;
                        lat[idx+0]=ro; lat[idx+1]=go; lat[idx+2]=bo;
                    }

            // (a) exact on lattice points
            for (int bi = 0; bi < N; bi += 4)
                for (int gi = 0; gi < N; gi += 4)
                    for (int ri = 0; ri < N; ri += 4) {
                        float er, eg, eb;
                        og::process(cam, enc, P, ri*d, gi*d, bi*d, er, eg, eb);
                        float lr = ri*d, lg = gi*d, lb = bi*d;
                        og::apply_lut(lat.data(), N, 1.0f, lr, lg, lb);
                        ok &= close(lr, er, 1e-5f) && close(lg, eg, 1e-5f) && close(lb, eb, 1e-5f);
                    }

            // (b) grey axis, off lattice, through the normal log range
            auto cl = [](float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); };
            for (int i = 0; i <= 200; ++i) {
                const float t = 0.10f + 0.60f * i / 200.f;
                float er, eg, eb; og::process(cam, enc, P, t, t, t, er, eg, eb);
                float lr = t, lg = t, lb = t;
                og::apply_lut(lat.data(), N, 1.0f, lr, lg, lb);
                ok &= std::fabs(cl(lr)-cl(er)) < 8.f/255.f
                   && std::fabs(cl(lg)-cl(eg)) < 8.f/255.f
                   && std::fabs(cl(lb)-cl(eb)) < 8.f/255.f;
            }
        }
        check(ok, "LUT export: bake is exact on-lattice, close on the grey axis");
    }

    // 14. CLEAN AUTO GRADE premise: grading a measured DISPLAY percentile through lgg_core
    //     predicts exactly what the pipeline renders for that input.
    //
    //     This is the whole basis of the containment solver. It measures the frame once at
    //     neutral, then places p1/p50/p99 by evaluating lgg_core on three scalars instead of
    //     re-rendering 200k samples per iteration. That shortcut is only valid because, for a
    //     display-referred encode, the grade happens in r709_g_enc(x, dg) and the output
    //     encode IS that same function -- so the encode/decode pair on either side of step 6
    //     cancels and the grade acts directly on the measured numbers.
    //
    //     If step 6 ever stops being expressible that way, the solver silently starts placing
    //     percentiles somewhere other than where it claims. This catches that.
    {
        bool ok = true;
        for (int enc : {0, 1, 2}) {                    // Scene OETF, gamma 2.2, gamma 2.4
            for (int cam : {0, 2, 11}) {
                for (float x = 0.10f; x <= 0.90f; x += 0.08f) {
                    float P0[og::analysis::kParamN]; neutral13(P0);
                    float n_r, n_g, n_b;
                    og::process(cam, enc, P0, x, x, x, n_r, n_g, n_b);   // neutral = "measured"

                    for (auto lgg : { std::array<float,3>{0.05f, 1.15f, 0.80f},
                                      std::array<float,3>{-0.03f, 0.90f, 1.30f},
                                      std::array<float,3>{0.12f, 1.00f, 0.55f} }) {
                        float P1[og::analysis::kParamN]; neutral13(P1);
                        P1[3] = lgg[0]; P1[4] = lgg[1]; P1[5] = lgg[2];
                        float a_r, a_g, a_b;
                        og::process(cam, enc, P1, x, x, x, a_r, a_g, a_b);       // actual render
                        const float pred = og::lgg_core(n_r, lgg[0], lgg[1], lgg[2]);  // prediction
                        ok &= close(pred, a_r, 1.5e-3f);
                    }
                }
            }
        }
        check(ok, "Clean auto grade: lgg_core on a display percentile predicts the render");
    }

    // =====================================================================================
    // SCENE DESCRIPTORS + THE CONTROL JACOBIAN (OneGradeAnalysis.h)
    // =====================================================================================

    // 15. The descriptors describe the FOOTAGE: a warm-over-cool frame separates into two
    //     populations, an achromatic one does not. If clustering ever silently degenerates to
    //     one population (or splits noise), every colour heuristic built on `sep` is nonsense
    //     and nothing else would notice.
    {
        const int cam = 11, enc = 1;
        float P0[og::analysis::kParamN]; neutral13(P0);

        oga::SampleSet two = make_frame(0);
        oga::Extras e2 = oga::classify(two, cam, enc);
        oga::Desc d2 = oga::describe(two, cam, enc, P0);

        oga::SampleSet flat = make_frame(1);
        oga::Extras ef = oga::classify(flat, cam, enc);
        oga::Desc df = oga::describe(flat, cam, enc, P0);

        const bool ok =
            d2.v[oga::D_SEP] > 20.f &&        // two genuinely distinct colour populations
            df.v[oga::D_SEP] < 1.f  &&        // achromatic frame: nothing to separate
            d2.v[oga::D_DB]  > 10.f &&        // top band is the warm one
            d2.v[oga::D_DL]  > 0.f  &&        // ...and the brighter one
            e2.share[0] > 15.f && e2.share[1] > 15.f &&   // neither population is a rounding error
            ef.share[0] > 15.f && ef.share[1] > 15.f &&
            std::fabs(df.v[oga::D_DB]) < 1.f;
        check(ok, "descriptors: two-tone frame splits into two populations, achromatic does not");
    }

    // 16. The Jacobian recovers what each control MEANS, without anyone writing it down.
    //     This is the whole point of measuring the controls instead of describing them: the
    //     system is never told that negative Offset Temp adds blue, it perturbs the control
    //     and observes b* fall. Signs are checked against the physics, so a sign flip
    //     anywhere in the pipeline shows up here as a control that now means its opposite.
    {
        const int cam = 11, enc = 1;
        float P0[og::analysis::kParamN]; neutral13(P0);
        oga::SampleSet S = make_frame(0);
        oga::classify(S, cam, enc);
        oga::Jac J = oga::jacobian(S, cam, enc, P0);

        const bool ok =
            J.at(oga::D_B, 6)      > 0.2f &&   // Offset Temp up  -> warmer (b* up)
            J.at(oga::D_B, 0)      > 0.1f &&   // Balance Temp up -> warmer
            J.at(oga::D_B, 11)     > 0.1f &&   // RAW Temp up     -> warmer
            J.at(oga::D_A, 7)      < -0.2f &&  // Offset Tint up  -> greener (a* down)
            J.at(oga::D_CHROMA, 2) > 0.2f &&   // Density up      -> more colourful
            J.at(oga::D_MID, 5)    > 0.f &&    // Gain up         -> brighter midtone
            J.at(oga::D_WHITE, 5)  > J.at(oga::D_MID, 5) &&  // ...and the top moves most (pivots black)
            J.at(oga::D_BLACK, 3)  > 0.f &&    // Lift up         -> floor rises
            J.at(oga::D_MID, 8)    > 0.f &&    // Post Exposure up-> brighter
            J.at(oga::D_WHITE, 12) < 0.f;      // Rolloff up      -> top pulled down
        check(ok, "Jacobian recovers the meaning of each control from measurement alone");
    }

    // 17. It is a real derivative, not a plausible-looking table: halve the move and the
    //     linear prediction error should fall roughly fourfold. A forward difference, a wrong
    //     step size, or a descriptor whose mask moves with the grade would all break the rate
    //     while still producing numbers that look reasonable in isolation.
    {
        const int cam = 11, enc = 1;
        float P0[og::analysis::kParamN]; neutral13(P0);
        oga::SampleSet S = make_frame(0);
        oga::classify(S, cam, enc);
        oga::Jac J = oga::jacobian(S, cam, enc, P0);
        oga::Desc d0 = oga::describe(S, cam, enc, P0);

        bool ok = true;
        for (int p : {2, 5, 6, 9}) {          // density, gain, offset temp, contrast
            double prev = 0.0;
            for (float mag : {0.8f, 0.4f, 0.2f}) {
                float dp[og::analysis::kParamN] = {0}; dp[p] = mag;
                float pred[oga::kDescN]; oga::jac_predict(J, dp, pred);
                float P1[og::analysis::kParamN]; oga::apply_move(P0, dp, P1);
                oga::Desc d1 = oga::describe(S, cam, enc, P1);
                double e = 0.0;
                for (int d = 0; d < oga::kDescN; ++d)
                    e = std::max(e, (double)std::fabs(pred[d] - (d1.v[d] - d0.v[d])));
                if (prev > 1e-5) ok &= (e < prev / 3.0);   // quadratic would give 4x; allow slack
                prev = e;
            }
        }
        check(ok, "Jacobian converges quadratically (it is a derivative, not a lookup table)");
    }

    // 18. Prediction accuracy at a realistic move, over the STEERABLE controls only.
    //     Rolloff and (at its default) RAW Temp are excluded by steer_mask because both are
    //     discontinuous there — see the header. Test 20 pins that discontinuity separately so
    //     the exclusion stays honest rather than becoming a way to hide a bad column.
    {
        const int cam = 11, enc = 1;
        float P0[og::analysis::kParamN]; neutral13(P0);
        oga::SampleSet S = make_frame(0);
        oga::classify(S, cam, enc);
        oga::Jac J = oga::jacobian(S, cam, enc, P0);
        oga::Desc d0 = oga::describe(S, cam, enc, P0);
        float scale[oga::kDescN]; desc_scales(J, scale);
        bool allow[og::analysis::kParamN]; oga::steer_mask(P0, allow);

        double worst = 0.0;
        for (int p = 0; p < 13; ++p) {
            if (!allow[p]) continue;
            for (float mag : {-0.5f, 0.5f}) {
                float dp[og::analysis::kParamN] = {0}; dp[p] = mag;
                float pred[oga::kDescN]; oga::jac_predict(J, dp, pred);
                float P1[og::analysis::kParamN]; oga::apply_move(P0, dp, P1);
                oga::Desc d1 = oga::describe(S, cam, enc, P1);
                for (int d = 0; d < oga::kDescN; ++d)
                    worst = std::max(worst, (double)std::fabs(pred[d] - (d1.v[d]-d0.v[d]))
                                            / (scale[d] * std::fabs(mag)));
            }
        }
        check(worst < 0.35, "Jacobian predicts a half-step move to within a third of one step");
    }

    // 19. THE INVERSE — the reason any of this exists. "Make it bluer" is an intent expressed
    //     in the descriptor the user's own grade moved (b*), and the solver has to come back
    //     with the slider a colourist would have reached for. Nothing anywhere tells it that
    //     Offset Temp is the warm/cool control; it falls out of the measured Jacobian.
    {
        const int cam = 11, enc = 1;
        float P0[og::analysis::kParamN]; neutral13(P0);
        oga::SampleSet S = make_frame(0);
        oga::classify(S, cam, enc);
        oga::Jac J = oga::jacobian(S, cam, enc, P0);
        oga::Desc d0 = oga::describe(S, cam, enc, P0);

        // (a) single intent, single control
        float dd[oga::kDescN] = {0}, w[oga::kDescN] = {0};
        dd[oga::D_B] = -3.0f; w[oga::D_B] = 1.0f;
        bool allow[og::analysis::kParamN] = {false}; allow[6] = true;
        float dp[og::analysis::kParamN]; oga::solve_intent(J, dd, w, allow, 1e-4f, dp);
        float P1[og::analysis::kParamN]; oga::apply_move(P0, dp, P1);
        oga::Desc d1 = oga::describe(S, cam, enc, P1);
        const float got = d1.v[oga::D_B] - d0.v[oga::D_B];

        bool ok = (P1[6] < 0.f)                    // it reached for NEGATIVE Offset Temp
               && (std::fabs(got - (-3.0f)) < 0.3f);  // and landed within 10% of the ask
        for (int i = 0; i < og::analysis::kParamN; ++i) if (!allow[i]) ok &= close(P1[i], P0[i], 1e-6f);

        // (b) two intents, two controls — the case a real Magic Grade rule would produce
        float dd2[oga::kDescN] = {0}, w2[oga::kDescN] = {0};
        dd2[oga::D_B] = -3.0f;      w2[oga::D_B] = 1.0f;
        dd2[oga::D_CHROMA] = 2.0f;  w2[oga::D_CHROMA] = 1.0f;
        bool allow2[og::analysis::kParamN] = {false}; allow2[6] = true; allow2[2] = true;
        float dp2[og::analysis::kParamN]; oga::solve_intent(J, dd2, w2, allow2, 1e-4f, dp2);
        float P2[og::analysis::kParamN]; oga::apply_move(P0, dp2, P2);
        oga::Desc d2 = oga::describe(S, cam, enc, P2);
        ok &= std::fabs((d2.v[oga::D_B]      - d0.v[oga::D_B])      - (-3.0f)) < 0.3f;
        ok &= std::fabs((d2.v[oga::D_CHROMA] - d0.v[oga::D_CHROMA]) - ( 2.0f)) < 0.2f;
        ok &= (P2[6] < 0.f) && (P2[2] > 0.f);      // cooler balance, more density
        for (int i = 0; i < og::analysis::kParamN; ++i) if (!allow2[i]) ok &= close(P2[i], P0[i], 1e-6f);

        check(ok, "intent solve: 'bluer' resolves to negative Offset Temp and lands the target");
    }

    // 20. The two discontinuities steer_mask() exists to dodge, pinned so they cannot quietly
    //     change. Both are properties of the shipping pipeline, found by the Jacobian rather
    //     than looked for. If either is ever fixed, THIS test fails first and says so — at
    //     which point the control becomes steerable and should be let back into the mask.
    {
        // Rolloff: softclip asymptotes hard at 1.0 for any amt > 0, so the first nudge off
        // zero is a step, not a ramp.
        const float a = og::softclip(1.26f, 0.0f);
        const float b = og::softclip(1.26f, 0.0001f);
        bool ok = close(a, 1.26f, 1e-5f) && close(b, 1.0f, 1e-4f);

        // RAW Temp: white_balance() forces identity on 6499 < T < 6501, but the Planckian
        // locus at 6500 K is not D65 (dy = -0.0053), so the skipped adaptation is not an
        // identity and a neutral grey jumps when the slider leaves its default.
        float P[og::analysis::kParamN]; neutral13(P);
        float r0,g0,b0, r1,g1,b1;
        og::process(11, 1, P, 0.45f, 0.45f, 0.45f, r0, g0, b0);
        P[11] = 6501.f;
        og::process(11, 1, P, 0.45f, 0.45f, 0.45f, r1, g1, b1);
        ok &= close(r0, g0, 1e-6f) && close(g0, b0, 1e-6f);   // exactly neutral at 6500
        ok &= (std::fabs(g1 - r1) > 0.008f);                  // ...and not at 6501
        check(ok, "known discontinuities pinned: Rolloff at 0, RAW Temp at 6500 K");
    }

    // 21. The masked descriptors are gated on coverage. A skin number taken off forty pixels
    //     is noise, and a solver that weighted it would be steering on nothing — so they read
    //     exactly zero until the mask finds enough to trust, which is also how a caller knows
    //     not to weight them.
    {
        const int cam = 11, enc = 1;
        float P0[og::analysis::kParamN]; neutral13(P0);

        oga::SampleSet withSkin = make_frame(2);
        oga::Extras es = oga::classify(withSkin, cam, enc);
        oga::Desc ds = oga::describe(withSkin, cam, enc, P0);

        oga::SampleSet noSkin = make_frame(1);
        oga::Extras en = oga::classify(noSkin, cam, enc);
        oga::Desc dn = oga::describe(noSkin, cam, enc, P0);

        const bool ok =
            es.skinOk && es.skinPct > 5.f &&
            ds.v[oga::D_SKINL] > 0.f && std::fabs(ds.v[oga::D_SKINB]) > 1.f &&
            !en.skinOk && en.skinPct < 1.f &&
            close(dn.v[oga::D_SKINL], 0.f, 1e-6f) && close(dn.v[oga::D_SKINB], 0.f, 1e-6f);
        check(ok, "skin descriptors activate on coverage and read zero without it");
    }

    // 22. ITERATING BEATS ONE LINEAR SHOT ON A BIG MOVE, which is the whole reason it exists.
    //     Measured on the user's own hand grade: Offset Temp -0.167 is 3.3 natural steps, the
    //     linear model said that buys b* -8.8, and it delivered -7.0 — 84% of linear, so a
    //     single solve asked for b* -7.0 comes back a quarter short. This asks for a move well
    //     outside the half-step range test 18 validates and checks that re-measuring closes the
    //     gap rather than merely pointing the right way.
    {
        const int cam = 11, enc = 1;
        float P0[og::analysis::kParamN]; neutral13(P0);
        oga::SampleSet S = make_frame(0);
        oga::classify(S, cam, enc);
        oga::Jac J = oga::jacobian(S, cam, enc, P0);
        oga::Desc d0 = oga::describe(S, cam, enc, P0);

        float dd[oga::kDescN] = {0}, w[oga::kDescN] = {0};
        dd[oga::D_B] = -8.0f; w[oga::D_B] = 1.0f;          // a big ask, ~3 steps of Offset Temp
        bool allow[og::analysis::kParamN] = {false}; allow[6] = true;

        float dp1[og::analysis::kParamN]; oga::solve_intent(J, dd, w, allow, 1e-4f, dp1);
        float Pone[og::analysis::kParamN]; oga::apply_move(P0, dp1, Pone);
        const float errOne = std::fabs((oga::describe(S, cam, enc, Pone).v[oga::D_B]
                                        - d0.v[oga::D_B]) - (-8.0f));

        float Pit[og::analysis::kParamN];
        oga::solve_intent_iter(S, cam, enc, P0, dd, w, allow, 1e-4f, 3, Pit);
        const float errIt = std::fabs((oga::describe(S, cam, enc, Pit).v[oga::D_B]
                                       - d0.v[oga::D_B]) - (-8.0f));

        const bool ok = (errOne > 0.15f)        // the single shot really does miss, as measured
                     && (errIt < errOne / 4.f)  // and iterating closes most of the gap
                     && (errIt < 0.05f)         // landing the value, not just the direction
                     && (Pit[6] < Pone[6]);     // by reaching FURTHER, since the response saturates
        check(ok, "iterative solve lands a large move that one linear shot undershoots");
    }

    // 23. ATTRIBUTION — which control actually caused which descriptor change.
    //     Exists because I got this wrong on real data: chroma rose 1.2 between the Creative
    //     grade and the hand grade, I called it "more density", and density had in fact gone
    //     DOWN 0.053 while Lift, Gain and Offset Temp pushed chroma up. Reading a descriptor
    //     and naming the obvious control does not work; the controls overlap too much.
    {
        const int cam = 11, enc = 1;
        float P0[og::analysis::kParamN]; neutral13(P0);
        oga::SampleSet S = make_frame(0);
        oga::classify(S, cam, enc);
        oga::Jac J = oga::jacobian(S, cam, enc, P0);

        // The shape of the real case: chroma pulled DOWN by density while other controls push
        // it up, so the net can move opposite to the control a human would blame.
        float P1[og::analysis::kParamN]; neutral13(P1);
        P1[2] = -0.05f;    // density down
        P1[5] =  1.10f;    // gain up
        P1[6] = -0.05f;    // offset temp down
        oga::Attribution A = oga::attribute(S, cam, enc, J, P0, P1);

        bool ok = true;
        // Parts sum to the linear total, for every descriptor — the decomposition is complete,
        // not a selection of the interesting terms.
        for (int d = 0; d < oga::kDescN; ++d) {
            float s = 0.f;
            for (int p = 0; p < oga::kParamN; ++p) s += A.at(d, p);
            ok &= close(s, A.linear[d], 1e-3f);
        }
        // Over a move this size the linear estimate should still track the measurement.
        ok &= std::fabs(A.linear[oga::D_B] - A.actual[oga::D_B]) < 0.35f;

        // Offset Temp is the top driver of b*, and it is named without anyone saying so.
        int drv[oga::kParamN];
        const int nd = oga::top_drivers(A, oga::D_B, 3, drv);
        ok &= (nd >= 1) && (drv[0] == 6);

        // And the point of the whole test: density's contribution to chroma is NEGATIVE while
        // other controls are positive, so the net sign does not identify the control.
        ok &= (A.at(oga::D_CHROMA, 2) < 0.f);
        ok &= (A.at(oga::D_CHROMA, 5) > 0.f);

        // Untouched controls contribute exactly nothing.
        ok &= close(A.at(oga::D_B, 3), 0.f, 1e-9f) && close(A.at(oga::D_B, 9), 0.f, 1e-9f);
        check(ok, "attribution decomposes a grade into per-control contributions");
    }

    // 24. SIGNED AXES STEER, MAGNITUDES DO NOT — the finding that reshaped the descriptor set.
    //     Measured on the beach sunset, neutral -> grade: b* predicted to 5%, C* to 37-57%, and
    //     `sep` came back +1.1 against a measurement of -3.8, i.e. the wrong SIGN. A distance is
    //     a positive quantity built from squares, so a linear model cannot express "apart in a,
    //     together in b" cancelling. The separation triple exists because of that: three signed
    //     Lab components instead of one distance, which also gives the TONE axis the user named
    //     ("a different hue or tone level") and the original sep did not have at all.
    //
    //     This checks the property the whole steerable set depends on, over a multi-control move
    //     of the kind a real grade makes.
    {
        const int cam = 11, enc = 1;
        float P0[og::analysis::kParamN]; neutral13(P0);
        oga::SampleSet S = make_frame(0);
        oga::classify(S, cam, enc);
        oga::Jac J = oga::jacobian(S, cam, enc, P0);
        oga::Desc d0 = oga::describe(S, cam, enc, P0);
        float scale[oga::kDescN]; desc_scales(J, scale);

        // Several controls at once, in the proportions a look actually uses.
        float dp[og::analysis::kParamN] = {0};
        dp[2] = 0.5f; dp[3] = -0.5f; dp[5] = 0.5f; dp[6] = -0.5f;
        float pred[oga::kDescN]; oga::jac_predict(J, dp, pred);
        float P1[og::analysis::kParamN]; oga::apply_move(P0, dp, P1);
        oga::Desc d1 = oga::describe(S, cam, enc, P1);

        double worstSigned = 0.0; int wd = 0;
        for (int d = 0; d < oga::kSteerableDescN; ++d) {
            const double e = std::fabs(pred[d] - (d1.v[d]-d0.v[d])) / scale[d];
            if (e > worstSigned) { worstSigned = e; wd = d; }
        }
        // The separation triple is inside the steerable range and the two magnitudes are not —
        // structural, so a future edit cannot quietly let a distance back into a solve.
        bool ok = (oga::D_DL < oga::kSteerableDescN)
               && (oga::D_DA < oga::kSteerableDescN)
               && (oga::D_DB < oga::kSteerableDescN)
               && (oga::D_CHROMA >= oga::kSteerableDescN)
               && (oga::D_SEP    >= oga::kSteerableDescN);
        ok &= (worstSigned < 0.30);
        if (!ok) printf("      (worst signed descriptor: %s at %.3f of scale)\n",
                        oga::desc_name(wd), worstSigned);
        check(ok, "signed descriptors steer; magnitudes are excluded from the steerable set");
    }

    // 25. The separation triple carries TONE as well as hue, which is the half the original
    //     distance was missing. On a frame that is brighter AND warmer up top, all three
    //     components have to register it — and on a flat frame all three must vanish, or a rule
    //     targeting separation would chase noise on footage that has none.
    {
        const int cam = 11, enc = 1;
        float P0[og::analysis::kParamN]; neutral13(P0);

        oga::SampleSet two = make_frame(0);
        oga::classify(two, cam, enc);
        oga::Desc d2 = oga::describe(two, cam, enc, P0);

        oga::SampleSet flat = make_frame(1);
        oga::classify(flat, cam, enc);
        oga::Desc df = oga::describe(flat, cam, enc, P0);

        const bool ok =
            d2.v[oga::D_DL] > 2.f  &&        // top band genuinely lighter, in L*
            d2.v[oga::D_DB] > 10.f &&        // ...and warmer
            std::fabs(df.v[oga::D_DL]) < 1.f &&   // flat frame: no tone separation
            std::fabs(df.v[oga::D_DA]) < 1.f &&   // ...no hue separation either
            std::fabs(df.v[oga::D_DB]) < 1.f;
        check(ok, "separation triple registers tone and hue, and vanishes on a flat frame");
    }

    // 26. CREATIVE'S BLACK POINT: solving it is consistent across footage, stamping a fixed
    //     Lift is not. This is the defect three hand grades kept correcting by hand — the
    //     preset wrote Lift 0.11 on every shot, which lands wherever the footage's own floor
    //     happens to put it. Measured: the beach came out ~0.15 and the city 0.161, and both
    //     were pulled down manually, the user's words for the city and car being that the
    //     shadows were lifted too far.
    //
    //     The claim under test is not "the new number is better" — that is a taste question
    //     settled on footage. It is that the RESULT no longer depends on the footage.
    {
        const double target = 0.050, postExp = 0.55, gamma = 1.0, gain = 0.80;
        const double ex = std::exp2(postExp);

        // Four plausible measured floors, spanning what real cameras hand over: a clip that
        // reaches zero, and ones sitting progressively further off it.
        const double floors[4] = { 0.000, 0.030, 0.067, 0.120 };

        double stampedLo = 1e9, stampedHi = -1e9, worstSolved = 0.0;
        for (double d01 : floors) {
            // What the preset used to do: one Lift for everybody.
            const double stamped = (double)og::lgg_core((float)d01, 0.11f, (float)gamma, (float)gain) * ex;
            stampedLo = std::min(stampedLo, stamped);
            stampedHi = std::max(stampedHi, stamped);

            // What it does now: bisect Lift until the black point lands on the target. Same
            // monotonic curve and same 1-D solve Base has always used for its floor.
            double lo = -0.5, hi = 0.5;
            for (int i = 0; i < 40; ++i) {
                const double mid = 0.5*(lo + hi);
                const double v = (double)og::lgg_core((float)d01, (float)mid, (float)gamma, (float)gain) * ex;
                if (v < target) lo = mid; else hi = mid;
            }
            const double lift = 0.5*(lo + hi);
            const double got = (double)og::lgg_core((float)d01, (float)lift, (float)gamma, (float)gain) * ex;
            worstSolved = std::max(worstSolved, std::fabs(got - target));
        }

        const bool ok = (stampedHi - stampedLo > 0.05)   // a fixed lift really does scatter
                     && (worstSolved < 1e-3);            // solving lands every one of them
        check(ok, "Creative places its black point by solving, so footage no longer decides it");
    }

    // 27. MAGIC GRADE picks its control from the pipeline's own arithmetic, not from a table.
    //     Offset is additive, so it is a large RELATIVE shift on a dark region; Gain is
    //     multiplicative, so it grips the bright end. A dark subject therefore resolves to
    //     Offset Temp and a bright one to Gain Temp. On the beach that yields Offset Temp
    //     negative for the water — the control and direction the user reached for by hand.
    {
        oga::RegionStat st[oga::kRegionN];
        // The measured beach: water dark and near-neutral, sky bright and very warm.
        st[oga::R_WATER] = { 51.3f, 40.5f, 12.9f,  4.1f };
        st[oga::R_SKY]   = { 35.7f, 61.1f, 41.2f, 58.3f };
        st[oga::R_TERRAIN] = { 12.0f, 27.6f, 17.7f, 13.1f };

        const oga::MagicChoice c0 = oga::magic_decide(st, 0);
        bool ok = c0.ok && c0.subject == oga::R_WATER && c0.param == 6 && c0.sign < 0;

        // Second press must offer a genuinely different move, and the cycle must wrap rather
        // than run off the end.
        const oga::MagicChoice c1 = oga::magic_decide(st, 1);
        ok &= c1.ok && !(c1.param == c0.param && c1.sign == c0.sign);
        const oga::MagicChoice cw = oga::magic_decide(st, c0.options);
        ok &= cw.ok && cw.param == c0.param && cw.sign == c0.sign;
        ok &= (c0.options >= 2) && (c0.option == 0);
        check(ok, "Magic Grade: dark subject -> Offset Temp, and presses cycle distinctly");
    }

    // 28. A PROTECTED SUBJECT INVERTS THE RULE. Skin is never pushed, so when it is the subject
    //     the move is spent on the surround instead: grip whatever the surround is, and push
    //     away from skin's hue. Cool the room, let the face come forward — which is the same
    //     operation a colourist does, addressed from the other side.
    //
    //     Also checks the veto: a frame with one region has nothing to read a subject against,
    //     and must produce no move rather than an arbitrary one.
    {
        oga::RegionStat st[oga::kRegionN];
        // The measured car portrait: a dark face against a brighter, slightly cooler interior.
        st[oga::R_BUILT] = { 77.7f, 41.0f, -5.5f, -3.7f };
        st[oga::R_SKIN]  = { 22.3f, 14.4f, -2.1f, -1.4f };

        const oga::MagicChoice c = oga::magic_decide(st, 0);
        // Salience puts the face first despite covering a fifth of the frame.
        bool ok = c.ok && c.subject == oga::R_SKIN;
        // Surround is the brighter half, so Gain Temp has the grip on it; skin is the warmer of
        // the two, so the surround goes cooler.
        ok &= (c.param == 0) && (c.sign < 0);
        // Both candidates here resolve to the same move, so the frame offers exactly one and
        // pressing again must not pretend otherwise.
        ok &= (c.options == 1);

        oga::RegionStat one[oga::kRegionN];
        one[oga::R_BUILT] = { 99.9f, 31.0f, -3.3f, -5.4f };
        ok &= !oga::magic_decide(one, 0).ok;      // aerial city: nothing to separate

        // One region that IS the frame, with a sliver beside it. The macro-of-leaves frame
        // measures 98.3% against 1.7%; pushing those apart is a colour cast justified by
        // speckle, not separation.
        oga::RegionStat macro[oga::kRegionN];
        macro[oga::R_BUILT] = { 98.3f, 47.3f,  8.7f, 34.5f };
        macro[oga::R_VEG]   = {  1.7f, 110.9f, 9.0f, 117.2f };
        ok &= !oga::magic_decide(macro, 0).ok;

        // ...but a frame whose second region genuinely clears the coverage floor DOES act, even
        // when the first is large. The downward city view is 92.9% structure against 7.1% roofs
        // and streets, and those two halves look visibly different. The floor and the ceiling
        // used to be independent numbers that disagreed about this exact frame; they are
        // complementary now, so a region is either big enough to matter or it is not, and the
        // answer no longer depends on which constant is consulted.
        oga::RegionStat cityish[oga::kRegionN];
        cityish[oga::R_BUILT]  = { 92.9f, 59.0f, -3.3f,  -8.7f };
        cityish[oga::R_GROUND] = {  7.1f, 55.5f, -3.0f, -15.8f };
        ok &= oga::magic_decide(cityish, 0).ok;

        oga::RegionStat none[oga::kRegionN];
        ok &= !oga::magic_decide(none, 0).ok;     // empty: no move, no crash

        check(ok, "Magic Grade: skin is protected, the surround moves, single-region vetoes");
    }

    // 29. Region statistics respect the fixed-membership rule the whole file depends on: regions
    //     are assigned once and only the STATISTICS move with the grade. If assignment drifted
    //     with the parameters, "this region got cooler" would be unmeasurable by construction.
    {
        const int cam = 11, enc = 1;
        oga::SampleSet S = make_frame(0);
        oga::classify(S, cam, enc);
        oga::stub_regions(S, cam, enc);
        std::vector<uint8_t> before = S.region;

        float P0[og::analysis::kParamN]; neutral13(P0);
        float P1[og::analysis::kParamN]; neutral13(P1); P1[6] = -0.20f;      // a firm Offset Temp move

        oga::RegionStat a[oga::kRegionN], b[oga::kRegionN];
        oga::region_stats(S, cam, enc, P0, a);
        oga::region_stats(S, cam, enc, P1, b);

        bool ok = (S.region == before);                   // assignment untouched by the grade
        float cover = 0.f; int live = 0;
        for (int r = 0; r < oga::kRegionN; ++r) {
            cover += a[r].cover;
            ok &= close(a[r].cover, b[r].cover, 1e-6f);    // coverage cannot move either
            if (a[r].cover > 1.f) ++live;
        }
        ok &= close(cover, 100.f, 0.01f);                 // every sample lands in exactly one
        ok &= (live >= 2);                                 // the stub finds structure at all
        bool moved = false;
        for (int r = 0; r < oga::kRegionN; ++r)
            if (a[r].cover > 1.f && std::fabs(a[r].b - b[r].b) > 0.05f) moved = true;
        ok &= moved;                                       // ...and the statistics DO move

        // DECIMATION MUST CARRY THE REGION LABELS. It did not, and the failure was silent in the
        // worst way: region_stats() bails when region.size() != size(), so a decimated set
        // returned all-zero coverage. Magic Grade measures its move's magnitude that way, so
        // every move came out at exactly zero — a feature that runs, reports what it chose, and
        // changes nothing on screen. Nothing in the decision tests would have caught it.
        oga::SampleSet D = oga::decimate(S, 4000);
        ok &= (D.region.size() == D.size());
        oga::RegionStat d[oga::kRegionN];
        oga::region_stats(D, cam, enc, P0, d);
        float dcover = 0.f;
        for (int r = 0; r < oga::kRegionN; ++r) dcover += d[r].cover;
        ok &= close(dcover, 100.f, 0.01f);
        check(ok, "region stats: membership fixed, coverage sums to 100, survives decimation");
    }

    // 30. BIAS IS CONTINUOUS ACROSS ITS WHOLE RANGE, and holds rather than declining.
    //
    //     The fourth discontinuity in this project, and the first at a FEASIBILITY boundary
    //     rather than at a control's own default. Bias shifts the ceiling target by
    //     -bias*kBiasCeilingPer, so negative Bias walks it upward; it used to clamp at 1.000
    //     while solve_magic_tone_from declines anything reaching kFrameBlown (0.999). From
    //     about bias -1.07 down, ON EVERY FRAME, the solve was asked for exactly what the next
    //     line then refused it for delivering. applyBias() read that decline as "not armed" and
    //     fell through to its coefficient path -- a different control law -- so Lift stepped
    //     -0.134 -> +0.162 between neighbouring slider positions and the picture inverted.
    //
    //     Pinned as CONTINUITY rather than as specific values, because the values are taste and
    //     will move; what must never come back is a step in the middle of a drag.
    {
        float P0[og::analysis::kParamN]; neutral13(P0);
        P0[8] = 0.55f;                       // Creative's post-exposure, which render() applies
        og::grade::Tunables tn;
        // A FLAT FRAME, chosen because it actually reaches the limit. The first version of this
        // test used comfortable percentiles, and they stayed solvable across the whole slider --
        // so it passed with the hold-at-limit logic deleted, testing nothing. These are close
        // together on purpose: a hazy, low-contrast shot is where negative Bias runs out of room
        // first, and it starts declining at about -1.2.
        const double sLo = 0.22, sMid = 0.24, sHi = 0.33, fHi = 0.42, fLo = 0.198;

        bool ok = og::grade::kFrameCeilingMax < og::grade::kFrameBlown;   // the structural fix

        // The contradiction is real, and this is what makes the clamp load-bearing: ask for the
        // ceiling the unclamped code used to ask for and the solve refuses its own answer. If
        // someone raises kFrameCeilingMax to 1.0, this line fails rather than the bug returning.
        const og::grade::MagicTone blown = og::grade::solve_magic_tone_from(
            sLo, sMid, sHi, fHi, P0, nullptr, 0,
            tn.subjFloor, tn.subjMid, 1.000, fLo, tn.frameFloorMax);
        ok &= !blown.ok;

        og::grade::MagicTone prev = og::grade::solve_magic_tone_bias(
            sLo, sMid, sHi, fHi, P0, nullptr, 0, tn, fLo, 2.0);
        ok &= prev.ok;                       // bias 0 solves, so every bias must return a grade
        double worstLift = 0.0, worstGamma = 0.0, worstGain = 0.0;
        bool held = false;                   // did the limit actually get exercised?
        for (double b = 2.0 - 0.02; b >= -2.0001; b -= 0.02) {
            const og::grade::MagicTone t = og::grade::solve_magic_tone_bias(
                sLo, sMid, sHi, fHi, P0, nullptr, 0, tn, fLo, b);
            if (!t.ok) { ok = false; break; }
            // The raw solve at this same bias: where IT declines and the line above still
            // returns a grade is precisely the hold doing its job.
            const og::grade::MagicTone raw = og::grade::solve_magic_tone_from(
                sLo, sMid, sHi, fHi, P0, nullptr, 0,
                std::min(0.40, std::max(0.00, tn.subjFloor + b * og::grade::kBiasSubjFloorPer)),
                tn.subjMid,
                std::min(og::grade::kFrameCeilingMax,
                         std::max(0.60, tn.frameCeiling - b * og::grade::kBiasCeilingPer)),
                fLo, tn.frameFloorMax);
            if (!raw.ok) held = true;
            worstLift  = std::max(worstLift,  (double)std::fabs(t.lift  - prev.lift));
            worstGamma = std::max(worstGamma, (double)std::fabs(t.gamma - prev.gamma));
            worstGain  = std::max(worstGain,  (double)std::fabs(t.gain  - prev.gain));
            prev = t;
        }
        // SELF-CHECKING, so this cannot quietly become a no-op again. If a future change makes
        // this configuration solvable everywhere, the continuity assertion below would still
        // pass while testing none of the holding -- which is exactly how the first draft of this
        // test survived having the hold deleted.
        ok &= held;
        // Generous next to a 0.02 step of the slider, and an order of magnitude under the 0.296
        // jump the bug produced -- this is a cliff detector, not a smoothness assertion.
        ok &= (worstLift < 0.05) && (worstGamma < 0.15) && (worstGain < 0.05);
        check(ok, "Bias is continuous over its full range and holds at the feasible limit");
    }

    // 31. BIAS NEVER CROSSES THE CEILING-GIVES-WAY FALLBACK.
    //
    //     Bit 2 of MagicTone::branch is the fallback that drops the ceiling condition and lets
    //     Gain take the midtone off Gamma. Crossing it changes WHICH CONTROL DOES WHAT, which is
    //     a step in the picture however smoothly the targets moved: on real footage it went Lift
    //     0.003 -> 0.081 and Gain 0.555 -> 0.282 between two neighbouring slider positions, and
    //     the far side was not a different look -- it was a washed-out frame with the blacks off
    //     the floor. Reported twice as the plugin looking broken.
    //
    //     Bit 0 is deliberately NOT included. The frame-floor condition comes and goes smoothly
    //     (one frame runs Lift 0.122 -> 0.083 -> -0.019 straight through it), and holding on any
    //     branch change at all capped that frame at -0.06 -- zero jumps because nothing moved.
    {
        float P0[og::analysis::kParamN]; neutral13(P0);
        P0[8] = 0.55f;
        og::grade::Tunables tn;
        // Found by searching for a configuration where bit 2 actually engages, rather than by
        // reasoning about one -- the same lesson as test 30, which was a no-op until it was
        // pointed at a case that reaches its limit. Here bit 2 wants to engage at about +0.40.
        const double sLo = 0.18, sMid = 0.24, sHi = 0.295, fHi = 0.35, fLo = 0.162;

        const og::grade::MagicTone base = og::grade::solve_magic_tone_bias(
            sLo, sMid, sHi, fHi, P0, nullptr, 0, tn, fLo, 0.0);
        bool ok = base.ok;
        const int shape0 = base.branch & 2;

        bool wouldHaveCrossed = false;
        for (double b = -2.0; b <= 2.0001; b += 0.02) {
            const og::grade::MagicTone t = og::grade::solve_magic_tone_bias(
                sLo, sMid, sHi, fHi, P0, nullptr, 0, tn, fLo, b);
            if (!t.ok) { ok = false; break; }
            ok &= ((t.branch & 2) == shape0);          // the invariant
            const og::grade::MagicTone raw = og::grade::solve_magic_tone_from(
                sLo, sMid, sHi, fHi, P0, nullptr, 0,
                std::min(0.40, std::max(0.00, tn.subjFloor + b * og::grade::kBiasSubjFloorPer)),
                tn.subjMid,
                std::min(og::grade::kFrameCeilingMax,
                         std::max(0.60, tn.frameCeiling - b * og::grade::kBiasCeilingPer)),
                fLo, tn.frameFloorMax);
            if (raw.ok && (raw.branch & 2) != shape0) wouldHaveCrossed = true;
        }
        ok &= wouldHaveCrossed;     // self-checking: this case must still exercise the hold
        check(ok, "Bias never crosses the ceiling-gives-way fallback");
    }

    // 32. A HAND EDIT BECOMES THE THING BIAS LEANS AWAY FROM, instead of being solved away.
    //
    //     Re-anchoring preserves a manual Lift/Gamma/Gain edit on the offset path and CANNOT on
    //     the solving path, which drives back to conditions stored when the button was pressed --
    //     the anchor is only the seed for a bracketed solve. So the conditions move instead.
    //
    //     The identity case is the one that matters most and is the one that failed first: solve
    //     to the conditions a grade ALREADY meets and you must get that grade back. It did not,
    //     because the frame-floor cap is also a constraint and it was still the fitted value, so
    //     re-solving an untouched grade reassigned Lift. Both halves are checked here.
    {
        float P0[og::analysis::kParamN]; neutral13(P0);
        P0[8] = 0.55f;
        og::grade::Tunables tn;
        const double sLo = 0.22, sMid = 0.24, sHi = 0.33, fHi = 0.42, fLo = 0.198;

        const og::grade::MagicTone m0 = og::grade::solve_magic_tone_bias(
            sLo, sMid, sHi, fHi, P0, nullptr, 0, tn, fLo, 0.0);
        bool ok = m0.ok;

        // The grade as armed, then asked for itself.
        float Pm[og::analysis::kParamN]; for (int k = 0; k < og::analysis::kParamN; ++k) Pm[k] = P0[k];
        Pm[3] = m0.lift; Pm[4] = m0.gamma; Pm[5] = m0.gain;
        const og::grade::ToneTargets idt =
            og::grade::tone_targets_of(sLo, sMid, fHi, fLo, Pm, nullptr, 0);
        const og::grade::MagicTone idb = og::grade::solve_magic_tone_bias(
            sLo, sMid, sHi, fHi, Pm, nullptr, 0, tn, fLo, 0.0, idt);
        ok &= idb.ok && close(idb.lift, Pm[3], 0.005f)
                     && close(idb.gamma, Pm[4], 0.02f)
                     && close(idb.gain,  Pm[5], 0.01f);

        // Now a real edit: darker mids, which keeps the highlight where it was. An edit that
        // BLOWS the frame highlight is deliberately not preserved -- honouring it would leave
        // Bias with nowhere to go, which is the whole reason a ceiling condition exists.
        float Ph[og::analysis::kParamN]; for (int k = 0; k < og::analysis::kParamN; ++k) Ph[k] = Pm[k];
        Ph[4] = Pm[4] * 0.85f;
        const og::grade::ToneTargets hnd =
            og::grade::tone_targets_of(sLo, sMid, fHi, fLo, Ph, nullptr, 0);
        const og::grade::MagicTone back = og::grade::solve_magic_tone_bias(
            sLo, sMid, sHi, fHi, Ph, nullptr, 0, tn, fLo, 0.0, hnd);
        ok &= back.ok && close(back.lift, Ph[3], 0.005f)
                      && close(back.gamma, Ph[4], 0.02f)
                      && close(back.gain,  Ph[5], 0.01f);
        ok &= (std::fabs(hnd.mid - idt.mid) > 0.005);   // the edit really did change something

        // AN EDIT THAT LIFTS THE FRAME'S FLOOR PAST THE FITTED CAP. This is the case that makes
        // the frame-floor derivation load-bearing: the cap is a constraint like any other, so a
        // grade sitting above it gets its Lift reassigned and the round trip fails. Checked with
        // a value that clears the cap (0.085) while keeping the highlight under the ceiling
        // clamp, because a blown highlight is refused for its own separate and correct reason.
        float Pf[og::analysis::kParamN]; for (int k = 0; k < og::analysis::kParamN; ++k) Pf[k] = Pm[k];
        Pf[3] = Pm[3] + 0.04f;
        const og::grade::ToneTargets flr =
            og::grade::tone_targets_of(sLo, sMid, fHi, fLo, Pf, nullptr, 0);
        ok &= (flr.floorMax > tn.frameFloorMax);        // ...it really is over the cap
        ok &= (flr.ceil < og::grade::kFrameCeilingMax); // ...and not refused for the other reason
        const og::grade::MagicTone fb = og::grade::solve_magic_tone_bias(
            sLo, sMid, sHi, fHi, Pf, nullptr, 0, tn, fLo, 0.0, flr);
        ok &= fb.ok && close(fb.lift, Pf[3], 0.005f)
                    && close(fb.gamma, Pf[4], 0.02f)
                    && close(fb.gain,  Pf[5], 0.01f);

        check(ok, "a hand edit re-bases Bias instead of being solved away");
    }

    // 33. THE FRAME CEILING HAS TWO CANDIDATES AND NOTHING BETWEEN THEM.
    //
    //     Both endpoints are validated: 0.968 is the hand-graded interview, 0.890 was checked on
    //     five clips where it was visually indistinguishable. Everything in between is a number
    //     nobody has looked at, so the search is a choice between two, not a solve.
    //
    //     Written after the bisection version was tried and was wrong BY CONSTRUCTION. A bisection
    //     converges to the feasibility boundary, so whatever it returns is within its tolerance of
    //     infeasible: on the underexposed clip it settled at 0.9339 while 0.9340 crosses into the
    //     ceiling-gives-way branch and blows the face from 0.566 to 0.993. The grade was correct
    //     and balanced on a knife edge, which is the same defect as the four Bias discontinuities
    //     -- and the reason to pin the SHAPE here rather than the two numbers, since re-fitting an
    //     endpoint is expected and re-introducing a search is not.
    //
    //     Self-checking in the same style as test 31: the sweep must actually reach both endpoints,
    //     or it would pass by never exercising the fallback at all.
    {
        og::grade::Tunables tn;
        bool ok = true, sawLow = false, sawHigh = false;
        for (int step = 0; step <= 14 && ok; ++step) {
            // One knob: how hot the frame's top is. Low, and the subject and the ceiling can both
            // be had at 0.890; high, and holding the ceiling starts costing the subject, which is
            // exactly the condition the second candidate exists for.
            const float hl = 0.50f + 0.02f * (float)step;
            og::analysis::SampleSet S;
            const int N = 256;
            for (int i = 0; i < N; ++i) {
                float v; uint8_t reg;
                if      (i <  40) { v = 0.30f + 0.0015f * (float)i;         reg = og::analysis::R_SKIN; }
                else if (i < 236) { v = 0.32f + 0.0012f * (float)(i - 40);  reg = og::analysis::R_VEG;  }
                else              { v = hl;                                 reg = og::analysis::R_VEG;  }
                S.rgb.push_back(v); S.rgb.push_back(v); S.rgb.push_back(v);
                S.region.push_back(reg);
            }
            float P0[og::analysis::kParamN]; neutral13(P0);
            P0[8] = 0.55f;
            const og::grade::MagicTone r = og::grade::solve_magic_tone(
                S, og::analysis::R_SKIN, 1, 1, nullptr, 0, P0, tn);
            if (!r.ok) continue;                       // a decline is a legitimate answer
            const bool low  = std::fabs(r.ceil - tn.frameCeilingLow) < 1e-5;
            const bool high = std::fabs(r.ceil - tn.frameCeiling)    < 1e-5;
            ok &= (low || high);
            sawLow |= low; sawHigh |= high;
        }
        ok &= sawLow && sawHigh;
        check(ok, "the frame ceiling picks between two validated values, never a solved-for edge");
    }

    // 34. THE SEPARATION TRIPLE OVER REAL REGIONS, alongside the band version rather than
    //     replacing it.
    //
    //     The band triple splits the frame by HEIGHT, which works on a landscape and fails the
    //     moment the subject is not above or below its surround -- two people side by side, a face
    //     against a window. The enum note has called that a stand-in since it was written. This is
    //     the same three signed Lab components asked of the segmentation's subject against
    //     everything else, and it is deliberately a SECOND set of descriptors: the band version and
    //     the fits that stand on it keep working unchanged, and the two can be read off one frame.
    //
    //     Checked on a frame where the two disagree by construction -- subject brighter and warmer
    //     than its surround, but distributed so the vertical thirds cannot see it. A test where
    //     both triples agree would pass with the region masks ignored entirely.
    {
        og::analysis::SampleSet S;
        const int N = 3000;
        for (int i = 0; i < N; ++i) {
            // Subject samples are spread evenly down the frame, so the band split has nothing to
            // find. The period is 5 against decimate()'s stride of 2 below on purpose: the first
            // version used every third sample with a stride of 3, and the thinned set came out
            // 100%% subject with an empty surround -- the gate correctly refused to report a
            // difference against a mean of nothing, and the test caught its own construction.
            const bool subj = (i % 5 < 2);
            const float r = subj ? 0.62f : 0.40f;
            const float g = subj ? 0.58f : 0.40f;
            const float b = subj ? 0.48f : 0.44f;      // subject warmer, surround cooler
            S.rgb.push_back(r); S.rgb.push_back(g); S.rgb.push_back(b);
            S.region.push_back(subj ? og::analysis::R_SKIN : og::analysis::R_VEG);
            S.band.push_back((uint8_t)((i * 3) / N));  // thirds by index, blind to the subject
            S.group.push_back(2); S.mid.push_back(1); S.skin.push_back(0);
        }
        float P0[og::analysis::kParamN]; neutral13(P0);

        // No subject named: the triple reads zero rather than guessing, exactly like the skin pair.
        S.subject = -1;
        const og::analysis::Desc none = og::analysis::describe(S, 1, 1, P0);
        bool ok = (none.v[og::analysis::D_RDL] == 0.f)
               && (none.v[og::analysis::D_RDA] == 0.f)
               && (none.v[og::analysis::D_RDB] == 0.f);

        S.subject = og::analysis::R_SKIN;
        const og::analysis::Desc d = og::analysis::describe(S, 1, 1, P0);
        ok &= (d.v[og::analysis::D_RDL] > 1.f);    // subject is lighter, in L* units
        ok &= (d.v[og::analysis::D_RDB] > 0.5f);   // ...and warmer, b* toward yellow

        // The band triple is blind to it here, which is the point of the construction.
        ok &= (std::fabs(d.v[og::analysis::D_DL]) < 0.5f);

        // decimate() has to carry the subject or the Jacobian differentiates a descriptor that
        // reads zero on the thinned set while the operating point had a value -- the same defect
        // the region copy beside it was added to fix.
        const og::analysis::SampleSet D = og::analysis::decimate(S, 1500);
        const og::analysis::Desc dd = og::analysis::describe(D, 1, 1, P0);
        ok &= (D.subject == og::analysis::R_SKIN);
        ok &= (dd.v[og::analysis::D_RDL] > 1.f);
        ok &= close(dd.v[og::analysis::D_RDL], d.v[og::analysis::D_RDL], 0.5f);

        check(ok, "the separation triple reads real regions, and survives decimation");
    }

    // 35. The Range Balance latch splits the frame where the gap is, not at a fixed percentile.
    // Two synthetic frames with the SAME two populations in very different proportions: a window
    // that is 5% of frame, and a sky that is 60% of it. A percentile cannot serve both -- p98 sits
    // inside the window on one and far above the sky on the other -- and that is what this pins.
    {
        auto build = [](double brightShare) {
            std::vector<float> y;
            for (int i = 0; i < 10000; ++i) {
                const bool hi = (double)i / 10000.0 < brightShare;
                // Each population spread over ~10 units so the histogram has real width.
                const float jitter = 0.10f * (float)(i % 100) / 100.f;
                y.push_back(hi ? 0.80f + jitter : 0.10f + jitter);
            }
            return y;
        };
        bool ok = true;
        const og::grade::RangeLatch w = og::grade::range_latch(build(0.05));
        const og::grade::RangeLatch s = og::grade::range_latch(build(0.60));
        ok &= w.ok && s.ok;
        // Both land in the gap between the populations (10..20 dark, 80..90 bright), regardless
        // of how much of the frame the bright side occupies.
        ok &= (w.latch >= 20.0 && w.latch <= 80.0);
        ok &= (s.latch >= 20.0 && s.latch <= 80.0);
        // ...and the coverage it reports is the bright population itself, not a fixed slice.
        ok &= (w.cover > 3.0  && w.cover < 8.0);
        ok &= (s.cover > 55.0 && s.cover < 65.0);
        // A frame with ONE population has no gap, and saying so is the honest answer -- a latch
        // invented on a flat frame would matte either all of it or none of it, and the caller
        // needs to tell that apart from a measurement.
        ok &= !og::grade::range_latch(std::vector<float>(1000, 0.4f)).ok;
        ok &= !og::grade::range_latch(std::vector<float>(8, 0.4f)).ok;   // too few to split

        // AND THE GAP MUST BE ABSOLUTE, not Otsu's own separability -- which is scale-invariant
        // and so scores a frame spanning seven code values the same as a window against a room.
        // Two clean populations two units apart: bimodal, and nothing worth holding apart.
        std::vector<float> tight;
        for (int i = 0; i < 10000; ++i) tight.push_back(i < 3000 ? 0.30f : 0.32f);
        const og::grade::RangeLatch t = og::grade::range_latch(tight);
        ok &= !t.ok && t.gap < og::grade::kRangeGapMin;
        ok &= (w.gap > og::grade::kRangeGapMin && s.gap > og::grade::kRangeGapMin);
        check(ok, "the range latch splits by population, not by percentile");
    }

    // 36. The locked mask holds its shape while the grade under it moves.
    // Range Balance's mask reads the graded picture, so changing exposure re-cuts it -- which is
    // the defect the reference grade in P[21..23] exists to remove. Measured as coverage: run a
    // ramp through the stage twice, once with the reference following the grade and once with it
    // held, and count how many samples come out held.
    {
        bool ok = true;
        auto coverage = [](float gain, bool lock) {
            float P[og::analysis::kParamN]; neutral13(P);
            P[5]  = gain;                       // the grade that moves under the mask
            P[13] = 45.f;                       // latch
            P[14] = 2.6f;
            P[18] = 1.f;                        // render the matte: the mask IS the output
            P[21] = 0.f; P[22] = 1.f;
            P[23] = lock ? 1.0f : gain;         // reference: held, or following the grade
            int held = 0;
            for (int i = 0; i < 200; ++i) {
                const float v = (float)i / 199.f;
                float r, g, b;
                og::process(/*cam=*/1, /*enc=*/1, P, v, v, v, r, g, b);
                if (r > 0.5f) ++held;
            }
            return held;
        };
        const int base = coverage(1.00f, false);
        ok &= (base > 10 && base < 190);        // the latch actually cuts the ramp somewhere

        // UNLOCKED: brightening pulls more of the picture over the latch, darkening pulls less.
        ok &= (coverage(1.60f, false) > base);
        ok &= (coverage(0.60f, false) < base);

        // LOCKED: the same two grades select exactly what the reference grade selected.
        ok &= (coverage(1.60f, true) == base);
        ok &= (coverage(0.60f, true) == base);

        check(ok, "a locked mask keeps its coverage while the grade under it moves");
    }

    // 37. The shape restricts WHERE Range Balance acts, and multiplies rather than replaces.
    // The case it exists for: two equally bright things, one of which must not be held. No
    // threshold separates them; their positions do.
    {
        bool ok = true;
        auto M = [](float u, float v, int type, float soft, bool inv) {
            return og::shape_mask(u, v, type, /*cx=*/0.f, /*cy=*/0.f,
                                  /*sx=*/0.5f, /*sy=*/0.5f, /*rot=*/0.f, soft, inv);
        };
        ok &= (M(0.f, 0.f, 0, 0.f, false) == 1.f);       // no shape: the stage is untouched
        ok &= (M(9.f, 9.f, 0, 0.f, false) == 1.f);       // ...anywhere at all

        // Inside is held, outside is not, and the boundary sits at the size.
        ok &= (M(0.f,  0.f,  1, 0.f, false) > 0.99f);
        ok &= (M(1.2f, 0.f,  1, 0.f, false) < 0.01f);
        ok &= close(M(0.5f, 0.f, 1, 0.f, false), 0.5f, 0.02f);   // exactly on the edge

        // ROUND ON A 16:9 FRAME: both axes are normalised by half-height, so a point the same
        // distance out in x and in y gets the same answer. Normalising each axis by its own
        // extent -- the obvious thing -- would fail this.
        ok &= close(M(0.35f, 0.f, 1, 0.3f, false), M(0.f, 0.35f, 1, 0.3f, false), 1e-5f);

        // Rectangle holds its corners where the ellipse does not.
        ok &= (M(0.45f, 0.45f, 2, 0.f, false) > 0.99f);
        ok &= (M(0.45f, 0.45f, 1, 0.f, false) < 0.01f);

        // Invert swaps the two sides.
        ok &= (M(0.f, 0.f, 1, 0.f, true) < 0.01f);
        ok &= (M(1.2f, 0.f, 1, 0.f, true) > 0.99f);

        // SOFTNESS FEATHERS SYMMETRICALLY, so softening does not shrink the selection: the
        // half-way point stays on the boundary however soft the edge is.
        ok &= close(M(0.5f, 0.f, 1, 0.8f, false), 0.5f, 0.02f);

        // ...and it MULTIPLIES the luminance mask. Two pixels of identical brightness, one inside
        // the shape and one outside: only the first is held. That is the silk-pillow case, and no
        // latch on its own can do it.
        float P[og::analysis::kParamN]; neutral13(P);
        P[13] = 45.f; P[18] = 1.f;                       // latch, render the matte
        P[24] = 1.f; P[27] = 0.5f; P[28] = 0.5f; P[30] = 0.f;   // ellipse at centre, hard edge
        float r0, g0, b0, r1, g1, b1;
        og::process(1, 1, P, 0.9f, 0.9f, 0.9f, r0, g0, b0, /*shapeM=*/1.0f);   // inside
        og::process(1, 1, P, 0.9f, 0.9f, 0.9f, r1, g1, b1, /*shapeM=*/0.0f);   // outside
        ok &= (r0 > 0.99f && r1 < 0.01f);
        check(ok, "the shape restricts where Range Balance acts, and multiplies the latch");
    }

    // 38. The tone map: contains the range without moving anything below its knee.
    // The defect it exists for: measured over the training corpus at neutral parameters, 9 of 18
    // frames pushed data past 1.0 -- up to 47.8% of channels -- while the SOURCE was pinned
    // essentially nowhere. The footage had the range; the pipeline had no shoulder.
    {
        bool ok = true;
        const float k = 0.40f, W = 3.0f;

        // OFF is white <= knee, and it must be EXACTLY identity -- that is what lets the four
        // render paths carry two floats and no branch.
        for (float v : {0.f, 0.2f, 0.5f, 1.0f, 3.26f})
            ok &= (og::tone_map(v, k, 0.f) == v);

        // Below the knee nothing moves at all. Not "close to" -- the same value.
        for (float v : {0.f, 0.1f, 0.25f, 0.399f})
            ok &= (og::tone_map(v, k, W) == v);

        // C1 AT THE KNEE: slope is exactly 1 there, so no seam appears where it engages. A curve
        // that merely joins up leaves a visible crease in a gradient.
        const float e = 1e-3f;
        const float slope = (og::tone_map(k + e, k, W) - k) / e;
        ok &= close(slope, 1.0f, 5e-3f);

        // The white point maps to display white EXACTLY -- that is what makes it mean "the value
        // that becomes white" instead of an asymptote nobody reaches. softclip() only approaches.
        ok &= close(og::tone_map(W, k, W), 1.0f, 1e-4f);

        // Monotone, and nothing escapes the range however far past white it starts. The rational
        // form diverges above `white`, so the clamp is load-bearing rather than defensive.
        float prev = -1.f;
        for (int i = 0; i <= 400; ++i) {
            const float v = (float)i * 0.05f;
            const float o = og::tone_map(v, k, W);
            ok &= (o <= 1.0f) && (o >= prev);
            prev = o;
        }

        // AND IT KEEPS MORE HIGHLIGHT RANGE THAN THE SOFT CLIP IT REPLACES, which is the whole
        // reason it is not just softclip(): on the frame that started this, softclip left the top
        // decile spanning 0.033 of display range. Same comparison in miniature -- two scene values
        // an octave apart stay further apart under the tone map than under the soft clip.
        const float tmGap = og::tone_map(3.2f, k, W) - og::tone_map(1.6f, k, W);
        const float scGap = og::softclip(3.2f, 0.8f)   - og::softclip(1.6f, 0.8f);
        ok &= (tmGap > 3.0f * scGap);

        check(ok, "the tone map contains the range and keeps the highlights apart");
    }

    printf("%s (%d failure%s)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail, g_fail==1?"":"s");
    return g_fail ? 1 : 0;
}
