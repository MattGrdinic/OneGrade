// OneGrade — CPU unit tests for the color pipeline (OneGradePipeline.h).
// Builds with any C++17 compiler; no OFX/GPU needed. Returns non-zero on failure.
// Copyright (C) 2026 Matthew Grdinic
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/OneGradePipeline.h"
#include "../src/OneGradeAnalysis.h"
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
static void neutral(float P[12]) { for (int i=0;i<12;i++) P[i]=0.f; P[4]=1.f; P[5]=1.f; P[9]=1.f; P[11]=6500.f; }
// Full 13-wide vector (adds P[12] rolloff) for tests that chain whole nodes together.
static void neutral13(float P[13]) { for (int i=0;i<13;i++) P[i]=0.f; P[4]=1.f; P[5]=1.f; P[9]=1.f; P[11]=6500.f; }

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
        float P[12]; neutral(P);
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
        float P[12]; neutral(P); P[5] = 2.0f;            // gain = 2
        float r,g,b; og::process(1, 0, P, 0.f, 0.f, 0.f, r, g, b);
        check(close(r,0.f,2e-3f)&&close(g,0.f,2e-3f)&&close(b,0.f,2e-3f), "gain pins black");
    }

    // 7. Lift pivots white: diffuse white (BMD/DI code ~0.5139 -> linear 1.0) unchanged by lift
    {
        const float whiteCode = og::di_encode(1.0f);     // camera code that decodes to linear 1.0
        float P0[12]; neutral(P0);
        float P1[12]; neutral(P1); P1[3] = -0.25f;        // lift down
        float a0,b0,c0, a1,b1,c1;
        og::process(1, 0, P0, whiteCode, whiteCode, whiteCode, a0, b0, c0);
        og::process(1, 0, P1, whiteCode, whiteCode, whiteCode, a1, b1, c1);
        check(close(a0,a1,5e-3f)&&close(b0,b1,5e-3f)&&close(c0,c1,5e-3f), "lift pins white (diffuse white unchanged)");
    }

    // 8. Lift does not amplify superwhites (BMD/DI code 1.0 -> linear ~100)
    {
        float P0[12]; neutral(P0);
        float P1[12]; neutral(P1); P1[3] = -0.25f;
        float a0,b0,c0, a1,b1,c1;
        og::process(1, 5, P0, 1.0f, 1.0f, 1.0f, a0, b0, c0);   // enc=5 linear so we compare raw
        og::process(1, 5, P1, 1.0f, 1.0f, 1.0f, a1, b1, c1);
        check(close(a0,a1,1e-2f), "lift leaves superwhites untouched");
    }

    // 9. RAW exposure: +1 stop doubles scene-linear (linear output path)
    {
        float P0[12]; neutral(P0);
        float P1[12]; neutral(P1); P1[10] = 1.0f;    // +1 stop
        float a0,b0,c0, a1,b1,c1;
        og::process(1, 5, P0, 0.5f, 0.5f, 0.5f, a0, b0, c0);   // enc=5 = linear output
        og::process(1, 5, P1, 0.5f, 0.5f, 0.5f, a1, b1, c1);
        check(close(a1,2.0f*a0,2e-2f)&&close(b1,2.0f*b0,2e-2f)&&close(c1,2.0f*c0,2e-2f),
              "Scene Exposure +1 stop doubles linear output");
    }

    // 10. RAW temperature: neutral at 6500; warmer raises R / lowers B, cooler the reverse
    {
        float P6[12]; neutral(P6);                    // rawTemp = 6500 (neutral)
        float Pw[12]; neutral(Pw); Pw[11] = 9000.f;   // warmer
        float Pc[12]; neutral(Pc); Pc[11] = 4000.f;   // cooler
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
        float Pn[13]; neutral13(Pn);
        float Pg[13]; neutral13(Pg);                 // a real look on the output node
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
        float P[13]; neutral13(P);
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
        float P[13]; neutral13(P);
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
                    float P0[13]; neutral13(P0);
                    float n_r, n_g, n_b;
                    og::process(cam, enc, P0, x, x, x, n_r, n_g, n_b);   // neutral = "measured"

                    for (auto lgg : { std::array<float,3>{0.05f, 1.15f, 0.80f},
                                      std::array<float,3>{-0.03f, 0.90f, 1.30f},
                                      std::array<float,3>{0.12f, 1.00f, 0.55f} }) {
                        float P1[13]; neutral13(P1);
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
        float P0[13]; neutral13(P0);

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
        float P0[13]; neutral13(P0);
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
        float P0[13]; neutral13(P0);
        oga::SampleSet S = make_frame(0);
        oga::classify(S, cam, enc);
        oga::Jac J = oga::jacobian(S, cam, enc, P0);
        oga::Desc d0 = oga::describe(S, cam, enc, P0);

        bool ok = true;
        for (int p : {2, 5, 6, 9}) {          // density, gain, offset temp, contrast
            double prev = 0.0;
            for (float mag : {0.8f, 0.4f, 0.2f}) {
                float dp[13] = {0}; dp[p] = mag;
                float pred[oga::kDescN]; oga::jac_predict(J, dp, pred);
                float P1[13]; oga::apply_move(P0, dp, P1);
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
        float P0[13]; neutral13(P0);
        oga::SampleSet S = make_frame(0);
        oga::classify(S, cam, enc);
        oga::Jac J = oga::jacobian(S, cam, enc, P0);
        oga::Desc d0 = oga::describe(S, cam, enc, P0);
        float scale[oga::kDescN]; desc_scales(J, scale);
        bool allow[13]; oga::steer_mask(P0, allow);

        double worst = 0.0;
        for (int p = 0; p < 13; ++p) {
            if (!allow[p]) continue;
            for (float mag : {-0.5f, 0.5f}) {
                float dp[13] = {0}; dp[p] = mag;
                float pred[oga::kDescN]; oga::jac_predict(J, dp, pred);
                float P1[13]; oga::apply_move(P0, dp, P1);
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
        float P0[13]; neutral13(P0);
        oga::SampleSet S = make_frame(0);
        oga::classify(S, cam, enc);
        oga::Jac J = oga::jacobian(S, cam, enc, P0);
        oga::Desc d0 = oga::describe(S, cam, enc, P0);

        // (a) single intent, single control
        float dd[oga::kDescN] = {0}, w[oga::kDescN] = {0};
        dd[oga::D_B] = -3.0f; w[oga::D_B] = 1.0f;
        bool allow[13] = {false}; allow[6] = true;
        float dp[13]; oga::solve_intent(J, dd, w, allow, 1e-4f, dp);
        float P1[13]; oga::apply_move(P0, dp, P1);
        oga::Desc d1 = oga::describe(S, cam, enc, P1);
        const float got = d1.v[oga::D_B] - d0.v[oga::D_B];

        bool ok = (P1[6] < 0.f)                    // it reached for NEGATIVE Offset Temp
               && (std::fabs(got - (-3.0f)) < 0.3f);  // and landed within 10% of the ask
        for (int i = 0; i < 13; ++i) if (!allow[i]) ok &= close(P1[i], P0[i], 1e-6f);

        // (b) two intents, two controls — the case a real Magic Grade rule would produce
        float dd2[oga::kDescN] = {0}, w2[oga::kDescN] = {0};
        dd2[oga::D_B] = -3.0f;      w2[oga::D_B] = 1.0f;
        dd2[oga::D_CHROMA] = 2.0f;  w2[oga::D_CHROMA] = 1.0f;
        bool allow2[13] = {false}; allow2[6] = true; allow2[2] = true;
        float dp2[13]; oga::solve_intent(J, dd2, w2, allow2, 1e-4f, dp2);
        float P2[13]; oga::apply_move(P0, dp2, P2);
        oga::Desc d2 = oga::describe(S, cam, enc, P2);
        ok &= std::fabs((d2.v[oga::D_B]      - d0.v[oga::D_B])      - (-3.0f)) < 0.3f;
        ok &= std::fabs((d2.v[oga::D_CHROMA] - d0.v[oga::D_CHROMA]) - ( 2.0f)) < 0.2f;
        ok &= (P2[6] < 0.f) && (P2[2] > 0.f);      // cooler balance, more density
        for (int i = 0; i < 13; ++i) if (!allow2[i]) ok &= close(P2[i], P0[i], 1e-6f);

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
        float P[13]; neutral13(P);
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
        float P0[13]; neutral13(P0);

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
        float P0[13]; neutral13(P0);
        oga::SampleSet S = make_frame(0);
        oga::classify(S, cam, enc);
        oga::Jac J = oga::jacobian(S, cam, enc, P0);
        oga::Desc d0 = oga::describe(S, cam, enc, P0);

        float dd[oga::kDescN] = {0}, w[oga::kDescN] = {0};
        dd[oga::D_B] = -8.0f; w[oga::D_B] = 1.0f;          // a big ask, ~3 steps of Offset Temp
        bool allow[13] = {false}; allow[6] = true;

        float dp1[13]; oga::solve_intent(J, dd, w, allow, 1e-4f, dp1);
        float Pone[13]; oga::apply_move(P0, dp1, Pone);
        const float errOne = std::fabs((oga::describe(S, cam, enc, Pone).v[oga::D_B]
                                        - d0.v[oga::D_B]) - (-8.0f));

        float Pit[13];
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
        float P0[13]; neutral13(P0);
        oga::SampleSet S = make_frame(0);
        oga::classify(S, cam, enc);
        oga::Jac J = oga::jacobian(S, cam, enc, P0);

        // The shape of the real case: chroma pulled DOWN by density while other controls push
        // it up, so the net can move opposite to the control a human would blame.
        float P1[13]; neutral13(P1);
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

    printf("%s (%d failure%s)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail, g_fail==1?"":"s");
    return g_fail ? 1 : 0;
}
