#!/usr/bin/env python3
"""Compare AOT LQ/SQ/ILW/ISW bodies and queued payloads, without retiring store pipelines."""
from pathlib import Path
import argparse,json,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('header');a=p.parse_args();header=Path(a.header).resolve();root=Path(__file__).resolve().parents[2];base=root/'tools/PS2Recomp/ps2xRuntime'
meta=json.loads(Path(str(header)+'.json').read_text());rows=[r for r in meta['lower_supported'] if r['kind'] in ('LQ','SQ','ILW','ISW','LQI','SQI','LQD','SQD','ILWR','ISWR')]
source=(base/'src/lib/vu/ps2_vu1_lower.cpp').read_text()
reference='void referenceLower(VU1Interpreter& host,uint32_t instr,uint8_t* vuData,uint32_t dataSize){auto& m_state=host.m_state;auto applyDest=[&](float* d,const float* r,uint8_t m){host.applyDest(d,r,m);};auto queueStore=[&](uint32_t a,const uint32_t* w,uint8_t m){host.queueStore(a,w,m);};switch(instr>>25&127){'
for code in (0,1,4,5):
 start=source.index(f'case 0x{code:02X}: // ');start=source.index('\n',start)+1;end=source.index('return;',start)+7
 reference+=f'case {code}: '+source[start:end]+'\n}'
reference+='case 64:{unsigned vfS=(instr>>11)&31,vfT=(instr>>16)&31,viS=vfS&15,viT=vfT&15,dest=(instr>>21)&15;switch((instr&3)|((instr>>4)&124)){'
for code,name in ((52,'LQI'),(53,'SQI'),(54,'LQD'),(55,'SQD'),(62,'ILWR'),(63,'ISWR')):
 start=source.index(f'case 0x{code:02X}: // {name}');start=source.index('\n',start)+1;end=source.index('return;',start)+7
 reference+=f'case {code}: '+source[start:end]+'\n}'
reference+='default:std::abort();}}default:std::abort();}}' 
program='''#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <random>
#define private public
#include "runtime/ps2_vu1.h"
#undef private
#include "ps2_vu1_detail.h"
struct Store{uint32_t address,words[4];uint8_t mask;};Store captured{};unsigned calls=0;
VU1Interpreter::VU1Interpreter(Unit u):m_unit(u){}
void VU1Interpreter::queueFcset(uint32_t){std::abort();}
void VU1Interpreter::queueFsset(uint16_t){std::abort();}
void VU1Interpreter::queueQ(float,uint32_t,uint32_t){std::abort();}
void VU1Interpreter::startXgkick(uint32_t){std::abort();}
float VU1Interpreter::normalizeResult(float,uint32_t&) const{std::abort();}
uint32_t VU1Interpreter::microAddressMask() const{return m_unit==Unit::VU1?0x3fff:0xfff;}
int32_t VU1Interpreter::readBranchVi(uint8_t r) const{return r==0?0:m_viBranchBackupValid && m_viBranchBackupReg==r?m_viBranchBackupValue:m_state.vi[r];}
void VU1Interpreter::applyDest(float* d,const float* r,uint8_t mask){for(unsigned c=0;c<4;c++)if(mask&(8>>c))d[c]=r[c];}
void VU1Interpreter::queueStore(uint32_t a,const uint32_t* w,uint8_t m){++calls;captured.address=a;std::memcpy(captured.words,w,16);captured.mask=m;}
float VU1Interpreter::normalizeOperand(float v) const{return v;}
void VU1Interpreter::applyFmacDest(float*,float*,uint8_t){std::abort();}
void VU1Interpreter::applyFmacDestAcc(float*,uint8_t){std::abort();}
void VU1Interpreter::queueClip(uint32_t){std::abort();}
'''+reference+'\n#include "'+str(header)+'"\n'+r'''
int main(){VU1Interpreter actual,ref;std::mt19937 rng(105);uint8_t memory[16384],before[16384];
struct Case{uint32_t pc,lower,upper;};Case cases[]={ROWS};uint32_t sizes[]={8,16,4096,16384};
for(auto c:cases)for(unsigned n=0;n<128;n++){
for(auto& vi:actual.m_state.vi)vi=static_cast<int16_t>(rng());
if(n<4)for(auto& vi:actual.m_state.vi)vi=n==0?0:n==1?-1:n==2?32767:-32768;
for(auto& vf:actual.m_state.vf)for(float& v:vf){uint32_t bits=rng();std::memcpy(&v,&bits,4);}
for(auto& b:memory)b=rng();std::memcpy(before,memory,sizeof(memory));ref.m_state=actual.m_state;uint32_t size=sizes[n%4];
calls=0;captured={};referenceLower(ref,c.lower,memory,size);unsigned expectedCalls=calls;Store expected=captured;
calls=0;captured={};assert(BlackVuUpperAot::executeLower(actual,c.pc,memory,size));assert(calls==expectedCalls);
if(calls){assert(captured.address==expected.address);assert(captured.mask==expected.mask);assert(!std::memcmp(captured.words,expected.words,16));}
assert(!std::memcmp(&actual.m_state,&ref.m_state,sizeof(actual.m_state)));assert(!std::memcmp(before,memory,sizeof(memory)));
auto state=actual.m_state;calls=0;assert(!BlackVuUpperAot::executeLower(actual,c.pc));assert(!BlackVuUpperAot::executeLower(actual,c.pc,memory,0));assert(!calls);
struct Pair{uint32_t upper,lower;bool iBit;uint8_t upperVfShadowReg;};Pair pair{c.upper,c.lower,false,0};auto priorUpper=actual.m_currentUpperInstruction;
assert(!BlackVuUpperAot::executePairBodies(actual,c.pc,pair));assert(!BlackVuUpperAot::executePairBodies(actual,c.pc,pair,memory,0));assert(!calls);assert(priorUpper==actual.m_currentUpperInstruction);
assert(!std::memcmp(&state,&actual.m_state,sizeof(state)));
}
printf("PASS: %zu memory sites x128 inputs, 4 memory sizes; state/address/store payload/mask match production; retirement not covered\n",sizeof(cases)/sizeof(cases[0]));}
'''.replace('ROWS',','.join('{'+str(r['pc'])+','+str(r['lower'])+'u,'+str(r['upper'])+'u}' for r in rows))
with tempfile.TemporaryDirectory() as td:
 path=Path(td);(path/'test.cpp').write_text(program);subprocess.run(['clang++','-std=c++20','-O2','-fsanitize=address,undefined','-I'+str(base/'include'),'-I'+str(base/'src/lib/vu'),str(path/'test.cpp'),'-o',str(path/'test')],check=True);subprocess.run([str(path/'test')],check=True)
