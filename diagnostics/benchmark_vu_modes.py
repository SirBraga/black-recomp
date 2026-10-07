#!/usr/bin/env python3
"""Alternating end-to-end VU benchmark, aligned to gameplay entry rather than menu counters."""
import argparse,hashlib,json,os,platform,re,signal,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output');p.add_argument('--timeout',type=int,default=240);a=p.parse_args()
root=Path(__file__).resolve().parents[2];out=Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True)
runner=root/'tools/PS2Recomp/out/rt/ps2xRuntime/ps2EntryRunner';disc=root/'recomp/disc'
pattern=re.compile(r'\[black-state\] t=([\d.]+) g=([\d.]+) upd=(\d+).*mode=(\d+) cur=(0x[0-9a-f]+)')
pad='9:up:0.3,9.5:cross:0.3,20:select:0.3,22:cross:0.3,24:cross:0.3,26:cross:0.3,30:select:0.3,40:select:0.3,46:cross:0.3,48:cross:0.3,50:cross:0.3,54:select:0.3,62:cross:0.3,64:cross:0.3,68:select:0.3,80:select:0.3'
env=os.environ.copy()
for k in list(env):
 if k.startswith(('PS2X_','BLACK_')):del env[k]
env.update(BLACK_DEBUG='1',BLACK_CUTSCENE_SKIP='1',BLACK_CD_IMAGE=str(root/'orig/Black.iso'),PS2X_PAD_SCRIPT=pad,PS2X_GS_PARALLEL='1',PS2X_GS_PARALLEL_MODULE=str(root/'recomp/gpu/build/libblack-parallel-gs.so'),PS2X_GS_MOLTENVK=str(root/'recomp/gpu/moltenvk/libMoltenVK.dylib'))
results=[]
report={'runner_sha256':hashlib.sha256(runner.read_bytes()).hexdigest(),'host':platform.platform(),'order':['interpreter','aot','aot','interpreter'],'warmup_updates':120,'measurement_updates':180,'configuration':env|{},'runs':results}
# Keep only benchmark-specific configuration; never serialize the inherited environment.
report['configuration']={k:v for k,v in env.items() if k.startswith(('PS2X_','BLACK_'))}
def save():
 (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
for index,mode in enumerate(report['order'],1):
 log=out/f'{index:02d}-{mode}.log';runenv=env.copy()
 if mode=='aot':runenv['PS2X_VU_AOT']='1'
 entry=start=end=None;deadline=time.monotonic()+a.timeout;print(f'START {index}/4 {mode}',flush=True)
 with log.open('w') as stream:
  child=subprocess.Popen([str(runner),str(disc/'SLUS_213.76')],cwd=disc,env=runenv,stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
  try:
   with log.open() as reader:
    while child.poll() is None and time.monotonic()<deadline:
     for line in reader:
      m=pattern.search(line)
      if not m:continue
      t,g,u,state,cur=m.groups();t=float(t);g=float(g);u=int(u)
      if state!='0' or cur!='0x004bcf78':continue
      point={'seconds':t,'guest_seconds':g,'updates':u}
      if entry is None:entry=point;print(f'ENTRY {mode}: {point}',flush=True)
      if start is None and u-entry['updates']>=120:start=point;print(f'MEASURE {mode}: {point}',flush=True)
      if start and u-start['updates']>=180:end=point;break
     if end:break
     time.sleep(0.5)
  finally:
   if child.poll() is None:
    os.killpg(child.pid,signal.SIGTERM)
    try:child.wait(timeout=5)
    except subprocess.TimeoutExpired:os.killpg(child.pid,signal.SIGKILL);child.wait()
 text=log.read_text();faults=re.findall(r'^.*(?:guest-fault|missing-target|reserved detail|exhaustion|Assertion failed).*$',text,re.M)
 row={'index':index,'mode':mode,'log':str(log),'entry':entry,'start':start,'end':end,'faults':faults,'aot_seen':'[VU AOT]' in text,'completed':bool(end)}
 if end:
  row['elapsed_seconds']=round(end['seconds']-start['seconds'],3);row['updates']=end['updates']-start['updates'];row['updates_per_second']=row['updates']/row['elapsed_seconds']
 results.append(row);save();print('RESULT '+json.dumps(row),flush=True)
 if not end or faults:raise SystemExit('Incomplete/faulted run; benchmark stopped.')
 time.sleep(3)
base=[r['updates_per_second'] for r in results if r['mode']=='interpreter'];exp=[r['updates_per_second'] for r in results if r['mode']=='aot']
report['summary']={'interpreter_mean_updates_per_second':sum(base)/len(base),'aot_mean_updates_per_second':sum(exp)/len(exp),'difference_percent':(sum(exp)/sum(base)-1)*100,'interpreter_range': [min(base),max(base)],'aot_range':[min(exp),max(exp)],'limitations':'Two runs per mode, gameplay-relative windows, wall-clock scripted inputs, no reference frame/state equivalence, end-to-end diagnostic runner; not isolated VU throughput or window FPS.'};save();print('SUMMARY '+json.dumps(report['summary']),flush=True)
