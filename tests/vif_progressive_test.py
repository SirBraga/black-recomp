#!/usr/bin/env python3
"""Compare actual VIF1 parsing as one chain and resumable command prefixes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/PS2Recomp/ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp').read_text()
enum = source[source.index('enum VIFCmd'):source.index('\nnamespace')]
body = source[source.index('uint32_t PS2Memory::processVIF1DataPrefix'):]
program = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>
constexpr uint32_t PS2_VU1_DATA_SIZE=16384, PS2_VU1_CODE_SIZE=16384;
struct PS2Memory {
 struct Registers { uint32_t stat=0,cycle=0x101,code=0,num=0,base=0,ofst=0,
  tops=0,top=0,itops=0,itop=0,mode=0,mark=0,mask=0,row[4]{},col[4]{}; } vif1_regs;
 std::array<uint8_t,16384> data{},code{};
 uint8_t *m_vu1Data=data.data(),*m_vu1Code=code.data();
 uint32_t m_vif1PendingDirectQw=0,codeChanges=0,unmasks=0;
 bool m_vif1PendingPath2DirectHl=false,m_path3Masked=false;
 std::vector<uint8_t> direct;
 std::vector<uint32_t> events;
 std::function<void(uint32_t,uint32_t,uint32_t)> m_vu1MscalCallback;
 std::function<void(uint32_t,uint32_t)> m_vu1MscntCallback;
 PS2Memory() {
  m_vu1MscalCallback=[&](uint32_t pc,uint32_t top,uint32_t itop){
   events.insert(events.end(),{pc,top,itop});
   for(unsigned i=0;i<64;i+=4){uint32_t word;memcpy(&word,m_vu1Data+i,4);events.push_back(word);}
  };
  m_vu1MscntCallback=[&](uint32_t top,uint32_t itop){events.insert(events.end(),{0xffffffffu,top,itop});};
 }
 void feedVif1DirectPayload(const uint8_t* p,uint32_t qw,bool hl){
  direct.insert(direct.end(),p,p+qw*16);events.push_back(hl?0x51u:0x50u);
 }
 void markVU1CodeModified(){++codeChanges;}
 void flushMaskedPath3Packets(){++unmasks;}
 uint32_t processVIF1DataPrefix(const uint8_t*,uint32_t,uint32_t);
};
'''+enum+'\n'+body+r'''
void word(std::vector<uint8_t>& p,uint32_t x){auto n=p.size();p.resize(n+4);memcpy(p.data()+n,&x,4);}
void cmd(std::vector<uint8_t>& p,unsigned op,unsigned imm=0,unsigned count=0){word(p,(op<<24)|(count<<16)|imm);}
int main(){
 for(unsigned padding=0;padding<16;padding+=4){
  std::vector<uint8_t> packet(padding,0);
  cmd(packet,VIF_BASE,32);cmd(packet,VIF_OFFSET,64);cmd(packet,VIF_ITOP,17);
  cmd(packet,VIF_STROW);for(unsigned i=0;i<4;i++)word(packet,90+i);
  cmd(packet,VIF_STCOL);for(unsigned i=0;i<4;i++)word(packet,20+i);
  cmd(packet,VIF_STCYCL,0x0402);cmd(packet,0x6c,0,7);
  for(unsigned i=0;i<16;i++)word(packet,i+1);
  cmd(packet,VIF_MSCAL,3);
  cmd(packet,VIF_STCYCL,0x0101);cmd(packet,0x6a,0,3);
  for(unsigned i=0;i<3;i++)word(packet,0x08070605u+i);
  cmd(packet,VIF_STMASK);word(packet,0xeeeeeeeeu);
  cmd(packet,VIF_STMOD,2);cmd(packet,0x70,0,2);word(packet,0x12345678);word(packet,0x87654321);
  cmd(packet,VIF_MPG,4,2);for(unsigned i=0;i<4;i++)word(packet,0x23450000+i);
  cmd(packet,VIF_DIRECTHL,1);for(unsigned i=0;i<4;i++)word(packet,0x67890000+i);
  cmd(packet,VIF_MSKPATH3,0x8000);cmd(packet,VIF_MSKPATH3,0);
  cmd(packet,VIF_MSCNT);cmd(packet,VIF_STCYCL,1);cmd(packet,0x60,1023,3);word(packet,42);
  cmd(packet,VIF_MARK,55);
  PS2Memory whole;
  assert(whole.processVIF1DataPrefix(packet.data(),packet.size(),UINT32_MAX)==packet.size());
  for(unsigned budget:{1u,2u,8u,32u}){
   PS2Memory split;size_t offset=0;
   while(offset<packet.size()){
    auto consumed=split.processVIF1DataPrefix(packet.data()+offset,packet.size()-offset,budget);
    assert(consumed>0 && consumed<=packet.size()-offset);offset+=consumed;
   }
   assert(split.data==whole.data && split.code==whole.code && split.direct==whole.direct);
   assert(split.events==whole.events && split.codeChanges==whole.codeChanges && split.unmasks==whole.unmasks);
   assert(!memcmp(&split.vif1_regs,&whole.vif1_regs,sizeof(whole.vif1_regs)));
   assert(split.m_vif1PendingDirectQw==whole.m_vif1PendingDirectQw);
  }
 }
 puts("PASS: full VIF1 parser vs command prefixes; V3 lookahead/alignment, fill cycles, masks, ROW/COL, MPG, DIRECTHL, and MSCAL/MSCNT order");
}
'''
with tempfile.TemporaryDirectory(prefix='black-vif-progressive-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(program)
    subprocess.run(['clang++', '-std=c++20', '-fsanitize=address,undefined', str(path/'test.cpp'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
