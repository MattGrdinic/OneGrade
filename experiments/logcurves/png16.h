#pragma once
#include <zlib.h>
#include <cstdio>
#include <vector>
static void og_be32(std::vector<unsigned char>& o, unsigned v){
    o.push_back(v>>24); o.push_back(v>>16); o.push_back(v>>8); o.push_back(v);
}
static void og_chunk(FILE* f, const char* type, const unsigned char* d, size_t n){
    unsigned char len[4]={(unsigned char)(n>>24),(unsigned char)(n>>16),(unsigned char)(n>>8),(unsigned char)n};
    fwrite(len,1,4,f); fwrite(type,1,4,f); if(n) fwrite(d,1,n,f);
    uLong c=crc32(0L,(const Bytef*)type,4); if(n) c=crc32(c,(const Bytef*)d,(uInt)n);
    unsigned char cb[4]={(unsigned char)(c>>24),(unsigned char)(c>>16),(unsigned char)(c>>8),(unsigned char)c};
    fwrite(cb,1,4,f);
}
static bool og_write_png16(const char* path,int w,int h,const unsigned short* px){
    FILE* f=fopen(path,"wb"); if(!f) return false;
    const unsigned char sig[8]={137,80,78,71,13,10,26,10}; fwrite(sig,1,8,f);
    std::vector<unsigned char> ih; og_be32(ih,w); og_be32(ih,h);
    ih.push_back(16); ih.push_back(2); ih.push_back(0); ih.push_back(0); ih.push_back(0);
    og_chunk(f,"IHDR",ih.data(),ih.size());
    std::vector<unsigned char> raw; raw.reserve((size_t)h*(1+(size_t)w*6));
    for(int y=0;y<h;++y){ raw.push_back(0); const unsigned short* r=px+(size_t)y*w*3;
        for(int i=0;i<w*3;++i){ raw.push_back(r[i]>>8); raw.push_back(r[i]&0xFF);} }
    uLongf cap=compressBound(raw.size()); std::vector<unsigned char> z(cap);
    if(compress2(z.data(),&cap,raw.data(),raw.size(),6)!=Z_OK){fclose(f);return false;}
    og_chunk(f,"IDAT",z.data(),cap); og_chunk(f,"IEND",nullptr,0); fclose(f); return true;
}
