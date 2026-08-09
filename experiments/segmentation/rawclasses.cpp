// What ADE20K classes does the shipped model actually see? The region map collapses 150 into
// 7, and when a frame comes back as one region the useful question is which classes got merged.
#include <cmath>
#include <cstring>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "net.h"
#include <cstdio>
#include <vector>
#include <algorithm>
static const char* ade[150] = {"wall","building","sky","floor","tree","ceiling","road","bed","windowpane","grass","cabinet","sidewalk","person","earth","door","table","mountain","plant","curtain","chair","car","water","painting","sofa","shelf","house","sea","mirror","rug","field","armchair","seat","fence","desk","rock","wardrobe","lamp","bathtub","railing","cushion","base","box","column","signboard","chest","counter","sand","sink","skyscraper","fireplace","fridge","grandstand","path","stairs","runway","case","pooltable","pillow","screendoor","stairway","river","bridge","bookcase","blind","coffeetable","toilet","flower","book","hill","bench","countertop","stove","palm","kitchenisland","computer","swivelchair","boat","bar","arcade","hovel","bus","towel","light","truck","tower","chandelier","awning","streetlight","booth","tv","airplane","dirttrack","apparel","pole","land","bannister","escalator","ottoman","bottle","buffet","poster","stage","van","ship","fountain","conveyer","canopy","washer","plaything","swimmingpool","stool","barrel","basket","waterfall","tent","bag","minibike","cradle","oven","ball","food","step","tank","tradename","microwave","pot","animal","bicycle","lake","dishwasher","screen","blanket","sculpture","hood","sconce","vase","trafficlight","tray","ashcan","fan","pier","crtscreen","plate","monitor","bulletinboard","shower","radiator","glass","clock","flag"};
int main(int argc, char** argv){
  ncnn::Net net; net.opt.use_vulkan_compute=false; net.opt.num_threads=1; net.opt.lightmode=true;
  char pp[512], bp[512];
  snprintf(pp,sizeof pp,"%s/ade20k.param",argv[1]); snprintf(bp,sizeof bp,"%s/ade20k.bin",argv[1]);
  if(net.load_param(pp)||net.load_model(bp)){fprintf(stderr,"model load failed\n");return 2;}
  const float mean[3]={0.485f*255,0.456f*255,0.406f*255};
  const float norm[3]={1/(0.229f*255),1/(0.224f*255),1/(0.225f*255)};
  for(int f=2; f<argc; ++f){
    int w,h,c; unsigned char* img=stbi_load(argv[f],&w,&h,&c,3);
    if(!img){fprintf(stderr,"cannot read %s\n",argv[f]);continue;}
    ncnn::Mat in=ncnn::Mat::from_pixels_resize(img,ncnn::Mat::PIXEL_RGB,w,h,512,512);
    in.substract_mean_normalize(mean,norm);
    ncnn::Extractor ex=net.create_extractor(); ex.input("in0",in);
    ncnn::Mat out; ex.extract("out0",out);
    std::vector<long long> hist(150,0);
    const int step=std::max(1,std::max(out.w,out.h)/256);
    long long n=0;
    for(int y=0;y<out.h;y+=step) for(int x=0;x<out.w;x+=step){
      int best=0; float bv=-1e30f;
      for(int k=0;k<out.c&&k<150;k++){float v=out.channel(k).row(y)[x]; if(v>bv){bv=v;best=k;}}
      hist[best]++; n++;
    }
    std::vector<std::pair<long long,int>> v;
    for(int i=0;i<150;i++) if(hist[i]) v.push_back({hist[i],i});
    std::sort(v.rbegin(),v.rend());
    const char* nm=strrchr(argv[f],'/'); nm=nm?nm+1:argv[f];
    printf("\n%s:\n", nm);
    for(size_t i=0;i<v.size()&&i<8;i++)
      printf("   %-14s %5.1f%%  (class %d)\n", ade[v[i].second], 100.0*v[i].first/n, v[i].second);
    stbi_image_free(img);
  }
  return 0;}
