#!/usr/bin/env python3
"""Compare production upper NOP dispatch against the frozen pre-change code."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
base = root / 'tools/PS2Recomp/ps2xRuntime'
header = (base / 'include/runtime/ps2_vu1.h').read_text()
classdef = header[header.index('class VU1Interpreter'):header.rindex('};') + 2]
reference_class = classdef.replace('VU1Interpreter', 'ReferenceVU').replace('private:', 'public:')
upper = (base / 'src/lib/vu/ps2_vu1_upper.cpp').read_text()
reference = (root / 'ps2recomp/tests/vu_upper_reference.inc').read_text()
# Includes are shared; avoid defining the local conversion helper twice.
reference = reference[reference.index('void VU1Interpreter::execUpper'):].replace('VU1Interpreter::', 'ReferenceVU::')
core = (base / 'src/lib/vu/ps2_vu1_core.cpp').read_text()
a = core.index('float VU1Interpreter::normalizeOperand(')
b = core.index('\nfloat VU1Interpreter::normalizeResult(', a)
normalize = core[a:b]
support = r'''
VU1Interpreter::VU1Interpreter(Unit u):m_unit(u){}
float VU1Interpreter::broadcast(const float* v,uint8_t c){return normalizeOperand(v[c&3]);}
void VU1Interpreter::applyDest(float*,const float*,uint8_t){std::abort();}
void VU1Interpreter::applyFmacDest(float*,float*,uint8_t){std::abort();}
void VU1Interpreter::applyFmacDestAcc(float*,uint8_t){std::abort();}
void VU1Interpreter::queueClip(uint32_t){std::abort();}
void VU1Interpreter::reportReservedInstruction(bool,uint32_t){m_state.status++;}
'''
test = r'''
int main(){
 VU1Interpreter actual;ReferenceVU expected;std::mt19937 rng(73);
 for(unsigned n=0;n<100000;n++){
  for(auto& reg:actual.m_state.vf)for(auto& f:reg){uint32_t bits=rng();std::memcpy(&f,&bits,4);}
  for(auto& f:actual.m_state.acc){uint32_t bits=rng();std::memcpy(&f,&bits,4);}
  uint32_t q=rng(),i=rng();std::memcpy(&actual.m_state.q,&q,4);std::memcpy(&actual.m_state.i,&i,4);
  actual.m_state.mac=rng();actual.m_state.clip=rng();actual.m_state.status=rng();
  expected.m_state=actual.m_state;
  // Random register/lane/flag fields; selector 0x2f or 0x30 is kept intact.
  uint32_t instruction=(rng()&0xfffff800u)|((n&1)?0x2ffu:0x33cu);
  VU1State before=actual.m_state;
  actual.execUpper(instruction);expected.execUpper(instruction);
  assert(std::memcmp(&actual.m_state,&expected.m_state,sizeof(before))==0);
  assert(std::memcmp(&actual.m_state,&before,sizeof(before))==0);
  assert(actual.m_currentUpperInstruction==instruction&&expected.m_currentUpperInstruction==instruction);
 }
 // The same low op outside the special group remains a reserved instruction.
 for(uint32_t op=0x30;op<0x3c;op++){
  actual.m_state.status=0;expected.m_state=actual.m_state;
  actual.execUpper(op);expected.execUpper(op);
  assert(actual.m_state.status==1&&expected.m_state.status==1);
 }
 puts("PASS: 100000 upper NOPs preserve VF/VI/ACC/flags and instruction tracking; reserved dispatch unchanged");
 auto bench=[](auto& unit){
  auto start=std::chrono::steady_clock::now();
  for(unsigned n=0;n<10000000;n++)unit.execUpper((n&1)?0x2ffu:0x33cu);
  return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 };
 double baseline=bench(expected),optimized=bench(actual);
 printf("NOP dispatch microbenchmark (not gameplay FPS): reference %.2f ms, optimized %.2f ms; last=%x/%x\n",baseline,optimized,expected.m_currentUpperInstruction,actual.m_currentUpperInstruction);
}
'''
head = '''#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <random>
#define private public
#include "runtime/ps2_vu1.h"
#undef private
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    source = path / 'test.cpp'
    exe = path / 'test'
    source.write_text(head + reference_class + '\n' + upper + reference + normalize +
                      normalize.replace('VU1Interpreter::', 'ReferenceVU::') + support +
                      support.replace('VU1Interpreter::', 'ReferenceVU::').replace('ReferenceVU::VU1Interpreter', 'ReferenceVU::ReferenceVU') + test)
    subprocess.run(['clang++', '-std=c++20', '-O2', '-I' + str(base / 'include'),
                    '-I' + str(base / 'src/lib/vu'), str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
