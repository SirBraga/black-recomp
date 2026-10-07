// Find which captured host upload last writes each byte of a selected CLUT block.
#include "runtime/gs/ps2_gs_memory.h"
#include <cstdio>
#include <vector>
#include <array>
#include <cstdlib>
int main(int argc,char**argv){
 if(argc!=3)return 2;unsigned cbp=std::strtoul(argv[2],nullptr,0);
 FILE*f=std::fopen(argv[1],"rb");if(!f)return 3;
 GSMem::InitLookupTables();std::array<unsigned char,256> bytes{};std::array<unsigned,256> writers{};
 unsigned header[8],seq=0;
 while(std::fread(header,sizeof(header),1,f)==1){
  unsigned psm=header[0],bp=header[1],bw=header[2],x0=header[3],y0=header[4],w=header[5],h=header[6];
  std::vector<unsigned char> data(header[7]);if(std::fread(data.data(),1,data.size(),f)!=data.size())return 4;
  ++seq;unsigned bpp=psm==0?32:psm==1?24:(psm==2||psm==10)?16:psm==19?8:psm==20?4:0;
  if(!bpp)continue;unsigned touched=0;
  for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){
   unsigned bit=GSMem::PixelBitAddress(psm,bp,bw,x0+x,y0+y);
   for(unsigned k=0;k<bpp;k++){
    unsigned dest=((bit+k)/8)&(4*1024*1024-1),source=((y*w+x)*bpp+k);
    if(dest<cbp*256||dest>=cbp*256+256||source/8>=data.size())continue;
    unsigned index=dest-cbp*256,mask=1u<<((bit+k)%8);
    bytes[index]=(bytes[index]&~mask)|(((data[source/8]>>(source%8))&1u)?mask:0);
    writers[index]=seq;++touched;
   }
  }
  if(touched)printf("write %u: PSM=%02x BP=%u BW=%u origin=%u,%u size=%ux%u bytes=%zu CLUT-bits=%u\n",seq,psm,bp,bw,x0,y0,w,h,data.size(),touched);
 }
 for(unsigned i=0;i<16;i++){
  unsigned physical=(i&~24u)|((i&8u)<<1)|((i&16u)>>1);
  unsigned addr=GSMem::PixelBitAddress(0,cbp,1,physical&15,physical>>4)/8-cbp*256;
  unsigned value;std::memcpy(&value,bytes.data()+addr,4);
  printf("palette[%u]=%08x last-writes=%u,%u,%u,%u\n",i,value,writers[addr],writers[addr+1],writers[addr+2],writers[addr+3]);
 }
 std::fclose(f);
}
