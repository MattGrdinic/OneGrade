// OneGrade — CUDA GPU kernel (Linux/Windows NVIDIA). Mirrors src/OneGradePipeline.h.
// Copyright (C) 2026 Matthew Grdinic
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cuda_runtime.h>

__device__ float og_pow(float b, float e){ return powf(fmaxf(b,0.0f), e); }

__device__ float og_decode(int cam, float x){
    if(cam==0){ float A=0.08692876f,B=0.00549407f,C=0.53001334f,D=8.28360593f,E=0.09246575f,LC=0.13388378f;
        return (x>LC)?(expf((x-C)/A)-B):((x-E)/D); } // Blackmagic Film Gen 5
    else if(cam==1){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LC=0.02740668f;
        return (x>LC)?(exp2f(x/C-B)-A):(x/M); }
    else if(cam==2){ return (x>=171.2102946f/1023.0f)?(og_pow(10.0f,(x*1023.0f-420.0f)/261.5f)*0.19f-0.01f):((x*1023.0f-95.0f)*0.01125f/(171.2102946f-95.0f)); }
    else if(cam==3){ float a=5.555556f,b=0.052272f,c=0.247190f,d=0.385537f,e=5.367655f,f=0.092809f,cut=0.010591f;
        return (x>e*cut+f)?((og_pow(10.0f,(x-d)/c)-b)/a):((x-f)/e); }
    else if(cam==4){ float a=(exp2f(18.0f)-16.0f)/117.45f,b=(1023.0f-95.0f)/1023.0f,c=95.0f/1023.0f;
        float s=(7.0f*logf(2.0f)*exp2f(7.0f-14.0f*c/b))/(a*b); float t=(exp2f(14.0f*(-c/b)+6.0f)-64.0f)/a;
        float p=(x-c)/b; float hi=(exp2f(14.0f*p+6.0f)-64.0f)/a; return (hi>t)?hi:(x*s); }
    else if(cam==5){ float v=x; if(v<0.09755646f) return -(og_pow(10.0f,(0.07623209f-v)/0.42889912f)-1.0f)/14.98325f;
        if(v<=0.15277891f) return (v-0.12512219f)/1.9754798f; return (og_pow(10.0f,(v-0.19022340f)/0.42889912f)-1.0f)/14.98325f; }
    else if(cam==6){ return (og_pow(10.0f,x/0.224282f)-1.0f)/155.975327f-0.01f; }
    else if(cam==7){ return (x<=0.14f)?((x-0.0929f)/6.025f):(og_pow(10.0f,(x-0.5595f)/0.9892f)-0.0108f); }
    else if(cam==8){ float a=5.555556f,b=0.064829f,c=0.245281f,d=0.384316f,e=8.799461f,f=0.092864f,cut=0.100686685f;
        return (x>=cut)?((og_pow(10.0f,(x-d)/c)-b)/a):((x-f)/e); }
    else if(cam==9){ return (x<0.181f)?((x-0.125f)/5.6f):(og_pow(10.0f,(x-0.598206f)/0.241514f)-0.00873f); } // Panasonic V-Log
    else if(cam==10){ float a=0.17883277f,b=0.28466892f,c=0.55991073f; float e=(x<=0.5f)?(x*x/3.0f):((expf((x-c)/a)+b)/12.0f); return e*3.774f; } // Rec.2100 HLG
    else { float m1=0.1593017578125f,m2=78.84375f,c1=0.8359375f,c2=18.8515625f,c3=18.6875f; float p=og_pow(x,1.0f/m2); float num=fmaxf(p-c1,0.0f); float e=og_pow(num/(c2-c3*p),1.0f/m1); return e*49.26f; } // Rec.2100 PQ
}

__device__ void mul33(const float m[9], const float v[3], float o[3]){
    o[0]=m[0]*v[0]+m[1]*v[1]+m[2]*v[2]; o[1]=m[3]*v[0]+m[4]*v[1]+m[5]*v[2]; o[2]=m[6]*v[0]+m[7]*v[1]+m[8]*v[2];
}
__device__ void og_toXYZ(int cam, const float v[3], float o[3]){
    float m[9];
    if(cam==0){ float t[9]={0.6065384f,0.2204127f,0.1235048f,0.2679929f,0.8327485f,-0.1007414f,-0.0294426f,-0.0866124f,1.2048076f}; for(int i=0;i<9;i++)m[i]=t[i]; } // Blackmagic Wide Gamut Gen 4/5
    else if(cam==1){ float t[9]={0.7006223f,0.1487748f,0.1010587f,0.2740150f,0.8736457f,-0.1476607f,-0.0989629f,-0.1378905f,1.3259942f}; for(int i=0;i<9;i++)m[i]=t[i]; } // DaVinci Wide Gamut
    else if(cam==2){ float t[9]={0.5990839f,0.2489255f,0.1024464f,0.2150758f,0.8850685f,-0.1001443f,-0.0320658f,-0.0276902f,1.1487819f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    else if(cam==3){ float t[9]={0.6380076f,0.2147014f,0.0977226f,0.2919283f,0.8238731f,-0.1158014f,0.0027932f,-0.0670795f,1.1533751f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    else if(cam==4){ float t[9]={0.7048583f,0.1297602f,0.1158373f,0.2545241f,0.7814843f,-0.0360084f,0.0f,0.0f,1.0890577f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    else if(cam==6){ float t[9]={0.7352750f,0.0686090f,0.1465710f,0.2866940f,0.8429790f,-0.1296730f,-0.0796810f,-0.3473430f,1.5164950f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    else if(cam==9){ float t[9]={0.6796440f,0.1522110f,0.1186000f,0.2606860f,0.7748940f,-0.0355800f,-0.0093100f,-0.0046120f,1.1029800f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    else { float t[9]={0.6369580f,0.1446169f,0.1688810f,0.2627002f,0.6779981f,0.0593017f,0.0f,0.0280727f,1.0609851f}; for(int i=0;i<9;i++)m[i]=t[i]; }
    mul33(m,v,o);
}
__device__ void og_XYZtoDWG(const float v[3], float o[3]){ const float m[9]={1.5166283f,-0.2814601f,-0.1469306f,-0.4647205f,1.2513509f,0.1747665f,0.0648641f,0.1091221f,0.7613593f}; mul33(m,v,o); }
__device__ void og_DWGtoXYZ(const float v[3], float o[3]){ const float m[9]={0.7006223f,0.1487748f,0.1010587f,0.2740150f,0.8736457f,-0.1476607f,-0.0989629f,-0.1378905f,1.3259942f}; mul33(m,v,o); }
__device__ void og_XYZto709(const float v[3], float o[3]){ const float m[9]={3.2409699f,-1.5373832f,-0.4986108f,-0.9692436f,1.8759675f,0.0415551f,0.0556301f,-0.2039770f,1.0569715f}; mul33(m,v,o); }

__device__ void og_cct_xy(float T, float& x, float& y){
    float t1=1.0f/T,t2=t1*t1,t3=t2*t1;
    float xc=(T<4000.0f)?(-0.2661239e9f*t3-0.2343589e6f*t2+0.8776956e3f*t1+0.179910f)
                        :(-3.0258469e9f*t3+2.1070379e6f*t2+0.2226347e3f*t1+0.240390f);
    float xc2=xc*xc,xc3=xc2*xc;
    float yc=(T<2222.0f)?(-1.1063814f*xc3-1.34811020f*xc2+2.18555832f*xc-0.20219683f)
            :(T<4000.0f)?(-0.9549476f*xc3-1.37418593f*xc2+2.09137015f*xc-0.16748867f)
                        :(3.0817580f*xc3-5.87338670f*xc2+3.75112997f*xc-0.37001483f);
    x=xc; y=yc;
}
__device__ void og_whitebalance(float T, float v[3]){
    if(T<=0.0f || (T>6499.0f && T<6501.0f)) return;
    float sx,sy; og_cct_xy(T,sx,sy);
    float sw[3]={sx/sy,1.0f,(1.0f-sx-sy)/sy}; const float dw[3]={0.95047f,1.0f,1.08883f};
    const float MA[9]={0.8951f,0.2664f,-0.1614f,-0.7502f,1.7135f,0.0367f,0.0389f,-0.0685f,1.0296f};
    const float MAi[9]={0.9869929f,-0.1470543f,0.1599627f,0.4323053f,0.5183603f,0.0492912f,-0.0085287f,0.0400428f,0.9684867f};
    float sl[3],dl[3],pl[3]; mul33(MA,sw,sl); mul33(MA,dw,dl); mul33(MA,v,pl);
    pl[0]*=dl[0]/sl[0]; pl[1]*=dl[1]/sl[1]; pl[2]*=dl[2]/sl[2]; mul33(MAi,pl,v);
}

__device__ void og_rgb2hsv(const float c[3], float o[3]){
    float mx=fmaxf(c[0],fmaxf(c[1],c[2])), mn=fminf(c[0],fminf(c[1],c[2])); float v=mx,d=mx-mn; float s=(mx<=0.0f)?0.0f:(d/mx); float h=0.0f;
    if(d>0.0f){ if(mx==c[0]) h=(c[1]-c[2])/d+(c[1]<c[2]?6.0f:0.0f); else if(mx==c[1]) h=(c[2]-c[0])/d+2.0f; else h=(c[0]-c[1])/d+4.0f; h/=6.0f; }
    o[0]=h; o[1]=s; o[2]=v;
}
__device__ void og_hsv2rgb(const float c[3], float o[3]){
    float h=c[0],s=c[1],v=c[2]; if(s<=0.0f){ o[0]=o[1]=o[2]=v; return; }
    h*=6.0f; int i=(int)floorf(h); float f=h-(float)i; float p=v*(1.0f-s),q=v*(1.0f-s*f),t=v*(1.0f-s*(1.0f-f)); i%=6; if(i<0)i+=6;
    if(i==0){o[0]=v;o[1]=t;o[2]=p;} else if(i==1){o[0]=q;o[1]=v;o[2]=p;} else if(i==2){o[0]=p;o[1]=v;o[2]=t;}
    else if(i==3){o[0]=p;o[1]=q;o[2]=v;} else if(i==4){o[0]=t;o[1]=p;o[2]=v;} else {o[0]=v;o[1]=p;o[2]=q;}
}
__device__ float og_r709e(float L){ return (L<0.018f)?(4.5f*L):(1.099f*og_pow(L,0.45f)-0.099f); }
__device__ float og_r709d(float V){ return (V<0.081f)?(V/4.5f):og_pow((V+0.099f)/1.099f,1.0f/0.45f); }
__device__ float og_r709ge(float L, float g){ return og_pow(L,1.0f/g); }
__device__ float og_r709gd(float V, float g){ return og_pow(V,g); }
__device__ float og_lggc(float v,float gain,float lift,float gamma){ v=v*gain; v=v+lift*(1.0f-fminf(v,1.0f)); v=(v<0.0f)?v:og_pow(v,1.0f/gamma); return v; }
__device__ float og_lgg(float L,float gain,float lift,float gamma,float dg){ float v=(dg>0.0f)?og_r709ge(L,dg):og_r709e(L); v=og_lggc(v,gain,lift,gamma); return (dg>0.0f)?og_r709gd(v,dg):og_r709d(v); }
__device__ float og_sm01(float t){ t=fminf(fmaxf(t,0.0f),1.0f); return t*t*(3.0f-2.0f*t); }
__device__ float og_hlmask(float Y,float lo,float hi,float ls,float hs){ float le=fmaxf(ls,1e-4f),he=fmaxf(hs,1e-4f); return og_sm01((Y-(lo-le))/(2.0f*le))*(1.0f-og_sm01((Y-(hi-he))/(2.0f*he))); }
__device__ float og_dienc(float x){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LIN=0.00262409f; return (x>LIN)?((log2f(x+A)+B)*C):(x*M); }
__device__ float og_didec(float x){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LC=0.02740668f; return (x>LC)?(exp2f(x/C-B)-A):(x/M); }

__device__ float og_enc(int enc, float x){
    if(enc==0) return og_r709e(x);
    else if(enc==1) return og_r709ge(x,2.2f);
    else if(enc==2) return og_r709ge(x,2.4f);
    else if(enc==3){ float code=685.0f+300.0f*log10f(fmaxf(x,1e-4f)); return fminf(fmaxf(code/1023.0f,0.0f),1.0f); }
    else if(enc==4){ float A=0.0075f,B=7.0f,C=0.07329248f,M=10.44426855f,LIN=0.00262409f; return (x>LIN)?((log2f(x+A)+B)*C):(x*M); }
    else return x;
}

__device__ void og_sampleLUT(const float* lut,int N,const float c[3],float o[3]){
    float cr=fminf(fmaxf(c[0],0.0f),1.0f)*(N-1), cg=fminf(fmaxf(c[1],0.0f),1.0f)*(N-1), cb=fminf(fmaxf(c[2],0.0f),1.0f)*(N-1);
    int r0=(int)cr,g0=(int)cg,b0=(int)cb; int r1=r0<N-1?r0+1:r0,g1=g0<N-1?g0+1:g0,b1=b0<N-1?b0+1:b0;
    float fr=cr-r0,fg=cg-g0,fb=cb-b0;
    for(int ch=0;ch<3;ch++){
        float c00=lut[((b0*N+g0)*N+r0)*3+ch]*(1-fr)+lut[((b0*N+g0)*N+r1)*3+ch]*fr;
        float c10=lut[((b0*N+g1)*N+r0)*3+ch]*(1-fr)+lut[((b0*N+g1)*N+r1)*3+ch]*fr;
        float c01=lut[((b1*N+g0)*N+r0)*3+ch]*(1-fr)+lut[((b1*N+g0)*N+r1)*3+ch]*fr;
        float c11=lut[((b1*N+g1)*N+r0)*3+ch]*(1-fr)+lut[((b1*N+g1)*N+r1)*3+ch]*fr;
        float d0=c00*(1-fg)+c10*fg,d1=c01*(1-fg)+c11*fg; o[ch]=d0*(1-fb)+d1*fb;
    }
}

__device__ float og_softclip(float v, float amt){
    if(amt<=0.0f || v<=0.0f) return v;
    float k=1.0f-0.6f*amt; if(v<=k) return v;
    float r=1.0f-k; return k + r*(1.0f-expf(-(v-k)/r));
}

__global__ void OneGradeKernel(int W,int H,const float* P,int cam,int enc,const float* lut,int lutN,float lutMix,const float* in,float* out){
    const int x=blockIdx.x*blockDim.x+threadIdx.x;
    const int y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x<W && y<H){
        const int i=((y*W)+x)*4;
        float temp=P[0],tint=P[1],density=P[2],lift=P[3],gamma=P[4],gain=P[5],offTemp=P[6],offTint=P[7],rawExp=P[10],rawTemp=P[11];
        float ex0=exp2f(rawExp);   // RAW exposure (stops), pre-CST
        float lin[3]={og_decode(cam,in[i])*ex0,og_decode(cam,in[i+1])*ex0,og_decode(cam,in[i+2])*ex0};
        float xyz[3]; og_toXYZ(cam,lin,xyz);
        og_whitebalance(rawTemp,xyz);            // RAW white balance in XYZ
        float w[3];   og_XYZtoDWG(xyz,w);
        w[0]*=(1.0f+temp*0.20f); w[2]*=(1.0f-temp*0.20f); w[1]*=(1.0f+tint*0.20f);
        w[0]+=offTemp*0.10f; w[2]-=offTemp*0.10f; w[1]+=offTint*0.10f;
        if(density!=0.0f){ float l[3]={og_dienc(w[0]),og_dienc(w[1]),og_dienc(w[2])}; float hsv[3]; og_rgb2hsv(l,hsv); hsv[1]=fminf(fmaxf(hsv[1]*(1.0f+density),0.0f),1.0f); og_hsv2rgb(hsv,l); w[0]=og_didec(l[0]); w[1]=og_didec(l[1]); w[2]=og_didec(l[2]); }
        float outc[3];
        if(enc<=3){ float x2[3]; og_DWGtoXYZ(w,x2); og_XYZto709(x2,outc); } else { outc[0]=w[0];outc[1]=w[1];outc[2]=w[2]; }  // 709 primaries for Scene/2.2/2.4/Cineon
        float dg=(enc==1)?2.2f:((enc==2)?2.4f:0.0f);                  // grade curve follows output (pure gamma vs Scene OETF)
        float d[3]; for(int k=0;k<3;k++) d[k]=(dg>0.0f)?og_r709ge(outc[k],dg):og_r709e(outc[k]);
        // Range Balance — mask on the PRE-grade display luma, so it cannot chase its own output.
        const float rbL=P[13],rbS=P[14],rbH=P[15],rbF=P[16],rbG=P[17];
        const bool rbW=(P[18]>0.5f); const float rbHg=P[19], rbLg=P[20];
        const bool rbOn=(rbL>0.0f)&&(rbW||rbH!=1.0f||rbF!=0.0f||rbG!=1.0f||rbHg!=1.0f||rbLg!=1.0f);
        float v3[3]; for(int k=0;k<3;k++) v3[k]=og_lggc(d[k],gain,lift,gamma);
        const float rbM=rbOn?og_hlmask(100.0f*(0.2126f*v3[0]+0.7152f*v3[1]+0.0722f*v3[2]),rbL,100.0f,rbS,rbS):0.0f;
        for(int k=0;k<3;k++){
            float v=v3[k];
            if(rbOn){ float rm=og_lggc(v,rbLg,rbF,rbG), hi3=og_lggc(v,rbH,0.0f,rbHg); v=rm*(1.0f-rbM)+hi3*rbM; }
            outc[k]=(dg>0.0f)?og_r709gd(v,dg):og_r709d(v);
        }
        if(rbW){ out[i]=rbM; out[i+1]=rbM; out[i+2]=rbM; out[i+3]=in[i+3]; return; }  // matte: no encode/LUT/trim
        float e[3]={og_enc(enc,outc[0]),og_enc(enc,outc[1]),og_enc(enc,outc[2])};
        if(lutN>=2 && lutMix>0.0f){ float s[3]; og_sampleLUT(lut,lutN,e,s); for(int k=0;k<3;k++) e[k]=e[k]+(s[k]-e[k])*lutMix; }
        float ex=exp2f(P[8]); for(int k=0;k<3;k++) e[k]=(e[k]*ex-0.5f)*P[9]+0.5f;  // post-LUT trim
        if(P[12]>0.0f && (enc<=2 || (lutN>=2 && lutMix>0.0f))) for(int k=0;k<3;k++) e[k]=og_softclip(e[k],P[12]);  // highlight roll-off (display-referred only)
        out[i]=e[0]; out[i+1]=e[1]; out[i+2]=e[2]; out[i+3]=in[i+3];
    }
}

void RunCudaKernel(void* p_Stream, int p_Width, int p_Height, const float* p_Params, int p_Camera, int p_Encode,
                   const float* p_Lut, int p_LutSize, float p_LutMix, const float* p_Input, float* p_Output)
{
    cudaStream_t stream = static_cast<cudaStream_t>(p_Stream);
    dim3 threads(16, 16, 1);
    dim3 blocks((p_Width + threads.x - 1) / threads.x, (p_Height + threads.y - 1) / threads.y, 1);

    float* d_params = nullptr;
    cudaMalloc(&d_params, sizeof(float) * 21);
    cudaMemcpyAsync(d_params, p_Params, sizeof(float) * 21, cudaMemcpyHostToDevice, stream);

    int lutN = (p_Lut && p_LutSize >= 2) ? p_LutSize : 0;
    float* d_lut = nullptr;
    size_t lutCount = lutN ? (size_t)lutN*lutN*lutN*3 : 1;
    cudaMalloc(&d_lut, sizeof(float) * lutCount);
    if (lutN) cudaMemcpyAsync(d_lut, p_Lut, sizeof(float) * lutCount, cudaMemcpyHostToDevice, stream);

    OneGradeKernel<<<blocks, threads, 0, stream>>>(p_Width, p_Height, d_params, p_Camera, p_Encode, d_lut, lutN, p_LutMix, p_Input, p_Output);
    cudaStreamSynchronize(stream);
    cudaFree(d_params);
    cudaFree(d_lut);
}
