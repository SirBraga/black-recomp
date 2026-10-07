#!/usr/bin/env python3
"""Exercise actual PATH2 IMAGE continuation across DIRECT packets."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'tools/PS2Recomp/ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp').read_text()
a=s.index('    uint32_t pendingGifImageQwc(');b=s.index('\n}\n',a)
parser=s[a:b]
a=s.index('void PS2Memory::feedVif1DirectPayload(');b=s.index('\nvoid PS2Memory::processVIF1Data(',a)
feed=s[a:b]
head=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
constexpr uint8_t kGifFmtImage=2;
enum class GifPathId{Path2};
struct PS2Memory {
 uint32_t m_vif1PendingPath2ImageQwc=0; bool m_vif1PendingPath2DirectHl=false;
 struct Packet{std::vector<uint8_t> data;bool hl;bool continuation;};std::vector<Packet> packets;
 void submitGifPacket(GifPathId,const uint8_t*d,uint32_t n,bool,bool hl,bool continuation=false){packets.push_back({{d,d+n},hl,continuation});}
 void feedVif1DirectPayload(const uint8_t*,uint32_t,bool);
};
'''
tail=r'''
int main(){for(unsigned fmt:{2u,3u}){
 PS2Memory m;std::vector<uint8_t> first(32,0);uint64_t tag=3ull|(1ull<<15)|(uint64_t(fmt)<<58);memcpy(first.data(),&tag,8);
 for(int i=0;i<16;++i)first[16+i]=0xc0+i;
 m.feedVif1DirectPayload(first.data(),2,true);
 assert(m.m_vif1PendingPath2ImageQwc==2 && m.m_vif1PendingPath2DirectHl);
 std::vector<uint8_t> next(48);for(unsigned i=0;i<32;++i)next[i]=0x80+i;
 uint64_t trailing=(1ull<<15)|(1ull<<60);memcpy(next.data()+32,&trailing,8);
 m.feedVif1DirectPayload(next.data(),3,false);
 assert(m.m_vif1PendingPath2ImageQwc==0 && m.packets.size()==3);
 auto& continuation=m.packets[1];assert(continuation.continuation && continuation.hl && continuation.data.size()==48);
 memcpy(&tag,continuation.data.data(),8);assert((tag&32767)==2 && ((tag>>58)&3)==2 && (tag&(1ull<<15)));
 assert(memcmp(continuation.data.data()+16,next.data(),32)==0);
 assert(!m.packets[0].continuation && !m.packets[2].continuation);
 assert(!m.packets[2].hl && m.packets[2].data.size()==16);
 assert(memcmp(m.packets[2].data.data(),next.data()+32,16)==0);
}}
'''
with tempfile.TemporaryDirectory() as directory:
 d=Path(directory);(d/'test.cpp').write_text(head+parser+feed+tail)
 subprocess.run(['clang++','-std=c++20','-O2',str(d/'test.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
print('PASS: IMAGE and IMAGE2 preserve split PATH2 payload, DIRECTHL and following GIFtag')
