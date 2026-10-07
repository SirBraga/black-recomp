#!/usr/bin/env python3
"""Bitwise equivalence of production ARM64 vector normalization and scalar VU rules."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
upper = (root / 'tools/PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_upper.cpp').read_text()
a = upper.index('    void normalizeVuVector(')
b = upper.index('\n#endif', a)
vector = upper[a:b]
core = (root / 'tools/PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp').read_text()
a = core.index('float VU1Interpreter::normalizeOperand(')
b = core.index('\nfloat VU1Interpreter::normalizeResult(', a)
scalar = core[a:b].replace('VU1Interpreter::normalizeOperand', 'normalizeScalar').replace(' const\n', '\n')
program = r'''
#include <arm_neon.h>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
VECTOR
SCALAR
int main(){
 std::mt19937 rng(357);
 std::array<uint32_t,18> edges{0u,0x80000000u,1u,0x80000001u,0x007fffffu,0x807fffffu,
  0x00800000u,0x80800000u,0x7f7fffffu,0xff7fffffu,0x7f800000u,0xff800000u,
  0x7fc00000u,0xffc00000u,0x7f800001u,0xff800001u,0x3f800000u,0xbf800000u};
 float input[4],actual[4],expected[4];
 for(unsigned n=0;n<1000000;n++){
  for(unsigned c=0;c<4;c++){
   uint32_t bits=n<edges.size()*edges.size()?edges[(n+c*7)%edges.size()]:rng();
   std::memcpy(input+c,&bits,4);expected[c]=normalizeScalar(input[c]);
  }
  normalizeVuVector(actual,input);
  assert(std::memcmp(actual,expected,16)==0);
  normalizeVuVector(actual,actual);assert(std::memcmp(actual,expected,16)==0);
 }
 std::vector<std::array<float,4>> vectors(65536),output(65536);
 for(auto& v:vectors)for(auto& f:v){uint32_t bits=rng();std::memcpy(&f,&bits,4);}
 auto bench=[&](bool neon){auto start=std::chrono::steady_clock::now();
  for(unsigned repeat=0;repeat<100;repeat++)for(unsigned n=0;n<vectors.size();n++){
   if(neon)normalizeVuVector(output[n].data(),vectors[n].data());
   else for(unsigned c=0;c<4;c++)output[n][c]=normalizeScalar(vectors[n][c]);
  }
  unsigned checksum=0;for(auto& v:output){uint32_t bits;std::memcpy(&bits,v.data(),4);checksum^=bits;}
  printf("%s: %.2fms checksum=%08x\n",neon?"NEON":"scalar",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),checksum);
  return checksum;
 };
 auto reference=bench(false),native=bench(true);assert(reference==native);
 puts("PASS: 4000000 values match bitwise, including signed zero, subnormals, infinities and NaNs");
}
'''.replace('VECTOR', vector).replace('SCALAR', scalar)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    source = path / 'test.cpp'
    exe = path / 'test'
    source.write_text(program)
    subprocess.run(['clang++', '-std=c++20', '-O3', str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
