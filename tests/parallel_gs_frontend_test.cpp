#include "runtime/gs/gs_frontend.h"
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cstring>
int main(int argc,char**argv){
 if(argc!=3)return 2;
 setenv("PS2X_GS_PARALLEL","1",1);setenv("PS2X_GS_PARALLEL_MODULE",argv[1],1);setenv("PS2X_GS_MOLTENVK",argv[2],1);
 unsetenv("PS2X_GS_METAL");unsetenv("PS2X_GS_THREAD");
 std::vector<uint8_t> ram(4*1024*1024);
 GS gs; // Calls reset/flush before initialization, the first integration crash.
 gs.init(ram.data(),uint32_t(ram.size()));
 std::vector<uint64_t> packet{11|(uint64_t(1)<<15)|(uint64_t(1)<<60),0xe,
 1,0x1a,uint64_t(1)<<16,0x4c,uint64_t(1)<<32,0x4e,0,0x18,
 (uint64_t(63)<<16)|(uint64_t(31)<<48),0x40,0,0x47,1,0x46,6,0,
 (uint64_t(0x3f800000)<<32)|0x80402010,1,0,5,uint64_t(64*16)|(uint64_t(32*16)<<16),5};
 gs.processGIFPacket(reinterpret_cast<uint8_t*>(packet.data()),uint32_t(packet.size()*8),1);
 if(gs.ReadVram(0,0,1,0,0)!=0x80402010)return 3;
 if(!gs.clearFramebufferContext(0,0x80705030)||gs.ReadVram(0,0,1,0,0)!=0x80705030)return 4;
 uint32_t pixels[4]={0xff030201,0xff060504,0xff090807,0xff0c0b0a};
 gs.uploadImageNative(uint64_t(1)<<48,0,4|(uint64_t(1)<<32),0,reinterpret_cast<uint8_t*>(pixels),16);
 if(gs.ReadVram(0,0,1,0,0)!=pixels[0])return 5;
 gs.writeRegister(0x50,uint64_t(1)<<16);gs.writeRegister(0x51,0);gs.writeRegister(0x52,4|(uint64_t(1)<<32));gs.writeRegister(0x53,1);
 if(gs.downloadFifoQwc()!=1)return 9;
 uint32_t download[4]={};if(gs.consumeLocalToHostBytes(reinterpret_cast<uint8_t*>(download),16)!=16||std::memcmp(download,pixels,16))return 6;
 // Distinct pixels over several pages validate GPU VRAM addressing through presentation decoder.
 std::vector<uint32_t> pattern(128*96);
 for(size_t i=0;i<pattern.size();++i)pattern[i]=0x80000000u|uint32_t(i*37+19);
 gs.uploadImageNative((uint64_t(2240)<<32)|(uint64_t(2)<<48),0,128|(uint64_t(96)<<32),0,reinterpret_cast<uint8_t*>(pattern.data()),uint32_t(pattern.size()*4));
 // VIF DIRECT split: GPU keeps the original IMAGE state; only the legacy
 // frontend receives the auxiliary continuation tag.
 gs.writeRegister(0x50,(uint64_t(2240)<<32)|(uint64_t(2)<<48));
 gs.writeRegister(0x51,0);gs.writeRegister(0x52,128|(uint64_t(96)<<32));gs.writeRegister(0x53,0);
 for(auto& pixel:pattern)pixel^=0x00123456;
 std::vector<uint8_t> first(32),rest(16+pattern.size()*4-16);
 uint64_t imageTag=uint64_t(pattern.size()/4)|(uint64_t(2)<<58);
 std::memcpy(first.data(),&imageTag,8);std::memcpy(first.data()+16,pattern.data(),16);
 gs.processGIFPacket(first.data(),uint32_t(first.size()),2);
 imageTag=(pattern.size()/4-1)|(uint64_t(2)<<58)|(uint64_t(1)<<15);
 std::memcpy(rest.data(),&imageTag,8);std::memcpy(rest.data()+16,reinterpret_cast<uint8_t*>(pattern.data())+16,pattern.size()*4-16);
 gs.processGIFPacket(rest.data(),uint32_t(rest.size()),2,true);
 for(uint32_t y=0;y<96;y++)for(uint32_t x=0;x<128;x++){
  auto pixel=gs.ReadVram(0,2240,2,x,y);
  if(pixel!=pattern[y*128+x]){std::fprintf(stderr,"VRAM layout mismatch x=%u y=%u actual=%08x expected=%08x\n",x,y,pixel,pattern[y*128+x]);return 7;}
 }
 gs.processGIFPacket(reinterpret_cast<uint8_t*>(packet.data()),uint32_t(packet.size()*8),2);
 if(gs.ReadVram(0,0,1,0,0)!=0x80402010)return 8;
 if(gs.downloadFifoQwc()!=0)return 10;
 gs.reset();std::puts("PASS: actual frontend pre-init lifecycle, raw GIF GPU sprite, GPU clear, native upload, FIFO");return 0;
}
