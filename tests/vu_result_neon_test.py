#!/usr/bin/env python3
"""Compare production SIMD FMAC results/flags with the unchanged scalar oracle."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'tools/PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp').read_text()
a=s.index('    void normalizeVuResults(');b=s.index('\n#endif',a);vector=s[a:b]
a=s.index('float VU1Interpreter::normalizeResult(');b=s.index('\nuint32_t VU1Interpreter::microAddressMask',a)
scalar=s[a:b].replace('VU1Interpreter::normalizeResult','scalarResult').replace(' const\n','\n')
program=r'''
#include <arm_neon.h>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <random>
#include <cstdio>
#include <array>
VECTOR
SCALAR
int main(){
 std::mt19937 rng(741); const uint32_t edge[]={0,0x80000000,1,0x80000001,0x007fffff,0x807fffff,0x00800000,0x80800000,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc00001,0xffc00001};
 for(unsigned n=0;n<1000000;++n){
  float actual[4],expected[4];uint8_t flags[4],reference[4];unsigned mask=n&15;
  for(unsigned c=0;c<4;++c){uint32_t bits=n<256?edge[(n+c)%14]:rng();std::memcpy(actual+c,&bits,4);expected[c]=actual[c];uint32_t f=0;if(mask&(8>>c))expected[c]=scalarResult(expected[c],f);reference[c]=f;}
  normalizeVuResults(actual,mask,flags);
  assert(!std::memcmp(actual,expected,16));assert(!std::memcmp(flags,reference,4));
 }
 puts("PASS: 4000000 FMAC values/flags, all masks, inactive lanes and IEEE edge cases");
}
'''.replace('VECTOR',vector).replace('SCALAR',scalar)
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.cpp').write_text(program)
 subprocess.run(['clang++','-std=c++20','-O2','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
