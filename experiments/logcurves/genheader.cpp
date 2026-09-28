#include <cmath>
#include <cstdlib>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "OneGradePipeline.h"
#include <cstdio>
#include <vector>
#include <string>
int main(int argc,char** argv){
    std::string dir=argv[1], out=argv[2];
    struct E { const char* id; const char* resolve; const char* file; int cam; };
    const E T[13]={
        {"BMD_GEN5","Blackmagic Design Film Gen 5","cam00",0},
        {"DWG_DI","DaVinci Intermediate","cam01",1},
        {"SLOG3","S-Log3","cam02",2},
        {"LOGC3","ARRI LogC3","cam03",3},
        {"LOGC4","ARRI LogC4","cam04",4},
        {"CLOG3","Canon Log 3","cam05",5},
        {"LOG3G10","RED Log3G10","cam06",6},
        {"DLOG","DJI D-Log","cam07",7},
        {"FLOG2","Fujifilm F-Log2","cam08",8},
        {"VLOG","Panasonic V-Log","cam09",9},
        {"HLG","Rec.2100 HLG","cam10",10},
        {"PQ","Rec.2100 ST2084","cam11",11},
        {"GPLOG2","GoPro GP-Log2","gplog2",-1}};
    FILE* f=fopen(out.c_str(),"w");
    fprintf(f,
"// Ground-truth log transfer functions, measured from DaVinci Resolve 21.1.\n"
"//\n"
"// GENERATED -- do not hand-edit. Regenerate with experiments/logcurves/ (see its README).\n"
"//\n"
"// Method: a 4096-wide neutral 16-bit ramp is rendered through Resolve's RCM with the input\n"
"// gamut pinned equal to the timeline and output gamut, so the 3x3 is identity and ONLY the\n"
"// transfer function acts; output gamma is DaVinci Intermediate, which holds ~9 stops over\n"
"// mid-gray inside [0,1] so nothing clips, and whose exact inverse we already own as cam 1.\n"
"// An identity round-trip through that same path measured 0.498 of a 16-bit LSB, so the\n"
"// harness is lossless -- that check ran FIRST, before any curve was trusted.\n"
"//\n"
"// KNOWN LIMITS OF THE MEASUREMENT:\n"
"//  - 16-bit PNG cannot carry negatives, so the toe below each curve's zero crossing reads a\n"
"//    flat 0.0 and is NOT evidence. Fit the log branch here; take the toe from the published\n"
"//    form and check it for continuity at the knee instead.\n"
"//  - DaVinci Intermediate clips at linear 100, so curves reaching +9.12 EV are flat at the\n"
"//    very top. That is nine stops over mid-gray, far past anything real.\n"
"//\n"
"// Columns: code in [0,1] -> scene linear, mid-gray = 0.18.\n"
"#pragma once\n\n"
"namespace ogref {\n\n"
"struct LogCurve {\n"
"    const char*  id;          // our name\n"
"    const char*  resolveName; // the exact string Resolve's RCM accepts\n"
"    int          cam;         // og::decode_log index, or -1 if we do not implement it yet\n"
"    float        midGrayCode; // code where the curve reaches linear 0.18\n"
"    int          n;\n"
"    const float* code;\n"
"    const float* linear;\n"
"};\n\n");
    const int STEP=32;
    for(int i=0;i<13;++i){
        char p[1024]; snprintf(p,sizeof p,"%s/%s_00086400.png",dir.c_str(),T[i].file);
        int w,h,c; unsigned short* d=stbi_load_16(p,&w,&h,&c,3);
        if(!d){ snprintf(p,sizeof p,"%s/%s00086400.png",dir.c_str(),T[i].file); d=stbi_load_16(p,&w,&h,&c,3); }
        if(!d){ fprintf(stderr,"missing %s\n",T[i].file); continue; }
        std::vector<double> L;
        for(int x=0;x<w;++x){ double g=0; for(int y=0;y<h;++y) g+=d[((size_t)y*w+x)*3+1];
            L.push_back(og::decode_log(1,(float)(g/h/65535.0))); }
        stbi_image_free(d);
        double mg=-1; for(size_t k=1;k<L.size();++k) if(L[k]>=0.18){
            double t=(0.18-L[k-1])/(L[k]-L[k-1]); mg=((k-1)+t)/(L.size()-1); break; }
        std::vector<int> idx; for(int x=0;x<(int)L.size();x+=STEP) idx.push_back(x);
        if(idx.back()!=(int)L.size()-1) idx.push_back(L.size()-1);
        fprintf(f,"// %s -- mid-gray at code %.5f, headroom %+.2f EV\n",
                T[i].resolve, mg, log2(L.back()/0.18));
        fprintf(f,"static const float k%s_code[%zu] = {\n",T[i].id,idx.size());
        for(size_t j=0;j<idx.size();++j) fprintf(f,"%s%.6ff%s",j%8==0?"    ":" ",
            (double)idx[j]/(L.size()-1), j+1==idx.size()?"":(j%8==7?",\n":","));
        fprintf(f,"};\n");
        fprintf(f,"static const float k%s_lin[%zu] = {\n",T[i].id,idx.size());
        for(size_t j=0;j<idx.size();++j) fprintf(f,"%s%.8ff%s",j%6==0?"    ":" ",
            L[idx[j]], j+1==idx.size()?"":(j%6==5?",\n":","));
        fprintf(f,"};\n\n");
    }
    fprintf(f,"static const LogCurve kCurves[] = {\n");
    for(int i=0;i<13;++i){
        char p[1024]; snprintf(p,sizeof p,"%s/%s_00086400.png",dir.c_str(),T[i].file);
        int w,h,c; unsigned short* d=stbi_load_16(p,&w,&h,&c,3);
        if(!d){ snprintf(p,sizeof p,"%s/%s00086400.png",dir.c_str(),T[i].file); d=stbi_load_16(p,&w,&h,&c,3); }
        if(!d) continue;
        std::vector<double> L;
        for(int x=0;x<w;++x){ double g=0; for(int y=0;y<h;++y) g+=d[((size_t)y*w+x)*3+1];
            L.push_back(og::decode_log(1,(float)(g/h/65535.0))); }
        stbi_image_free(d);
        double mg=-1; for(size_t k=1;k<L.size();++k) if(L[k]>=0.18){
            double t=(0.18-L[k-1])/(L[k]-L[k-1]); mg=((k-1)+t)/(L.size()-1); break; }
        int n=(int)((L.size()+STEP-1)/STEP); if((L.size()-1)%STEP) n++;
        fprintf(f,"    { \"%s\", \"%s\", %d, %.6ff, (int)(sizeof(k%s_code)/sizeof(float)), k%s_code, k%s_lin },\n",
                T[i].id,T[i].resolve,T[i].cam,mg,T[i].id,T[i].id,T[i].id);
    }
    fprintf(f,"};\nstatic const int kCurveN = (int)(sizeof(kCurves)/sizeof(kCurves[0]));\n\n");
    fprintf(f,"} // namespace ogref\n");
    fclose(f);
    printf("wrote %s\n",out.c_str());
    return 0;
}
