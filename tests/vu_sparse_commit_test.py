#!/usr/bin/env python3
"""Compare production sparse commits with the prior full-scan implementation."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'tools/PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp').read_text()
h=(root/'tools/PS2Recomp/ps2xRuntime/include/runtime/ps2_vu1.h').read_text()
def method(name, result="void"):
 a=s.index(result+' VU1Interpreter::'+name+'(');b=s.index('{',a);depth=1;i=b+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]+'\n'
classdef=h[h.index('class VU1Interpreter'):h.rindex('};')+2].replace('VU1Interpreter','ReferenceVU').replace('private:','public:')
methods=''.join(method(n) for n in ['queueVfWrite','queueViWrite','queueAccWrite','queueStore','queueFsset','queueClip','queueFcset','queueQ','queueP','updateFmacFlags','resetScheduler'])
a=s.index('float VU1Interpreter::normalizeResult(');b=s.index('\nuint32_t VU1Interpreter::microAddressMask',a)
methods+=s[a:b]+'\n'
scalar_reference=s[a:b].replace('VU1Interpreter::','ReferenceVU::')
scalar_reference+=''.join(method(n).replace('VU1Interpreter::','ReferenceVU::') for n in ['queueQ','queueP'])
methods+=method('pipelinesPending','bool')
scalar_reference+=(root/'ps2recomp/tests/vu_pending_reference.inc').read_text()
commit=method('commitReadyPipelines')
reference=(root/'ps2recomp/tests/vu_commit_reference.inc').read_text()
head='''#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <random>
#define private public
#include "runtime/ps2_vu1.h"
#undef private
constexpr uint8_t laneForComponent(uint32_t c){return 8u >> c;}
'''
constructors='''
unsigned errorsA=0,errorsB=0;
VU1Interpreter::VU1Interpreter(Unit u):m_unit(u){std::memset(&m_state,0,sizeof(m_state));}
ReferenceVU::ReferenceVU(Unit u):m_unit(u){std::memset(&m_state,0,sizeof(m_state));}
void VU1Interpreter::reportReservedInstruction(bool,uint32_t){errorsA++;}
void ReferenceVU::reportReservedInstruction(bool,uint32_t){errorsB++;}
'''
tail=r'''
int main(){
 VU1Interpreter unit;ReferenceVU reference;std::mt19937 rng(42);
 std::array<uint8_t,2048> memory{},referenceMemory{};
 unit.m_activeVuData=memory.data();unit.m_activeVuDataSize=memory.size();
 reference.m_activeVuData=referenceMemory.data();reference.m_activeVuDataSize=referenceMemory.size();
 for(unsigned n=0;n<150000;n++){
  unsigned op=rng()%11,reg=rng()%32,mask=rng()%16,latency=rng()%9;
  float v[4];for(auto& x:v)x=float(int(rng()%2000)-1000)/16;
  if(op==0){unit.queueVfWrite(reg,mask,v,latency);reference.queueVfWrite(reg,mask,v,latency);}
  if(op==1){unit.queueViWrite(reg%16,int(v[0]),latency);reference.queueViWrite(reg%16,int(v[0]),latency);}
  if(op==2){unit.queueAccWrite(mask,v,latency);reference.queueAccWrite(mask,v,latency);}
  if(op==3){unit.m_cycle+=rng()%4;reference.m_cycle=unit.m_cycle;unit.commitReadyPipelines();reference.commitReadyPipelines();reference.commitReadyPipelines();}
  if(op==4){uint32_t words[4];for(auto& w:words)w=rng();unsigned address=(rng()%128)*16;unit.queueStore(address,words,mask);reference.queueStore(address,words,mask);}
  if(op==5){unsigned value=rng();unit.queueFsset(value);reference.queueFsset(value);}
  if(op==6){unsigned value=rng();unit.queueClip(value);reference.queueClip(value);}
  if(op==7){unsigned value=rng();unit.queueFcset(value);reference.queueFcset(value);}
  if(op==8){uint8_t flags[4];for(auto& f:flags)f=rng()%16;unsigned sticky=rng()%16;unit.updateFmacFlags(flags,mask,sticky);reference.updateFmacFlags(flags,mask,sticky);}
  if(op==9){unsigned status=rng()%64;unit.queueQ(v[0],latency,status);reference.queueQ(v[0],latency,status);}
  if(op==10){unit.queueP(v[0],latency);reference.queueP(v[0],latency);}
  assert(unit.m_fdiv.valid==reference.m_fdiv.valid);
  for(unsigned j=0;j<unit.m_efu.size();++j)assert(unit.m_efu[j].valid==reference.m_efu[j].valid);
  assert(unit.pipelinesPending()==reference.pipelinesPending());
  assert(memory==referenceMemory);
  assert(std::memcmp(&unit.m_state,&reference.m_state,sizeof(unit.m_state))==0);
  assert(errorsA==errorsB);
  for(unsigned j=0;j<unit.m_vfWritePipeline.size();j++)assert(unit.m_vfWritePipeline[j].valid==reference.m_vfWritePipeline[j].valid);
  for(unsigned j=0;j<unit.m_viWritePipeline.size();j++)assert(unit.m_viWritePipeline[j].valid==reference.m_viWritePipeline[j].valid);
  for(unsigned j=0;j<unit.m_accWritePipeline.size();j++)assert(unit.m_accWritePipeline[j].valid==reference.m_accWritePipeline[j].valid);
  unsigned flagPending=0,storePending=0;
  for(unsigned j=0;j<unit.m_flagPipeline.size();j++){
   assert(unit.m_flagPipeline[j].valid==reference.m_flagPipeline[j].valid);
   if(unit.m_flagPipeline[j].valid)flagPending|=1u<<j;
  }
  for(unsigned j=0;j<unit.m_storePipeline.size();j++){
   assert(unit.m_storePipeline[j].valid==reference.m_storePipeline[j].valid);
   if(unit.m_storePipeline[j].valid)storePending|=1u<<j;
  }
  assert(unit.m_flagPending==flagPending&&unit.m_storePending==storePending);
 }
 unit.m_cycle+=100;reference.m_cycle=unit.m_cycle;unit.commitReadyPipelines();reference.commitReadyPipelines();
 assert(!unit.m_vfPending&&!unit.m_viPending&&!unit.m_accPending);
 assert(!unit.m_flagPending&&!unit.m_storePending);
 assert(std::memcmp(&unit.m_state,&reference.m_state,sizeof(unit.m_state))==0);
 unit.queueViWrite(1,7,100);unit.queueVfWrite(1,15,unit.m_state.vf[2],100);unit.queueAccWrite(15,unit.m_state.vf[3],100);
 unit.queueFsset(0xfc0);uint32_t words[4]={1,2,3,4};unit.queueStore(0,words,15);
 unit.resetScheduler();assert(!unit.m_vfPending&&!unit.m_viPending&&!unit.m_accPending&&!unit.m_flagPending&&!unit.m_storePending);
 // Sparse, not-yet-ready entry: captures the full-scan cost without changing semantics.
 auto bench=[](auto& v){float value[4]={1,2,3,4};v.queueVfWrite(1,15,value,100000000);auto a=std::chrono::steady_clock::now();for(unsigned i=0;i<3000000;i++){v.m_cycle++;v.commitReadyPipelines();}printf("%.2f ms\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-a).count());};
 bench(unit);bench(reference);
 puts("PASS: 150000 randomized queue/commit operations preserve Q/P, registers, masked lanes, WAW sequence, capacity failures and pending entries");
}
'''
with tempfile.TemporaryDirectory() as directory:
 d=Path(directory);cpp=d/'test.cpp';exe=d/'test'
 cpp.write_text(head+classdef+constructors+methods+scalar_reference+(root/'ps2recomp/tests/vu_queue_reference.inc').read_text()+commit+reference+tail)
 subprocess.run(['clang++','-std=c++20','-O3','-I'+str(root/'tools/PS2Recomp/ps2xRuntime/include'),str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
