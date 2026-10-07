#!/usr/bin/env python3
"""Sample interpreter and AOT during Level 00; timings during sampling are not benchmarks."""
import os,time,subprocess,signal,re
from pathlib import Path
root=Path(__file__).resolve().parents[2];out=root/'recomp/diagnostics/vu-dispatch-profile';out.mkdir(parents=True,exist_ok=True)
env=os.environ.copy();env.update(BLACK_DEBUG='1',BLACK_CUTSCENE_SKIP='1',BLACK_CD_IMAGE=str(root/'orig/Black.iso'),PS2X_GS_PARALLEL='1',PS2X_GS_PARALLEL_MODULE=str(root/'recomp/gpu/build/libblack-parallel-gs.so'),PS2X_GS_MOLTENVK=str(root/'recomp/gpu/moltenvk/libMoltenVK.dylib'),PS2X_PAD_SCRIPT='9:up:0.3,9.5:cross:0.3,20:select:0.3,22:cross:0.3,24:cross:0.3,26:cross:0.3,30:select:0.3,40:select:0.3,46:cross:0.3,48:cross:0.3,50:cross:0.3,54:select:0.3,62:cross:0.3,64:cross:0.3,68:select:0.3,80:select:0.3')
for mode in ('interpreter','aot'):
 env.pop('PS2X_VU_AOT',None)
 if mode=='aot':env['PS2X_VU_AOT']='1'
 log=out/f'{mode}.log';print('START '+mode,flush=True)
 with log.open('w') as f:
  child=subprocess.Popen([str(root/'tools/PS2Recomp/out/rt/ps2xRuntime/ps2EntryRunner'),str(root/'recomp/disc/SLUS_213.76')],cwd=root/'recomp/disc',env=env,stdout=f,stderr=subprocess.STDOUT,start_new_session=True)
  try:
   deadline=time.monotonic()+150
   while time.monotonic()<deadline and child.poll() is None:
    if re.search(r'\[black-state\].*mode=0 cur=0x004bcf78',log.read_text()):
     result=subprocess.run(['/usr/bin/sample',str(child.pid),'5','1','-file',str(out/f'{mode}-sample.txt')],capture_output=True,text=True);print(mode+' sample '+str(result.returncode)+' '+result.stderr[-400:],flush=True);break
    time.sleep(1)
  finally:
   if child.poll() is None:
    os.killpg(child.pid,signal.SIGTERM)
    try:child.wait(timeout=5)
    except subprocess.TimeoutExpired:os.killpg(child.pid,signal.SIGKILL);child.wait()
