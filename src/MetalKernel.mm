// OneGrade — Metal GPU kernel (Apple). Mirrors src/OneGradePipeline.h.
// SPDX-License-Identifier: BSD-3-Clause
#import <Metal/Metal.h>
#include <unordered_map>
#include <mutex>

const char* kernelSource = R"KERNEL(
#include <metal_stdlib>
using namespace metal;

inline float og_pow(float b, float e){ return pow(fmax(b,0.0f), e); }

inline float og_decode(int cam, float x){
    if(cam==0){ float A=0.08692876f,B=0.00549407f,C=0.53001334f,D=8.28360593f,E=0.09246575f,LC=0.13388378f;
        return (x>LC)?(exp((x-C)/A)-B):((x-E)/D); } // Blackmagic Film Gen 5
    else if(cam==1){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LC=0.02740668f;
        return (x>LC)?(exp2(x/C-B)-A):(x/M); }
    else if(cam==2){ return (x>=171.2102946f/1023.0f)?(og_pow(10.0f,(x*1023.0f-420.0f)/261.5f)*0.19f-0.01f):((x*1023.0f-95.0f)*0.01125f/(171.2102946f-95.0f)); }
    else if(cam==3){ float a=5.555556f,b=0.052272f,c=0.247190f,d=0.385537f,e=5.367655f,f=0.092809f,cut=0.010591f;
        return (x>e*cut+f)?((og_pow(10.0f,(x-d)/c)-b)/a):((x-f)/e); }
    else if(cam==4){ float a=(exp2(18.0f)-16.0f)/117.45f,b=(1023.0f-95.0f)/1023.0f,c=95.0f/1023.0f;
        float s=(7.0f*log(2.0f)*exp2(7.0f-14.0f*c/b))/(a*b); float t=(exp2(14.0f*(-c/b)+6.0f)-64.0f)/a;
        float p=(x-c)/b; float hi=(exp2(14.0f*p+6.0f)-64.0f)/a; return (hi>t)?hi:(x*s); }
    else if(cam==5){ float v=x; if(v<0.09755646f) return -(og_pow(10.0f,(0.07623209f-v)/0.42889912f)-1.0f)/14.98325f;
        if(v<=0.15277891f) return (v-0.12512219f)/1.9754798f; return (og_pow(10.0f,(v-0.19022340f)/0.42889912f)-1.0f)/14.98325f; }
    else if(cam==6){ return (og_pow(10.0f,x/0.224282f)-1.0f)/155.975327f-0.01f; }
    else if(cam==7){ return (x<=0.14f)?((x-0.0929f)/6.025f):(og_pow(10.0f,(x-0.5595f)/0.9892f)-0.0108f); }
    else if(cam==8){ float a=5.555556f,b=0.064829f,c=0.245281f,d=0.384316f,e=8.799461f,f=0.092864f,cut=0.100686685f;
        return (x>=cut)?((og_pow(10.0f,(x-d)/c)-b)/a):((x-f)/e); }
    else if(cam==9){ return (x<0.181f)?((x-0.125f)/5.6f):(og_pow(10.0f,(x-0.598206f)/0.241514f)-0.00873f); } // Panasonic V-Log
    else if(cam==10){ float a=0.17883277f,b=0.28466892f,c=0.55991073f; float e=(x<=0.5f)?(x*x/3.0f):((exp((x-c)/a)+b)/12.0f); return e*3.774f; } // Rec.2100 HLG
    else { float m1=0.1593017578125f,m2=78.84375f,c1=0.8359375f,c2=18.8515625f,c3=18.6875f; float p=og_pow(x,1.0f/m2); float num=fmax(p-c1,0.0f); float e=og_pow(num/(c2-c3*p),1.0f/m1); return e*49.26f; } // Rec.2100 PQ
}

inline float3 og_mv(float3 r0,float3 r1,float3 r2,float3 v){ return float3(dot(r0,v),dot(r1,v),dot(r2,v)); }

inline float3 og_toXYZ(int cam, float3 v){
    if(cam==0) return og_mv(float3(0.6065384f,0.2204127f,0.1235048f),float3(0.2679929f,0.8327485f,-0.1007414f),float3(-0.0294426f,-0.0866124f,1.2048076f),v); // Blackmagic Wide Gamut Gen 4/5
    else if(cam==1) return og_mv(float3(0.7006223f,0.1487748f,0.1010587f),float3(0.2740150f,0.8736457f,-0.1476607f),float3(-0.0989629f,-0.1378905f,1.3259942f),v); // DaVinci Wide Gamut
    else if(cam==2) return og_mv(float3(0.5990839f,0.2489255f,0.1024464f),float3(0.2150758f,0.8850685f,-0.1001443f),float3(-0.0320658f,-0.0276902f,1.1487819f),v);
    else if(cam==3) return og_mv(float3(0.6380076f,0.2147014f,0.0977226f),float3(0.2919283f,0.8238731f,-0.1158014f),float3(0.0027932f,-0.0670795f,1.1533751f),v);
    else if(cam==4) return og_mv(float3(0.7048583f,0.1297602f,0.1158373f),float3(0.2545241f,0.7814843f,-0.0360084f),float3(0.0f,0.0f,1.0890577f),v);
    else if(cam==6) return og_mv(float3(0.7352750f,0.0686090f,0.1465710f),float3(0.2866940f,0.8429790f,-0.1296730f),float3(-0.0796810f,-0.3473430f,1.5164950f),v);
    else if(cam==9) return og_mv(float3(0.6796440f,0.1522110f,0.1186000f),float3(0.2606860f,0.7748940f,-0.0355800f),float3(-0.0093100f,-0.0046120f,1.1029800f),v);
    else return og_mv(float3(0.6369580f,0.1446169f,0.1688810f),float3(0.2627002f,0.6779981f,0.0593017f),float3(0.0f,0.0280727f,1.0609851f),v);
}
inline float3 og_XYZtoDWG(float3 v){ return og_mv(float3(1.5166283f,-0.2814601f,-0.1469306f),float3(-0.4647205f,1.2513509f,0.1747665f),float3(0.0648641f,0.1091221f,0.7613593f),v); }
inline float3 og_DWGtoXYZ(float3 v){ return og_mv(float3(0.7006223f,0.1487748f,0.1010587f),float3(0.2740150f,0.8736457f,-0.1476607f),float3(-0.0989629f,-0.1378905f,1.3259942f),v); }
inline float3 og_XYZto709(float3 v){ return og_mv(float3(3.2409699f,-1.5373832f,-0.4986108f),float3(-0.9692436f,1.8759675f,0.0415551f),float3(0.0556301f,-0.2039770f,1.0569715f),v); }

inline float2 og_cct_xy(float T){
    float t1=1.0f/T,t2=t1*t1,t3=t2*t1;
    float xc=(T<4000.0f)?(-0.2661239e9f*t3-0.2343589e6f*t2+0.8776956e3f*t1+0.179910f)
                        :(-3.0258469e9f*t3+2.1070379e6f*t2+0.2226347e3f*t1+0.240390f);
    float xc2=xc*xc,xc3=xc2*xc;
    float yc=(T<2222.0f)?(-1.1063814f*xc3-1.34811020f*xc2+2.18555832f*xc-0.20219683f)
            :(T<4000.0f)?(-0.9549476f*xc3-1.37418593f*xc2+2.09137015f*xc-0.16748867f)
                        :(3.0817580f*xc3-5.87338670f*xc2+3.75112997f*xc-0.37001483f);
    return float2(xc,yc);
}
inline float3 og_whitebalance(float T, float3 v){
    if(T<=0.0f || (T>6499.0f && T<6501.0f)) return v;
    float2 xy=og_cct_xy(T);
    float3 sw=float3(xy.x/xy.y, 1.0f, (1.0f-xy.x-xy.y)/xy.y);
    float3 dw=float3(0.95047f,1.0f,1.08883f);
    float3 ma0=float3(0.8951f,0.2664f,-0.1614f),ma1=float3(-0.7502f,1.7135f,0.0367f),ma2=float3(0.0389f,-0.0685f,1.0296f);
    float3 mai0=float3(0.9869929f,-0.1470543f,0.1599627f),mai1=float3(0.4323053f,0.5183603f,0.0492912f),mai2=float3(-0.0085287f,0.0400428f,0.9684867f);
    float3 sl=og_mv(ma0,ma1,ma2,sw), dl=og_mv(ma0,ma1,ma2,dw), pl=og_mv(ma0,ma1,ma2,v);
    pl=pl*(dl/sl);
    return og_mv(mai0,mai1,mai2,pl);
}

inline float3 og_rgb2hsv(float3 c){
    float mx=max(c.x,max(c.y,c.z)), mn=min(c.x,min(c.y,c.z));
    float v=mx, d=mx-mn; float s=(mx<=0.0f)?0.0f:(d/mx); float h=0.0f;
    if(d>0.0f){
        if(mx==c.x) h=(c.y-c.z)/d + (c.y<c.z?6.0f:0.0f);
        else if(mx==c.y) h=(c.z-c.x)/d + 2.0f;
        else h=(c.x-c.y)/d + 4.0f;
        h/=6.0f;
    }
    return float3(h,s,v);
}
inline float3 og_hsv2rgb(float3 c){
    float h=c.x, s=c.y, v=c.z;
    if(s<=0.0f) return float3(v,v,v);
    h*=6.0f; int i=(int)floor(h); float f=h-(float)i;
    float p=v*(1.0f-s), q=v*(1.0f-s*f), t=v*(1.0f-s*(1.0f-f));
    i=i%6; if(i<0)i+=6;
    if(i==0) return float3(v,t,p); else if(i==1) return float3(q,v,p); else if(i==2) return float3(p,v,t);
    else if(i==3) return float3(p,q,v); else if(i==4) return float3(t,p,v); else return float3(v,p,q);
}

inline float og_r709e(float L){ return (L<0.018f)?(4.5f*L):(1.099f*og_pow(L,0.45f)-0.099f); }
inline float og_r709d(float V){ return (V<0.081f)?(V/4.5f):og_pow((V+0.099f)/1.099f,1.0f/0.45f); }
inline float og_r709ge(float L, float g){ return og_pow(L,1.0f/g); }
inline float og_r709gd(float V, float g){ return og_pow(V,g); }
inline float og_lgg(float L, float gain, float lift, float gamma, float dg){
    float v = (dg>0.0f) ? og_r709ge(L,dg) : og_r709e(L);
    v=v*gain; v=v+lift*(1.0f-min(v,1.0f)); v=(v<0.0f)?v:og_pow(v,1.0f/gamma);   // negatives pass through (see og::process)
    return (dg>0.0f) ? og_r709gd(v,dg) : og_r709d(v);
}
inline float og_dienc(float x){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LIN=0.00262409f; return (x>LIN)?((log2(x+A)+B)*C):(x*M); }
inline float og_didec(float x){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LC=0.02740668f; return (x>LC)?(exp2(x/C-B)-A):(x/M); }

inline float og_enc(int enc, float x){
    if(enc==0) return og_r709e(x);
    else if(enc==1) return og_r709ge(x,2.2f);
    else if(enc==2) return og_r709ge(x,2.4f);
    else if(enc==3){ float code=685.0f+300.0f*log10(fmax(x,1e-4f)); return fmin(fmax(code/1023.0f,0.0f),1.0f); }
    else if(enc==4){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LIN=0.00262409f; return (x>LIN)?((log2(x+A)+B)*C):(x*M); }
    else return x;
}

inline float3 og_sampleLUT(const device float* lut, int N, float3 c){
    float cr=clamp(c.x,0.0f,1.0f)*(N-1), cg=clamp(c.y,0.0f,1.0f)*(N-1), cb=clamp(c.z,0.0f,1.0f)*(N-1);
    int r0=(int)cr,g0=(int)cg,b0=(int)cb;
    int r1=r0<N-1?r0+1:r0, g1=g0<N-1?g0+1:g0, b1=b0<N-1?b0+1:b0;
    float fr=cr-r0, fg=cg-g0, fb=cb-b0;
    float3 res;
    for(int ch=0;ch<3;ch++){
        float c00=lut[((b0*N+g0)*N+r0)*3+ch]*(1-fr)+lut[((b0*N+g0)*N+r1)*3+ch]*fr;
        float c10=lut[((b0*N+g1)*N+r0)*3+ch]*(1-fr)+lut[((b0*N+g1)*N+r1)*3+ch]*fr;
        float c01=lut[((b1*N+g0)*N+r0)*3+ch]*(1-fr)+lut[((b1*N+g0)*N+r1)*3+ch]*fr;
        float c11=lut[((b1*N+g1)*N+r0)*3+ch]*(1-fr)+lut[((b1*N+g1)*N+r1)*3+ch]*fr;
        float c0=c00*(1-fg)+c10*fg, c1=c01*(1-fg)+c11*fg;
        res[ch]=c0*(1-fb)+c1*fb;
    }
    return res;
}

inline float og_softclip(float v, float amt){
    if(amt<=0.0f || v<=0.0f) return v;
    float k=1.0f-0.6f*amt; if(v<=k) return v;
    float r=1.0f-k; return k + r*(1.0f-exp(-(v-k)/r));
}

kernel void OneGradeKernel(constant int& W [[buffer(11)]], constant int& H [[buffer(12)]],
    constant float* P [[buffer(13)]], constant int& cam [[buffer(14)]], constant int& enc [[buffer(15)]],
    const device float* lut [[buffer(1)]], constant int& lutN [[buffer(16)]], constant float& lutMix [[buffer(17)]],
    const device float* in [[buffer(0)]], device float* out [[buffer(8)]], uint2 id [[thread_position_in_grid]])
{
    if(id.x<(uint)W && id.y<(uint)H){
        int i=((id.y*W)+id.x)*4;
        float temp=P[0],tint=P[1],density=P[2],lift=P[3],gamma=P[4],gain=P[5],offTemp=P[6],offTint=P[7],rawExp=P[10],rawTemp=P[11];
        float ex0=exp2(rawExp);                              // RAW exposure (stops), pre-CST
        float3 lin = float3(og_decode(cam,in[i]),og_decode(cam,in[i+1]),og_decode(cam,in[i+2]))*ex0;
        float3 w = og_XYZtoDWG(og_whitebalance(rawTemp,og_toXYZ(cam,lin)));  // RAW WB in XYZ, then DWG linear
        w.x*=(1.0f+temp*0.20f); w.z*=(1.0f-temp*0.20f); w.y*=(1.0f+tint*0.20f);   // gain balance
        w.x+=offTemp*0.10f; w.z-=offTemp*0.10f; w.y+=offTint*0.10f;               // offset balance
        if(density!=0.0f){ float3 l=float3(og_dienc(w.x),og_dienc(w.y),og_dienc(w.z)); float3 hsv=og_rgb2hsv(l); hsv.y=fmin(fmax(hsv.y*(1.0f+density),0.0f),1.0f); l=og_hsv2rgb(hsv); w=float3(og_didec(l.x),og_didec(l.y),og_didec(l.z)); } // density in DI-log
        float3 outc = (enc<=3) ? og_XYZto709(og_DWGtoXYZ(w)) : w;                 // output primaries (linear): 709 for Scene/2.2/2.4/Cineon
        float dg = (enc==1) ? 2.2f : (enc==2) ? 2.4f : 0.0f;                      // grade curve follows output (pure gamma vs Scene OETF)
        outc.x=og_lgg(outc.x,gain,lift,gamma,dg); outc.y=og_lgg(outc.y,gain,lift,gamma,dg); outc.z=og_lgg(outc.z,gain,lift,gamma,dg); // LGG
        float3 e = float3(og_enc(enc,outc.x), og_enc(enc,outc.y), og_enc(enc,outc.z));
        if(lutN>=2 && lutMix>0.0f){ float3 s=og_sampleLUT(lut,lutN,e); e = e + (s-e)*lutMix; }  // LUT + mix
        float ex=exp2(P[8]); e = (e*ex - 0.5f)*P[9] + 0.5f;                                     // post-LUT trim (exposure, contrast)
        if(P[12]>0.0f && (enc<=2 || (lutN>=2 && lutMix>0.0f))){ e.x=og_softclip(e.x,P[12]); e.y=og_softclip(e.y,P[12]); e.z=og_softclip(e.z,P[12]); } // highlight roll-off (display-referred only)
        out[i]=e.x; out[i+1]=e.y; out[i+2]=e.z; out[i+3]=in[i+3];
    }
}
)KERNEL";

std::mutex s_PipelineQueueMutex;
typedef std::unordered_map<id<MTLCommandQueue>, id<MTLComputePipelineState>> PipelineQueueMap;
PipelineQueueMap s_PipelineQueueMap;

void RunMetalKernel(void* p_CmdQ, int p_Width, int p_Height, const float* p_Params, int p_Camera, int p_Encode,
                    const float* p_Lut, int p_LutSize, float p_LutMix, const float* p_Input, float* p_Output)
{
    const char* kernelName = "OneGradeKernel";

    id<MTLCommandQueue> queue = static_cast<id<MTLCommandQueue> >(p_CmdQ);
    id<MTLDevice> device = queue.device;
    id<MTLComputePipelineState> pipelineState;

    std::unique_lock<std::mutex> lock(s_PipelineQueueMutex);
    const auto it = s_PipelineQueueMap.find(queue);
    if (it == s_PipelineQueueMap.end())
    {
        id<MTLLibrary> metalLibrary;
        id<MTLFunction> kernelFunction;
        NSError* err;

        MTLCompileOptions* options = [MTLCompileOptions new];
        options.fastMathEnabled = YES;
        if (!(metalLibrary = [device newLibraryWithSource:@(kernelSource) options:options error:&err]))
        {
            fprintf(stderr, "OneGrade: failed to load metal library, %s\n", err.localizedDescription.UTF8String);
            return;
        }
        [options release];
        if (!(kernelFunction = [metalLibrary newFunctionWithName:[NSString stringWithUTF8String:kernelName]]))
        {
            fprintf(stderr, "OneGrade: failed to retrieve kernel\n");
            [metalLibrary release];
            return;
        }
        if (!(pipelineState = [device newComputePipelineStateWithFunction:kernelFunction error:&err]))
        {
            fprintf(stderr, "OneGrade: unable to compile, %s\n", err.localizedDescription.UTF8String);
            [metalLibrary release];
            [kernelFunction release];
            return;
        }
        s_PipelineQueueMap[queue] = pipelineState;
        [metalLibrary release];
        [kernelFunction release];
    }
    else
    {
        pipelineState = it->second;
    }

    id<MTLBuffer> srcDeviceBuf = reinterpret_cast<id<MTLBuffer> >(const_cast<float*>(p_Input));
    id<MTLBuffer> dstDeviceBuf = reinterpret_cast<id<MTLBuffer> >(p_Output);

    // LUT buffer: upload the 3D LUT (or a tiny dummy when none), released on GPU completion.
    int lutN = (p_Lut && p_LutSize >= 2) ? p_LutSize : 0;
    NSUInteger lutBytes = lutN ? (NSUInteger)lutN*lutN*lutN*3*sizeof(float) : sizeof(float);
    float dummy = 0.0f;
    id<MTLBuffer> lutBuf = [device newBufferWithBytes:(lutN ? (const void*)p_Lut : (const void*)&dummy)
                                               length:lutBytes options:MTLResourceStorageModeShared];

    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    commandBuffer.label = @"OneGradeKernel";

    id<MTLComputeCommandEncoder> computeEncoder = [commandBuffer computeCommandEncoder];
    [computeEncoder setComputePipelineState:pipelineState];

    int exeWidth = [pipelineState threadExecutionWidth];
    MTLSize threadGroupCount = MTLSizeMake(exeWidth, 1, 1);
    MTLSize threadGroups     = MTLSizeMake((p_Width + exeWidth - 1) / exeWidth, p_Height, 1);

    [computeEncoder setBuffer:srcDeviceBuf offset:0 atIndex:0];
    [computeEncoder setBuffer:lutBuf       offset:0 atIndex:1];
    [computeEncoder setBuffer:dstDeviceBuf offset:0 atIndex:8];
    [computeEncoder setBytes:&p_Width  length:sizeof(int) atIndex:11];
    [computeEncoder setBytes:&p_Height length:sizeof(int) atIndex:12];
    [computeEncoder setBytes:p_Params  length:sizeof(float)*13 atIndex:13];
    [computeEncoder setBytes:&p_Camera length:sizeof(int) atIndex:14];
    [computeEncoder setBytes:&p_Encode length:sizeof(int) atIndex:15];
    [computeEncoder setBytes:&lutN     length:sizeof(int) atIndex:16];
    [computeEncoder setBytes:&p_LutMix length:sizeof(float) atIndex:17];

    [computeEncoder dispatchThreadgroups:threadGroups threadsPerThreadgroup:threadGroupCount];
    [computeEncoder endEncoding];
    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>){ [lutBuf release]; }];
    [commandBuffer commit];
}
