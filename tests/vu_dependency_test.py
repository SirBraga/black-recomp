#!/usr/bin/env python3
"""Compare production VU dependency traversal to exhaustive register traversal."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'tools/PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp').read_text()
a=s.index('uint64_t VU1Interpreter::calculatePairReadyCycle(');b=s.index('\nvoid VU1Interpreter::markPairWrites(',a)
ready=s[a:b];a=b+1;b=s.index('\nVU1Interpreter::InstructionUsage',a);writes=s[a:b]
old_ready=ready.replace('VU1Interpreter::','Reference::')
a=old_ready.index('        // Most pairs');b=old_ready.index('        for (uint32_t component',a)
old_ready=old_ready[:a]+'''        for (uint32_t reg=1;reg<m_viReady.size();++reg)
            if (usage->viRead & (1u<<reg)) ready=std::max(ready,m_viReady[reg]);
'''+old_ready[b:]
old_writes=writes.replace('VU1Interpreter::','Reference::')
a=old_writes.index('    uint32_t viWrites');b=old_writes.index('    for (uint32_t component',a)
old_writes=old_writes[:a]+'''    for (uint32_t reg=1;reg<m_viReady.size();++reg)
        if (decoded.lowerUsage.viWrite & (1u<<reg))
            m_viReady[reg]=m_cycle+(decoded.lowerUsage.viLatency?decoded.lowerUsage.viLatency:decoded.lowerUsage.latency);
'''+old_writes[b:]
head=r'''
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
constexpr uint8_t laneForComponent(uint32_t c){return 1u<<(3u-c);}
struct VU1Interpreter {
 enum {PipelineFdiv=1,PipelineEfu,PipelineXgkick};
 struct VfAccess{uint8_t reg=0,lanes=0;};
 struct InstructionUsage {std::array<VfAccess,4> vfRead{};uint32_t vfReadCount=0;uint16_t viRead=0,viWrite=0;uint8_t accRead=0,accWrite=0;VfAccess vfWrite{};uint32_t pipeline=0,latency=0,vfLatency=0,viLatency=0;bool waitQ=false,waitP=false;};
 struct DecodedInstructionPair {InstructionUsage upperUsage,lowerUsage;uint8_t suppressedLowerVf=0;};
 struct Scalar{uint64_t readyCycle=0;bool valid=false;}; using ScalarPipelineEntry=Scalar;
 static constexpr uint32_t kAccForwardLatency=1;
 uint64_t m_cycle=3,m_efuResourceReady=0;
 std::array<std::array<uint64_t,4>,32> m_vfReady{};
 std::array<uint64_t,16> m_viReady{};std::array<uint64_t,4> m_accReady{};
 Scalar m_fdiv{};std::array<Scalar,2> m_efu{};struct{bool active=false;}m_xgkick;
 uint64_t calculatePairReadyCycle(const DecodedInstructionPair&) const;
 void markPairWrites(const DecodedInstructionPair&);
};
struct Reference:VU1Interpreter {
 uint64_t calculatePairReadyCycle(const DecodedInstructionPair&) const;
 void markPairWrites(const DecodedInstructionPair&);
};
'''
tail=r'''
int main(){
 VU1Interpreter v;Reference reference;
 for(unsigned mask=0;mask<65536;++mask){
  for(unsigned reg=0;reg<16;++reg)v.m_viReady[reg]=(reg*313+mask*177)%8192;
  for(unsigned reg=0;reg<32;++reg)for(unsigned c=0;c<4;++c)v.m_vfReady[reg][c]=(reg*107+c*71+mask)%2048;
  for(unsigned c=0;c<4;++c)v.m_accReady[c]=(mask+c*57)%4096;
  v.m_cycle=mask%1024;v.m_fdiv={mask%7300,(mask&1)!=0};v.m_efu[0]={mask%7000,(mask&2)!=0};v.m_efu[1]={mask%9000,(mask&4)!=0};v.m_efuResourceReady=mask%5000;v.m_xgkick.active=mask&8;
  VU1Interpreter::DecodedInstructionPair p;
  p.upperUsage.viRead=mask;p.lowerUsage.viRead=mask^0x8123;
  p.lowerUsage.viWrite=mask;p.lowerUsage.viLatency=mask%8;p.lowerUsage.latency=4;
  p.lowerUsage.pipeline=mask%5;p.lowerUsage.waitQ=mask&16;p.lowerUsage.waitP=mask&32;
  p.upperUsage.accRead=mask%16;p.upperUsage.accWrite=(mask>>4)%16;
  p.upperUsage.vfReadCount=1;p.upperUsage.vfRead[0]={(uint8_t)(mask%32),(uint8_t)(mask%16)};
  p.lowerUsage.vfWrite={(uint8_t)(mask%32),(uint8_t)(mask%16)};p.suppressedLowerVf=(mask&1)?mask%32:0;
  static_cast<VU1Interpreter&>(reference)=v;
  assert(v.calculatePairReadyCycle(p)==reference.calculatePairReadyCycle(p));
  v.markPairWrites(p);reference.markPairWrites(p);
  assert(v.m_viReady==reference.m_viReady && v.m_vfReady==reference.m_vfReady && v.m_accReady==reference.m_accReady);
 }
 // Benchmark sparse VI dependencies typical of arithmetic pairs; identical workloads.
 auto benchmark=[](auto &unit){uint64_t sum=0;VU1Interpreter::DecodedInstructionPair p;auto start=std::chrono::steady_clock::now();for(unsigned i=0;i<3000000;++i){p.lowerUsage.viRead=(i%3==0)?(1u<<(i%15+1)):0;p.upperUsage.viRead=0;unit.m_cycle=i%1000;sum+=unit.calculatePairReadyCycle(p);}auto end=std::chrono::steady_clock::now();printf("checksum=%llu time=%.2fms\n",(unsigned long long)sum,std::chrono::duration<double,std::milli>(end-start).count());return sum;};
 auto x=benchmark(v);auto y=benchmark(reference);assert(x==y);
}
'''
with tempfile.TemporaryDirectory() as directory:
 d=Path(directory);(d/'test.cpp').write_text(head+ready+writes+old_ready+old_writes+tail)
 subprocess.run(['clang++','-std=c++20','-O3',str(d/'test.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
print('PASS: all 65536 VI masks preserve readiness/write state, vector lanes and other pipelines')
