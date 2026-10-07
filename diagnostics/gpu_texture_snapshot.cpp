// Decode a texture from a GPU VRAM snapshot; does not modify the running game.
#include "runtime/gs/ps2_gs_memory.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
int main(int argc,char** argv){
 if(argc!=4&&argc!=5)return 2;
 std::vector<uint8_t> memory(4*1024*1024);
 FILE* file=std::fopen(argv[1],"rb");if(!file)return 3;
 bool complete=std::fread(memory.data(),1,memory.size(),file)==memory.size();std::fclose(file);if(!complete)return 4;
 uint64_t tex0=std::strtoull(argv[2],nullptr,16);
 unsigned bp=tex0&16383,bw=(tex0>>14)&63,psm=(tex0>>20)&63;
 unsigned width=1u<<std::min(10u,unsigned((tex0>>26)&15)),height=1u<<std::min(10u,unsigned((tex0>>30)&15));
 unsigned cbp=(tex0>>37)&16383,cpsm=(tex0>>51)&15,csm=(tex0>>55)&1,csa=(tex0>>56)&31;
 bool p4=psm==20||psm==36||psm==44,p8=psm==19||psm==27;
 // This diagnostic covers the CSM1/PSMCT32 combination observed in Black.
 if((p4||p8)&&(csm!=0||cpsm!=0)){std::fprintf(stderr,"unsupported diagnostic CLUT layout\n");return 5;}
 GSMem::InitLookupTables();
 GSMem::TexturePageCache cache;
 auto read=[&](unsigned format,unsigned base,unsigned stride,unsigned x,unsigned y){return GSMem::ReadTexture(cache,memory.data(),format,base,stride,x,y);};
 uint32_t palette[256]{};
 if(p4||p8)for(unsigned i=0;i<(p4?16u:256u);i++){
  unsigned logical=i+(csa&15)*16;
  unsigned physical=(logical&~24u)|((logical&8u)<<1)|((logical&16u)>>1);
  palette[i]=read(cpsm,cbp,1,physical&15,physical>>4);
  if(i<16)std::printf("palette[%u]=%08x\n",i,palette[i]);
 }
 if(argc==5 && (p4||p8)){
  // paraLLEl-GS stores cached CT32 colors in two 256-halfword planes.
  uint16_t planes[512]{};FILE* clut=std::fopen(argv[4],"rb");if(!clut)return 7;
  const bool complete=std::fread(planes,1,sizeof(planes),clut)==sizeof(planes);std::fclose(clut);if(!complete)return 8;
  for(unsigned i=0;i<(p4?16u:256u);i++){
   const unsigned index=(i+(csa&15)*16)&255;
   palette[i]=uint32_t(planes[index])|(uint32_t(planes[index+256])<<16);
   if(i<16)std::printf("cached_palette[%u]=%08x\n",i,palette[i]);
  }
 }
 std::string prefix(argv[3]);
 FILE* rgb=std::fopen((prefix+".ppm").c_str(),"wb");
 FILE* index=std::fopen((prefix+".pgm").c_str(),"wb");if(!rgb||!index)return 6;
 std::fprintf(rgb,"P6\n%u %u\n255\n",width,height);std::fprintf(index,"P5\n%u %u\n255\n",width,height);
 for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++){
  uint32_t value=read(psm,bp,std::max(1u,bw),x,y);
  uint32_t color=(p4||p8)?palette[value&(p4?15:255)]:value;
  uint8_t bytes[3]={uint8_t(color),uint8_t(color>>8),uint8_t(color>>16)};
  std::fwrite(bytes,1,3,rgb);std::fputc(p4?(value&15)*17:value&255,index);
 }
 std::fclose(rgb);std::fclose(index);
 std::printf("decoded %ux%u PSM=%02x TBP=%u CBP=%u\n",width,height,psm,bp,cbp);
}
