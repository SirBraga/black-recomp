#include "runtime/gs/gs_cpu_backend.h"
#include <chrono>
#include <cfenv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>
int main(int argc,char**argv) {
 if(const char* mode=std::getenv("GS_TEST_ROUND")) std::fesetround(std::atoi(mode)==1?FE_TOWARDZERO:std::atoi(mode)==2?FE_DOWNWARD:std::atoi(mode)==3?FE_UPWARD:FE_TONEAREST);
 std::vector<uint8_t> ram(4*1024*1024); GSCpuBackend gs; gs.Initialize(ram.data(),ram.size());
 for(unsigned i=0;i<ram.size();++i) ram[i]=(i*13+(i>>8))&255;
 GSPrimitiveBatch b; b.vertexCount=3; b.state.prim.type=GS_PRIM_TRIANGLE; b.state.prim.tme=true; b.state.prim.fst=true; b.state.prim.iip=true;
 auto& c=b.state.context; c.frame.fbw=10; c.scissor={0,639,0,447}; c.zbuf.zbp=150; c.zbuf.psm=48; c.test=1ull<<17; c.tex0.tbp0=12000; c.tex0.tbw=2; c.tex0.tcc=1; b.state.textureWidth=128; b.state.textureHeight=128;
 b.vertices[0].x=0; b.vertices[0].y=0; b.vertices[1].x=640; b.vertices[1].y=30; b.vertices[2].x=30; b.vertices[2].y=448;
 for(int i=0;i<3;++i){auto&v=b.vertices[i];v.r=70+i*30;v.g=128;v.b=120;v.a=128;v.z=100+i;v.u=i==1?2047:0;v.v=i==2?2047:0;}
 std::ofstream output; if(argc>1) output.open(argv[1],std::ios::binary);
 auto snapshot=[&]{if(output) output.write((char*)ram.data(),ram.size());};
 auto start=std::chrono::steady_clock::now();
 for(int i=0;i<60;++i) gs.Submit(b);
 std::cerr<<"benchmark_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"\n";
 snapshot();
 // Feedback, depth/frame aliasing, 16-bit targets, and region wrap must preserve serial ordering.
 for(int i=0;i<12;++i){ c.frame.psm=i==4?2:0; c.zbuf.zbp=i==5?0:150; c.tex0.tbp0=i==6?16370:(i%2?0:12000); c.clamp=i==7?(3ull|(3ull<<2)|(1023ull<<4)|(1023ull<<24)):0; c.frame.fbmsk=i==8?0xff00:0; b.state.linearFilter=i==9; b.state.prim.abe=i==10; c.alpha=0x44; gs.Submit(b); snapshot(); }

 c.frame.psm=0; c.frame.fbmsk=0; c.zbuf.zbp=150; c.clamp=0; b.state.prim.abe=false;
 for(unsigned psm : {0u,1u,2u,10u,19u,20u,27u,36u,44u,48u,49u,50u,58u}) {
  c.tex0.psm=psm; c.tex0.tbp0=12001; c.tex0.cbp=11000; c.tex0.cpsm=0;
  gs.LoadClut(c.tex0,b.state.texclut); gs.Submit(b); snapshot();
 }

 c.tex0.psm=0; c.tex0.tbp0=12000;
 for(unsigned framePsm : {0u,1u,2u,10u}) for(unsigned depthPsm : {48u,49u,50u,58u}) {
  c.frame.psm=framePsm; c.zbuf.psm=depthPsm; c.test=1ull<<17;
  c.frame.fbmsk=0x1040; b.state.prim.abe=true;
  gs.Submit(b); snapshot();
 }

 // Mipmapped draws used to force serial rows. None of these bases overlap the
 // frame or Z, so workers must produce the same VRAM as the serial path.
 c.frame.psm=0; c.frame.fbmsk=0; c.zbuf.zbp=150; c.zbuf.psm=48; b.state.prim.abe=false;
 c.tex0.psm=20; c.tex0.tbp0=12000; c.tex0.tbw=2; c.tex0.cbp=11000; c.tex0.cpsm=0; c.tex0.cld=1;
 c.tex1=(3u<<2)|(1u<<5)|(4u<<6);
 c.miptbp1=12000ull|(2ull<<14);
 gs.LoadClut(c.tex0,b.state.texclut);
 gs.Submit(b); snapshot();
}
