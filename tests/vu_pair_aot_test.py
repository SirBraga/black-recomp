#!/usr/bin/env python3
"""Compare supported paired bodies and atomic fallback with production body oracles."""
from pathlib import Path
import subprocess,tempfile,json,argparse
p=argparse.ArgumentParser();p.add_argument('header');p.add_argument('--real-helpers',action='store_true');p.add_argument('--scheduler',action='store_true');p.add_argument('--program',action='store_true');p.add_argument('--xgkick',action='store_true');p.add_argument('--timing',action='store_true');p.add_argument('--commit-stats',action='store_true');p.add_argument('--save-binary');p.add_argument('--iterations',type=int,default=2000000);a=p.parse_args();root=Path(__file__).resolve().parents[2];base=root/'tools/PS2Recomp/ps2xRuntime';a.timing=a.timing or a.commit_stats;a.scheduler=a.scheduler or a.program or a.xgkick or a.timing;a.real_helpers=a.real_helpers or a.scheduler;header=Path(a.header).resolve();metadata=json.loads(Path(str(header)+'.json').read_text())
a.real_helpers=a.real_helpers or any(r['kind'] in ('FCSET','FSSET','DIV','SQRT','RSQRT','WAITQ','XGKICK','MFP','ESQRT','ESIN','ERCPR') for r in metadata['lower_supported'])
upper=(base/'src/lib/vu/ps2_vu1_upper.cpp').read_text();core=(base/'src/lib/vu/ps2_vu1_core.cpp').read_text();start=core.index('float VU1Interpreter::normalizeOperand(');end=core.index('\nfloat VU1Interpreter::normalizeResult(',start)
support=r'''
struct Store{uint32_t address,words[4];uint8_t mask;};Store store{};unsigned storeCalls=0;
uint32_t capturedClip=0;unsigned calls=0;bool capturedFmac=false;
float captured[4];uint8_t capturedDest=0;
VU1Interpreter::VU1Interpreter(Unit u):m_unit(u){std::memset(&m_state,0,sizeof(m_state));}
uint32_t VU1Interpreter::microAddressMask() const{return m_unit==Unit::VU1?0x3fff:0xfff;}
int32_t VU1Interpreter::readBranchVi(uint8_t r) const{return r==0?0:m_viBranchBackupValid && m_viBranchBackupReg==r?m_viBranchBackupValue:m_state.vi[r];}
void VU1Interpreter::queueStore(uint32_t a,const uint32_t* w,uint8_t m){++storeCalls;store.address=a;std::memcpy(store.words,w,16);store.mask=m;}
float VU1Interpreter::broadcast(const float* v,uint8_t c){return normalizeOperand(v[c&3]);}
void VU1Interpreter::applyDest(float* dst,const float* r,uint8_t d){++calls;capturedFmac=false;std::memcpy(captured,r,16);capturedDest=d;for(unsigned c=0;c<4;c++)if(d&(8>>c))dst[c]=r[c];}
void VU1Interpreter::applyFmacDest(float* dst,float* r,uint8_t d){++calls;capturedFmac=true;std::memcpy(captured,r,16);capturedDest=d;for(unsigned c=0;c<4;c++)if(d&(8>>c))dst[c]=r[c];}
void VU1Interpreter::applyFmacDestAcc(float* r,uint8_t d){applyFmacDest(m_state.acc,r,d);}
void VU1Interpreter::queueClip(uint32_t v){++calls;capturedClip=v;}
void VU1Interpreter::queueFcset(uint32_t){std::abort();}
void VU1Interpreter::queueFsset(uint16_t){std::abort();}
void VU1Interpreter::queueQ(float,uint32_t,uint32_t){std::abort();}
void VU1Interpreter::startXgkick(uint32_t){std::abort();}
float VU1Interpreter::normalizeResult(float,uint32_t&) const{std::abort();}
void VU1Interpreter::reportReservedInstruction(bool,uint32_t){std::abort();}
'''
lower=(base/'src/lib/vu/ps2_vu1_lower.cpp').read_text()
def case_body(code):
 marker=f'case 0x{code:02X}: // '
 start=lower.index(marker);start=lower.index('\n',start)+1;end=lower.index('return;',start)+7
 body=lower[start:end]
 return body + "\n}"*(body.count("{")-body.count("}"))
reference='void referenceLower(VU1Interpreter& host,uint32_t instr,uint32_t upper,uint8_t* vuData,uint32_t dataSize){auto& m_state=host.m_state;auto normalizeOperand=[&](float v){return host.normalizeOperand(v);};auto normalizeResult=[&](float v,uint32_t& f){return host.normalizeResult(v,f);};auto queueQ=[&](float v,uint32_t l,uint32_t f){host.queueQ(v,l,f);};auto queueP=[&](float v,uint32_t l){host.queueP(v,l);};auto startXgkick=[&](uint32_t v){host.startXgkick(v);};uint32_t pcMask=host.microAddressMask();auto readBranchVi=[&](uint8_t r){return host.readBranchVi(r);};auto applyDest=[&](float* d,const float* r,uint8_t mask){host.applyDest(d,r,mask);};auto queueFcset=[&](uint32_t v){host.queueFcset(v);};auto queueFsset=[&](uint16_t v){host.queueFsset(v);};auto queueStore=[&](uint32_t a,const uint32_t* w,uint8_t mask){host.queueStore(a,w,mask);};auto VIT=[](uint32_t v){return (v>>16)&15;};auto VIS=[](uint32_t v){return (v>>11)&15;};unsigned viD=(instr>>6)&15,viS=VIS(instr),viT=VIT(instr);if(upper&0x80000000u){float v;std::memcpy(&v,&instr,4);m_state.i=host.normalizeOperand(v);return;}if(instr==0 || instr==0x8000033cu)return;switch((instr>>25)&127){'
for code in (0,1,4,5,8,9,16,17,18,19,20,21,22,23,24,26,27,28,32,33,36,37,40,41,44,45,46,47):reference+=f'case {code}: '+case_body(code)+'\n'
reference+='case 64:switch(instr&63){'
for code in (48,49,50,52,53):reference+=f'case {code}: '+case_body(code)+'\n'
reference+='case 60:case 61:case 62:case 63:{unsigned vfS=(instr>>11)&31,vfT=(instr>>16)&31,dest=(instr>>21)&15;auto applyDest=[&](float* d,const float* r,uint8_t mask){host.applyDest(d,r,mask);};switch((instr&3)|((instr>>4)&124)){'
for code in (48,49,52,53,54,55,56,57,58,60,61,62,63,100,104,105,120,121,122):
 marker=f'case 0x{code:02X}: // '+{48:'MOVE',49:'MR32',52:'LQI',53:'SQI',54:'LQD',55:'SQD',56:'DIV',57:'SQRT',58:'RSQRT',60:'MTIR',61:'MFIR',62:'ILWR',63:'ISWR',100:'MFP',104:'XTOP',105:'XITOP',120:'ESQRT',121:'ESIN',122:'ERCPR'}[code]
 pos=lower.index(marker);begin=lower.index('\n',pos)+1;finish=lower.index('return;',begin)+7;body=lower[begin:finish]
 reference+=f'case {code}: '+body+'\n}'
reference+='case 59:return;case 108:startXgkick(static_cast<uint32_t>(static_cast<uint16_t>(m_state.vi[viS])));return;default:std::abort();}}default:std::abort();}default:std::abort();}}' 

if a.real_helpers:
 def method(name):
  pos=core.index('VU1Interpreter::'+name+'(');begin=core.rfind('\n',0,pos)+1;brace=core.index('{',pos);depth=1;end=brace+1
  while depth:depth+=(core[end]=='{')-(core[end]=='}');end+=1
  return core[begin:end]+'\n'
 names=['applyDest','applyDestAcc','normalizeFmacResult','calculateFmacExactResult','normalizeFmacExactResult','calculateFmacProductSticky','updateFmacFlags','applyFmacDest','applyFmacDestAcc','normalizeResult','queueStore','queueQ','queueP','startXgkick','queueClip','queueFcset','queueFsset','commitReadyPipelines','resetScheduler','readBranchVi','microAddressMask']
 if a.scheduler:names+=['addVfRead','addVfWrite','vfReadLanes','decodeUpperUsage','decodeLowerUsage','decodeInstructionPair','calculatePairReadyCycle','markPairWrites','queueVfWrite','queueViWrite','queueAccWrite','recordViWriteForBranch','pipelinesPending']
 helper_start=core.index('    constexpr uint8_t laneForComponent(');helper_end=core.index('    // One-shot context',helper_start)
 support='uint32_t capturedClip=0;unsigned calls=0;bool capturedFmac=false;float captured[4]{};uint8_t capturedDest=0;struct Store{uint32_t address,words[4];uint8_t mask;};Store store{};unsigned storeCalls=0;\n'
 support+='VU1Interpreter::VU1Interpreter(Unit u):m_unit(u){std::memset(&m_state,0,sizeof(m_state));}\nfloat VU1Interpreter::broadcast(const float* v,uint8_t c){return normalizeOperand(v[c&3]);}\nvoid VU1Interpreter::reportReservedInstruction(bool,uint32_t){std::abort();}\n'
 support+=core[helper_start:helper_end]+''.join(method(n) for n in names)
 def extract_free(source,signature):
  pos=source.index(signature);begin=source.rfind('\n',0,pos)+1;brace=source.index('{',pos);depth=1;end=brace+1
  while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
  return source[begin:end]+'\n'
 # referenceLower contains the full opcode oracle even when the current image
 # has no ESIN site, so keep the production helper available in every fixture.
 support+=extract_free(lower,'float vuEsin(float value)')
 if any(r['kind']=='ESIN' for r in metadata['lower_supported']):
  support+=extract_free(lower,'void VU1Interpreter::queueEsin(float value)')

if a.commit_stats:
 signature='void VU1Interpreter::commitReadyPipelines()'
 pos=support.index(signature);brace=support.index('{',pos);depth=1;stats_end=brace+1
 while depth:depth+=(support[stats_end]=='{')-(support[stats_end]=='}');stats_end+=1
 count_expr='std::popcount(m_flagPending)+std::popcount(m_storePending)+std::popcount(m_vfPending)+std::popcount(m_viPending)+std::popcount(m_accPending)+unsigned(m_fdiv.valid)+unsigned(m_efu[0].valid)+unsigned(m_efu[1].valid)'
 entry=f'\n unsigned beforePending={count_expr};++commitCalls;commitVisited+=beforePending;if(!beforePending)++commitEmpty;\n'
 tail=f'\n unsigned afterPending={count_expr};commitRetired+=beforePending-afterPending;if(beforePending==afterPending)++commitNoDue;\n'
 support=support[:brace+1]+entry+support[brace+1:stats_end-1]+tail+support[stats_end-1:]
 support='uint64_t commitCalls=0,commitVisited=0,commitRetired=0,commitEmpty=0,commitNoDue=0;\n'+support

program='''#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <random>
#include <cfenv>
#include <bit>
#include <algorithm>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "ps2_vu1_detail.h"
#define private public
#include "runtime/ps2_vu1.h"
#undef private
'''+upper+core[start:end]+support+reference+'\n#include "'+str(header)+'"\n'
if not a.xgkick:
 kicks={r['pc'] for r in metadata['lower_supported'] if r['kind']=='XGKICK'}
 metadata['paired_body_pcs']=[pc for pc in metadata['paired_body_pcs'] if pc not in kicks]
rejected=','.join(str(pc) for pc in sorted(set(metadata['unsupported_pcs'])|set(metadata['lower_unsupported_pcs'])))
rows=','.join('{'+str(row['pc'])+','+str(row['upper'])+'u,'+str(next(r['lower'] for r in metadata['lower_supported'] if r['pc']==row['pc']))+'u}' for row in metadata['supported'] if row['pc'] in metadata['paired_body_pcs'])
program+=r'''
int main(){
 std::fesetround(FE_TOWARDZERO);VU1Interpreter actual,reference;std::mt19937 rng(76);
 uint8_t memory[16384],originalMemory[16384];uint32_t sizes[]={8,16,4096,16384};
 struct Case{uint32_t pc,upper,lower;};Case cases[]={ROWS};
 for(auto test:cases)for(unsigned n=0;n<64;++n){
  for(auto& vf:actual.m_state.vf)for(float& v:vf){uint32_t bits=rng();std::memcpy(&v,&bits,4);}
  if(n<16){const uint32_t edges[]={0,0x80000000u,1,0x80000001u,0x00800000u,0x80800000u,0x3f800000u,0xbf800000u,0x7f7fffffu,0xff7fffffu,0x7f800000u,0xff800000u,0x7fc00000u,0xffc00000u,0x40000000u,0xc0000000u};for(unsigned r=0;r<32;r++)for(unsigned c=0;c<4;c++){uint32_t bits=edges[(n+r+c)%16];std::memcpy(&actual.m_state.vf[r][c],&bits,4);}}
  for(float& v:actual.m_state.acc){uint32_t bits=rng();std::memcpy(&v,&bits,4);}
  uint32_t bits=rng();std::memcpy(&actual.m_state.q,&bits,4);bits=rng();std::memcpy(&actual.m_state.i,&bits,4);
  {const uint32_t edges[]={0,1,0x3ff,0x400,0x7ff,0xffff,0x80000000u,0xffffffffu};actual.m_state.top=n<8?edges[n]:rng();actual.m_state.itop=n<8?edges[7-n]:rng();}
  for(auto& vi:actual.m_state.vi)vi=static_cast<int16_t>(rng());
  for(auto& b:memory)b=rng();std::memcpy(originalMemory,memory,sizeof(memory));uint32_t size=sizes[n%4];
  reference.m_state=actual.m_state;
  unsigned fd=(test.upper>>6)&31,dest=(test.upper>>21)&15;
  bool move=!(test.upper&0x80000000u) && ((test.lower>>25)&127)==64 && (test.lower&63)>=60;
  unsigned fs=(test.lower>>11)&31,ft=(test.lower>>16)&31,ldest=(test.lower>>21)&15;
  bool upperNop=(test.upper&63)>=60;
  unsigned lowerOp=(test.lower>>25)&127;
  bool immediate=test.upper&0x80000000u;
  bool lowerReads=move || (!immediate && lowerOp==1 && test.lower!=0x8000033cu);
  bool lowerWrites=move || (!immediate && lowerOp==0 && test.lower!=0);
  unsigned shadow=!immediate && !upperNop && fd && dest && ldest && ((lowerReads && fd==fs) || (lowerWrites && fd==ft))?fd:0;
  calls=0;storeCalls=0;store={};float oldVf[4],newVf[4];if(shadow)std::memcpy(oldVf,reference.m_state.vf[shadow],16);
  reference.execUpper(test.upper);
  if(shadow){std::memcpy(newVf,reference.m_state.vf[shadow],16);std::memcpy(reference.m_state.vf[shadow],oldVf,16);}
  referenceLower(reference,test.lower,test.upper,memory,size);
  if(shadow)std::memcpy(reference.m_state.vf[shadow],newVf,16);uint32_t expectedClip=capturedClip;unsigned expectedCalls=calls;float expected[4];std::memcpy(expected,captured,16);uint8_t expectedDest=capturedDest;bool expectedFmac=capturedFmac;
  unsigned expectedStores=storeCalls;Store expectedStore=store;
  calls=0;storeCalls=0;store={};struct Pair{uint32_t upper,lower;bool iBit;uint8_t upperVfShadowReg;};Pair pair{test.upper,test.lower,bool(test.upper&0x80000000u),static_cast<uint8_t>(shadow)};
  assert(BlackVuUpperAot::executePairBodies(actual,test.pc,pair,memory,size));
  assert(calls==expectedCalls);assert(capturedClip==expectedClip);assert(storeCalls==expectedStores);if(storeCalls){assert(store.address==expectedStore.address);assert(store.mask==expectedStore.mask);assert(!std::memcmp(store.words,expectedStore.words,16));}
  assert(!std::memcmp(memory,originalMemory,sizeof(memory)));
  if(calls){assert(!std::memcmp(expected,captured,16));assert(expectedDest==capturedDest);assert(expectedFmac==capturedFmac);}
  assert(!std::memcmp(&actual.m_state,&reference.m_state,sizeof(actual.m_state)));
  assert(actual.m_efuResourceReady==reference.m_efuResourceReady);for(unsigned k=0;k<actual.m_efu.size();k++){auto& x=actual.m_efu[k];auto& y=reference.m_efu[k];assert(x.valid==y.valid);if(x.valid)assert(x.readyCycle==y.readyCycle&&!std::memcmp(&x.value,&y.value,sizeof(float)));}
  assert(actual.m_currentUpperInstruction==reference.m_currentUpperInstruction);
 }
 auto before=actual.m_state;
 struct InvalidPair{uint32_t upper=0,lower=0;bool iBit=false;uint8_t upperVfShadowReg=0;};InvalidPair bad;
 assert(!BlackVuUpperAot::executePairBodies(actual,0xffffffff,bad));
 uint32_t rejected[]={0xffffffff,REJECTED};for(uint32_t pc:rejected)assert(!BlackVuUpperAot::executePairBodies(actual,pc,bad));
 for(auto test:cases){bad.upper=test.upper^1;bad.lower=test.lower;assert(!BlackVuUpperAot::executePairBodies(actual,test.pc,bad));bad.upper=test.upper;bad.lower=test.lower^1;assert(!BlackVuUpperAot::executePairBodies(actual,test.pc,bad));}
 assert(!std::memcmp(&before,&actual.m_state,sizeof(before)));
 printf("PASS: %zu paired bodies x64 inputs match upper+lower production oracles; VF shadow, memory/store payload and atomic fallback checked; retirement/timing/flags not covered\n",sizeof(cases)/sizeof(cases[0]));
}
'''.replace('ROWS',rows).replace('REJECTED',rejected)
if a.real_helpers:
 program=program.replace('uint8_t memory[16384],originalMemory[16384];','uint8_t memory[16384],referenceMemory[16384],originalMemory[16384];')
 program=program.replace('reference.m_state=actual.m_state;', 'actual.resetScheduler();reference.resetScheduler();actual.m_cycle=reference.m_cycle=100;actual.m_activeVuData=memory;reference.m_activeVuData=referenceMemory;actual.m_activeVuDataSize=reference.m_activeVuDataSize=size;std::memcpy(referenceMemory,memory,sizeof(memory));reference.m_state=actual.m_state;')
 program=program.replace('referenceLower(reference,test.lower,test.upper,memory,size);','referenceLower(reference,test.lower,test.upper,referenceMemory,size);')
 marker='assert(actual.m_currentUpperInstruction==reference.m_currentUpperInstruction);'
 checks=r"""
 assert(actual.m_fdiv.valid==reference.m_fdiv.valid);if(actual.m_fdiv.valid)assert(actual.m_fdiv.readyCycle==reference.m_fdiv.readyCycle && actual.m_fdiv.statusDi==reference.m_fdiv.statusDi && !std::memcmp(&actual.m_fdiv.value,&reference.m_fdiv.value,4));assert(actual.m_flagPending==reference.m_flagPending);assert(actual.m_storePending==reference.m_storePending);
 for(unsigned k=0;k<actual.m_flagPipeline.size();k++){
 auto& a=actual.m_flagPipeline[k];auto& b=reference.m_flagPipeline[k];assert(a.valid==b.valid);if(a.valid){assert(a.readyCycle==b.readyCycle);assert(a.issueCycle==b.issueCycle);assert(a.mac==b.mac);assert(a.status==b.status);assert(a.extraSticky==b.extraSticky);assert(a.writesMac==b.writesMac);assert(a.writesStatus==b.writesStatus);}}
 for(unsigned k=0;k<actual.m_storePipeline.size();k++){
 auto& a=actual.m_storePipeline[k];auto& b=reference.m_storePipeline[k];assert(a.valid==b.valid);if(a.valid){assert(a.readyCycle==b.readyCycle);assert(a.address==b.address);assert(a.words==b.words);assert(a.laneMask==b.laneMask);}}
 for(unsigned cycle=0;cycle<=13;cycle++){
 actual.m_cycle=reference.m_cycle=100+cycle;actual.commitReadyPipelines();reference.commitReadyPipelines();
 assert(!std::memcmp(memory,referenceMemory,sizeof(memory)));assert(!std::memcmp(&actual.m_state,&reference.m_state,sizeof(actual.m_state)));
 assert(actual.m_fdiv.valid==reference.m_fdiv.valid);if(actual.m_fdiv.valid)assert(actual.m_fdiv.readyCycle==reference.m_fdiv.readyCycle && actual.m_fdiv.statusDi==reference.m_fdiv.statusDi && !std::memcmp(&actual.m_fdiv.value,&reference.m_fdiv.value,4));assert(actual.m_flagPending==reference.m_flagPending);assert(actual.m_storePending==reference.m_storePending);
 }
 assert(!actual.m_flagPending && !actual.m_storePending);
"""
 program=program.replace(marker,marker+checks)
 program=program.replace('VF shadow, memory/store payload and atomic fallback checked; retirement/timing/flags not covered','real FMAC flags/store queues and commits at cycles 0..13 match; full scheduler not covered')

if a.scheduler:
 issue_start=core.index('        const uint32_t viWrites = decoded.lowerUsage.viWrite')
 issue_end=core.index('        m_state.vf[0][0] = 0.0f;',issue_start)
 issue=core[issue_start:issue_end]
 if '#if defined(BLACK_VU_AOT_AVAILABLE)' in issue:
  start=issue.index('#if defined(BLACK_VU_AOT_AVAILABLE)');end=issue.index('        if (decoded.iBit)',start)
  issue=issue[:start]+issue[end:]
  issue=issue.replace('        }\n\n        m_viBranchBackupValid', '        m_viBranchBackupValid',1)
 branch_start=issue.index('        if (decoded.iBit)');branch_end=issue.index('        m_viBranchBackupValid = false;',branch_start)
 original=issue[branch_start:branch_end].replace('execUpper(decoded.upper);','host.execUpper(decoded.upper);').replace('execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);','referenceLower(host,decoded.lower,decoded.upper,vuData,dataSize);')
 issue=issue[:branch_start]+'if(generated){assert(BlackVuUpperAot::executePairBodies(host,pc,decoded,vuData,dataSize));}else{'+original+'}'+issue[branch_end:]
 import re
 issue=re.sub(r'\bm_[A-Za-z0-9_]+',lambda m:'host.'+m.group(),issue)
 for name in ('queueVfWrite','queueViWrite','queueAccWrite','markPairWrites','recordViWriteForBranch','normalizeOperand'):issue=issue.replace(name+'(', 'host.'+name+'(')
 pc_start=core.index('        uint32_t nextPc = m_state.pc + 8u;',issue_end)
 pc_end=core.index('        const bool dHalt',pc_start)
 pc_step=core[pc_start:pc_end].replace('codeSize','16384u').replace('microAddressMask()','host.microAddressMask()')
 pc_step=re.sub(r'\bm_[A-Za-z0-9_]+',lambda m:'host.'+m.group(),pc_step)
 issue=issue.replace('const VfAccess','const VU1Interpreter::VfAccess').replace('kAccForwardLatency','VU1Interpreter::kAccForwardLatency')
 scheduler=r"""
void step(VU1Interpreter& host,uint32_t pc,uint32_t lower,uint32_t upper,uint8_t* vuData,uint32_t dataSize,bool generated,bool accurateVu){
 uint32_t words[]={lower,upper};auto decoded=host.decodeInstructionPair(reinterpret_cast<uint8_t*>(words),0);assert(!decoded.upperUsage.reserved && !decoded.lowerUsage.reserved);
 host.commitReadyPipelines();uint64_t ready=accurateVu?host.calculatePairReadyCycle(decoded):host.m_cycle;if(!accurateVu && host.m_fdiv.valid && (decoded.lowerUsage.waitQ || decoded.lowerUsage.pipeline==VU1Interpreter::PipelineFdiv))ready=std::max(ready,host.m_fdiv.readyCycle);if(!accurateVu && decoded.lowerUsage.pipeline==VU1Interpreter::PipelineEfu)ready=std::max(ready,host.m_efuResourceReady);if(!accurateVu && decoded.lowerUsage.waitP)for(auto& entry:host.m_efu)if(entry.valid)ready=std::max(ready,entry.readyCycle);while(host.m_cycle<ready){++host.m_cycle;host.commitReadyPipelines();}
 ISSUE
 host.m_state.vf[0][0]=host.m_state.vf[0][1]=host.m_state.vf[0][2]=0;host.m_state.vf[0][3]=1;host.m_state.vi[0]=0;
 PC_STEP
 ++host.m_cycle;host.commitReadyPipelines();host.m_state.cycles=host.m_cycle;
}
void compare(VU1Interpreter& a,VU1Interpreter& b,uint8_t* am,uint8_t* bm){
 assert(!std::memcmp(&a.m_state,&b.m_state,sizeof(a.m_state)));assert(!std::memcmp(am,bm,16384));assert(a.m_cycle==b.m_cycle);
 assert(a.m_fdiv.valid==b.m_fdiv.valid);if(a.m_fdiv.valid)assert(a.m_fdiv.readyCycle==b.m_fdiv.readyCycle && a.m_fdiv.statusDi==b.m_fdiv.statusDi && !std::memcmp(&a.m_fdiv.value,&b.m_fdiv.value,4));
 assert(a.m_efuResourceReady==b.m_efuResourceReady);for(unsigned k=0;k<a.m_efu.size();k++){auto& x=a.m_efu[k];auto& y=b.m_efu[k];assert(x.valid==y.valid);if(x.valid)assert(x.readyCycle==y.readyCycle&&!std::memcmp(&x.value,&y.value,sizeof(float)));}
 assert(a.m_vfReady==b.m_vfReady && a.m_viReady==b.m_viReady && a.m_accReady==b.m_accReady);
 assert(a.m_flagPending==b.m_flagPending && a.m_storePending==b.m_storePending && a.m_vfPending==b.m_vfPending && a.m_viPending==b.m_viPending && a.m_accPending==b.m_accPending);
 assert(a.m_vfLatestWrite==b.m_vfLatestWrite && a.m_viLatestWrite==b.m_viLatestWrite && a.m_accLatestWrite==b.m_accLatestWrite);
 for(unsigned k=0;k<a.m_flagPipeline.size();k++){auto& x=a.m_flagPipeline[k];auto& y=b.m_flagPipeline[k];assert(x.valid==y.valid);if(x.valid){assert(x.readyCycle==y.readyCycle && x.issueCycle==y.issueCycle && x.mac==y.mac && x.status==y.status && x.extraSticky==y.extraSticky && x.clip==y.clip && x.writesClip==y.writesClip && x.writesStatus==y.writesStatus && x.writesSticky==y.writesSticky);}}
 for(unsigned k=0;k<a.m_storePipeline.size();k++){auto& x=a.m_storePipeline[k];auto& y=b.m_storePipeline[k];assert(x.valid==y.valid);if(x.valid)assert(x.readyCycle==y.readyCycle && x.address==y.address && x.laneMask==y.laneMask && x.words==y.words);}
 for(unsigned k=0;k<a.m_vfWritePipeline.size();k++){auto& x=a.m_vfWritePipeline[k];auto& y=b.m_vfWritePipeline[k];assert(x.valid==y.valid);if(x.valid)assert(x.readyCycle==y.readyCycle && x.reg==y.reg && x.laneMask==y.laneMask && x.sequence==y.sequence && !std::memcmp(x.value.data(),y.value.data(),16));}
 for(unsigned k=0;k<a.m_accWritePipeline.size();k++){auto& x=a.m_accWritePipeline[k];auto& y=b.m_accWritePipeline[k];assert(x.valid==y.valid);if(x.valid)assert(x.readyCycle==y.readyCycle && x.laneMask==y.laneMask && x.sequence==y.sequence && !std::memcmp(x.value.data(),y.value.data(),16));}
 for(unsigned k=0;k<a.m_viWritePipeline.size();k++){auto& x=a.m_viWritePipeline[k];auto& y=b.m_viWritePipeline[k];assert(x.valid==y.valid);if(x.valid)assert(x.readyCycle==y.readyCycle && x.reg==y.reg && x.sequence==y.sequence && x.value==y.value);}
}
int main(){std::fesetround(FE_TOWARDZERO);std::mt19937 rng(981);struct Case{uint32_t pc,upper,lower;};Case cases[]={ROWS};
 for(bool accurate:{false,true}){VU1Interpreter actual,reference;uint8_t memory[16384],refMemory[16384];for(auto& b:memory)b=rng();std::memcpy(refMemory,memory,sizeof(memory));
 for(auto& vf:actual.m_state.vf)for(float& v:vf){uint32_t bits=rng();std::memcpy(&v,&bits,4);}for(auto& vi:actual.m_state.vi)vi=static_cast<int16_t>(rng());actual.m_state.top=rng();actual.m_state.itop=rng();reference.m_state=actual.m_state;
 actual.m_activeVuData=memory;reference.m_activeVuData=refMemory;actual.m_activeVuDataSize=reference.m_activeVuDataSize=16384;
 for(unsigned n=0;n<20000;n++){auto c=cases[rng()%(sizeof(cases)/sizeof(cases[0]))];step(actual,c.pc,c.lower,c.upper,memory,16384,true,accurate);step(reference,c.pc,c.lower,c.upper,refMemory,16384,false,accurate);compare(actual,reference,memory,refMemory);}
 for(unsigned n=0;n<16;n++){++actual.m_cycle;++reference.m_cycle;actual.commitReadyPipelines();reference.commitReadyPipelines();compare(actual,reference,memory,refMemory);}
 assert(!actual.m_vfPending && !actual.m_viPending && !actual.m_flagPending && !actual.m_storePending);
 }
 puts("PASS: 40000 sequenced pairs with production decode, stalls, VF/VI queues, flags and store commits; fast/accurate paths match. Branch issue/delay commits compared; original CFG/budgets/XGKICK not covered.");}
""".replace('ISSUE',issue).replace('PC_STEP',pc_step).replace('ROWS',rows)
 program=program[:program.index('\nint main(){')]+scheduler

if a.timing:
 timing=r"""
#include <chrono>
#include <vector>
int main(){std::fesetround(FE_TOWARDZERO);struct Case{uint32_t pc,upper,lower;};Case cases[]={ROWS};
for(bool accurate:{false,true}){VU1Interpreter host;uint8_t memory[16384];std::mt19937 rng(981);for(auto& b:memory)b=rng();for(auto& vf:host.m_state.vf)for(float& v:vf){uint32_t bits=rng();std::memcpy(&v,&bits,4);}for(auto& vi:host.m_state.vi)vi=static_cast<int16_t>(rng());host.m_activeVuData=memory;host.m_activeVuDataSize=16384;std::vector<unsigned> sequence(500000);for(auto& index:sequence)index=rng()%(sizeof(cases)/sizeof(cases[0]));
auto begin=std::chrono::steady_clock::now();for(unsigned index:sequence){auto c=cases[index];step(host,c.pc,c.lower,c.upper,memory,16384,true,accurate);}auto end=std::chrono::steady_clock::now();
uint64_t hash=14695981039346656037ull;auto consume=[&](const void* p,size_t n){auto* bytes=static_cast<const uint8_t*>(p);for(size_t k=0;k<n;k++){hash^=bytes[k];hash*=1099511628211ull;}};consume(&host.m_state,sizeof(host.m_state));consume(memory,sizeof(memory));printf("{\"accurate\":%s,\"pairs\":500000,\"seconds\":%.9f,\"checksum\":\"%016llx\"}\n",accurate?"true":"false",std::chrono::duration<double>(end-begin).count(),(unsigned long long)hash);
}}
""".replace('ROWS',rows).replace('500000',str(a.iterations))
 program=program[:program.index('\nint main(){')]+timing
 if a.commit_stats:
  # Runtime commits once on entry and after each advanced cycle, not at every pair entry.
  program=program.replace('host.commitReadyPipelines();uint64_t ready=accurateVu?', 'uint64_t ready=accurateVu?')
  program=program.replace('auto begin=std::chrono::steady_clock::now();','commitCalls=commitVisited=commitRetired=commitEmpty=commitNoDue=0;auto begin=std::chrono::steady_clock::now();')
  # Additional JSON line; instrumentation invalidates throughput timings.
  marker='(unsigned long long)hash);'
  report=r'printf("{\"commit_statistics\":true,\"accurate\":%s,\"calls\":%llu,\"occupied_slots_visited\":%llu,\"retired_slots\":%llu,\"empty_calls\":%llu,\"no_due_calls\":%llu}\n",accurate?"true":"false",(unsigned long long)commitCalls,(unsigned long long)commitVisited,(unsigned long long)commitRetired,(unsigned long long)commitEmpty,(unsigned long long)commitNoDue);'
  program=program.replace(marker,marker+report)

if a.xgkick:
 support_xg=method('progressXgkick')+method('advanceOneCycle')
 program=program[:program.index('\nint main(){')].replace('++host.m_cycle;host.commitReadyPipelines();host.m_state.cycles=host.m_cycle;', 'host.advanceOneCycle();')+'''
#include <vector>
struct Event { uint64_t cycle; std::vector<uint8_t> bytes; };
VU1Interpreter* activeSender=nullptr;std::vector<Event> eventsA,eventsB;
// Capture at the GIF consumer boundary; this does not validate GS rasterization.
void VU1Interpreter::finishXgkick(){if(!m_xgkick.active)return;auto& events=this==activeSender?eventsA:eventsB;events.push_back({m_cycle,{m_xgkick.packet.data(),m_xgkick.packet.data()+m_xgkick.totalBytes}});m_xgkick.active=false;}
'''+support_xg+r'''
void compareTransfer(VU1Interpreter& a,VU1Interpreter& b,uint8_t* am,uint8_t* bm){compare(a,b,am,bm);auto& x=a.m_xgkick;auto& y=b.m_xgkick;assert(x.active==y.active && x.sourceAddress==y.sourceAddress && x.totalBytes==y.totalBytes && x.copiedBytes==y.copiedBytes && x.currentTagEnd==y.currentTagEnd && x.cycleCredit==y.cycleCredit && x.issueCycle==y.issueCycle && x.currentTagEop==y.currentTagEop);assert(!std::memcmp(x.packet.data(),y.packet.data(),x.copiedBytes));assert(eventsA.size()==eventsB.size());for(unsigned k=0;k<eventsA.size();k++)assert(eventsA[k].cycle==eventsB[k].cycle && eventsA[k].bytes==eventsB[k].bytes);}
int main(){struct Case{uint32_t pc,upper,lower;};Case cases[]={ROWS};unsigned count=0;
for(auto c:cases){if(((c.lower&3)|((c.lower>>4)&124))!=108 || (c.lower>>25&127)!=64)continue;
for(bool accurate:{false,true})for(unsigned format=0;format<4;format++)for(unsigned wrap=0;wrap<2;wrap++)for(unsigned loops=0;loops<4;loops++){
VU1Interpreter a,b;activeSender=&a;eventsA.clear();eventsB.clear();uint8_t am[16384]{},bm[16384]{};unsigned reg=c.lower>>11&15;unsigned source=reg==0?0:wrap?16368:32;a.m_state.vi[reg]=reg==0?0:wrap?-1:source/16;a.m_state.vf[0][3]=1;b.m_state=a.m_state;a.m_activeVuData=am;b.m_activeVuData=bm;a.m_activeVuDataSize=b.m_activeVuDataSize=16384;
// First GIFtag has no EOP; second ends the chain.
unsigned payload=format==0?loops*2:format==1?((loops*2+1)&~1)*8/16:loops;unsigned bytes=16+payload*16+16;std::vector<uint8_t> expected(bytes);uint64_t first=loops|(uint64_t(format)<<58)|(2ull<<60),last=(1ull<<15)|(1ull<<60);std::memcpy(expected.data(),&first,8);for(unsigned k=16;k<16+payload*16;k++)expected[k]=k*13;std::memcpy(expected.data()+16+payload*16,&last,8);for(unsigned k=0;k<bytes;k++)am[(source+k)%16384]=expected[k];std::memcpy(bm,am,16384);
a.m_state.pc=b.m_state.pc=c.pc;step(a,c.pc,c.lower,c.upper,am,16384,true,accurate);step(b,c.pc,c.lower,c.upper,bm,16384,false,accurate);compareTransfer(a,b,am,bm);assert(a.m_xgkick.active && eventsA.empty());
// LSU store issued at cycle1 must become visible before PATH1 reads payload.
uint32_t stored[4]={0x11223344,0x55667788,0x99aabbcc,0xddeeff00};if(payload){a.queueStore((source+16)%16384,stored,15);b.queueStore((source+16)%16384,stored,15);std::memcpy(expected.data()+16,stored,16);}
unsigned guard=0;while(a.m_xgkick.active || a.m_storePending){a.advanceOneCycle();b.advanceOneCycle();compareTransfer(a,b,am,bm);assert(++guard<100);}
assert(eventsA.size()==1 && eventsA[0].bytes==expected);assert(eventsA[0].cycle==bytes/8-1);++count;
}}printf("PASS: %u XGKICK transfers; production PATH1 parser/cycle progress, circular memory, formats, chained tags/EOP and LSU order match; consumer captured, rasterization not covered\n",count);}
'''.replace('ROWS',rows)

if a.program:
 controlled=r"""
struct Case{uint32_t pc,upper,lower;};Case cases[]={ROWS};
bool runSlice(VU1Interpreter& host,uint8_t* memory,uint64_t budget,bool generated,bool accurate){
 uint64_t end=host.m_cycle+budget;
 while(host.m_cycle<end){
 assert(host.m_state.pc/8<sizeof(cases)/sizeof(cases[0]));auto c=cases[host.m_state.pc/8];assert(c.pc==host.m_state.pc);
 uint32_t words[]={c.lower,c.upper};auto decoded=host.decodeInstructionPair(reinterpret_cast<uint8_t*>(words),0);
 host.commitReadyPipelines();uint64_t ready=accurate?host.calculatePairReadyCycle(decoded):host.m_cycle;if(!accurate && host.m_fdiv.valid && (decoded.lowerUsage.waitQ || decoded.lowerUsage.pipeline==VU1Interpreter::PipelineFdiv))ready=std::max(ready,host.m_fdiv.readyCycle);
 if(ready>=end){while(host.m_cycle<end){++host.m_cycle;host.commitReadyPipelines();}host.m_state.cycles=host.m_cycle;return false;}
 bool previousE=host.m_state.ebit;
 step(host,c.pc,c.lower,c.upper,memory,16384,generated,accurate);
 if(previousE){while(host.pipelinesPending()){++host.m_cycle;host.commitReadyPipelines();}host.m_state.ebit=false;host.m_state.cycles=host.m_cycle;return true;}
 if(decoded.eBit)host.m_state.ebit=true;
 }
 return false;
}
int main(){std::fesetround(FE_TOWARDZERO);std::mt19937 rng(88);unsigned slices=0;
 for(bool accurate:{false,true})for(unsigned budget:{1u,2u,3u,7u,16u})for(unsigned trial=0;trial<100;trial++){
 VU1Interpreter actual,reference,full;uint8_t am[16384]{},rm[16384]{},fm[16384]{};
 for(auto& vf:actual.m_state.vf)for(float& v:vf)v=float(int(rng()%200)-100)/8;
 actual.m_state.vi[0]=0;reference.m_state=full.m_state=actual.m_state;
 actual.m_activeVuData=am;reference.m_activeVuData=rm;full.m_activeVuData=fm;actual.m_activeVuDataSize=reference.m_activeVuDataSize=full.m_activeVuDataSize=16384;
 assert(runSlice(full,fm,1000,false,accurate));bool ended=false;
 for(unsigned n=0;n<1000 && !ended;n++){bool a=runSlice(actual,am,budget,true,accurate),b=runSlice(reference,rm,budget,false,accurate);assert(a==b);compare(actual,reference,am,rm);ended=a;++slices;}
 assert(ended);compare(actual,full,am,fm);assert(actual.m_state.pc==64);assert(!actual.m_state.branchPending && !actual.m_state.ebit);
 }
 printf("PASS: 1000 terminating PC-driven programs, %u budget slices; loop/delay slot/E-bit, stalls and resumes equal interpreter and uninterrupted execution. D/T/XGKICK not covered.\n",slices);
}
""".replace('ROWS',rows)
 program=program[:program.index('\nint main(){')]+controlled

if metadata.get('unit',1)==0:
 if a.xgkick:raise SystemExit('XGKICK transfer harness applies to VU1 only.')
 program=program.replace('m_unit(u)','m_unit(Unit::VU0)').replace('16384','4096')

with tempfile.TemporaryDirectory() as td:
 path=Path(td);(path/'test.cpp').write_text(program)
 binary=Path(a.save_binary).resolve() if a.save_binary else path/'test'
 flags=[] if a.timing else ['-fsanitize=address,undefined']
 subprocess.run(['clang++','-std=c++20','-O2',*flags,'-I'+str(base/'include'),'-I'+str(base/'src/lib/vu'),str(path/'test.cpp'),'-o',str(binary)],check=True)
 if not a.save_binary:subprocess.run([str(binary)],check=True)
