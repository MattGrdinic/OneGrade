// Does region-aware white balance behave? The failure that matters is turning a sunset grey,
// so the frames with no neutral surface in them must DECLINE, not guess.
#include <cmath>
#include <cstring>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "OneGradeAnalysis.h"
#include "OneGradeSegment.h"
#include <cstdio>
#include <vector>
int main(int argc, char** argv){
  og::seg::Segmenter seg; char pp[512], bp[512];
  snprintf(pp,sizeof pp,"%s/ade20k.param",argv[1]); snprintf(bp,sizeof bp,"%s/ade20k.bin",argv[1]);
  if(!seg.load(pp,bp)){fprintf(stderr,"model load failed\n");return 2;}
  printf("%-26s %8s %7s %7s %7s  %s\n","frame","ref%","a*","b*","C*","verdict");
  for(int f=2; f<argc; ++f){
    int w,h,c; unsigned char* img=stbi_load(argv[f],&w,&h,&c,3);
    if(!img) continue;
    std::vector<unsigned char> th((size_t)512*512*3);
    for(int y=0;y<512;y++) for(int x=0;x<512;x++){
      const unsigned char* s=img+(((size_t)(y*h/512)*w)+(x*w/512))*3;
      unsigned char* d=&th[((size_t)y*512+x)*3]; d[0]=s[0];d[1]=s[1];d[2]=s[2];
    }
    std::vector<unsigned char> mask; int mw=0,mh=0;
    seg.run(th.data(),512,512,mask,mw,mh);
    // reference coverage and mean b*, straight off the (already display-referred) export
    long long ref=0; double sum=0, suma=0;
    for(int y=0;y<512;y++) for(int x=0;x<512;x++){
      const unsigned char r = mask[(size_t)(y*mh/512)*mw + (x*mw/512)];
      if(r!=oga::R_BUILT && r!=oga::R_GROUND) continue;
      const unsigned char* px=&th[((size_t)y*512+x)*3];
      float L,a,b; oga::display_to_Lab(1, px[0]/255.f, px[1]/255.f, px[2]/255.f, L,a,b);
      sum += b; suma += a; ref++;
    }
    const double cover = 100.0*ref/(512.0*512.0);
    const char* nm=strrchr(argv[f],'/'); nm=nm?nm+1:argv[f];
    const double mb = ref? sum/ref : 0.0, ma = ref? suma/ref : 0.0;
    const double chroma = std::sqrt(ma*ma + mb*mb);
    printf("%-26s %7.1f%% %7.1f %7.1f %7.1f  %s\n", nm, cover, ma, mb, chroma,
           cover < 15.0 ? "DECLINE - no reference"
                        : (chroma > 9.0 ? "DECLINE - reference not neutral"
                        : (mb < -0.5 ? "would warm" : (mb > 0.5 ? "would cool" : "neutral already"))));
    stbi_image_free(img);
  }
  return 0;}
