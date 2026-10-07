#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <iostream>
#include <span>
#include <cstdlib>
int main() {
 void* allocation=std::aligned_alloc(16384,4*1024*1024); assert(allocation);
 std::unique_ptr<uint8_t,decltype(&std::free)> owned(static_cast<uint8_t*>(allocation),&std::free);
 std::span<uint8_t> ram(owned.get(),4*1024*1024); std::memset(ram.data(),0,ram.size()); GSCpuBackend gs; gs.Initialize(ram.data(),ram.size());
 auto pixel=[&](unsigned bp,unsigned x,unsigned y)->uint32_t& {return *reinterpret_cast<uint32_t*>(ram.data()+GSPSMCT32::addrPSMCT32(bp,1,x,y));};
 for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) pixel(4096,x,y)=0x80000000u|(y<<8)|x;
 for(bool fst : {true,false}) for(unsigned flip=0;flip<4;++flip) {
  GSPrimitiveBatch b{}; b.vertexCount=2; b.state.prim.type=GS_PRIM_SPRITE; b.state.prim.tme=true; b.state.prim.fst=fst;
  auto& c=b.state.context; c.frame.fbw=1; c.zbuf.zmask=true; c.zbuf.psm=48; c.zbuf.zbp=100; c.test=1ull<<17; c.scissor={0,63,0,31}; c.tex0.tbp0=4096; c.tex0.tbw=1; c.tex0.tfx=1; c.tex0.tcc=1;
  b.state.textureWidth=8; b.state.textureHeight=8;
  for(unsigned i=0;i<2;++i) { auto& v=b.vertices[i]; unsigned x=((flip&1)?1-i:i)*8,y=((flip&2)?1-i:i)*8;
   v.x=x; v.y=y; v.u=x*16; v.v=y*16; v.s=x/8.f; v.t=y/8.f; v.q=1; v.a=128;
  }
  std::memset(ram.data(),0,8192); gs.Submit(b); gs.Sync(GSSyncReason::DebugReadback);
  for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
   auto actual=pixel(0,x,y),expected=pixel(4096,x,y);
   if(actual!=expected) { std::cerr<<"fst="<<fst<<" flip="<<flip<<" pixel="<<x<<","<<y<<" actual="<<actual<<" expected="<<expected<<"\n"; return 1; }
  }
 }
 {
  GSPrimitiveBatch b{}; b.vertexCount=2;b.state.prim.type=GS_PRIM_SPRITE;b.state.prim.tme=true;
  auto& c=b.state.context;c.frame.fbw=1;c.zbuf.zmask=true;c.zbuf.psm=48;c.zbuf.zbp=100;c.test=1ull<<17;c.scissor={0,63,0,31};c.tex0.tbp0=4096;c.tex0.tbw=1;c.tex0.tfx=1;c.tex0.tcc=1;b.state.textureWidth=8;b.state.textureHeight=8;
  b.vertices[0].s=0.25f;b.vertices[0].t=0.25f;b.vertices[0].q=2;
  b.vertices[1].x=8;b.vertices[1].y=8;b.vertices[1].s=1;b.vertices[1].t=1;b.vertices[1].q=1;
  gs.Submit(b);gs.Sync(GSSyncReason::DebugReadback);
  for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){unsigned u=unsigned(2.f+6.f*(x+0.5f)/8),v=unsigned(2.f+6.f*(y+0.5f)/8);if(pixel(0,x,y)!=pixel(4096,u,v)){std::cerr<<"Sprite must use second vertex Q for both endpoints\n";return 1;}}
 }
 for(unsigned negative=0;negative<2;++negative) for(unsigned clamp=0;clamp<2;++clamp) {
  GSPrimitiveBatch b{}; b.vertexCount=2; b.state.prim.type=GS_PRIM_SPRITE; b.state.prim.abe=true;
  auto& c=b.state.context; c.frame.fbw=1; c.zbuf.zmask=true; c.zbuf.psm=48; c.zbuf.zbp=100; c.test=1ull<<17; c.scissor={0,63,0,31};
  b.state.colclamp=clamp; c.alpha=(128ull<<32)|(negative ? 0xA2u : 0x28u);
  b.vertices[1].x=1; b.vertices[1].y=1; b.vertices[1].r=200; b.vertices[1].a=128;
  pixel(0,0,0)=0; gs.Submit(b); gs.Sync(GSSyncReason::DebugReadback);
  unsigned expected=negative?(clamp?0:56):(clamp?255:144);
  if((pixel(0,0,0)&255)!=expected) {std::cerr<<"COLCLAMP="<<clamp<<" negative="<<negative<<" actual="<<(pixel(0,0,0)&255)<<" expected="<<expected<<"\n";return 1;}
 }
 std::cout<<"PASS: textured sprites preserve UV endpoints for normal, reversed X/Y and ST coordinates\n";
}
