#!/usr/bin/env python3
"""Compare emitted upper arithmetic/helper inputs with production execUpper."""
from pathlib import Path
import subprocess,tempfile,json,argparse
p=argparse.ArgumentParser();p.add_argument('header');a=p.parse_args();root=Path(__file__).resolve().parents[2];base=root/'tools/PS2Recomp/ps2xRuntime';header=Path(a.header).resolve();metadata=json.loads(Path(str(header)+'.json').read_text())
upper=(base/'src/lib/vu/ps2_vu1_upper.cpp').read_text();core=(base/'src/lib/vu/ps2_vu1_core.cpp').read_text();start=core.index('float VU1Interpreter::normalizeOperand(');end=core.index('\nfloat VU1Interpreter::normalizeResult(',start)
support=r'''
uint32_t capturedClip=0;unsigned calls=0;bool capturedFmac=false;
float captured[4];uint8_t capturedDest=0;
VU1Interpreter::VU1Interpreter(Unit u):m_unit(u){}
float VU1Interpreter::broadcast(const float* v,uint8_t c){return normalizeOperand(v[c&3]);}
void VU1Interpreter::applyDest(float* dst,const float* r,uint8_t d){++calls;capturedFmac=false;std::memcpy(captured,r,16);capturedDest=d;for(unsigned c=0;c<4;c++)if(d&(8>>c))dst[c]=r[c];}
void VU1Interpreter::applyFmacDest(float* dst,float* r,uint8_t d){++calls;capturedFmac=true;std::memcpy(captured,r,16);capturedDest=d;for(unsigned c=0;c<4;c++)if(d&(8>>c))dst[c]=r[c];}
void VU1Interpreter::applyFmacDestAcc(float* r,uint8_t d){applyFmacDest(m_state.acc,r,d);}
void VU1Interpreter::queueClip(uint32_t v){++calls;capturedClip=v;}
void VU1Interpreter::reportReservedInstruction(bool,uint32_t){std::abort();}
'''
program='''#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <random>
#include <cfenv>
#define private public
#include "runtime/ps2_vu1.h"
#undef private
'''+upper+core[start:end]+support+'\n#include "'+str(header)+'"\n'
rows=','.join('{'+str(row['pc'])+','+str(row['upper'])+'u}' for row in metadata['supported'])
program+=r'''
int main(){
 std::fesetround(FE_TOWARDZERO);VU1Interpreter actual,reference;std::mt19937 rng(76);
 uint32_t edges[]={0,0x80000000u,1,0x80000001u,0x7fffffu,0x807fffffu,0x800000u,0x80800000u,0x7f7fffffu,0xff7fffffu,0x7f800000u,0xff800000u,0x7fc00001u,0xffc00001u,0x4f000000u,0x4effffffu,0xcf000000u,0xceffffffu,0x3f800000u};
 struct Case{uint32_t pc,upper;};Case cases[]={ROWS};
 for(auto test:cases)for(unsigned n=0;n<64;++n){
  for(auto& vf:actual.m_state.vf)for(float& v:vf){uint32_t bits=n<20?edges[rng()%(sizeof(edges)/sizeof(edges[0]))]:rng();std::memcpy(&v,&bits,4);}
  for(float& v:actual.m_state.acc){uint32_t bits=rng();std::memcpy(&v,&bits,4);}
  uint32_t bits=rng();std::memcpy(&actual.m_state.q,&bits,4);bits=rng();std::memcpy(&actual.m_state.i,&bits,4);
  reference.m_state=actual.m_state;
  calls=0;reference.execUpper(test.upper);uint32_t expectedClip=capturedClip;unsigned expectedCalls=calls;float expected[4];std::memcpy(expected,captured,16);uint8_t expectedDest=capturedDest;bool expectedFmac=capturedFmac;
  calls=0;assert(BlackVuUpperAot::execute(actual,test.pc));
  assert(calls==expectedCalls);assert(capturedClip==expectedClip);
  if(calls){assert(!std::memcmp(expected,captured,16));assert(expectedDest==capturedDest);assert(expectedFmac==capturedFmac);}
  assert(!std::memcmp(&actual.m_state,&reference.m_state,sizeof(actual.m_state)));
  assert(actual.m_currentUpperInstruction==reference.m_currentUpperInstruction);
 }
 assert(!BlackVuUpperAot::execute(actual,0xffffffff));
 printf("PASS: %zu upper sites x64 inputs match production arithmetic, helper arguments and state; lower/timing not covered\n",sizeof(cases)/sizeof(cases[0]));
}
'''.replace('ROWS',rows)
with tempfile.TemporaryDirectory() as td:
 path=Path(td);(path/'test.cpp').write_text(program);subprocess.run(['clang++','-std=c++20','-O2','-fsanitize=address,undefined','-I'+str(base/'include'),'-I'+str(base/'src/lib/vu'),str(path/'test.cpp'),'-o',str(path/'test')],check=True);subprocess.run([str(path/'test')],check=True)
