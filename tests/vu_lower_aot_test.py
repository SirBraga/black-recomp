#!/usr/bin/env python3
"""Compare selected emitted lower bodies with production case bodies (no pair scheduler)."""
from pathlib import Path
import argparse,json,re,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('header');a=p.parse_args()
root=Path(__file__).resolve().parents[2];base=root/'tools/PS2Recomp/ps2xRuntime';header=Path(a.header).resolve();meta=json.loads(Path(str(header)+'.json').read_text())
meta['lower_supported']=[r for r in meta['lower_supported'] if r['kind'] not in ('LQ','SQ','ILW','ISW','LQI','SQI','LQD','SQD','ILWR','ISWR','XGKICK','XTOP','XITOP','DIV','SQRT','RSQRT','WAITQ','FCEQ','FCSET','FCAND','FCOR','FSEQ','FSSET','FSAND','FSOR','FMEQ','FMAND','FMOR','FCGET','MTIR','MFIR','B','BAL','JR','JALR','IBEQ','IBNE','IBLTZ','IBGTZ','IBLEZ','IBGEZ')]
lower=(base/'src/lib/vu/ps2_vu1_lower.cpp').read_text();core=(base/'src/lib/vu/ps2_vu1_core.cpp').read_text();start=core.index('float VU1Interpreter::normalizeOperand(');end=core.index('\nfloat VU1Interpreter::normalizeResult(',start)
def case_body(code):
 marker=f'case 0x{code:02X}: // '
 start=lower.index(marker);start=lower.index('\n',start)+1;end=lower.index('return;',start)+7
 body=lower[start:end]
 return body + ("\n}" if body.count("{")>body.count("}") else "")
reference='void referenceLower(VU1Interpreter& host,uint32_t instr,uint32_t upper){auto& m_state=host.m_state;if(instr==0 || instr==0x8000033cu)return;auto VIT=[](uint32_t v){return (v>>16)&15;};auto VIS=[](uint32_t v){return (v>>11)&15;};unsigned viD=(instr>>6)&15,viS=VIS(instr),viT=VIT(instr);if(upper&0x80000000u){float v;std::memcpy(&v,&instr,4);m_state.i=host.normalizeOperand(v);return;}switch((instr>>25)&127){'
for code in (8,9):reference+=f'case {code}: '+case_body(code)+'\n'
reference+='case 64:switch(instr&63){'
for code in (48,49,50,52,53):reference+=f'case {code}: '+case_body(code)+'\n'
reference+='case 60:case 61:case 62:case 63:{unsigned vfS=(instr>>11)&31,vfT=(instr>>16)&31,dest=(instr>>21)&15;auto applyDest=[&](float* d,const float* r,uint8_t mask){host.applyDest(d,r,mask);};switch((instr&3)|((instr>>4)&124)){'
for code in (48,49):
 marker=f'case 0x{code:02X}: // '+('MOVE' if code==48 else 'MR32')
 pos=lower.index(marker);begin=lower.index('\n',pos)+1;finish=lower.index('return;',begin)+7;body=lower[begin:finish]
 reference+=f'case {code}: '+body+'\n}'
reference+='default:std::abort();}}default:std::abort();}default:std::abort();}}' 
rows=','.join('{'+str(r['pc'])+','+str(r['lower'])+'u,'+str(r['upper'])+'u}' for r in meta['lower_supported'])
program='''#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <random>
#define private public
#include "runtime/ps2_vu1.h"
#undef private
VU1Interpreter::VU1Interpreter(Unit u):m_unit(u){}
void VU1Interpreter::queueFcset(uint32_t){std::abort();}
void VU1Interpreter::queueFsset(uint16_t){std::abort();}
void VU1Interpreter::queueQ(float,uint32_t,uint32_t){std::abort();}
void VU1Interpreter::startXgkick(uint32_t){std::abort();}
float VU1Interpreter::normalizeResult(float,uint32_t&) const{std::abort();}
uint32_t VU1Interpreter::microAddressMask() const{return m_unit==Unit::VU1?0x3fff:0xfff;}
int32_t VU1Interpreter::readBranchVi(uint8_t r) const{return r==0?0:m_viBranchBackupValid && m_viBranchBackupReg==r?m_viBranchBackupValue:m_state.vi[r];}
void VU1Interpreter::queueStore(uint32_t,const uint32_t*,uint8_t){std::abort();}
void VU1Interpreter::applyDest(float* d,const float* r,uint8_t mask){for(unsigned c=0;c<4;c++)if(mask&(8>>c))d[c]=r[c];}
'''+core[start:end]+reference+'\n#include "'+str(header)+'"\n'+r'''
int main(){VU1Interpreter actual,reference;std::mt19937 rng(91);struct Case{uint32_t pc,lower,upper;};Case cases[]={ROWS};
for(auto c:cases)for(unsigned n=0;n<256;n++){
for(auto& vi:actual.m_state.vi)vi=static_cast<int16_t>(rng());
if(n<4)for(auto& vi:actual.m_state.vi)vi=n==0?0:n==1?32767:n==2?-32768:-1;
for(auto& vf:actual.m_state.vf)for(float& v:vf){uint32_t bits=rng();std::memcpy(&v,&bits,4);}
uint32_t bits=rng();std::memcpy(&actual.m_state.i,&bits,4);reference.m_state=actual.m_state;
referenceLower(reference,c.lower,c.upper);assert(BlackVuUpperAot::executeLower(actual,c.pc));
assert(!std::memcmp(&actual.m_state,&reference.m_state,sizeof(actual.m_state)));
}
auto before=actual.m_state;assert(!BlackVuUpperAot::executeLower(actual,0xffffffff));assert(!std::memcmp(&before,&actual.m_state,sizeof(before)));
printf("PASS: %zu lower sites x256 inputs match production case bodies; pair dependencies/timing not covered\n",sizeof(cases)/sizeof(cases[0]));}
'''.replace('ROWS',rows)
with tempfile.TemporaryDirectory() as td:
 path=Path(td);(path/'test.cpp').write_text(program);subprocess.run(['clang++','-std=c++20','-O2','-fsanitize=address,undefined','-I'+str(base/'include'),str(path/'test.cpp'),'-o',str(path/'test')],check=True);subprocess.run([str(path/'test')],check=True)
