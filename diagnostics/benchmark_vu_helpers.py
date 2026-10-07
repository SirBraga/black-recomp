#!/usr/bin/env python3
"""Compare two prebuilt deterministic VU harnesses sequentially; not gameplay FPS."""
import argparse,json,statistics,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('before');p.add_argument('after');p.add_argument('output');p.add_argument('--repetitions',type=int,default=6);a=p.parse_args()
binaries={'before':str(Path(a.before).resolve()),'after':str(Path(a.after).resolve())};rows=[]
def run(mode):
 rows=[json.loads(s) for s in subprocess.check_output([binaries[mode]],text=True).splitlines()]
 if any(r.get('commit_statistics') for r in rows):raise SystemExit('Instrumented commit-statistics runs cannot be benchmarked.')
 return rows
for mode in ('before','after'):run(mode) # untimed process warmup
order=[m for k in range(a.repetitions) for m in (('before','after') if k%2==0 else ('after','before'))]
for index,mode in enumerate(order,1):
 for row in run(mode):rows.append({'run':index,'mode':mode,**row})
for accurate in (False,True):
 checks={r['checksum'] for r in rows if r['accurate']==accurate}
 if len(checks)!=1:raise SystemExit('State/memory checksums differ; no performance result accepted.')
summary=[]
for accurate in (False,True):
 before=[r['seconds'] for r in rows if r['mode']=='before' and r['accurate']==accurate];after=[r['seconds'] for r in rows if r['mode']=='after' and r['accurate']==accurate];b=statistics.median(before);n=statistics.median(after)
 summary.append({'accurate':accurate,'before_median_seconds':b,'after_median_seconds':n,'throughput_difference_percent':(b/n-1)*100,'before_range': [min(before),max(before)],'after_range':[min(after),max(after)]})
report={'binaries':binaries,'rows':rows,'summary':summary,'scope':'Deterministic synthetic pairs per timing, repeated alternating versions; same generated bodies, decode/scheduler/commit. No XGKICK, original CFG/budgets, EE/IOP or GPU; no gameplay/FPS claim. Whole state+memory checksums must match.'}
Path(a.output).write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(summary,indent=2))
