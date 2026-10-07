#!/usr/bin/env python3
"""Compare full production XGKICK transfers with the prior full-buffer reset."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'tools/PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp').read_text()
h=(root/'tools/PS2Recomp/ps2xRuntime/include/runtime/ps2_vu1.h').read_text()
def method(name):
 a=s.index('void VU1Interpreter::'+name+'(');b=s.index('{',a);d=1;i=b+1
 while d:d+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]+'\n'
klass=h[h.index('class VU1Interpreter'):h.rindex('};')+2].replace('VU1Interpreter','ReferenceVU').replace('private:','public:')
program=r'''
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstring>
#include <vector>
#include <random>
#include <cstdio>
#define private public
#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#undef private
class GS {public:std::vector<std::vector<uint8_t>> packets;void processGIFPacket(const uint8_t*p,uint32_t n,unsigned){packets.emplace_back(p,p+n);}};
class PS2Memory {public:void submitGifPacket(GifPathId,const uint8_t*,uint32_t){assert(false);}};
CLASS
VU1Interpreter::VU1Interpreter(Unit u):m_unit(u){}
ReferenceVU::ReferenceVU(Unit u):m_unit(u){}
unsigned errorsA=0,errorsB=0;
void VU1Interpreter::reportReservedInstruction(bool,uint32_t){++errorsA;}
void ReferenceVU::reportReservedInstruction(bool,uint32_t){++errorsB;}
METHODS
REFERENCE
int main(){
 VU1Interpreter v;ReferenceVU r;GS gpu,referenceGpu;std::mt19937 rng(83);
 std::array<uint8_t,16384> data{};
 v.m_activeVuData=r.m_activeVuData=data.data();v.m_activeVuDataSize=r.m_activeVuDataSize=data.size();
 v.m_activeGs=&gpu;r.m_activeGs=&referenceGpu;
 v.m_xgkick.packet.fill(0xa5);r.m_xgkick.packet.fill(0x73);
 for(unsigned trial=0;trial<10000;++trial){
  std::vector<uint8_t> packet;unsigned tags=1+rng()%3;
  for(unsigned tag=0;tag<tags;++tag){
   unsigned fmt=rng()%3,nreg=1+rng()%4,loops=rng()%30;
   uint64_t low=loops|((uint64_t)fmt<<58)|((uint64_t)nreg<<60)|(tag+1==tags?32768u:0u);
   unsigned bytes=16+(fmt==0?loops*nreg*16:fmt==1?((loops*nreg+1)/2)*16:loops*16);
   unsigned offset=packet.size();packet.resize(offset+bytes);
   for(unsigned j=offset;j<packet.size();++j)packet[j]=rng();
   std::memcpy(packet.data()+offset,&low,8);
  }
  unsigned address=(rng()%1024)*16;
  for(unsigned j=0;j<packet.size();++j)data[(address+j)%data.size()]=packet[j];
  v.m_cycle=r.m_cycle=trial*10000;v.startXgkick(address/16);r.startXgkick(address/16);
  for(unsigned cycle=0;v.m_xgkick.active||r.m_xgkick.active;++cycle){
   assert(cycle<2000);v.progressXgkick();r.progressXgkick();
   assert(v.m_xgkick.active==r.m_xgkick.active);
   assert(v.m_xgkick.copiedBytes==r.m_xgkick.copiedBytes);
   assert(v.m_xgkick.currentTagEnd==r.m_xgkick.currentTagEnd);
   assert(v.m_xgkick.cycleCredit==r.m_xgkick.cycleCredit);
  }
  assert(gpu.packets.back()==packet);assert(gpu.packets==referenceGpu.packets);
  gpu.packets.clear();referenceGpu.packets.clear();assert(errorsA==0&&errorsB==0);
 }
 puts("PASS: 10000 reused XGKICK transfers match bytes/cycles, multi-tag formats and memory wrap");
}
'''.replace('CLASS',klass).replace('METHODS',''.join(method(n) for n in ['startXgkick','progressXgkick','finishXgkick'])).replace('REFERENCE',(root/'ps2recomp/tests/vu_xgkick_start_reference.inc').read_text()+''.join(method(n).replace('VU1Interpreter::','ReferenceVU::') for n in ['progressXgkick','finishXgkick']))
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.cpp').write_text(program)
 subprocess.run(['clang++','-std=c++20','-O2','-fsanitize=address,undefined','-I'+str(root/'tools/PS2Recomp/ps2xRuntime/include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
