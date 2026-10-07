#!/usr/bin/env python3
"""Exercise production GIF_STAT readback occupancy without inventing FIFO data."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'tools/PS2Recomp/ps2xRuntime/src/lib/ps2_memory.cpp').read_text()
a = source.index('    if (address == 0x10003020u) // GIF_STAT')
z = source.index('\n    if (address >= 0x10000000', a)
body = source[a:z]
head = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>
#include <cstdio>
#include <cstdlib>
struct Memory {
 std::map<uint32_t,uint32_t> m_ioRegisters;
 bool m_path3Masked=false;
 std::vector<std::vector<uint8_t>> m_path3MaskedFifo;
 struct {uint64_t busdir=0;} gs_regs;
 std::function<uint32_t()> m_gifDownloadFifoCallback;
 uint32_t read(uint32_t address) {
'''
tail = r'''
 return 0; }
};
int main() {
 Memory m;unsigned available=0;
 m.m_gifDownloadFifoCallback=[&]{return available;};
 m.m_ioRegisters[0x10003010]=5;m.m_ioRegisters[0x10003000]=8;m.m_path3Masked=true;
 m.gs_regs.busdir=1;
 for(unsigned q:{0u,1u,7u,16u,32u}) {
  available=q;m.m_ioRegisters[0x10003020]=0x0a000000;
  unsigned s=m.read(0x10003020);
  assert(((s>>24)&31)==std::min(q,16u));
  assert((s&15)==15 && (s&(1u<<12)));
 }
 available=0;assert(((m.read(0x10003020)>>24)&31)==0);
 m.gs_regs.busdir=0;m.m_ioRegisters[0x10003020]=3u<<24;available=16;
 assert(((m.read(0x10003020)>>24)&31)==3); // upload direction unchanged
 m.m_path3MaskedFifo.push_back(std::vector<uint8_t>(64));
 m.m_ioRegisters[0x10003020]=0; // timer ticks clear only transient counter
 assert(((m.read(0x10003020)>>24)&31)==4);
 m.m_path3MaskedFifo.push_back(std::vector<uint8_t>(512));
 assert(((m.read(0x10003020)>>24)&31)==16);
 m.m_path3MaskedFifo.clear();assert(((m.read(0x10003020)>>24)&31)==0);
}
'''
with tempfile.TemporaryDirectory() as directory:
 d=Path(directory);(d/'test.cpp').write_text(head+body+tail)
 subprocess.run(['clang++','-std=c++20','-O2',str(d/'test.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
print('PASS: GIF download occupancy empty, available, saturated, consumed; upload and mode bits preserved')
