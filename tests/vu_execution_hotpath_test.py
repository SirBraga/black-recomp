#!/usr/bin/env python3
"""Exercise production VI selection and XGKICK qword copy against prior behavior."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
core = (root / 'tools/PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp').read_text()
start = core.index('        const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes)')
copy = core[start:core.index('        m_xgkick.copiedBytes += 16u;', start)]
start = core.index('        const uint32_t viWrites = decoded.lowerUsage.viWrite & 0xFFFEu;')
select = core[start:core.index('\n\n        const VfAccess upperWrite', start)]
program = r'''
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
struct Copy {
 struct {uint32_t sourceAddress=0,copiedBytes=0;std::array<uint8_t,0x10000> packet{};}m_xgkick;
 uint8_t* m_activeVuData=nullptr;uint32_t m_activeVuDataSize=0;
 void qword(){COPY_BODY}
};
struct Select {
 struct {std::array<int32_t,16> vi{};}m_state;
 struct {struct {uint16_t viWrite=0;}lowerUsage;}decoded;
 std::pair<uint8_t,int32_t> pick(){SELECT_BODY return {writtenVi,oldVi};}
};
int main(){
 Select select;for(unsigned j=0;j<16;j++)select.m_state.vi[j]=int(j*987)-431;
 for(unsigned mask=0;mask<65536;mask++){
  select.decoded.lowerUsage.viWrite=mask;uint8_t reg=0;int32_t value=0;
  for(unsigned j=1;j<16;j++)if(mask&(1u<<j)){reg=j;value=select.m_state.vi[j];break;}
  assert(select.pick()==std::make_pair(reg,value));
 }
 std::mt19937 rng(91);Copy unit;unsigned tested=0;
 auto exercise=[&](uint32_t size,uint32_t address,uint32_t written){
  std::vector<uint8_t> memory(size);for(auto& b:memory)b=rng();
  unit.m_activeVuData=memory.data();unit.m_activeVuDataSize=size;
  unit.m_xgkick.sourceAddress=address;unit.m_xgkick.copiedBytes=written;
  unit.m_xgkick.packet.fill(0xa5);unit.qword();
  for(unsigned j=0;j<16;j++)assert(unit.m_xgkick.packet[written+j]==memory[(address+written+j)%size]);
  if(written)assert(unit.m_xgkick.packet[written-1]==0xa5);
  if(written+16<unit.m_xgkick.packet.size())assert(unit.m_xgkick.packet[written+16]==0xa5);
  tested++;
 };
 // Tiny buffers, real VU0/VU1 sizes, unaligned starts, end wrapping,
 // first/final packet qwords and repeated circular wrapping.
 for(uint32_t size:{1u,2u,3u,7u,15u,16u,17u,4096u,16384u}){
  for(uint32_t source=0;source<size;source++){
   exercise(size,source,0);exercise(size,source,0xfff0);
  }
 }
 for(unsigned n=0;n<20000;n++)exercise(1+rng()%32768,rng()%16384,(rng()%4096)*16);
 printf("PASS: all 65536 VI masks; %u XGKICK qword copies preserve bytes, wrap and packet boundaries\n",tested);
}
'''.replace('COPY_BODY', copy).replace('SELECT_BODY', select)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    source = path / 'test.cpp'
    exe = path / 'test'
    source.write_text(program)
    subprocess.run(['clang++', '-std=c++20', '-O2', '-fsanitize=address,undefined',
                    str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
