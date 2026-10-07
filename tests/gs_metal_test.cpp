#include "runtime/gs/gs_cpu_backend.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <span>
#include <iostream>
#include <chrono>
int main() {
 setenv("PS2X_GS_METAL_MIN_PIXELS","0",1);
 constexpr unsigned size=4*1024*1024;
 auto alloc=[](){auto p=static_cast<uint8_t*>(std::aligned_alloc(16384,size));assert(p);return std::unique_ptr<uint8_t,decltype(&std::free)>(p,&std::free);};
 auto a=alloc(),b=alloc();for(unsigned i=0;i<size;++i)a.get()[i]=uint8_t(i*17+(i>>8));std::memcpy(b.get(),a.get(),size);
 setenv("PS2X_GS_METAL","0",1);GSCpuBackend cpu;cpu.Initialize(a.get(),size);GSPrimitiveBatch prime{};prime.vertexCount=1;cpu.Submit(prime);
 setenv("PS2X_GS_METAL","1",1);GSCpuBackend gpu;gpu.Initialize(b.get(),size);
 GSPrimitiveBatch draw{};draw.vertexCount=3;auto& s=draw.state;auto& c=s.context;
 s.prim.type=GS_PRIM_TRIANGLE;s.prim.tme=true;s.prim.fst=true;s.prim.iip=true;s.colclamp=1;
 c.frame.fbw=2;c.scissor={0,127,0,127};c.zbuf.zbp=100;c.zbuf.psm=48;c.test=1ull<<17;c.tex0.tbp0=12001;c.tex0.tbw=2;c.tex0.tcc=1;c.tex0.cbp=11000;c.tex0.cld=1;s.textureWidth=128;s.textureHeight=128;
 draw.vertices[0].x=0;draw.vertices[0].y=0;draw.vertices[1].x=128;draw.vertices[1].y=0;draw.vertices[2].x=0;draw.vertices[2].y=128;
 for(unsigned j=0;j<3;++j){auto& v=draw.vertices[j];v.r=128;v.g=128;v.b=128;v.a=128;v.z=100;v.u=j==1?2048:0;v.v=j==2?2048:0;v.s=j==1?1:0;v.t=j==2?1:0;v.q=1;v.fog=128;}
 unsigned count=0;
 auto check=[&](){cpu.LoadClut(c.tex0,s.texclut);gpu.LoadClut(c.tex0,s.texclut);cpu.Submit(draw);gpu.Submit(draw);gpu.Sync(GSSyncReason::DebugReadback);++count;
  if(std::memcmp(a.get(),b.get(),size)){unsigned n=0;for(unsigned i=0;i<size;++i)if(a.get()[i]!=b.get()[i]){if(n++<5)std::cerr<<"byte "<<i<<" cpu="<<unsigned(a.get()[i])<<" gpu="<<unsigned(b.get()[i])<<"\n";}std::cerr<<"case="<<count<<" mismatches="<<n<<" psm="<<unsigned(c.tex0.psm)<<"\n";std::exit(1);}};
 for(unsigned format:{0u,1u,2u,10u,19u,20u,27u,36u,44u}){c.tex0.psm=format;for(bool filter:{false,true}){s.linearFilter=filter;check();}}
 c.tex0.psm=0;s.linearFilter=false;
 for(unsigned tfx=0;tfx<4;++tfx){c.tex0.tfx=tfx;check();}
 for(unsigned mode=0;mode<4;++mode){c.clamp=mode|(uint64_t(mode)<<2)|(31ull<<4)|(63ull<<14)|(31ull<<24)|(63ull<<34);check();}
 c.clamp=0;
 for(unsigned atst=0;atst<8;++atst)for(unsigned afail=0;afail<4;++afail){c.test=1|(uint64_t(atst)<<1)|(128ull<<4)|(uint64_t(afail)<<12)|(1ull<<17);check();}
 c.test=1ull<<17;
 for(unsigned clamp=0;clamp<2;++clamp)for(unsigned mode:{0x28u,0xA2u,0x44u}){s.colclamp=clamp;s.prim.abe=true;c.alpha=(128ull<<32)|mode;check();}
 s.prim.abe=false;s.colclamp=1;
 for(unsigned frame:{0u,1u})for(unsigned depth:{48u,49u}){c.frame.psm=frame;c.zbuf.psm=depth;c.frame.fbmsk=0x00102040;c.fba=1;check();}
 c.frame.psm=0;c.zbuf.psm=48;c.frame.fbmsk=0;c.fba=0;
 s.prim.fge=true;s.fogR=170;s.fogG=64;s.fogB=20;check();s.prim.fge=false;
 s.prim.fst=false;check();s.prim.fst=true;
 // A texture overlapping the framebuffer must execute through the compatibility path,
 // after all outstanding GPU writes have become visible.
 c.tex0.tbp0=0;check();
 // Ordered batches: the GPU must preserve overlapping draws without a CPU wait per primitive.
 c.tex0.tbp0=12001;c.tex0.psm=0;c.tex0.tfx=0;c.frame.fbw=8;c.scissor={0,511,0,255};
 draw.vertices[1].x=512;draw.vertices[2].y=256;
 auto start=std::chrono::steady_clock::now();for(unsigned i=0;i<100;++i)cpu.Submit(draw);
 auto cpuMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 start=std::chrono::steady_clock::now();for(unsigned i=0;i<100;++i)gpu.Submit(draw);gpu.Sync(GSSyncReason::DebugReadback);
 auto gpuMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 assert(std::memcmp(a.get(),b.get(),size)==0);
 std::cout<<"ordered synthetic raster CPU="<<cpuMs<<"ms GPU="<<gpuMs<<"ms (not game FPS)\n";
 assert(gpu.MetalDrawCount()>=count-1);
 std::cout<<"PASS: "<<count<<" complete VRAM comparisons, "<<gpu.MetalDrawCount()<<" real Metal dispatches, formats/filter/CLUT/wrap/alpha/blend/depth/fog/feedback\n";
}
