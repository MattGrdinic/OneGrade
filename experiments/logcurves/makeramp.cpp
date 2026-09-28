// A neutral 16-bit ramp: column x carries code x/(W-1) in all three channels.
// Pushed through Resolve's RCM it reads back as that colour space's transfer function.
#include "png16.h"
#include <cstdio>
#include <vector>
int main(int argc,char** argv){
    const int W = 4096, H = 256;
    std::vector<unsigned short> px((size_t)W*H*3);
    for(int y=0;y<H;++y) for(int x=0;x<W;++x){
        unsigned short v = (unsigned short)((double)x*65535.0/(W-1) + 0.5);
        size_t i=((size_t)y*W+x)*3; px[i]=px[i+1]=px[i+2]=v;
    }
    const char* out = argc>1?argv[1]:"ramp.png";
    printf("%s %dx%d  %s\n", out, W, H, og_write_png16(out,W,H,px.data())?"ok":"FAILED");
    return 0;
}
