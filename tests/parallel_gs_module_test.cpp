#include "runtime/gs/gs_parallel_api.h"
#include <dlfcn.h>
#include <vector>
#include <cstdio>
#include <cstring>
#include <array>
#include <stdexcept>
#include <cstdlib>
#if defined(__APPLE__)
extern "C" void* black_test_window_create();
extern "C" void black_test_window_destroy(void*);
#endif
int main(int argc,char**argv){
 const bool native=argc==4&&std::strcmp(argv[3],"--metal-present")==0;
 if(argc<3||argc>4)return 2;
#if defined(__APPLE__)
 void* window=nullptr;
 if(native){setenv("PS2X_GS_NATIVE_PRESENT","1",1);window=black_test_window_create();if(!window)return 2;}
#else
 if(native)return 2;
#endif
 void* lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){std::puts(dlerror());return 2;}
 auto get=reinterpret_cast<const GSParallelAPI*(*)()>(dlsym(lib,"black_parallel_gs_api"));if(!get||get()->version!=5||!get()->attach_window)return 3;
 auto a=get();std::vector<uint8_t> ram(4*1024*1024);void* gpu=a->create(argv[2],ram.data());if(!gpu)return 4;
#if defined(__APPLE__)
 if(native&&!a->attach_window(gpu,window))throw std::runtime_error("Metal presenter attach failed");
#endif
 auto check=[&](uint32_t expected){uint32_t pixel=0;a->read(gpu,0,reinterpret_cast<uint8_t*>(&pixel),4);if(pixel!=expected){std::fprintf(stderr,"pixel %08x expected %08x\n",pixel,expected);throw std::runtime_error("VRAM mismatch");}};
 try{
  // Packed A+D packet split between tag and payload on PATH1; PATH2 interleaves a NOP tag.
  std::vector<uint64_t> packet{10|(uint64_t(1)<<15)|(uint64_t(1)<<60),0xe,
   1,0x1a, uint64_t(1)<<16,0x4c,uint64_t(1)<<32,0x4e,0,0x18,
   (uint64_t(63)<<16)|(uint64_t(31)<<48),0x40,0,0x47,1,0x46,6,0x00,
   (uint64_t(0x3f800000)<<32)|0x80402010,0x01,0,0x05};
  a->gif(gpu,1,reinterpret_cast<uint8_t*>(packet.data()),16);
  std::array<uint64_t,2> nop{uint64_t(1)<<15,0};a->gif(gpu,2,reinterpret_cast<uint8_t*>(nop.data()),16);
  a->gif(gpu,1,reinterpret_cast<uint8_t*>(packet.data()+2),uint32_t(packet.size()*8-16));
  a->reg(gpu,5,uint64_t(64*16)|(uint64_t(32*16)<<16));a->flush(gpu);check(0x80402010);
  a->clear(gpu,0,1,0,0,63,31,0x80705030,0,0);check(0x80705030);
  // Alpha and individual RGB mask bits preserve the previous framebuffer.
  a->clear(gpu,0,1,0,0,63,31,0x12345678,0xff00ff00,0);check(0x80345078);
  a->clear(gpu,0,1,0,0,63,31,0x01020304,0,1);check(0x81020304);
  a->clear(gpu,0,1,0,0,63,31,0,0xffffffff,0);check(0x81020304);
  // NOP prefix + partial A+D batch. Guard qwords must never be consumed.
  std::vector<uint64_t> guarded{0,0,5|(uint64_t(1)<<60),0xe,
    (uint64_t(0x3f800000)<<32)|0x80554433,1,0,0x47,uint64_t(1)<<16,0x4c,
    (uint64_t(0x3f800000)<<32)|0x80aabbcc,1,1,0x46};
  a->gif(gpu,3,reinterpret_cast<uint8_t*>(guarded.data()),5*16);
  std::array<uint64_t,4> remaining{0,0x3f,0,0x47};
  a->gif(gpu,3,reinterpret_cast<uint8_t*>(remaining.data()),32);
  a->reg(gpu,0,6);a->reg(gpu,5,0);a->reg(gpu,5,uint64_t(64*16)|(uint64_t(32*16)<<16));
  a->flush(gpu);check(0x80554433);
  // Native host-to-local upload and subsequent local-to-host FIFO.
  a->reg(gpu,0x50,uint64_t(1)<<48);a->reg(gpu,0x51,0);a->reg(gpu,0x52,4|(uint64_t(1)<<32));a->reg(gpu,0x53,0);
  std::array<uint32_t,4> pixels{0xff030201,0xff060504,0xff090807,0xff0c0b0a};a->image(gpu,reinterpret_cast<uint8_t*>(pixels.data()),16);check(pixels[0]);
  a->reg(gpu,0x50,uint64_t(1)<<16);a->reg(gpu,0x52,4|(uint64_t(1)<<32));a->reg(gpu,0x53,1);
  std::array<uint32_t,4> download{};a->fifo(gpu,reinterpret_cast<uint8_t*>(download.data()),16);if(download!=pixels)throw std::runtime_error("FIFO mismatch");
  // Valid multi-page local copy with nonzero X/Y: old page mask underflows.
  std::vector<uint32_t> source(224*240);
  for(size_t i=0;i<source.size();i++)source[i]=0x80000000u|uint32_t(i*37+19);
  a->reg(gpu,0x50,(uint64_t(1)<<32)|(uint64_t(8)<<48));
  a->reg(gpu,0x51,(uint64_t(48)<<32)|(uint64_t(24)<<48));
  a->reg(gpu,0x52,224|(uint64_t(240)<<32));a->reg(gpu,0x53,0);
  a->image(gpu,reinterpret_cast<uint8_t*>(source.data()),uint32_t(source.size()*4));
  a->reg(gpu,0x50,uint64_t(1)|(uint64_t(8)<<16)|(uint64_t(8)<<48)|(uint64_t(1025)<<32));
  a->reg(gpu,0x51,uint64_t(48)|(uint64_t(24)<<16));a->reg(gpu,0x53,2);
  a->flush(gpu);
  a->reg(gpu,0x50,uint64_t(1025)|(uint64_t(8)<<16));a->reg(gpu,0x51,0);a->reg(gpu,0x53,1);
  std::vector<uint32_t> copied(source.size());a->fifo(gpu,reinterpret_cast<uint8_t*>(copied.data()),uint32_t(copied.size()*4));
  if(copied!=source)throw std::runtime_error("multi-page local copy mismatch");
  // CRT setup must reach GPU scanout instead of silently using CPU conversion.
  a->clear(gpu,0,1,0,0,63,31,0x80554433,0,0);
  std::vector<uint8_t> screen(640*512*4);
  GSParallelScanout scan{};
  scan.pmode=0xff21;scan.smode1=4ull|(32ull<<3)|(1ull<<10)|(2ull<<13)|(4ull<<21)|(1ull<<30)|(1ull<<32)|(1ull<<33)|(1ull<<34);
  scan.dispfb1=uint64_t(1)<<9;
  scan.display1=159ull|(25ull<<12)|(63ull<<32)|(31ull<<44);
  scan.rgba=screen.data();scan.stride=640*4;scan.maxHeight=512;
  if(!a->scanout(gpu,&scan)||!scan.width||!scan.height)throw std::runtime_error("GPU CRT scanout empty");
  if(!native&&(screen[0]!=0x33||screen[1]!=0x44||screen[2]!=0x55))throw std::runtime_error("GPU CRT scanout color mismatch");
  std::printf("GPU scanout: %ux%u\n",scan.width,scan.height);
  a->clear(gpu,0,10,0,0,639,447,0x80554433,0,0);
  scan.pmode=0x8003;scan.smode2=3;
  scan.dispfb1=scan.dispfb2=uint64_t(10)<<9;
  scan.display1=scan.display2=636ull|(50ull<<12)|(3ull<<23)|(2559ull<<32)|(447ull<<44);
  if(!a->scanout(gpu,&scan)||scan.width!=640||scan.height!=448)throw std::runtime_error("NTSC dual CRT dimensions mismatch");
  if(!native&&(screen[0]!=0x33||screen[1]!=0x44||screen[2]!=0x55))throw std::runtime_error("NTSC dual CRT color mismatch");
  // Black retains a literal 640x448 circuit with non-interlaced DISPLAY.
  // Host presentation must use those circuit pixels, not crop a 2560x224 CRT.
  scan.smode2=0;scan.display1=(639ull<<32)|(447ull<<44);
  scan.display2=(639ull<<32)|(446ull<<44);
  scan.dispfb2=scan.dispfb1|(1ull<<43);
  if(!a->scanout(gpu,&scan)||scan.width!=640||scan.height!=448)throw std::runtime_error("literal dual circuit dimensions mismatch");
  if(!native&&(screen[0]!=0x33||screen[1]!=0x44||screen[2]!=0x55))throw std::runtime_error("literal dual circuit color mismatch");
  // Each readback must observe the new frame, not an earlier submission.
  for(unsigned frame=0;frame<32;frame++){
   uint32_t color=0x80000000u|((frame*7u)&255u)|(((frame*13u)&255u)<<8)|(((frame*19u)&255u)<<16);
   a->clear(gpu,0,10,0,0,639,447,color,0,0);
   scan.vsyncTick=frame;
   if(!a->scanout(gpu,&scan)||scan.width!=640||scan.height!=448)throw std::runtime_error("consecutive scanout failed");
   // Circuit 2 is only 447 lines tall; sample where both circuits exist.
   for(unsigned pixel:{0u,640u*223u+317u,640u*446u+639u}){
    if(native)continue;
    if(screen[pixel*4]!=(color&255u)||screen[pixel*4+1]!=((color>>8)&255u)||screen[pixel*4+2]!=((color>>16)&255u)){
     std::fprintf(stderr,"frame=%u pixel=%u RGB=%u,%u,%u expected=%08x\n",frame,pixel,screen[pixel*4],screen[pixel*4+1],screen[pixel*4+2],color);
     throw std::runtime_error("scanout returned stale frame pixels");
    }
   }
  }
  a->wait(gpu);a->destroy(gpu);dlclose(lib);
#if defined(__APPLE__)
  if(window)black_test_window_destroy(window);
#endif
  std::puts(native?"PASS: Metal texture export and GPU-resident CAMetalLayer scanout":"PASS: split/interleaved GIF paths, GPU sprite, clear, native upload, FIFO download, multi-page copy, CRT scanout");return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"FAIL: %s\n",e.what());a->destroy(gpu);
#if defined(__APPLE__)
  if(window)black_test_window_destroy(window);
#endif
  return 1;}
}
