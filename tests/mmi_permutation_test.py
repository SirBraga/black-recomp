#!/usr/bin/env python3
"""Exercise production MMI macros and generator dispatch against EE lane semantics."""
from pathlib import Path
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
header = (root/'tools/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h').read_text()
translator = (root/'tools/PS2Recomp/ps2xRecomp/src/lib/mmi_translation_helpers.cpp').read_text()
macros = '\n'.join(re.search(r'^#define PS2_'+name+r'\(rs\).*$',header,re.M).group() for name in ('PEXEH','PEXEW','PROT3W'))
for name in ('PEXEH','PEXEW','PROT3W'):
    body = translator.split('CodeGenerator::translate'+name+'(',1)[1].split('\n    }',1)[0]
    assert 'PS2_'+name+'(GPR_VEC(ctx, {}))' in body, name+' generator must use tested helper'
program = r'''
#include "sse2neon.h"
#include <array>
#include <cassert>
#include <cstring>
#include <random>
#include <cstdio>
MACROS
int main(){
 std::mt19937 rng(21376);
 constexpr unsigned wordExchange[4]={2,1,0,3},wordRotate[4]={1,2,0,3};
 constexpr unsigned halfExchange[8]={2,1,0,3,6,5,4,7};
 for(unsigned n=0;n<100001;n++){
  alignas(16) uint32_t input[4],expected[4],actual[4];
  for(unsigned i=0;i<4;i++)input[i]=n?rng():i;
  __m128i v=_mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
  for(unsigned i=0;i<4;i++)expected[i]=input[wordExchange[i]];
  _mm_storeu_si128(reinterpret_cast<__m128i*>(actual),PS2_PEXEW(v));assert(!memcmp(actual,expected,16));
  for(unsigned i=0;i<4;i++)expected[i]=input[wordRotate[i]];
  _mm_storeu_si128(reinterpret_cast<__m128i*>(actual),PS2_PROT3W(v));assert(!memcmp(actual,expected,16));
  uint16_t halves[8],wanted[8];memcpy(halves,input,16);
  for(unsigned i=0;i<8;i++)wanted[i]=halves[halfExchange[i]];
  _mm_storeu_si128(reinterpret_cast<__m128i*>(actual),PS2_PEXEH(v));assert(!memcmp(actual,wanted,16));
 }
 puts("PASS: PEXEH, PEXEW, PROT3W lane mapping; 100001 inputs and generator helper dispatch");
}
'''.replace('MACROS',macros)
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.cpp').write_text(program)
    subprocess.run(['clang++','-std=c++20','-O2','-fsanitize=address,undefined','-I'+str(root/'tools/PS2Recomp/out/rt/_deps/sse2neon-src'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
