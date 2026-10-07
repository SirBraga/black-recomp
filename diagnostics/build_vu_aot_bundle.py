#!/usr/bin/env python3
"""Build a local optional AOT header bundle; captured instruction words are never distributed."""
from pathlib import Path
import argparse,subprocess,tempfile,sys,json
p=argparse.ArgumentParser();p.add_argument('image');p.add_argument('coverage');p.add_argument('output');p.add_argument('second',nargs='?',default='');p.add_argument('--vu1',action='append',default=[]);p.add_argument('--vu0',action='append',default=[]);a=p.parse_args();images=[a.image]+([a.second] if a.second else [])+a.vu1+a.vu0;emitter=Path(__file__).with_name('emit_vu_upper_aot.py');parts=[];reports=[]
with tempfile.TemporaryDirectory() as td:
 for i,img in enumerate(images):
  path=Path(img)
  unit=0 if path.name.startswith('vu0-') else 1 if path.name.startswith('vu1-') else -1
  if unit<0 or path.stat().st_size!=(4096 if unit==0 else 16384):raise SystemExit('Bundle requires full 4 KiB VU0 or 16 KiB VU1 images.')
  if img in a.vu0 and unit!=0:raise SystemExit(f'--vu0 expects a vu0- image: {img}')
  if img in a.vu1 and unit!=1:raise SystemExit(f'--vu1 expects a vu1- image: {img}')
  header=Path(td)/f'image{i}.hpp';subprocess.run([sys.executable,str(emitter),img,str(header),'--coverage',a.coverage],check=True)
  report=json.loads(Path(str(header)+'.json').read_text());reports.append({'unit':unit,'image_bytes':path.stat().st_size,'image_hash':report['image_hash'],'inline_mode':report.get('inline_mode','compiler'),'observed_pairs':report['observed_pairs'],'upper_bodies':len(report['supported']),'lower_bodies':len(report['lower_supported']),'paired_bodies':len(report['paired_body_pcs']),'block_entries':len(report.get('blocks',[]))})
  parts.append(header.read_text().replace('BlackVuUpperAot',f'BlackVuUpperAot{i}'))
parts+=['namespace BlackVuAotBundle {','inline bool matchesHash(uint64_t hash,unsigned unit=1) { return '+' || '.join(f'(unit=={reports[i]["unit"]}u && hash==BlackVuUpperAot{i}::image_hash)' for i in range(len(images)))+'; }','template<class Host,class Pair> bool executePairBodies(uint64_t hash,Host& host,uint32_t pc,const Pair& decoded,uint8_t* data,uint32_t size,unsigned unit=1) {']
for i in range(len(images)):parts+=[f'if(unit=={reports[i]["unit"]}u && hash==BlackVuUpperAot{i}::image_hash) return BlackVuUpperAot{i}::executePairBodies(host,pc,decoded,data,size);']
parts+=['return false;','}',
 'template<class Pair,class Runner,class Host> auto makeBlockScheduler(Runner& run,Host& host) {return BlackVuUpperAot0::makeBlockScheduler<Pair>(run,host);}',
 'template<class Scheduler> int executeBlock(uint64_t hash,unsigned unit,Scheduler& scheduler,uint32_t pc) {']
for i in range(len(images)):
 parts+=[f'if(unit=={reports[i]["unit"]}u && hash==BlackVuUpperAot{i}::image_hash)return BlackVuUpperAot{i}::executeBlock(scheduler,pc);']
parts+=['return 0;','}','}'];output=Path(a.output);text='\n'.join(parts)+'\n'
if not output.exists() or output.read_text()!=text:output.write_text(text)
Path(a.output+'.json').write_text(json.dumps({'images':reports,'scope':'Runtime opt-in paired instruction bodies; matching VU0 and VU1 images dispatch generated blocks through the existing per-pair scheduler. Unsupported pairs/images use interpreter.'},indent=2)+'\n')
