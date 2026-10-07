#!/usr/bin/env python3
"""Compare store retirement against the prior RMW contract, including mixed queues."""
from pathlib import Path
import subprocess,tempfile,re
root=Path(__file__).resolve().parents[2];base=root/'tools/PS2Recomp/ps2xRuntime';core=(base/'src/lib/vu/ps2_vu1_core.cpp').read_text()
def extract(name):
 pos=core.index('VU1Interpreter::'+name+'(');start=core.rfind('\n',0,pos)+1;brace=core.index('{',pos);depth=1;end=brace+1
 while depth:depth+=(core[end]=='{')-(core[end]=='}');end+=1
 return core[start:end]
new=extract('commitReadyPipelines');oldblock='        if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)'
# Oracle keeps the old scalar masked read/modify/write, not the optimized branch.
start=new.index('{',new.index(oldblock))+1;end=new.index('\n        }\n        store.valid',start)
ref=new[:start]+'''            uint32_t oldWords[4]{};
            std::memcpy(oldWords, m_activeVuData + store.address, 16);
            for (uint32_t c=0;c<4;c++)if(store.laneMask&(8u>>c))oldWords[c]=store.words[c];
            std::memcpy(m_activeVuData + store.address,oldWords,16);'''+new[end:]
ref=ref.replace('void VU1Interpreter::commitReadyPipelines()','void referenceCommit(VU1Interpreter& host)')
ref=re.sub(r'\bm_[A-Za-z0-9_]+',lambda m:'host.'+m.group(),ref)
for name in ('FlagPipelineEntry','ScalarPipelineEntry','PendingStore','PendingVfWrite','PendingViWrite','PendingAccWrite'):ref=ref.replace(name,'VU1Interpreter::'+name)
program=r'''#include <cstring>
#include <cassert>
#include <cstdio>
#include <bit>
#include <algorithm>
#include <random>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#define private public
#include "runtime/ps2_vu1.h"
#undef private
constexpr uint8_t laneForComponent(uint32_t c){return 8u>>c;}
VU1Interpreter::VU1Interpreter(Unit u):m_unit(u){std::memset(&m_state,0,sizeof(m_state));}
'''+new+'\n'+ref+r'''
int main(){std::mt19937 rng(1421);unsigned count=0;
for(unsigned mask=0;mask<256;mask++)for(unsigned variant=0;variant<64;variant++){
 VU1Interpreter a,b;uint8_t am[128],bm[128];for(auto& v:am)v=rng();std::memcpy(bm,am,128);
 a.m_activeVuData=variant%8==0?nullptr:am;a.m_activeVuDataSize=variant%4==0?8:variant%4==1?16:128;a.m_cycle=10;
 a.m_state.status=rng();a.m_state.mac=rng();a.m_state.clip=rng();
 for(unsigned k=0;k<8;k++){
  auto& s=a.m_storePipeline[k];s.valid=variant%7!=0;s.readyCycle=k%3==0?11:10;s.address=variant%5==0?127:variant%5==1?0:variant%5==2?3+16*(k%6):16*(k%7);s.laneMask=(mask+k)%256;for(auto& w:s.words)w=rng();a.m_storePending|=1u<<k;
  auto& f=a.m_flagPipeline[k];f.valid=true;f.readyCycle=k%2==0?10:11;f.writesMac=f.writesStatus=f.writesSticky=f.writesClip=true;f.mac=rng();f.status=rng();f.extraSticky=rng();f.clip=rng();a.m_flagPending|=1u<<k;
 }
 a.m_fdiv={10,1.25f,0x30,true};a.m_efu[0]={10,2.5f,0,true};a.m_efu[1]={11,-3.0f,0,true};
 b.m_state=a.m_state;b.m_cycle=a.m_cycle;b.m_flagPipeline=a.m_flagPipeline;b.m_storePipeline=a.m_storePipeline;b.m_flagPending=a.m_flagPending;b.m_storePending=a.m_storePending;b.m_fdiv=a.m_fdiv;b.m_efu=a.m_efu;b.m_activeVuData=a.m_activeVuData?bm:nullptr;b.m_activeVuDataSize=a.m_activeVuDataSize;
 for(unsigned cycle=10;cycle<=12;cycle++){a.m_cycle=b.m_cycle=cycle;a.commitReadyPipelines();referenceCommit(b);assert(!std::memcmp(am,bm,128));assert(!std::memcmp(&a.m_state,&b.m_state,sizeof(a.m_state)));assert(a.m_storePending==b.m_storePending && a.m_flagPending==b.m_flagPending);for(unsigned k=0;k<8;k++){assert(a.m_storePipeline[k].valid==b.m_storePipeline[k].valid);assert(a.m_flagPipeline[k].valid==b.m_flagPipeline[k].valid);}assert(a.m_fdiv.valid==b.m_fdiv.valid);++count;}
}printf("PASS: %u commit comparisons; all 256 masks, future/due/invalid slots, overlapping stores, absent/small buffers and flag/Q/P ordering match prior RMW contract\n",count);}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.cpp').write_text(program);subprocess.run(['clang++','-std=c++20','-O2','-fsanitize=address,undefined','-I'+str(base/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
