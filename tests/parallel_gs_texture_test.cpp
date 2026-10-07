#include "runtime/gs/gs_parallel_api.h"
#include "runtime/gs/ps2_gs_memory.h"
#include <dlfcn.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

int main(int argc,char** argv)
{
 if(argc!=3)return 2;
 void* lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
 if(!lib){std::puts(dlerror());return 2;}
 auto get=reinterpret_cast<const GSParallelAPI*(*)()>(dlsym(lib,"black_parallel_gs_api"));
 if(!get||get()->version!=5)return 3;
 auto a=get();std::vector<uint8_t> ram(4*1024*1024);
 void* gpu=a->create(argv[2],ram.data());if(!gpu)return 4;
 try {
  std::array<uint32_t,256> palette{};
  for(unsigned i=0;i<256;i++)palette[i]=0x80000000u|i|((255u-i)<<8)|((i^0x5au)<<16);
  auto upload=[&](unsigned bp,unsigned bw,unsigned psm,unsigned w,unsigned h,const uint8_t* bytes,unsigned size){
   a->reg(gpu,0x50,(uint64_t(bp)<<32)|(uint64_t(bw)<<48)|(uint64_t(psm)<<56));
   a->reg(gpu,0x51,0);a->reg(gpu,0x52,w|(uint64_t(h)<<32));a->reg(gpu,0x53,0);
   a->image(gpu,bytes,size);
  };
  // Black commonly uploads swizzled texture assets using 16-bit transfers.
  // Exercise those transfers across pages with nonzero origins and base block.
  for(unsigned psm:{2u,10u}){
   std::vector<uint16_t> expected(224*240);
   for(unsigned i=0;i<expected.size();i++)expected[i]=uint16_t(i*37u+19u);
   a->reg(gpu,0x50,(uint64_t(1025)<<32)|(uint64_t(8)<<48)|(uint64_t(psm)<<56));
   a->reg(gpu,0x51,(uint64_t(48)<<32)|(uint64_t(24)<<48));
   a->reg(gpu,0x52,224|(uint64_t(240)<<32));a->reg(gpu,0x53,0);
   a->image(gpu,reinterpret_cast<const uint8_t*>(expected.data()),expected.size()*2);
   a->reg(gpu,0x50,1025|(uint64_t(8)<<16)|(uint64_t(psm)<<24));
   a->reg(gpu,0x51,48|(uint64_t(24)<<16));a->reg(gpu,0x53,1);
   std::vector<uint16_t> actual(expected.size());
   a->fifo(gpu,reinterpret_cast<uint8_t*>(actual.data()),actual.size()*2);
   if(actual!=expected)throw std::runtime_error("16-bit multi-page upload/download mismatch");
   std::printf("PASS: PSM=%02x, 53760 uploaded 16-bit values round-trip across pages\n",psm);
  }
  for(unsigned csm:{0u,1u})for(unsigned psm:{0u,0x13u,0x14u})for(unsigned transfer:{0u,2u,10u})for(bool keepClut:{false,true}){
   if(keepClut && (transfer!=0||csm!=0||psm==0))continue;
   if(psm==0 && transfer!=0)continue;
   std::array<uint32_t,256> arranged=palette;
   if(csm==0)for(unsigned i=0;i<256;i++){
    // CSM1 PSMCT32 swaps CLUT index bits 3 and 4.
    unsigned logical=(i&~24u)|((i&8u)<<1)|((i&16u)>>1);
    arranged[i]=palette[logical];
   }
   upload(64,csm?4:1,0,csm?256:16,csm?1:16,reinterpret_cast<const uint8_t*>(arranged.data()),1024);
   std::vector<uint8_t> texture(psm==0?16384:psm==0x13?4096:2048);
   std::vector<uint32_t> expected(4096);
   for(unsigned i=0;i<4096;i++){
    unsigned index=i&(psm==0x14?15:255);expected[i]=palette[index];
    if(psm==0){for(unsigned j=0;j<4;j++)texture[4*i+j]=expected[i]>>(8*j);}
    else if(psm==0x13)texture[i]=index;
    else texture[i/2]|=index<<((i&1)*4);
   }
   // A 256-wide PSMCT32 CLUT spans four GS pages, even at height 1.
   // Keep the texture beyond all of those pages, not just its linear bytes.
   if(transfer==0)upload(256,1,psm,64,64,texture.data(),texture.size());
   else {
    std::vector<uint16_t> words(4096);
    std::vector<uint8_t> model(4*1024*1024);
    GSMem::InitLookupTables();
    for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){
     unsigned i=y*64+x;words[i]=uint16_t(i*37u+19u);
     if(transfer==2)GSMem::WriteCT16(model.data(),256,1,x,y,words[i]);
     else GSMem::WriteCT16S(model.data(),256,1,x,y,words[i]);
    }
    GSMem::TexturePageCache cache;
    for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++)
     expected[y*64+x]=palette[GSMem::ReadTexture(cache,model.data(),psm,256,1,x,y)];
    upload(256,1,transfer,64,64,reinterpret_cast<const uint8_t*>(words.data()),words.size()*2);
   }
   a->clear(gpu,0,1,0,0,63,63,0,0,0);
   a->reg(gpu,0x1a,1);a->reg(gpu,0x4c,uint64_t(1)<<16);
   a->reg(gpu,0x4e,uint64_t(1)<<32);a->reg(gpu,0x18,0);
   a->reg(gpu,0x40,(uint64_t(63)<<16)|(uint64_t(63)<<48));
   a->reg(gpu,0x47,0);a->reg(gpu,0x46,1);a->reg(gpu,0x14,0);
   a->reg(gpu,0x08,0);a->reg(gpu,0x1c,4); // CSM2: 256-wide CLUT, origin 0.
   a->reg(gpu,0x3f,0);
   const uint64_t tex0=256ull|(1ull<<14)|(uint64_t(psm)<<20)|(6ull<<26)|(6ull<<30)|
                       (1ull<<34)|(1ull<<35)|(64ull<<37)|(uint64_t(csm)<<55)|(1ull<<61);
   // Use the real A+D transport for TEX0/CLUT loading, including the one-shot diagnostic.
   const uint64_t packet[4]={1ull|(1ull<<15)|(1ull<<60),14,tex0,6};
   a->gif(gpu,0,reinterpret_cast<const uint8_t*>(packet),sizeof(packet));
   if(keepClut){
    // TEX0 loads the internal CLUT before this VRAM region is reused.
    // CLD=0 must preserve those colors even when VRAM changes before the draw.
    std::array<uint32_t,256> replacement{};replacement.fill(0x80ff00ffu);
    upload(64,1,0,16,16,reinterpret_cast<const uint8_t*>(replacement.data()),1024);
    a->reg(gpu,0x06,tex0 & ~(7ull<<61));
   }
   a->reg(gpu,0,6|(1u<<4)|(1u<<8)); // Textured sprite, integer UV, no blending.
   a->reg(gpu,1,(uint64_t(0x3f800000)<<32)|0x80808080u);
   a->reg(gpu,3,0);a->reg(gpu,5,0);
   a->reg(gpu,3,1024|(uint64_t(1024)<<16));a->reg(gpu,5,1024|(uint64_t(1024)<<16));
   a->flush(gpu);
   a->reg(gpu,0x50,uint64_t(1)<<16);a->reg(gpu,0x51,0);
   a->reg(gpu,0x52,64|(uint64_t(64)<<32));a->reg(gpu,0x53,1);
   std::vector<uint32_t> actual(4096);
   a->fifo(gpu,reinterpret_cast<uint8_t*>(actual.data()),actual.size()*4);
   unsigned errors=0;
   for(unsigned i=0;i<4096;i++)if(actual[i]!=expected[i]){
    if(errors++<4)std::fprintf(stderr,"CSM=%u PSM=%02x pixel=(%u,%u) actual=%08x expected=%08x\n",csm,psm,i%64,i/64,actual[i],expected[i]);
   }
   if(errors)throw std::runtime_error("texture color/index mismatch");
   std::printf("PASS: CSM=%u PSM=%02x transfer=%02x keepClut=%u, 4096 textured pixels match known colors\n",csm,psm,transfer,unsigned(keepClut));
  }
  a->destroy(gpu);dlclose(lib);return 0;
 } catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());a->destroy(gpu);return 1;}
}
