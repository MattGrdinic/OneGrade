// OneGrade — OpenCL GPU kernel (buffers). Mirrors src/OneGradePipeline.h.
// Copyright (C) 2026 Matthew Grdinic
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN64
#include <Windows.h>
#else
#include <pthread.h>
#endif
#include <map>
#include <cstdio>

#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

const char* KernelSource = "\n" \
"inline float og_pow(float b, float e){ return pow(fmax(b,0.0f), e); }                                          \n" \
"inline float og_decode(int cam, float x){                                                                      \n" \
"  if(cam==0){ float A=0.08692876f,B=0.00549407f,C=0.53001334f,D=8.28360593f,E=0.09246575f,LC=0.13388378f;      \n" \
"    return (x>LC)?(exp((x-C)/A)-B):((x-E)/D); }                                                                 \n" \
"  else if(cam==1){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LC=0.02740668f;                              \n" \
"    return (x>LC)?(exp2(x/C-B)-A):(x/M); }                                                                      \n" \
"  else if(cam==2){ return (x>=171.2102946f/1023.0f)?(og_pow(10.0f,(x*1023.0f-420.0f)/261.5f)*0.19f-0.01f):((x*1023.0f-95.0f)*0.01125f/(171.2102946f-95.0f)); } \n" \
"  else if(cam==3){ float a=5.555556f,b=0.052272f,c=0.247190f,d=0.385537f,e=5.367655f,f=0.092809f,cut=0.010591f;\n" \
"    return (x>e*cut+f)?((og_pow(10.0f,(x-d)/c)-b)/a):((x-f)/e); }                                               \n" \
"  else if(cam==4){ float a=(exp2(18.0f)-16.0f)/117.45f,b=(1023.0f-95.0f)/1023.0f,c=95.0f/1023.0f;              \n" \
"    float s=(7.0f*log(2.0f)*exp2(7.0f-14.0f*c/b))/(a*b); float t=(exp2(14.0f*(-c/b)+6.0f)-64.0f)/a;            \n" \
"    float p=(x-c)/b; float hi=(exp2(14.0f*p+6.0f)-64.0f)/a; return (hi>t)?hi:(x*s); }                          \n" \
"  else if(cam==5){ float v=x; if(v<0.09755646f) return -(og_pow(10.0f,(0.07623209f-v)/0.42889912f)-1.0f)/14.98325f; \n" \
"    if(v<=0.15277891f) return (v-0.12512219f)/1.9754798f; return (og_pow(10.0f,(v-0.19022340f)/0.42889912f)-1.0f)/14.98325f; } \n" \
"  else if(cam==6){ return (og_pow(10.0f,x/0.224282f)-1.0f)/155.975327f-0.01f; }                                \n" \
"  else if(cam==7){ return (x<=0.14f)?((x-0.0929f)/6.025f):(og_pow(10.0f,(x-0.5595f)/0.9892f)-0.0108f); }       \n" \
"  else if(cam==8){ float a=5.555556f,b=0.064829f,c=0.245281f,d=0.384316f,e=8.799461f,f=0.092864f,cut=0.100686685f; \n" \
"    return (x>=cut)?((og_pow(10.0f,(x-d)/c)-b)/a):((x-f)/e); }                                                  \n" \
"  else if(cam==9){ return (x<0.181f)?((x-0.125f)/5.6f):(og_pow(10.0f,(x-0.598206f)/0.241514f)-0.00873f); }      \n" \
"  else if(cam==10){ float a=0.17883277f,b=0.28466892f,c=0.55991073f; float e=(x<=0.5f)?(x*x/3.0f):((exp((x-c)/a)+b)/12.0f); return e*3.774f; } \n" \
"  else { float m1=0.1593017578125f,m2=78.84375f,c1=0.8359375f,c2=18.8515625f,c3=18.6875f; float p=og_pow(x,1.0f/m2); float num=fmax(p-c1,0.0f); float e=og_pow(num/(c2-c3*p),1.0f/m1); return e*49.26f; } } \n" \
"inline float3 og_mv(float3 r0,float3 r1,float3 r2,float3 v){ return (float3)(dot(r0,v),dot(r1,v),dot(r2,v)); }  \n" \
"inline float3 og_toXYZ(int cam, float3 v){                                                                     \n" \
"  if(cam==0) return og_mv((float3)(0.6065384f,0.2204127f,0.1235048f),(float3)(0.2679929f,0.8327485f,-0.1007414f),(float3)(-0.0294426f,-0.0866124f,1.2048076f),v); \n" \
"  else if(cam==1) return og_mv((float3)(0.7006223f,0.1487748f,0.1010587f),(float3)(0.2740150f,0.8736457f,-0.1476607f),(float3)(-0.0989629f,-0.1378905f,1.3259942f),v); \n" \
"  else if(cam==2) return og_mv((float3)(0.5990839f,0.2489255f,0.1024464f),(float3)(0.2150758f,0.8850685f,-0.1001443f),(float3)(-0.0320658f,-0.0276902f,1.1487819f),v); \n" \
"  else if(cam==3) return og_mv((float3)(0.6380076f,0.2147014f,0.0977226f),(float3)(0.2919283f,0.8238731f,-0.1158014f),(float3)(0.0027932f,-0.0670795f,1.1533751f),v); \n" \
"  else if(cam==4) return og_mv((float3)(0.7048583f,0.1297602f,0.1158373f),(float3)(0.2545241f,0.7814843f,-0.0360084f),(float3)(0.0f,0.0f,1.0890577f),v); \n" \
"  else if(cam==6) return og_mv((float3)(0.7352750f,0.0686090f,0.1465710f),(float3)(0.2866940f,0.8429790f,-0.1296730f),(float3)(-0.0796810f,-0.3473430f,1.5164950f),v); \n" \
"  else if(cam==9) return og_mv((float3)(0.6796440f,0.1522110f,0.1186000f),(float3)(0.2606860f,0.7748940f,-0.0355800f),(float3)(-0.0093100f,-0.0046120f,1.1029800f),v); \n" \
"  else return og_mv((float3)(0.6369580f,0.1446169f,0.1688810f),(float3)(0.2627002f,0.6779981f,0.0593017f),(float3)(0.0f,0.0280727f,1.0609851f),v); }  \n" \
"inline float3 og_XYZtoDWG(float3 v){ return og_mv((float3)(1.5166283f,-0.2814601f,-0.1469306f),(float3)(-0.4647205f,1.2513509f,0.1747665f),(float3)(0.0648641f,0.1091221f,0.7613593f),v); } \n" \
"inline float3 og_DWGtoXYZ(float3 v){ return og_mv((float3)(0.7006223f,0.1487748f,0.1010587f),(float3)(0.2740150f,0.8736457f,-0.1476607f),(float3)(-0.0989629f,-0.1378905f,1.3259942f),v); } \n" \
"inline float3 og_XYZto709(float3 v){ return og_mv((float3)(3.2409699f,-1.5373832f,-0.4986108f),(float3)(-0.9692436f,1.8759675f,0.0415551f),(float3)(0.0556301f,-0.2039770f,1.0569715f),v); } \n" \
"inline float2 og_cct_xy(float T){                                                                              \n" \
"  float t1=1.0f/T,t2=t1*t1,t3=t2*t1;                                                                           \n" \
"  float xc=(T<4000.0f)?(-0.2661239e9f*t3-0.2343589e6f*t2+0.8776956e3f*t1+0.179910f):(-3.0258469e9f*t3+2.1070379e6f*t2+0.2226347e3f*t1+0.240390f); \n" \
"  float xc2=xc*xc,xc3=xc2*xc;                                                                                  \n" \
"  float yc=(T<2222.0f)?(-1.1063814f*xc3-1.34811020f*xc2+2.18555832f*xc-0.20219683f):(T<4000.0f)?(-0.9549476f*xc3-1.37418593f*xc2+2.09137015f*xc-0.16748867f):(3.0817580f*xc3-5.87338670f*xc2+3.75112997f*xc-0.37001483f); \n" \
"  return (float2)(xc,yc); }                                                                                    \n" \
"inline float3 og_whitebalance(float T, float3 v){                                                             \n" \
"  if(T<=0.0f || (T>6499.0f && T<6501.0f)) return v;                                                            \n" \
"  float2 xy=og_cct_xy(T); float3 sw=(float3)(xy.x/xy.y,1.0f,(1.0f-xy.x-xy.y)/xy.y); float3 dw=(float3)(0.95047f,1.0f,1.08883f); \n" \
"  float3 ma0=(float3)(0.8951f,0.2664f,-0.1614f),ma1=(float3)(-0.7502f,1.7135f,0.0367f),ma2=(float3)(0.0389f,-0.0685f,1.0296f); \n" \
"  float3 mai0=(float3)(0.9869929f,-0.1470543f,0.1599627f),mai1=(float3)(0.4323053f,0.5183603f,0.0492912f),mai2=(float3)(-0.0085287f,0.0400428f,0.9684867f); \n" \
"  float3 sl=og_mv(ma0,ma1,ma2,sw),dl=og_mv(ma0,ma1,ma2,dw),pl=og_mv(ma0,ma1,ma2,v); pl=pl*(dl/sl); return og_mv(mai0,mai1,mai2,pl); } \n" \
"inline float3 og_rgb2hsv(float3 c){                                                                            \n" \
"  float mx=fmax(c.x,fmax(c.y,c.z)), mn=fmin(c.x,fmin(c.y,c.z)); float v=mx,d=mx-mn; float s=(mx<=0.0f)?0.0f:(d/mx); float h=0.0f; \n" \
"  if(d>0.0f){ if(mx==c.x) h=(c.y-c.z)/d+(c.y<c.z?6.0f:0.0f); else if(mx==c.y) h=(c.z-c.x)/d+2.0f; else h=(c.x-c.y)/d+4.0f; h/=6.0f; } \n" \
"  return (float3)(h,s,v); }                                                                                    \n" \
"inline float3 og_hsv2rgb(float3 c){                                                                            \n" \
"  float h=c.x,s=c.y,v=c.z; if(s<=0.0f) return (float3)(v,v,v);                                                 \n" \
"  h*=6.0f; int i=(int)floor(h); float f=h-(float)i; float p=v*(1.0f-s),q=v*(1.0f-s*f),t=v*(1.0f-s*(1.0f-f)); i=i%6; if(i<0)i+=6; \n" \
"  if(i==0) return (float3)(v,t,p); else if(i==1) return (float3)(q,v,p); else if(i==2) return (float3)(p,v,t); \n" \
"  else if(i==3) return (float3)(p,q,v); else if(i==4) return (float3)(t,p,v); else return (float3)(v,p,q); }   \n" \
"inline float og_r709e(float L){ return (L<0.018f)?(4.5f*L):(1.099f*og_pow(L,0.45f)-0.099f); }                  \n" \
"inline float og_r709d(float V){ return (V<0.081f)?(V/4.5f):og_pow((V+0.099f)/1.099f,1.0f/0.45f); }             \n" \
"inline float og_r709ge(float L, float g){ return og_pow(L,1.0f/g); }                                           \n" \
"inline float og_r709gd(float V, float g){ return og_pow(V,g); }                                                \n" \
"inline float og_lggc(float v,float gain,float lift,float gamma){ v=v*gain; v=v+lift*(1.0f-fmin(v,1.0f)); v=(v<0.0f)?v:og_pow(v,1.0f/gamma); return v; } \n" \
"inline float og_lgg(float L,float gain,float lift,float gamma,float dg){ float v=(dg>0.0f)?og_r709ge(L,dg):og_r709e(L); v=og_lggc(v,gain,lift,gamma); return (dg>0.0f)?og_r709gd(v,dg):og_r709d(v); } \n" \
"inline float og_sm01(float t){ t=fmin(fmax(t,0.0f),1.0f); return t*t*(3.0f-2.0f*t); }                          \n" \
"inline float og_hlmask(float Y,float lo,float s){ float le=fmax(s,1e-4f); return og_sm01((Y-(lo-le))/(2.0f*le)); }        \n" \
"inline float og_dienc(float x){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LIN=0.00262409f; return (x>LIN)?((log2(x+A)+B)*C):(x*M); } \n" \
"inline float og_didec(float x){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LC=0.02740668f; return (x>LC)?(exp2(x/C-B)-A):(x/M); } \n" \
"inline float og_enc(int enc, float x){                                                                         \n" \
"  if(enc==0) return og_r709e(x);                                                             \n" \
"  else if(enc==1) return og_r709ge(x,2.2f);                                                  \n" \
"  else if(enc==2) return og_r709ge(x,2.4f);                                                  \n" \
"  else if(enc==3){ float code=685.0f+300.0f*log10(fmax(x,1e-4f)); return fmin(fmax(code/1023.0f,0.0f),1.0f); }  \n" \
"  else if(enc==4){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LIN=0.00262409f; return (x>LIN)?((log2(x+A)+B)*C):(x*M); } \n" \
"  else return x; }                                                                                             \n" \
"inline float3 og_sampleLUT(__global const float* lut,int N,float3 c){                                          \n" \
"  float cr=clamp(c.x,0.0f,1.0f)*(N-1),cg=clamp(c.y,0.0f,1.0f)*(N-1),cb=clamp(c.z,0.0f,1.0f)*(N-1);             \n" \
"  int r0=(int)cr,g0=(int)cg,b0=(int)cb; int r1=r0<N-1?r0+1:r0,g1=g0<N-1?g0+1:g0,b1=b0<N-1?b0+1:b0;             \n" \
"  float fr=cr-r0,fg=cg-g0,fb=cb-b0; float res[3];                                                              \n" \
"  for(int ch=0;ch<3;ch++){                                                                                     \n" \
"    float c00=lut[((b0*N+g0)*N+r0)*3+ch]*(1-fr)+lut[((b0*N+g0)*N+r1)*3+ch]*fr;                                 \n" \
"    float c10=lut[((b0*N+g1)*N+r0)*3+ch]*(1-fr)+lut[((b0*N+g1)*N+r1)*3+ch]*fr;                                 \n" \
"    float c01=lut[((b1*N+g0)*N+r0)*3+ch]*(1-fr)+lut[((b1*N+g0)*N+r1)*3+ch]*fr;                                 \n" \
"    float c11=lut[((b1*N+g1)*N+r0)*3+ch]*(1-fr)+lut[((b1*N+g1)*N+r1)*3+ch]*fr;                                 \n" \
"    float c0=c00*(1-fg)+c10*fg,c1=c01*(1-fg)+c11*fg; res[ch]=c0*(1-fb)+c1*fb; }                                \n" \
"  return (float3)(res[0],res[1],res[2]); }                                                                     \n" \
"inline float og_softclip(float v, float amt){                                                                  \n" \
"  if(amt<=0.0f || v<=0.0f) return v;                                                                            \n" \
"  float k=1.0f-0.6f*amt; if(v<=k) return v;                                                                     \n" \
"  float r=1.0f-k; return k + r*(1.0f-exp(-(v-k)/r)); }                                                          \n" \
"__kernel void OneGradeKernel(int W,int H,int cam,int enc,                                                    \n" \
"  float temp,float tint,float density,float lift,float gamma,float gain,float offTemp,float offTint,           \n" \
"  float postExp,float postCon,float rawExp,float rawTemp,float rolloff,                                     \n" \
"  float rbL,float rbS,float rbH,float rbF,float rbG,float rbWs,float rbHg,float rbLg,                       \n" \
"  float rfF,float rfG,float rfN,                                                                            \n" \
"  int lutN, float lutMix,                                                                                   \n" \
"  __global const float* lut,                                                                                \n" \
"  __global const float* in, __global float* out) {                                                             \n" \
"  const int x=get_global_id(0); const int y=get_global_id(1);                                                  \n" \
"  if(x<W && y<H){                                                                                              \n" \
"    const int i=((y*W)+x)*4;                                                                                    \n" \
"    float ex0=exp2(rawExp);                                                                                     \n" \
"    float3 lin=(float3)(og_decode(cam,in[i]),og_decode(cam,in[i+1]),og_decode(cam,in[i+2]))*ex0;               \n" \
"    float3 w=og_XYZtoDWG(og_whitebalance(rawTemp,og_toXYZ(cam,lin)));                                           \n" \
"    w.x*=(1.0f+temp*0.20f); w.z*=(1.0f-temp*0.20f); w.y*=(1.0f+tint*0.20f);                                     \n" \
"    w.x+=offTemp*0.10f; w.z-=offTemp*0.10f; w.y+=offTint*0.10f;                                                 \n" \
"    if(density!=0.0f){ float3 l=(float3)(og_dienc(w.x),og_dienc(w.y),og_dienc(w.z)); float3 hsv=og_rgb2hsv(l); hsv.y=fmin(fmax(hsv.y*(1.0f+density),0.0f),1.0f); l=og_hsv2rgb(hsv); w=(float3)(og_didec(l.x),og_didec(l.y),og_didec(l.z)); } \n" \
"    float3 outc=(enc<=3)?og_XYZto709(og_DWGtoXYZ(w)):w;                                                         \n" \
"    float dg=(enc==1)?2.2f:((enc==2)?2.4f:0.0f);                                                                \n" \
"    float3 d=(float3)((dg>0.0f)?og_r709ge(outc.x,dg):og_r709e(outc.x),(dg>0.0f)?og_r709ge(outc.y,dg):og_r709e(outc.y),(dg>0.0f)?og_r709ge(outc.z,dg):og_r709e(outc.z)); \n" \
"    int rbW=(rbWs>0.5f);                                                                                     \n" \
"    int rbOn=(rbL>0.0f)&&(rbW||rbH!=1.0f||rbF!=0.0f||rbG!=1.0f||rbHg!=1.0f||rbLg!=1.0f);                     \n" \
"    float3 v3=(float3)(og_lggc(d.x,gain,lift,gamma),og_lggc(d.y,gain,lift,gamma),og_lggc(d.z,gain,lift,gamma)); \n" \
"    float3 rf=(float3)(og_lggc(d.x,rfN,rfF,rfG),og_lggc(d.y,rfN,rfF,rfG),og_lggc(d.z,rfN,rfF,rfG));           \n" \
"    float rbM=rbOn?og_hlmask(100.0f*(0.2126f*rf.x+0.7152f*rf.y+0.0722f*rf.z),rbL,rbS):0.0f;                    \n" \
"    if(rbOn){ float3 rm=(float3)(og_lggc(v3.x,rbLg,rbF,rbG),og_lggc(v3.y,rbLg,rbF,rbG),og_lggc(v3.z,rbLg,rbF,rbG)); \n" \
"             float3 hi3=(float3)(og_lggc(v3.x,rbH,0.0f,rbHg),og_lggc(v3.y,rbH,0.0f,rbHg),og_lggc(v3.z,rbH,0.0f,rbHg)); \n" \
"             v3=rm*(1.0f-rbM)+hi3*rbM; }                                                                        \n" \
"    outc=(float3)((dg>0.0f)?og_r709gd(v3.x,dg):og_r709d(v3.x),(dg>0.0f)?og_r709gd(v3.y,dg):og_r709d(v3.y),(dg>0.0f)?og_r709gd(v3.z,dg):og_r709d(v3.z)); \n" \
"    float3 e=(float3)(og_enc(enc,outc.x),og_enc(enc,outc.y),og_enc(enc,outc.z));                                \n" \
"    if(rbW&&rbOn){ out[i]=rbM; out[i+1]=rbM; out[i+2]=rbM; out[i+3]=in[i+3]; return; }                       \n" \
"    if(lutN>=2 && lutMix>0.0f){ float3 s=og_sampleLUT(lut,lutN,e); e=e+(s-e)*lutMix; }                          \n" \
"    float ex=exp2(postExp); e=(e*ex-0.5f)*postCon+0.5f;                                                         \n" \
"    if(rolloff>0.0f && (enc<=2 || (lutN>=2 && lutMix>0.0f))){ e.x=og_softclip(e.x,rolloff); e.y=og_softclip(e.y,rolloff); e.z=og_softclip(e.z,rolloff); } \n" \
"    out[i]=e.x; out[i+1]=e.y; out[i+2]=e.z; out[i+3]=in[i+3];                                                  \n" \
"  } }                                                                                                          \n" \
"\n";

void CheckError(cl_int p_Error, const char* p_Msg)
{
    if (p_Error != CL_SUCCESS) fprintf(stderr, "OneGrade OpenCL: %s [%d]\n", p_Msg, p_Error);
}

class Locker
{
public:
    Locker()  {
#ifdef _WIN64
        InitializeCriticalSection(&mutex);
#else
        pthread_mutex_init(&mutex, NULL);
#endif
    }
    ~Locker() {
#ifdef _WIN64
        DeleteCriticalSection(&mutex);
#else
        pthread_mutex_destroy(&mutex);
#endif
    }
    void Lock()   {
#ifdef _WIN64
        EnterCriticalSection(&mutex);
#else
        pthread_mutex_lock(&mutex);
#endif
    }
    void Unlock() {
#ifdef _WIN64
        LeaveCriticalSection(&mutex);
#else
        pthread_mutex_unlock(&mutex);
#endif
    }
private:
#ifdef _WIN64
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif
};

void RunOpenCLKernelBuffers(void* p_CmdQ, int p_Width, int p_Height, const float* p_Params, int p_Camera, int p_Encode,
                            const float* p_Lut, int p_LutSize, float p_LutMix, const float* p_Input, float* p_Output)
{
    cl_int error;
    cl_command_queue cmdQ = static_cast<cl_command_queue>(p_CmdQ);

    static std::map<cl_command_queue, cl_device_id> deviceIdMap;
    static std::map<cl_command_queue, cl_kernel> kernelMap;
    static Locker locker;

    locker.Lock();

    cl_device_id deviceId = NULL;
    if (deviceIdMap.find(cmdQ) == deviceIdMap.end())
    {
        error = clGetCommandQueueInfo(cmdQ, CL_QUEUE_DEVICE, sizeof(cl_device_id), &deviceId, NULL);
        CheckError(error, "Unable to get the device");
        deviceIdMap[cmdQ] = deviceId;
    }
    else
    {
        deviceId = deviceIdMap[cmdQ];
    }

    cl_context clContext = NULL;
    error = clGetCommandQueueInfo(cmdQ, CL_QUEUE_CONTEXT, sizeof(cl_context), &clContext, NULL);
    CheckError(error, "Unable to get the context");

    cl_kernel kernel = NULL;
    if (kernelMap.find(cmdQ) == kernelMap.end())
    {
        cl_program program = clCreateProgramWithSource(clContext, 1, (const char**)&KernelSource, NULL, &error);
        CheckError(error, "Unable to create program");
        error = clBuildProgram(program, 0, NULL, NULL, NULL, NULL);
        CheckError(error, "Unable to build program");
        kernel = clCreateKernel(program, "OneGradeKernel", &error);
        CheckError(error, "Unable to create kernel");
        kernelMap[cmdQ] = kernel;
    }
    else
    {
        kernel = kernelMap[cmdQ];
    }

    locker.Unlock();

    // Upload the LUT (or a 1-float dummy when none).
    int lutN = (p_Lut && p_LutSize >= 2) ? p_LutSize : 0;
    size_t lutBytes = lutN ? (size_t)lutN*lutN*lutN*3*sizeof(float) : sizeof(float);
    float dummy = 0.0f;
    cl_mem lutBuf = clCreateBuffer(clContext, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, lutBytes,
                                   (void*)(lutN ? (const void*)p_Lut : (const void*)&dummy), &error);
    CheckError(error, "Unable to create LUT buffer");

    int count = 0;
    error  = clSetKernelArg(kernel, count++, sizeof(int), &p_Width);
    error |= clSetKernelArg(kernel, count++, sizeof(int), &p_Height);
    error |= clSetKernelArg(kernel, count++, sizeof(int), &p_Camera);
    error |= clSetKernelArg(kernel, count++, sizeof(int), &p_Encode);
    for (int i = 0; i < 24; ++i)
        error |= clSetKernelArg(kernel, count++, sizeof(float), &p_Params[i]);
    error |= clSetKernelArg(kernel, count++, sizeof(int), &lutN);
    error |= clSetKernelArg(kernel, count++, sizeof(float), &p_LutMix);
    error |= clSetKernelArg(kernel, count++, sizeof(cl_mem), &lutBuf);
    error |= clSetKernelArg(kernel, count++, sizeof(cl_mem), &p_Input);
    error |= clSetKernelArg(kernel, count++, sizeof(cl_mem), &p_Output);
    CheckError(error, "Unable to set kernel arguments");

    size_t localWorkSize[2], globalWorkSize[2];
    clGetKernelWorkGroupInfo(kernel, deviceId, CL_KERNEL_WORK_GROUP_SIZE, sizeof(size_t), localWorkSize, NULL);
    localWorkSize[1] = 1;
    globalWorkSize[0] = ((p_Width + localWorkSize[0] - 1) / localWorkSize[0]) * localWorkSize[0];
    globalWorkSize[1] = p_Height;

    clEnqueueNDRangeKernel(cmdQ, kernel, 2, NULL, globalWorkSize, localWorkSize, 0, NULL, NULL);
    clReleaseMemObject(lutBuf);
}
