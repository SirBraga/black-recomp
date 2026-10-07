#!/usr/bin/env python3
"""Compare common gameplay update counters from two limited run logs; window FPS is unrelated."""
from pathlib import Path
import argparse,re,json
p=argparse.ArgumentParser();p.add_argument('experimental');p.add_argument('baseline');a=p.parse_args()
pattern=re.compile(r'\[black-state\] t=([\d.]+) g=([\d.]+) upd=(\d+).*mode=(\d+)')
def points(path):
 result={}
 for t,g,u,mode in pattern.findall(Path(path).read_text()):
  if int(mode)==0 and float(g)>=34:result[int(u)]=float(t)
 return result
exp=points(a.experimental);base=points(a.baseline);common=sorted(exp.keys()&base.keys())
if len(common)<2:raise SystemExit('Not enough common gameplay update samples; no comparison.')
first,last=common[0],common[-1];updates=last-first;ed=exp[last]-exp[first];bd=base[last]-base[first]
print(json.dumps({'updates_from':first,'updates_to':last,'updates':updates,'experimental_seconds':round(ed,3),'baseline_seconds':round(bd,3),'experimental_updates_per_second':round(updates/ed,3),'baseline_updates_per_second':round(updates/bd,3),'speed_difference_percent':round((bd/ed-1)*100,2),'limitation':'One run per mode, common update counters after guest time 34s; not repeated statistical evidence or window FPS.'},indent=2))
