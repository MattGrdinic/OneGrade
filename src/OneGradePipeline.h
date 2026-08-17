// OneGrade — shared CPU-side color pipeline (single source of truth for the math).
// The GPU kernels (Metal/OpenCL/CUDA) mirror this exact math.
// Copyright (C) 2026 Matthew Grdinic
// SPDX-License-Identifier: GPL-3.0-or-later
//
//  Faithful to the reference node tree:  CST -> Balance -> Density -> Exposure -> Output
//    0. RAW       : exposure (stops) + white balance (Kelvin) on scene-linear, pre-CST
//                   (mirrors the Camera RAW tab so that tab can be left untouched)
//    1. camera log -> scene-linear            (per-camera decode)
//    2. camera gamut -> XYZ -> DaVinci Wide Gamut LINEAR   (working space, matches tree)
//    3. Balance   : 2-axis white balance (temp/tint) in linear      [linear color-wheel analog]
//    4. Density   : HSV saturation gain  (the "green channel of Gain in HSV" trick)
//    5. Exposure  : basic Lift / Gamma / Gain
//    6. Output    : DWG -> {Rec.709 Scene | Rec.709 Gamma 2.2 | Rec.709 Gamma 2.4 | Cineon Log |
//                   DaVinci Intermediate | Linear}
//                   Lift/Gamma/Gain runs in the chosen Rec.709 display curve (Scene OETF or
//                   pure 2.2/2.4) so the wheels read linearly on whichever timeline you match.
//
//  P[] layout: {temp, tint, density, lift, gamma, gain, offTemp, offTint, postExp, postCon,
//               rawExp, rawTemp, rolloff}   (postExp/postCon/rolloff applied by the caller,
//               not process(); rolloff only on display-referred output — see softclip())
#pragma once
#include <cmath>
#include <algorithm>

namespace og {

static inline float safe_pow(float b, float e) { return powf(b < 0.f ? 0.f : b, e); }
static inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

// ---------- camera log -> scene-linear (mid-gray 0.18) ----------
static inline float decode_log(int cam, float x)
{
    if (cam == 0) { // Blackmagic Film Gen 5 (BMD Gen 5 Color Science white paper)
        const float A=0.08692876f,B=0.00549407f,C=0.53001334f,D=8.28360593f,E=0.09246575f,LC=0.13388378f;
        return (x > LC) ? (expf((x - C)/A) - B) : ((x - E)/D);
    } else if (cam == 1) { // DaVinci Intermediate
        const float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LC=0.02740668f;
        return (x > LC) ? (exp2f(x/C - B) - A) : (x / M);
    } else if (cam == 2) { // Sony S-Log3
        return (x >= 171.2102946f/1023.0f)
            ? (safe_pow(10.0f,(x*1023.0f-420.0f)/261.5f)*0.19f - 0.01f)
            : ((x*1023.0f-95.0f)*0.01125000f/(171.2102946f-95.0f));
    } else if (cam == 3) { // ARRI LogC3 (EI800)
        const float a=5.555556f,b=0.052272f,c=0.247190f,d=0.385537f,e=5.367655f,f=0.092809f,cut=0.010591f;
        return (x > e*cut+f) ? ((safe_pow(10.0f,(x-d)/c)-b)/a) : ((x-f)/e);
    } else if (cam == 4) { // ARRI LogC4
        const float a=(exp2f(18.0f)-16.0f)/117.45f,b=(1023.0f-95.0f)/1023.0f,c=95.0f/1023.0f;
        const float s=(7.0f*logf(2.0f)*exp2f(7.0f-14.0f*c/b))/(a*b);
        const float t=(exp2f(14.0f*(-c/b)+6.0f)-64.0f)/a;
        float p=(x-c)/b; float hi=(exp2f(14.0f*p+6.0f)-64.0f)/a;
        return (hi > t) ? hi : (x*s);
    } else if (cam == 5) { // Canon Log 3
        float v=x;
        if (v < 0.09755646f) return -(safe_pow(10.0f,(0.07623209f - v)/0.42889912f)-1.0f)/14.98325f;
        if (v <= 0.15277891f) return (v - 0.12512219f)/1.9754798f;
        return (safe_pow(10.0f,(v - 0.19022340f)/0.42889912f)-1.0f)/14.98325f;
    } else if (cam == 6) { // RED Log3G10
        return (safe_pow(10.0f, x/0.224282f) - 1.0f)/155.975327f - 0.01f;
    } else if (cam == 7) { // DJI D-Log
        return (x <= 0.14f) ? ((x-0.0929f)/6.025f) : (safe_pow(10.0f,(x-0.5595f)/0.9892f) - 0.0108f);
    } else if (cam == 8) { // Fuji F-Log2
        const float a=5.555556f,b=0.064829f,c=0.245281f,d=0.384316f,e=8.799461f,f=0.092864f,cut=0.100686685f;
        return (x >= cut) ? ((safe_pow(10.0f,(x-d)/c)-b)/a) : ((x-f)/e);
    } else if (cam == 9) { // Panasonic V-Log
        return (x < 0.181f) ? ((x - 0.125f)/5.6f) : (safe_pow(10.0f,(x - 0.598206f)/0.241514f) - 0.00873f);
    } else if (cam == 10) { // Rec.2100 HLG (inverse OETF, reference-white normalised)
        const float a=0.17883277f,b=0.28466892f,c=0.55991073f;
        float e = (x <= 0.5f) ? (x*x/3.0f) : ((expf((x-c)/a)+b)/12.0f);
        return e * 3.774f;   // 75% HLG ref white -> ~1.0
    } else { // Rec.2100 PQ / ST.2084 (inverse EOTF, reference-white normalised)
        const float m1=0.1593017578125f,m2=78.84375f,c1=0.8359375f,c2=18.8515625f,c3=18.6875f;
        float p = safe_pow(x, 1.0f/m2);
        float num = p - c1; if (num < 0.f) num = 0.f;
        float e = safe_pow(num/(c2 - c3*p), 1.0f/m1);
        return e * 49.26f;   // 203-nit PQ ref white -> ~1.0
    }
}

static inline void mul33(const float m[9], const float v[3], float o[3])
{
    o[0]=m[0]*v[0]+m[1]*v[1]+m[2]*v[2];
    o[1]=m[3]*v[0]+m[4]*v[1]+m[5]*v[2];
    o[2]=m[6]*v[0]+m[7]*v[1]+m[8]*v[2];
}

// camera native gamut -> CIE XYZ (D65)
static inline void to_XYZ(int cam, const float v[3], float o[3])
{
    float m[9];
    if (cam == 0) { float t[9]={0.6065384f,0.2204127f,0.1235048f, 0.2679929f,0.8327485f,-0.1007414f, -0.0294426f,-0.0866124f,1.2048076f}; for(int i=0;i<9;i++)m[i]=t[i]; } // Blackmagic Wide Gamut Gen 4/5
    else if (cam == 1) { float t[9]={0.7006223f,0.1487748f,0.1010587f, 0.2740150f,0.8736457f,-0.1476607f, -0.0989629f,-0.1378905f,1.3259942f}; for(int i=0;i<9;i++)m[i]=t[i]; } // DaVinci Wide Gamut
    else if (cam == 2) { float t[9]={0.5990839f,0.2489255f,0.1024464f, 0.2150758f,0.8850685f,-0.1001443f, -0.0320658f,-0.0276902f,1.1487819f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    else if (cam == 3) { float t[9]={0.6380076f,0.2147014f,0.0977226f, 0.2919283f,0.8238731f,-0.1158014f, 0.0027932f,-0.0670795f,1.1533751f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    else if (cam == 4) { float t[9]={0.7048583f,0.1297602f,0.1158373f, 0.2545241f,0.7814843f,-0.0360084f, 0.0f,0.0f,1.0890577f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    else if (cam == 6) { float t[9]={0.7352750f,0.0686090f,0.1465710f, 0.2866940f,0.8429790f,-0.1296730f, -0.0796810f,-0.3473430f,1.5164950f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    else if (cam == 9) { float t[9]={0.6796440f,0.1522110f,0.1186000f, 0.2606860f,0.7748940f,-0.0355800f, -0.0093100f,-0.0046120f,1.1029800f}; for(int i=0;i<9;i++)m[i]=t[i]; } // Panasonic V-Gamut
    else { float t[9]={0.6369580f,0.1446169f,0.1688810f, 0.2627002f,0.6779981f,0.0593017f, 0.0f,0.0280727f,1.0609851f}; for(int i=0;i<9;i++)m[i]=t[i]; } // 5,7,8 -> Rec2020 stand-in
    mul33(m, v, o);
}

// CIE XYZ(D65) -> DaVinci Wide Gamut linear (working space)
static inline void XYZ_to_DWG(const float v[3], float o[3])
{
    const float m[9]={ 1.5166283f,-0.2814601f,-0.1469306f, -0.4647205f,1.2513509f,0.1747665f, 0.0648641f,0.1091221f,0.7613593f };
    mul33(m, v, o);
}
// DaVinci Wide Gamut linear -> CIE XYZ(D65)
static inline void DWG_to_XYZ(const float v[3], float o[3])
{
    const float m[9]={ 0.7006223f,0.1487748f,0.1010587f, 0.2740150f,0.8736457f,-0.1476607f, -0.0989629f,-0.1378905f,1.3259942f };
    mul33(m, v, o);
}
// CIE XYZ(D65) -> Rec.709 linear
static inline void XYZ_to_709(const float v[3], float o[3])
{
    const float m[9]={ 3.2409699f,-1.5373832f,-0.4986108f, -0.9692436f,1.8759675f,0.0415551f, 0.0556301f,-0.2039770f,1.0569715f };
    mul33(m, v, o);
}

// ---------- RAW-style white balance (mirrors the Camera RAW tab's Temp control) ----------
// Correlated colour temperature (Kelvin) -> CIE xy on the Planckian locus (Kim et al. 2002).
static inline void cct_to_xy(float T, float& x, float& y)
{
    float t1 = 1.0f/T, t2 = t1*t1, t3 = t2*t1;
    float xc = (T < 4000.0f)
        ? (-0.2661239e9f*t3 - 0.2343589e6f*t2 + 0.8776956e3f*t1 + 0.179910f)
        : (-3.0258469e9f*t3 + 2.1070379e6f*t2 + 0.2226347e3f*t1 + 0.240390f);
    float xc2 = xc*xc, xc3 = xc2*xc;
    float yc = (T < 2222.0f) ? (-1.1063814f*xc3 - 1.34811020f*xc2 + 2.18555832f*xc - 0.20219683f)
             : (T < 4000.0f) ? (-0.9549476f*xc3 - 1.37418593f*xc2 + 2.09137015f*xc - 0.16748867f)
                             : ( 3.0817580f*xc3 - 5.87338670f*xc2 + 3.75112997f*xc - 0.37001483f);
    x = xc; y = yc;
}
// Bradford chromatic adaptation of an XYZ triplet, treating the scene as lit by a
// blackbody at T Kelvin and adapting it to the D65 working white. Raising T (bluer
// assumed source) warms the image; lowering it cools — matching Resolve's Temp knob.
// Identity at 6500 K.
static inline void white_balance(float T, float v[3])
{
    if (T <= 0.0f || (T > 6499.0f && T < 6501.0f)) return;
    float sx, sy; cct_to_xy(T, sx, sy);
    float sw[3] = { sx/sy, 1.0f, (1.0f - sx - sy)/sy };   // source white (blackbody@T)
    const float dw[3] = { 0.95047f, 1.0f, 1.08883f };      // dest white (D65)
    const float MA[9]  = { 0.8951f,0.2664f,-0.1614f, -0.7502f,1.7135f,0.0367f, 0.0389f,-0.0685f,1.0296f };
    const float MAi[9] = { 0.9869929f,-0.1470543f,0.1599627f, 0.4323053f,0.5183603f,0.0492912f, -0.0085287f,0.0400428f,0.9684867f };
    float sl[3], dl[3], pl[3];
    mul33(MA, sw, sl); mul33(MA, dw, dl); mul33(MA, v, pl);
    pl[0] *= dl[0]/sl[0]; pl[1] *= dl[1]/sl[1]; pl[2] *= dl[2]/sl[2];   // von Kries scale in LMS
    mul33(MAi, pl, v);
}

// ---------- HSV (works on linear values; V = max, S in [0,1]) ----------
static inline void rgb2hsv(float r,float g,float b,float& h,float& s,float& v)
{
    float mx = fmaxf(r, fmaxf(g,b)), mn = fminf(r, fminf(g,b));
    v = mx; float d = mx - mn;
    s = (mx <= 0.f) ? 0.f : (d / mx);
    if (d <= 0.f) { h = 0.f; return; }
    if (mx == r)      h = (g-b)/d + (g < b ? 6.f : 0.f);
    else if (mx == g) h = (b-r)/d + 2.f;
    else              h = (r-g)/d + 4.f;
    h /= 6.f;
}
static inline void hsv2rgb(float h,float s,float v,float& r,float& g,float& b)
{
    if (s <= 0.f) { r = g = b = v; return; }
    h *= 6.f; int i = (int)floorf(h); float f = h - i;
    float p = v*(1.f-s), q = v*(1.f-s*f), t = v*(1.f-s*(1.f-f));
    i %= 6; if (i < 0) i += 6;
    switch (i) {
        case 0: r=v; g=t; b=p; break;
        case 1: r=q; g=v; b=p; break;
        case 2: r=p; g=v; b=t; break;
        case 3: r=p; g=q; b=v; break;
        case 4: r=t; g=p; b=v; break;
        default:r=v; g=p; b=q; break;
    }
}

// Rec.709 scene OETF (linear toe). LGG runs here so near-blacks stay pinned,
// matching Resolve's timeline wheels.
static inline float r709_enc(float L) { return (L < 0.018f) ? (4.5f*L) : (1.099f*safe_pow(L,0.45f) - 0.099f); }
static inline float r709_dec(float V) { return (V < 0.081f) ? (V/4.5f) : safe_pow((V+0.099f)/1.099f, 1.0f/0.45f); }

// Rec.709 pure-power display gamma (no linear toe), exponent g: 2.2 for web/streaming
// delivery (YouTube et al. — the default encode) or 2.4 for a broadcast/reference
// timeline. Used when the timeline is display-referred rather than Rec.709 (Scene).
static inline float r709_g_enc(float L, float g) { return safe_pow(L, 1.0f/g); }
static inline float r709_g_dec(float V, float g) { return safe_pow(V, g); }

// DaVinci Intermediate log encode/decode (the space the tree's Lift/Gamma/Gain runs in)
static inline float di_encode(float x)
{ const float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LIN=0.00262409f; return (x>LIN)?((log2f(x+A)+B)*C):(x*M); }
static inline float di_decode(float x)
{ const float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LC=0.02740668f; return (x>LC)?(exp2f(x/C-B)-A):(x/M); }

static inline float encode(int enc, float x)
{
    if (enc == 0) return r709_enc(x);                                    // Rec.709 (Scene) OETF
    if (enc == 1) return r709_g_enc(x, 2.2f);                            // Rec.709 (Gamma 2.2)
    if (enc == 2) return r709_g_enc(x, 2.4f);                            // Rec.709 (Gamma 2.4)
    if (enc == 3) { float code = 685.0f + 300.0f*log10f(x < 1e-4f ? 1e-4f : x); return std::min(std::max(code/1023.0f,0.0f),1.0f); } // Cineon
    if (enc == 4) { const float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LIN=0.00262409f; return (x>LIN)?((log2f(x+A)+B)*C):(x*M); } // DI
    return x;                                                             // linear
}

// Lift/Gamma/Gain, the arithmetic core, on a value ALREADY in the display curve.
// Extracted from step 6 of process() so there is one definition rather than two: the Clean
// auto-grade solver in OneGrade.cpp evaluates the same curve to place a frame's percentiles,
// and a second copy of this would drift the first time step 6 changed.
//
//   gain  : multiply, pivots black
//   lift  : pivots white, reach clamped at white so superwhites aren't amplified
//   gamma : power, pivots black AND white
//
// Monotonic in v, which is what lets the solver work on three percentiles instead of every
// sample -- a monotonic map commutes with percentiles.
//
// Negatives pass through untouched: safe_pow() floors at 0, which would hard-clip
// out-of-gamut values even at neutral gamma and destroy the scene-referred Scene/DI/Linear
// feeds. No-op for the display encodes (2.2/2.4/Cineon already clamp negatives upstream).
static inline float lgg_core(float v, float lift, float gamma, float gain)
{
    v = v * gain;
    v = v + lift * (1.0f - (v < 1.0f ? v : 1.0f));
    return (v < 0.f) ? v : safe_pow(v, 1.0f/gamma);
}

// ---------- 3D LUT trilinear sampling + mix ----------
//  lut: N*N*N*3, red index fastest. Input rgb clamped to [0,1]. mix in [0,1].
static inline void apply_lut(const float* lut, int N, float mix,
                             float& r, float& g, float& b)
{
    if (N < 2 || mix <= 0.f) return;
    float cr = clamp01(r)*(N-1), cg = clamp01(g)*(N-1), cb = clamp01(b)*(N-1);
    int r0=(int)cr, g0=(int)cg, b0=(int)cb;
    int r1 = r0<N-1?r0+1:r0, g1 = g0<N-1?g0+1:g0, b1 = b0<N-1?b0+1:b0;
    float fr=cr-r0, fg=cg-g0, fb=cb-b0;
    #define PG_IDX(R,G,B) (((size_t)(B)*N + (G))*N + (R))*3
    float out[3];
    for (int c=0;c<3;c++) {
        float c00 = lut[PG_IDX(r0,g0,b0)+c]*(1-fr) + lut[PG_IDX(r1,g0,b0)+c]*fr;
        float c10 = lut[PG_IDX(r0,g1,b0)+c]*(1-fr) + lut[PG_IDX(r1,g1,b0)+c]*fr;
        float c01 = lut[PG_IDX(r0,g0,b1)+c]*(1-fr) + lut[PG_IDX(r1,g0,b1)+c]*fr;
        float c11 = lut[PG_IDX(r0,g1,b1)+c]*(1-fr) + lut[PG_IDX(r1,g1,b1)+c]*fr;
        float c0 = c00*(1-fg) + c10*fg;
        float c1 = c01*(1-fg) + c11*fg;
        out[c] = c0*(1-fb) + c1*fb;
    }
    #undef PG_IDX
    r = r + (out[0]-r)*mix;
    g = g + (out[1]-g)*mix;
    b = b + (out[2]-b)*mix;
}

// Per-channel display-space highlight roll-off (soft clip): identity below the knee,
// smooth exponential shoulder above, asymptote at 1.0. Saturated super-brights converge
// to white instead of clipping at full chroma (the "neon" look). amt 0 = off; larger amt
// lowers the knee (amt 1 -> knee 0.4). Callers apply it after the trim, and only when the
// output is display-referred (Rec.709 encodes, or any path through a LUT) — never on
// Cineon/DI/Linear feeds to downstream nodes.
static inline float softclip(float v, float amt)
{
    if (amt <= 0.f || v <= 0.f) return v;
    float k = 1.0f - 0.6f*amt;
    if (v <= k) return v;
    float r = 1.0f - k;
    return k + r*(1.0f - expf(-(v - k)/r));
}

// Post-LUT trim in display space: exposure (multiply) then contrast about 0.5.
static inline void apply_trim(float postExp, float postCon, float& r, float& g, float& b)
{
    float ex = exp2f(postExp);
    r = (r*ex - 0.5f)*postCon + 0.5f;
    g = (g*ex - 0.5f)*postCon + 0.5f;
    b = (b*ex - 0.5f)*postCon + 0.5f;
}

// full per-pixel process. in/out are RGB (alpha handled by caller).
// ---------------------------------------------------------------------------------------
// HIGHLIGHT MASK — a luminance qualifier, per pixel.
//
// Deliberately NOT local. A qualifier is a function of the pixel's own value and nothing else, so
// it fits this scalar per-pixel function exactly as it stands: no neighbourhood, no second pass,
// no change to how the three kernels mirror it. Anything that needed neighbouring pixels -- real
// local tone mapping, a spatial blur on the mask -- would not.
//
// Matches Resolve's Luminance qualifier, on its 0..100 scale, so the four numbers a colourist
// reads off that panel can be typed straight in: Low / High with L.Soft / H.Soft widening the
// ramp either side. Returns 1 inside the window and 0 outside.
//
// SMOOTHSTEP, NOT A LINEAR RAMP, at both ends. A linear ramp has a corner where it meets flat,
// and a corner in a mask is a visible contour line in a gradient -- exactly where this gets used,
// on a window or a sky. The cubic is C1 at both joins and costs two multiplies.
//
// THE SOFTNESS IS IN LUMINANCE, NOT IN SPACE, and that is the whole limitation. Where the
// threshold lands on a steep edge -- a window frame -- the mask edge is stable. Where it lands
// inside noise, noise crosses it differently every frame and the mask shimmers. Resolve solves
// that with a spatial blur that a per-pixel function cannot have.
static inline float smooth01(float t)
{
    t = clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

static inline float highlight_mask(float Y, float lo, float hi, float lsoft, float hsoft)
{
    // Guard the degenerate width so a zero-soft edge is a hard step rather than a divide by zero.
    const float le = fmaxf(lsoft, 1e-4f), he = fmaxf(hsoft, 1e-4f);
    const float rise = smooth01((Y - (lo - le)) / (2.0f * le));
    const float fall = 1.0f - smooth01((Y - (hi - he)) / (2.0f * he));
    return rise * fall;
}

static inline void process(int cam, int enc, const float* P, float inR, float inG, float inB,
                           float& outR, float& outG, float& outB)
{
    const float temp=P[0], tint=P[1], density=P[2], lift=P[3], gamma=P[4], gain=P[5], offTemp=P[6], offTint=P[7];
    const float rawExp=P[10], rawTemp=P[11];   // RAW-tab analogs: exposure (stops) + white balance (Kelvin)

    // 0. RAW Exposure — linear gain in stops on scene light, before the CST (matches the
    //    Camera RAW tab's Exposure, which acts on sensor-linear before the log curve).
    float ex = exp2f(rawExp);
    // 1-2. decode + into DWG-linear working space
    float lin[3] = { decode_log(cam,inR)*ex, decode_log(cam,inG)*ex, decode_log(cam,inB)*ex };
    float xyz[3]; to_XYZ(cam, lin, xyz);
    white_balance(rawTemp, xyz);               // RAW Temp: chromatic adapt in XYZ (nearest to sensor)
    float w[3];   XYZ_to_DWG(xyz, w);

    // 3. Balance (2-axis white balance in linear)
    //    Gain (multiplicative) — neutral highlights
    w[0] *= (1.0f + temp*0.20f);
    w[2] *= (1.0f - temp*0.20f);
    w[1] *= (1.0f + tint*0.20f);
    //    Offset (additive) — shifts every tone's chroma evenly (best for stubborn casts)
    w[0] += offTemp*0.10f;
    w[2] -= offTemp*0.10f;
    w[1] += offTint*0.10f;

    // 4. Density (HSV saturation gain in DI-log — the "green of Gain in HSV" trick).
    //    Run in log (not linear) so saturated highlights get richer instead of blowing out.
    if (density != 0.0f) {
        float l0=di_encode(w[0]), l1=di_encode(w[1]), l2=di_encode(w[2]);
        float h,s,v; rgb2hsv(l0,l1,l2,h,s,v);
        s = clamp01(s * (1.0f + density));
        hsv2rgb(h,s,v,l0,l1,l2);
        w[0]=di_decode(l0); w[1]=di_decode(l1); w[2]=di_decode(l2);
    }

    // 5. Exposure — Lift / Gamma / Gain in DaVinci Intermediate (log), like the tree's
    //    default wheels. Running in log (not linear) makes all three behave consistently
    //    as normal SDR wheels instead of Lift acting like an HDR/linear wheel.
    // 5. Output primaries (still linear)
    float outc[3];
    if (enc <= 3) {                       // Rec.709 primaries: Scene(0), Gamma 2.2(1), Gamma 2.4(2), Cineon(3)
        float x2[3]; DWG_to_XYZ(w, x2);
        XYZ_to_709(x2, outc);
    } else {                              // DI(4) / Linear(5) keep DWG primaries
        outc[0]=w[0]; outc[1]=w[1]; outc[2]=w[2];
    }

    // 6. Lift / Gamma / Gain in the Rec.709 display curve, pivoting like Resolve's timeline
    //    wheels. The grade curve FOLLOWS the output so the wheels read linearly on whichever
    //    timeline you match: pure gamma 2.2 or 2.4 for those encodes, else the Scene OETF
    //    (linear toe). Applied as distinct steps so each pins correctly:
    //      gain  : multiply, pivot @ black
    //      lift  : pivot @ white, reach clamped at white so superwhites aren't amplified
    //      gamma : power, pivot @ black & white
    const float dg = (enc == 1) ? 2.2f : (enc == 2) ? 2.4f : 0.0f;   // 0 = Scene OETF
    float d[3];
    for (int i=0;i<3;i++) d[i] = (dg > 0.f) ? r709_g_enc(outc[i], dg) : r709_enc(outc[i]);

    // 6b. RANGE BALANCE — hold the bright areas, open the rest.
    //
    // For footage whose range WAS captured: a window and an unlit room both inside the sensor's
    // latitude, where a single curve has to blow one to serve the other. A luminance mask splits
    // the frame in two and each half gets its own move -- highlights pulled down, everything else
    // lifted -- which is the "qualifier, invert, second node" dance in Resolve reduced to a few
    // sliders.
    //
    // THE MASK READS THE PRE-GRADE DISPLAY LUMINANCE, computed here before lgg_core touches
    // anything. Reading the graded value would make the mask move as the room is lifted, so the
    // control would chase its own output -- the defect that made Magic Grade converge over three
    // presses and the reason Bias re-solves from a frozen anchor. It is measured off `d`, not off
    // `v`, and that ordering is the whole of it.
    //
    // m + (1-m) == 1, so the two moves are a PARTITION and never a double-apply. Holding the
    // highlights alone was tried first and does nothing useful: it cannot recover a window an
    // earlier stage already threw away, which on the test frame was blown identically with the
    // mask on and off while the mask was working correctly.
    const float rbLatch = P[13], rbSoft = P[14], rbHigh = P[15], rbLift = P[16], rbGamma = P[17];
    const bool  rbShow  = (P[18] > 0.5f);   // preview the matte instead of the picture
    // Showing the matte has to work BEFORE the three moves are dialled in -- that is the whole
    // point of it -- so the preview alone is enough to switch the stage on.
    const bool  rbOn = (rbLatch > 0.f) &&
                       (rbShow || rbHigh != 1.f || rbLift != 0.f || rbGamma != 1.f);
    // AFTER THE GRADE CURVE, BEFORE RANGE BALANCE'S OWN MOVES -- which is what a Resolve
    // qualifier sees when you drop it on a node, and the distinction the first version got wrong.
    //
    // Only Range Balance's OWN three moves can make the mask chase itself; Lift/Gamma/Gain cannot,
    // because they run once and are not driven by the mask. Excluding them too was over-correction
    // and it cost the selection: pre-grade the picture is flat, so a bright pillow and a window
    // sit within a few points of each other and NO threshold separates them. The user could not
    // match Resolve's window selection at any latch for exactly this reason. After the grade the
    // two are far apart and the same threshold picks the window alone.
    float v3[3];
    for (int i=0;i<3;i++) v3[i] = lgg_core(d[i], lift, gamma, gain);

    float rbM = 0.f;
    if (rbOn)
        rbM = highlight_mask(100.f*(0.2126f*v3[0] + 0.7152f*v3[1] + 0.0722f*v3[2]),
                             rbLatch, 100.f, rbSoft, rbSoft);

    for (int i=0;i<3;i++) {
        float v = v3[i];
        if (rbOn) {
            const float room = lgg_core(v, rbLift, rbGamma, 1.f);
            const float high = lgg_core(v, 0.f, 1.f, rbHigh);
            v = (1.f - rbM)*room + rbM*high;
        }
        outc[i] = (dg > 0.f) ? r709_g_dec(v, dg) : r709_dec(v);
    }

    // 7. Final output encode. The matte is a MEASUREMENT, not a picture, so it bypasses the
    //    encode entirely -- 0.5 of mask has to read as 0.5, and pushing it through a transfer
    //    function would make a matte that looks like coverage it does not have. The caller
    //    likewise skips the LUT and trim while this is on.
    if (rbShow) { outR = outG = outB = rbM; return; }
    outR = encode(enc, outc[0]);
    outG = encode(enc, outc[1]);
    outB = encode(enc, outc[2]);
}

} // namespace pg
