#!/usr/bin/env python3
"""Check actual VIF1 UNPACK against hardware lane/color/ROW/cycle rules."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'tools/PS2Recomp/ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp').read_text()
a=s.index('        else if ((opcode & 0x60) == 0x60)',s.index('void PS2Memory::processVIF1Data(const uint8_t'))
z=s.index('\n        else\n',a)
body=s[a:z].replace('else if','if',1)
head=r'''
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
constexpr uint32_t PS2_VU1_DATA_SIZE=16384;
struct PS2Memory {
 struct {uint32_t cycle=0x101,tops=0,mask=0,mode=0,row[4]{},col[4]{};} vif1_regs;
 uint8_t memory[16384]{};uint8_t* m_vu1Data=memory;
 alignas(16) uint8_t aligned[256]{};
 void feed(const std::vector<uint8_t>& packet){
  if(packet.size()>sizeof(aligned)){std::fprintf(stderr,"packet too big\n");return;}
  std::memset(aligned,0,sizeof(aligned));std::memcpy(aligned,packet.data(),packet.size());
  auto data=aligned;uint32_t sizeBytes=packet.size(),pos=0;
  while(pos+4<=sizeBytes){uint32_t cmd;memcpy(&cmd,data+pos,4);pos+=4;
   uint8_t opcode=cmd>>24,num=cmd>>16;uint16_t imm=cmd;
'''
tail=r'''
  }}
 std::array<uint32_t,4> vec(unsigned n=0){std::array<uint32_t,4> r;memcpy(r.data(),memory+n*16,16);return r;}
};
int failures=0;
void check(bool ok,const char* msg){if(!ok){if(failures<12)std::fprintf(stderr,"FAIL: %s\n",msg);failures++;}}
std::vector<uint8_t> packet(unsigned opcode,unsigned imm,unsigned count,const void* data,unsigned size){
 std::vector<uint8_t> p(4+((size+3)&~3u),0);uint32_t cmd=(opcode<<24)|(count<<16)|imm;
 memcpy(p.data(),&cmd,4);memcpy(p.data()+4,data,size);return p;
}
int main(){
 for(unsigned bits:{32u,16u,8u})for(unsigned usn:{0u,1u}){
  PS2Memory m;memset(m.memory,0xcc,sizeof(m.memory));uint32_t data[2]={0x12345678,0xabcdef00};
  unsigned vl=bits==32?0:bits==16?1:2;
  unsigned sz=bits/8*2;std::array<uint32_t,4> expected{};
  if(bits==32)expected={data[0],data[1],data[0],data[1]};
  else if(bits==16){uint16_t v[2]={0x8001,0x7fff};memcpy(data,v,4);uint32_t x=usn?v[0]:uint32_t(int32_t(int16_t(v[0])));expected={x,0x7fff,x,0x7fff};}
  else {uint8_t v[2]={0x81,0x7f};memcpy(data,v,2);uint32_t x=usn?v[0]:uint32_t(int32_t(int8_t(v[0])));expected={x,0x7f,x,0x7f};}
  m.feed(packet(0x64|vl,usn<<14,1,data,sz));check(m.vec()==expected,"V2 repeats XY into ZW");
 }
 for(unsigned prefix:{0u,4u,8u,12u})for(unsigned usn:{0u,1u}) {
  PS2Memory m;memset(m.memory,0xcc,sizeof(m.memory));
  uint8_t input[12]={0x81,2,3,0x84,5,6,0x87,8,9,0x8a,11,12};
  auto p=packet(0x6a,usn<<14,4,input,12);p.insert(p.begin(),prefix,0);m.feed(p);
  auto extend=[&](uint8_t x){return usn?uint32_t(x):uint32_t(int32_t(int8_t(x)));};
  for(unsigned j=0;j<4;j++) {
   const uint8_t* wSrc=m.aligned+prefix+4+3*j+3;
   uint32_t w=((reinterpret_cast<uintptr_t>(wSrc)&15u)!=0u && wSrc<m.aligned+p.size())?extend(*wSrc):0u;
   check(m.vec(j)==std::array<uint32_t,4>{extend(input[3*j]),extend(input[3*j+1]),extend(input[3*j+2]),w},"V3-8 W follows the next byte unless it starts a new qword");
  }
 }
 for(uint32_t value=0;value<65536;value++){
  PS2Memory m;uint16_t color=value;m.vif1_regs.mode=2;m.vif1_regs.row[0]=123;
  m.feed(packet(0x6f,0,1,&color,2));
  check(m.vec()==std::array<uint32_t,4>{(value&31)<<3,((value>>5)&31)<<3,((value>>10)&31)<<3,((value>>15)&1)<<7},"V4-5 expands RGB/alpha and ignores STMOD");
 }
 {
  PS2Memory m;memset(m.memory,0xcc,sizeof(m.memory));
  uint32_t words[6]={1,2,3,4,5,6};
  m.feed(packet(0x68,0,2,words,24));
  const uint8_t* w0=m.aligned+4+12;
  uint32_t expectW0=((reinterpret_cast<uintptr_t>(w0)&15u)!=0u)?4u:0u;
  check(m.vec(0)==std::array<uint32_t,4>{1,2,3,expectW0},"V3-32 W is the next word unless it starts a qword");
  const uint8_t* w1=m.aligned+4+12+12;
  uint32_t raw=0;if((reinterpret_cast<uintptr_t>(w1)&15u)!=0u && w1+4<=m.aligned+4+24)std::memcpy(&raw,w1,4);
  check(m.vec(1)==std::array<uint32_t,4>{4,5,6,raw},"V3-32 second vector W");
 }
 {
  PS2Memory m;m.vif1_regs.mode=3;uint32_t v[4]={4,5,6,7};
  m.feed(packet(0x6c,0,1,v,16));check(std::equal(v,v+4,m.vif1_regs.row),"STMOD3 updates ROW");
 }
 {
  PS2Memory m;m.vif1_regs.cycle=1; // CL1 / WL0 means WL256, filling
  for(unsigned i=0;i<4;i++)m.vif1_regs.row[i]=90+i;
  uint32_t scalar=42;m.feed(packet(0x60,0,3,&scalar,4));
  check(m.vec()==std::array<uint32_t,4>{42,42,42,42},"WL0 reads first vector");
  check(m.vec(1)==std::array<uint32_t,4>{90,91,92,93},"WL0 fills remaining vectors from ROW");
 }
 if(failures){std::fprintf(stderr,"%d mismatches\n",failures);return 1;}
 std::puts("PASS: V2 signed/unsigned 8/16/32, 65536 V4-5 colors, STMOD3 ROW, WL0 fill");
}
'''
with tempfile.TemporaryDirectory() as directory:
 d=Path(directory);(d/'test.cpp').write_text(head+body+tail)
 subprocess.run(['clang++','-std=c++20','-O2',str(d/'test.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
