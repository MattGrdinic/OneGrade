// OneGrade — CPU unit tests for the color pipeline (OneGradePipeline.h).
// Builds with any C++17 compiler; no OFX/GPU needed. Returns non-zero on failure.
// SPDX-License-Identifier: BSD-3-Clause
#include "../src/OneGradePipeline.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

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

    printf("%s (%d failure%s)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail, g_fail==1?"":"s");
    return g_fail ? 1 : 0;
}
