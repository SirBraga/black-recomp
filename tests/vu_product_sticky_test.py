#!/usr/bin/env python3
"""Differential test for FMAC product classification/broadcast hoisting."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'tools/PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp').read_text()
h=(root/'tools/PS2Recomp/ps2xRuntime/include/runtime/ps2_vu1.h').read_text()
def method(name):
 a=s.index('VU1Interpreter::'+name+'(');a=s.rfind('\n',0,a)+1;b=s.index('{',a);d=1;i=b+1
 while d:d+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]+'\n'
klass=h[h.index('class VU1Interpreter'):h.rindex('};')+2].replace('VU1Interpreter','ReferenceVU').replace('private:','public:')
helpers=''.join(method(n) for n in ['normalizeOperand','normalizeFmacExactResult'])
program=r'''
#include <array>
#include <cassert>
#include <cstring>
#include <random>
#include <cmath>
#include <limits>
#include <cstdio>
#define private public
#include "runtime/ps2_vu1.h"
#undef private
#include "ps2_vu1_detail.h"
constexpr uint8_t laneForComponent(uint32_t c){return 8u>>c;}
CLASS
VU1Interpreter::VU1Interpreter(Unit u):m_unit(u){}
ReferenceVU::ReferenceVU(Unit u):m_unit(u){}
METHODS
int main(){
 VU1Interpreter unit;ReferenceVU reference;std::mt19937 rng(259);
 uint32_t edges[]={0u,0x80000000u,1u,0x80000001u,0x007fffffu,0x807fffffu,0x00800000u,0x80800000u,0x7f7fffffu,0xff7fffffu,0x7f800000u,0xff800000u,0x7fc00001u,0xffc00001u};
 for(unsigned n=0;n<300000;++n){
  for(auto& vf:unit.m_state.vf)for(float& f:vf){uint32_t bits=n<32768?edges[rng()%14]:rng();std::memcpy(&f,&bits,4);}
  uint32_t bits=rng();std::memcpy(&unit.m_state.q,&bits,4);bits=rng();std::memcpy(&unit.m_state.i,&bits,4);
  reference.m_state=unit.m_state;
  unit.m_currentUpperInstruction=reference.m_currentUpperInstruction=(rng()&~2047u)|(n<32768 ? (n/16)&2047u : n&2047u);
  uint8_t mask=n<32768 ? n&15 : rng()&15;
  assert(unit.calculateFmacProductSticky(mask)==reference.calculateFmacProductSticky(mask));
  assert(!std::memcmp(&unit.m_state,&reference.m_state,sizeof(unit.m_state)));
 }
 puts("PASS: 300000 FMAC instructions preserve sticky flags, broadcast/cross forms and IEEE edge cases");
}
'''.replace('CLASS',klass).replace('METHODS',helpers+helpers.replace('VU1Interpreter::','ReferenceVU::')+method('calculateFmacProductSticky')+(root/'ps2recomp/tests/vu_product_sticky_reference.inc').read_text())
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.cpp').write_text(program)
 subprocess.run(['clang++','-std=c++20','-O2','-fsanitize=address,undefined','-I'+str(root/'tools/PS2Recomp/ps2xRuntime/include'),'-I'+str(root/'tools/PS2Recomp/ps2xRuntime/src/lib/vu'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
