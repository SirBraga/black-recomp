#!/usr/bin/env python3
"""Exercise the production XGKICK packet parser with IMAGE and IMAGE2 tags."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[2]
src = (root/'tools/PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp').read_text()
start = src.index('void VU1Interpreter::progressXgkick()')
end = src.index('\nvoid VU1Interpreter::', start+1)
body = src[start:end]
head = r'''
#include <array>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <vector>
struct VU1Interpreter {
 struct XgkickPipeline {static constexpr unsigned kBufferSize=65536; bool active=true,currentTagEop=false; unsigned cycleCredit=0,copiedBytes=0,sourceAddress=0,currentTagEnd=0,totalBytes=0; std::array<uint8_t,kBufferSize> packet{};} m_xgkick;
 uint8_t *m_activeVuData=nullptr; unsigned m_activeVuDataSize=0; bool failed=false,finished=false;
 void reportReservedInstruction(bool,uint32_t){failed=true;}
 void finishXgkick(){finished=true;m_xgkick.active=false;}
 void progressXgkick();
};
'''
tail = r'''
int main(){
 for(unsigned format=0;format<4;++format){
  std::array<uint8_t,64> data{}; uint64_t tag=1ull|(1ull<<15)|(uint64_t(format)<<58)|(1ull<<60);
  memcpy(data.data()+48,&tag,8);for(unsigned i=0;i<16;++i)data[i]=i+0xc0;
  VU1Interpreter vu;vu.m_activeVuData=data.data();vu.m_activeVuDataSize=data.size();vu.m_xgkick.sourceAddress=48;
  for(int i=0;i<20 && !vu.finished && !vu.failed;++i)vu.progressXgkick();
  assert(!vu.failed && vu.finished);assert(vu.m_xgkick.totalBytes==32);
  assert(memcmp(vu.m_xgkick.packet.data()+16,data.data(),16)==0);
 }
 // A packet larger than the staging buffer still fails, rather than truncating.
 std::array<uint8_t,64> data{};uint64_t tag=32767ull|(3ull<<58);memcpy(data.data(),&tag,8);
 VU1Interpreter vu;vu.m_activeVuData=data.data();vu.m_activeVuDataSize=data.size();vu.progressXgkick();vu.progressXgkick();assert(vu.failed && !vu.finished);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d); (p/'test.cpp').write_text(head+body+tail)
 subprocess.run(['clang++','-std=c++20','-O2',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: XGKICK PACKED/REGLIST/IMAGE/IMAGE2, wrapped payload, oversized packet rejection')
