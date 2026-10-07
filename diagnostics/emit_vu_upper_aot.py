#!/usr/bin/env python3
"""Experimental VU paired-body emitter with conservative sequential block dispatch."""
import argparse,json,struct
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('image');p.add_argument('output');p.add_argument('--coverage');p.add_argument('--inline-mode',choices=('compiler','helper','all'),default='helper');args=p.parse_args()
image=Path(args.image);data=image.read_bytes();h=14695981039346656037
for v in data:h=((h^v)*1099511628211)&((1<<64)-1)
unit=int(image.name[2]);pcs=set(range(0,len(data),8))
if args.coverage:
 b=Path(args.coverage).read_bytes();pcs={pc for u,pc,ch in struct.iter_unpack('<IIQ',b[:len(b)//16*16]) if u==unit and ch==h}
lines=['#pragma once','#include <cstdint>','#include <cmath>','#include <limits>','namespace BlackVuUpperAot {',f'inline constexpr uint64_t image_hash=0x{h:016x}ull;','template<class Host> bool execute(Host& host, uint32_t pc) {','switch(pc) {'];supported=[];unsupported=[]
for pc in sorted(pcs):
 lower,upper=struct.unpack_from('<II',data,pc);op=upper&63;special=(upper&3)|((upper>>4)&124)
 if op>=60 and special in (*range(16,24),29,31):
  fs=upper>>11&31;ft=upper>>16&31;dest=upper>>21&15
  lines+=[f'case 0x{pc:04x}: {{',f'host.m_currentUpperInstruction=0x{upper:08x}u;']
  if special==31:
   lines+=[f'uint32_t w;std::memcpy(&w,&host.m_state.vf[{ft}][3],4);',
           'int32_t limit=(w&0x7f800000u)?static_cast<int32_t>(w&0x7fffffffu):0x007fffff;',
           'uint32_t flags=0;',
           'auto exceeds=[limit](float value,uint32_t sign){uint32_t bits;std::memcpy(&bits,&value,4);bits^=sign;int32_t ordered;std::memcpy(&ordered,&bits,4);return ordered>limit;};']
   for c in range(3):
    for sign in range(2):lines+=[f'if(exceeds(host.m_state.vf[{fs}][{c}],0x{sign*0x80000000:08x}u))flags|={1<<(c*2+sign)}u;']
   lines+=['host.queueClip(flags);']
  else:
   lines+=['float result[4];']
   scale=(1,16,4096,32768)[special&3] if special!=29 else 1
   for c in range(4):
    if special<20:lines+=[f'int32_t v{c};std::memcpy(&v{c},&host.m_state.vf[{fs}][{c}],4);result[{c}]=static_cast<float>(v{c})/{scale}.0f;']
    elif special<24:
     lines+=[f'double scaled{c}=static_cast<double>(host.normalizeOperand(host.m_state.vf[{fs}][{c}]))*{scale}.0;',f'int32_t v{c}=scaled{c}>=static_cast<double>(INT32_MAX)?INT32_MAX:scaled{c}<=static_cast<double>(INT32_MIN)?INT32_MIN:static_cast<int32_t>(scaled{c});',f'std::memcpy(&result[{c}],&v{c},4);']
    else:lines+=[f'result[{c}]=std::fabs(host.normalizeOperand(host.m_state.vf[{fs}][{c}]));']
   lines+=[f'host.applyDest(host.m_state.vf[{ft}],result,{dest}u);']
  lines+=['return true;','}'];supported.append({'pc':pc,'upper':upper});continue
 nop=op>=60 and special in (47,48)
 acc_dest=op>=60 and special in (*range(16),*range(24,29),30,*range(32,43),44,45,46)
 if acc_dest:op=special
 operation='+' if op in (*range(4),32,34,40) else '-' if op in (*range(4,8),36,38,44) else '*' if op in (*range(24,28),28,30,42) else None
 madd=op in (*range(8,12),33,35,41)
 msub=op in (*range(12,16),37,39,45)
 maximum=op in (*range(16,20),29,43)
 minimum=op in (*range(20,24),31,47)
 if not nop and not (operation or madd or msub or maximum or minimum or op==46):unsupported.append(pc);continue
 lines+=[f'case 0x{pc:04x}: {{',f'host.m_currentUpperInstruction=0x{upper:08x}u;']
 if not nop:
  fs=upper>>11&31;ft=upper>>16&31;fd=upper>>6&31;dest=upper>>21&15
  lines+=['float result[4];']
  for c in range(4):
   left=f'host.normalizeOperand(host.m_state.vf[{fs}][{c}])'
   rc=op&3 if op<28 else c
   right=f'host.normalizeOperand(host.m_state.vf[{ft}][{rc}])'
   if op in (28,32,33,36,37):right='host.normalizeOperand(host.m_state.q)'
   elif op in (29,30,31,34,35,38,39):right='host.normalizeOperand(host.m_state.i)'
   acc=f'host.normalizeOperand(host.m_state.acc[{c}])'
   expr=f'{left} {operation} {right}' if operation else f'{acc} {"+" if madd else "-"} {left} * {right}'
   if maximum or minimum:expr=f'({left} {">" if maximum else "<"} {right}) ? {left} : {right}'
   if op==46:
    expr='0.0f' if c==3 else ('' if acc_dest else f'{acc} - ')+f'host.normalizeOperand(host.m_state.vf[{fs}][{(c+1)%3}]) * host.normalizeOperand(host.m_state.vf[{ft}][{(c+2)%3}])'
   lines+=[f'result[{c}]={expr};']
  helper='applyDest' if maximum or minimum else 'applyFmacDest'
  lines+=[f'host.applyFmacDestAcc(result,{dest}u);' if acc_dest else f'host.{helper}(host.m_state.vf[{fd}],result,{dest}u);']
 lines+=['return true;','}'];supported.append({'pc':pc,'upper':upper})
lines+=['default: return false;','}','}']
# Retain emitted upper bodies to combine them with lower bodies under one PC dispatch.
upper_bodies={}
for index,line in enumerate(lines):
 if line.startswith('case 0x'):
  pc=int(line.split(':')[0][5:],16);end=index+1
  while lines[end]!='return true;':end+=1
  upper_bodies[pc]=lines[index+1:end]
lower_bodies={}
lower_supported=[];lower_unsupported=[]
lines+=['template<class Host> bool executeLower(Host& host, uint32_t pc, uint8_t* vuData=nullptr, uint32_t dataSize=0) {','switch(pc) {']
for pc in sorted(pcs):
 lower,upper=struct.unpack_from('<II',data,pc);op=lower>>25&127;funct=lower&63
 it=lower>>16&15;is_=lower>>11&15;id_=lower>>6&15;body=[];kind=None
 if upper&0x80000000:
  body=[f'uint32_t bits=0x{lower:08x}u; float immediate; std::memcpy(&immediate,&bits,4); host.m_state.i=host.normalizeOperand(immediate);'];kind='immediate'
 elif lower in (0,0x8000033c):kind='NOP'
 elif op in (0,1,4,5):
  kind={0:'LQ',1:'SQ',4:'ILW',5:'ISW'}[op]
  imm=lower&2047;imm=imm-2048 if imm&1024 else imm
  dest=lower>>21&15;vfS=lower>>11&31;vfT=lower>>16&31;base=it if op==1 else is_
  body=['if(vuData==nullptr || dataSize==0) return false;',f'uint32_t addr=static_cast<uint32_t>(host.m_state.vi[{base}]+({imm}))*16u;',
        'addr &= dataSize-1;', 'if(addr+16<=dataSize) {']
  if op==0:body+=['float result[4]; std::memcpy(result,vuData+addr,16);',f'host.applyDest(host.m_state.vf[{vfT}],result,{dest}u);']
  elif op==1:body+=['uint32_t words[4];',f'std::memcpy(words,host.m_state.vf[{vfS}],16);',f'host.queueStore(addr,words,{dest}u);']
  elif op==4:
   comp=0 if dest&8 else 1 if dest&4 else 2 if dest&2 else 3
   body+=[f'uint32_t word; std::memcpy(&word,vuData+addr+{comp*4},4);']
   if it:body+=[f'host.m_state.vi[{it}]=static_cast<int16_t>(word&0xffffu);']
  else:body+=[f'uint32_t value=static_cast<uint16_t>(host.m_state.vi[{it}]&0xffff);','uint32_t words[4]={value,value,value,value};',f'host.queueStore(addr,words,{dest}u);']
  body+=['}']
 elif op in (16,17,18,19,20,21,22,23,24,26,27,28):
  kind={16:'FCEQ',17:'FCSET',18:'FCAND',19:'FCOR',20:'FSEQ',21:'FSSET',22:'FSAND',23:'FSOR',24:'FMEQ',26:'FMAND',27:'FMOR',28:'FCGET'}[op]
  imm24=lower&0xffffff;imm12=((lower>>21&1)<<11)|(lower&2047)
  if op==17:body=[f'host.queueFcset({imm24}u);']
  elif op==21:body=[f'host.queueFsset({imm12}u);']
  elif op in (16,18,19):
   expr=f'(host.m_state.clip&0xffffffu)=={imm24}u' if op==16 else f'(host.m_state.clip&{imm24}u)!=0' if op==18 else f'(host.m_state.clip|{imm24}u)==0xffffffu'
   body=[f'host.m_state.vi[1]=({expr})?1:0;']
  elif it:
   expr=f'(host.m_state.status&0xfffu)=={imm12}u?1:0' if op==20 else f'(host.m_state.status&0xfffu) {"&" if op==22 else "|"} {imm12}u' if op in (22,23) else f'(host.m_state.mac&0xffffu)==static_cast<uint16_t>(host.m_state.vi[{is_}])?1:0' if op==24 else f'host.m_state.mac {"&" if op==26 else "|"} static_cast<uint16_t>(host.m_state.vi[{is_}])' if op in (26,27) else 'host.m_state.clip&0xfffu'
   body=[f'host.m_state.vi[{it}]=static_cast<int32_t>({expr});']
 elif op in (32,33,36,37,40,41,44,45,46,47):
  kind={32:'B',33:'BAL',36:'JR',37:'JALR',40:'IBEQ',41:'IBNE',44:'IBLTZ',45:'IBGTZ',46:'IBLEZ',47:'IBGEZ'}[op]
  imm=lower&2047;imm=imm-2048 if imm&1024 else imm
  left=f'static_cast<int16_t>(host.readBranchVi({is_}))'
  right=f'static_cast<int16_t>(host.readBranchVi({it}))'
  condition='true' if op in (32,33,36,37) else f'{left} {"==" if op==40 else "!="} {right}' if op in (40,41) else f'{left} '+{44:'<',45:'>',46:'<=',47:'>='}[op]+' 0'
  target=f'(static_cast<uint32_t>(static_cast<uint16_t>(host.readBranchVi({is_})))*8u)' if op in (36,37) else f'(host.m_state.pc+8u+({imm}*8))'
  body=[f'if({condition}) {{',f'uint32_t target={target} & host.microAddressMask();']
  if op in (33,37) and it:body+=[f'host.m_state.vi[{it}]=static_cast<int32_t>((host.m_state.pc+16u)/8u);']
  body+=['host.m_state.branchPending=true;host.m_state.branchTarget=target;host.m_state.branchDelay=1;','}']
 elif op in (8,9):
  imm=(lower&2047)|((lower>>10)&0x7800)
  if it:body=[f'host.m_state.vi[{it}]=static_cast<int16_t>(host.m_state.vi[{is_}] {"+" if op==8 else "-"} {imm});']
  kind='IADDIU' if op==8 else 'ISUBIU'
 elif op==64 and funct in (48,49,50,52,53):
  kind={48:'IADD',49:'ISUB',50:'IADDI',52:'IAND',53:'IOR'}[funct]
  if funct==50:
   imm=lower>>6&31;imm=imm-32 if imm&16 else imm
   if it:body=[f'host.m_state.vi[{it}]=static_cast<int16_t>(host.m_state.vi[{is_}]+({imm}));']
  elif id_:
   operation={48:'+',49:'-',52:'&',53:'|'}[funct]
   expr=f'host.m_state.vi[{is_}] {operation} host.m_state.vi[{it}]'
   if funct in (48,49):expr=f'static_cast<int16_t>({expr})'
   body=[f'host.m_state.vi[{id_}]={expr};']
 elif op==64 and funct>=60 and ((lower&3)|((lower>>4)&124))==108:
  kind='XGKICK';body=[f'host.startXgkick(static_cast<uint32_t>(static_cast<uint16_t>(host.m_state.vi[{is_}])));']
 elif op==64 and funct>=60 and ((lower&3)|((lower>>4)&124)) in (104,105):
  special=(lower&3)|((lower>>4)&124);kind='XTOP' if special==104 else 'XITOP'
  if it:body=[f'host.m_state.vi[{it}]=static_cast<int32_t>(host.m_state.{"top" if special==104 else "itop"}&0x3ffu);']
 elif op==64 and funct>=60 and ((lower&3)|((lower>>4)&124)) in (56,57,58,59):
  special=(lower&3)|((lower>>4)&124);kind={56:'DIV',57:'SQRT',58:'RSQRT',59:'WAITQ'}[special];fs=lower>>11&31;ft=lower>>16&31;fsf=lower>>21&3;ftf=lower>>23&3
  if special==57:body=[f'float val=host.normalizeOperand(host.m_state.vf[{ft}][{ftf}]);','host.queueQ(std::sqrt(std::fabs(val)),7u,val<0.0f?0x10u:0u);']
  elif special!=59:
   body=[f'float num=host.normalizeOperand(host.m_state.vf[{fs}][{fsf}]);',f'float operand=host.normalizeOperand(host.m_state.vf[{ft}][{ftf}]);']
   body+=['float den=operand;uint32_t statusDi=0;'] if special==56 else ['float den=std::sqrt(std::fabs(operand));uint32_t statusDi=operand<0.0f?0x10u:0u;']
   sign='std::signbit(num)!=std::signbit(den)' if special==56 else 'std::signbit(num)'
   body+=['float result=0.0f;','if(den!=0.0f)result=num/den;',f'else {{statusDi=num==0.0f?0x10u:0x20u;result=({sign})?-std::numeric_limits<float>::max():std::numeric_limits<float>::max();}}','uint32_t ignoredFlags=0;result=host.normalizeResult(result,ignoredFlags);',f'host.queueQ(result,{7 if special==56 else 13}u,statusDi);']
 elif op==64 and funct>=60 and ((lower&3)|((lower>>4)&124)) in (62,63):
  special=(lower&3)|((lower>>4)&124);kind='ILWR' if special==62 else 'ISWR';dest=lower>>21&15
  body=['if(vuData==nullptr || dataSize==0) return false;',f'uint32_t addr=static_cast<uint32_t>(static_cast<uint16_t>(host.m_state.vi[{is_}]))*16u;','addr &= dataSize-1;','if(addr+16<=dataSize) {']
  if special==62:
   comp=0 if dest&8 else 1 if dest&4 else 2 if dest&2 else 3
   body+=[f'uint32_t word;std::memcpy(&word,vuData+addr+{comp*4},4);']
   if it:body+=[f'host.m_state.vi[{it}]=static_cast<int16_t>(word&0xffffu);']
  else:body+=[f'uint32_t value=static_cast<uint16_t>(host.m_state.vi[{it}]&0xffff);','uint32_t words[4]={value,value,value,value};',f'host.queueStore(addr,words,{dest}u);']
  body+=['}']
 elif op==64 and funct>=60 and ((lower&3)|((lower>>4)&124)) in (60,61):
  special=(lower&3)|((lower>>4)&124);kind='MTIR' if special==60 else 'MFIR';fs=lower>>11&31;ft=lower>>16&31;dest=lower>>21&15
  if special==60:
   if it:body=[f'uint32_t bits;std::memcpy(&bits,&host.m_state.vf[{fs}][{lower>>21&3}],4);host.m_state.vi[{it}]=static_cast<int16_t>(bits&0xffffu);']
  else:body=[f'int32_t value=static_cast<int16_t>(host.m_state.vi[{is_}]&0xffff);','float result[4];std::memcpy(&result[0],&value,4);result[1]=result[2]=result[3]=result[0];',f'host.applyDest(host.m_state.vf[{ft}],result,{dest}u);']
 elif op==64 and funct>=60 and ((lower&3)|((lower>>4)&124))==100:
  kind='MFP';vfT=lower>>16&31;dest=lower>>21&15
  body=[f'float result[4]={{host.m_state.p,host.m_state.p,host.m_state.p,host.m_state.p}};host.applyDest(host.m_state.vf[{vfT}],result,{dest}u);']
 elif op==64 and funct>=60 and ((lower&3)|((lower>>4)&124)) in (120,121,122):
  special=(lower&3)|((lower>>4)&124);component=(lower>>21)&3;vfS=(lower>>11)&31
  kind={120:'ESQRT',121:'ESIN',122:'ERCPR'}[special]
  if special==120:body=[f'float value=host.normalizeOperand(host.m_state.vf[{vfS}][{component}]);host.queueP(value>=0.0f?std::sqrt(value):value,12u);']
  elif special==121:body=[f'host.queueEsin(host.m_state.vf[{vfS}][{component}]);']
  else:body=[f'float value=host.normalizeOperand(host.m_state.vf[{vfS}][{component}]);host.queueP(value!=0.0f?1.0f/value:value,12u);']
 elif op==64 and funct>=60 and ((lower&3)|((lower>>4)&124)) in (52,53,54,55):
  special=(lower&3)|((lower>>4)&124);kind={52:'LQI',53:'SQI',54:'LQD',55:'SQD'}[special]
  load=special in (52,54);base=is_ if load else it;fs=lower>>11&31;ft=lower>>16&31;dest=lower>>21&15
  body=['if(vuData==nullptr || dataSize==0) return false;']
  if special in (54,55) and base:body+=[f'host.m_state.vi[{base}]=static_cast<int16_t>(host.m_state.vi[{base}]-1);']
  body+=[f'uint32_t addr=static_cast<uint32_t>(static_cast<uint16_t>(host.m_state.vi[{base}]))*16u;', 'addr &= dataSize-1;','if(addr+16<=dataSize) {']
  if load:body+=['float result[4];std::memcpy(result,vuData+addr,16);',f'host.applyDest(host.m_state.vf[{ft}],result,{dest}u);']
  else:body+=['uint32_t words[4];',f'std::memcpy(words,host.m_state.vf[{fs}],16);',f'host.queueStore(addr,words,{dest}u);']
  body+=['}']
  if special in (52,53) and base:body+=[f'host.m_state.vi[{base}]=static_cast<int16_t>(host.m_state.vi[{base}]+1);']
 elif op==64 and funct>=60 and ((lower&3)|((lower>>4)&124)) in (48,49):
  special=(lower&3)|((lower>>4)&124);fs=lower>>11&31;ft=lower>>16&31;dest=lower>>21&15
  kind='MOVE' if special==48 else 'MR32'
  body=['float result[4];']+[f'result[{c}]=host.m_state.vf[{fs}][{c if special==48 else (c+1)%4}];' for c in range(4)]
  body+=[f'host.applyDest(host.m_state.vf[{ft}],result,{dest}u);']
 if kind is None:lower_unsupported.append(pc);continue
 lower_bodies[pc]=body.copy()
 lines+=[f'case 0x{pc:04x}: {{',*body,'return true;','}'];lower_supported.append({'pc':pc,'lower':lower,'upper':upper,'kind':kind})
lines+=['default: return false;','}','}']
paired=sorted({r['pc'] for r in supported}&{r['pc'] for r in lower_supported})
lines+=['// One PC dispatch executes both bodies; scheduling remains with the caller.',
        'template<class Host,class Pair,class Upper,class Lower> BLACK_VU_AOT_INLINE void executeFusedBodies(Host& host,const Pair& decoded,Upper&& upperBody,Lower&& lowerBody) {',
        'if (!decoded.iBit && decoded.upperVfShadowReg != 0u) {',
        'float oldVf[4],upperVf[4];',
        'std::memcpy(oldVf,host.m_state.vf[decoded.upperVfShadowReg],16);upperBody();',
        'std::memcpy(upperVf,host.m_state.vf[decoded.upperVfShadowReg],16);',
        'std::memcpy(host.m_state.vf[decoded.upperVfShadowReg],oldVf,16);lowerBody();',
        'std::memcpy(host.m_state.vf[decoded.upperVfShadowReg],upperVf,16);',
        '} else { upperBody();lowerBody(); }','}',
        'template<uint32_t Pc> struct PairBody;']
for pc in paired:
 lower,upper=struct.unpack_from('<II',data,pc)
 memory_guard='if(vuData==nullptr || dataSize==0) return false;' if any(r['pc']==pc and r['kind'] in ('LQ','SQ','ILW','ISW','LQI','SQI','LQD','SQD','ILWR','ISWR') for r in lower_supported) else ''
 lines+=[f'template<> struct PairBody<0x{pc:04x}> {{ template<class Host,class Pair> static bool execute(Host& host,const Pair& decoded,uint8_t* vuData,uint32_t dataSize) {{',memory_guard,f'if(decoded.upper != 0x{upper:08x}u || decoded.lower != 0x{lower:08x}u) return false;',
         'auto upperBody=[&]() BLACK_VU_AOT_BODY_INLINE {',*upper_bodies[pc],'};',
         'auto lowerBody=[&]() BLACK_VU_AOT_BODY_INLINE {',*lower_bodies[pc],'return true;','};',
         'executeFusedBodies(host,decoded,upperBody,lowerBody);return true;','} };']
lines+=['template<class Host,class Pair> bool executePairBodies(Host& host,uint32_t pc,const Pair& decoded,uint8_t* vuData=nullptr,uint32_t dataSize=0) {','switch(pc) {']
for pc in paired:lines+=[f'case 0x{pc:04x}: return PairBody<0x{pc:04x}>::execute(host,decoded,vuData,dataSize);']
lines+=['default:return false;','}','}']
lines+=['template<class Pair,class Runner,class Host> struct ScheduledBlockHost {',
        'Runner& run;Host& host;uint32_t pc() const {return host.m_state.pc;}',
        'template<uint32_t Pc,class Body> bool issue() {if(pc()!=Pc)return false;',
        'return run(+[](void* context,const Pair& decoded,uint8_t* data,uint32_t size)->bool {return Body::execute(*static_cast<Host*>(context),decoded,data,size);});}','};',
        'template<class Pair,class Runner,class Host> auto makeBlockScheduler(Runner& run,Host& host) {return ScheduledBlockHost<Pair,Runner,Host>{run,host};}',
        '// 0=unsupported entry, 1=continue at current PC, 2=stop/budget/halt.',
        'template<class Scheduler> int executeBlock(Scheduler& scheduler,uint32_t pc) {switch(pc) {']
blocks=[]
if unit in (0,1):
 paired_set=set(paired)
 for entry in paired:
  pcs_in_block=[];pc=entry
  while pc in paired_set and len(pcs_in_block)<8:
   pcs_in_block.append(pc);lower,upper=struct.unpack_from('<II',data,pc)
   branch=not (upper&0x80000000) and (lower>>25&127) in (32,33,36,37,40,41,44,45,46,47)
   if branch or upper&0x58000000:break
   pc+=8
  blocks.append({'entry':entry,'pcs':pcs_in_block})
  lines+=[f'case 0x{entry:04x}: {{']
  for index,pc in enumerate(pcs_in_block):
   lines+=[f'if(!scheduler.template issue<0x{pc:04x},PairBody<0x{pc:04x}>>())return 2;']
   if index+1<len(pcs_in_block):lines+=[f'if(scheduler.pc()!=0x{pc+8:04x}u)return 1;']
  lines+=['return 1;','}']
lines+=['default:return 0;','}','}','}']
inline_macros='#define BLACK_VU_AOT_INLINE\n#define BLACK_VU_AOT_BODY_INLINE'
if args.inline_mode!='compiler':
 body_attr='__attribute__((always_inline))' if args.inline_mode=='all' else ''
 inline_macros=f'#if defined(__clang__) || defined(__GNUC__)\n#define BLACK_VU_AOT_INLINE inline __attribute__((always_inline))\n#define BLACK_VU_AOT_BODY_INLINE {body_attr}\n#elif defined(_MSC_VER)\n#define BLACK_VU_AOT_INLINE __forceinline\n#define BLACK_VU_AOT_BODY_INLINE\n#else\n#define BLACK_VU_AOT_INLINE inline\n#define BLACK_VU_AOT_BODY_INLINE\n#endif'
lines.insert(2,'#include <cstring>\n#include <cmath>\n'+inline_macros)
lines+=['#undef BLACK_VU_AOT_INLINE','#undef BLACK_VU_AOT_BODY_INLINE']
Path(args.output).write_text('\n'.join(lines)+'\n')
report={'unit':unit,'image_bytes':len(data),'image_hash':f'{h:016x}','observed_pairs':len(pcs),'supported':supported,'unsupported_pcs':unsupported,'lower_supported':lower_supported,'lower_unsupported_pcs':lower_unsupported,'paired_body_pcs':paired,'inline_mode':args.inline_mode,'blocks':blocks,'scope':'Paired instruction bodies and conservative sequential blocks for matching VU images. Each pair still passes through the existing scheduler, which owns timing, flags, control flow, budgets, and resume.'}
Path(args.output+'.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'{len(supported)}/{len(pcs)} upper and {len(lower_supported)}/{len(pcs)} lower words emitted; fallback required for the remainder')
