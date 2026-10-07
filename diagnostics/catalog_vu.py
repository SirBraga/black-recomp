#!/usr/bin/env python3
"""Summarize executed VU images and entry/resume PCs (not instruction frequencies)."""
import argparse,collections,json,struct
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory');args=p.parse_args();root=Path(args.directory)
b=(root/'runs.bin').read_bytes();entries=collections.Counter();versions=collections.defaultdict(set)
for off in range(0,len(b)-23,24):
 unit,pc,h,g=struct.unpack_from('<IIQQ',b,off);entries[unit,h,pc]+=1;versions[unit,h].add(g)
coverage=collections.defaultdict(set)
coverage_path=root/'coverage.bin'
if coverage_path.exists():
 raw=coverage_path.read_bytes()
 for unit,pc,h in struct.iter_unpack('<IIQ',raw[:len(raw)//16*16]):coverage[unit,h].add(pc)
images=[]
for (unit,h),generations in versions.items():
 image=root/f'vu{unit}-{h:016x}.bin';data=image.read_bytes();check=14695981039346656037
 for v in data:check=((check^v)*1099511628211)&((1<<64)-1)
 assert check==h,f'Image checksum mismatch: {image}'
 rows=[{'pc':f'{pc:04x}','calls':calls} for (u,v,pc),calls in entries.items() if u==unit and v==h]
 rows.sort(key=lambda r:r['calls'],reverse=True)
 images.append({'unit':unit,'hash':f'{h:016x}','bytes':len(data),'generation_count':len(generations),'calls':sum(r['calls'] for r in rows),'entries':rows,'executed_pair_pcs':[f'{pc:04x}' for pc in sorted(coverage[unit,h])]})
images.sort(key=lambda r:r['calls'],reverse=True)
report={'records':sum(entries.values()),'incomplete_tail_bytes':len(b)%24,'images':images}
(root/'catalog.json').write_text(json.dumps(report,indent=2)+'\n')
print(f"{report['records']} run/resume calls, {len(images)} distinct code images; incomplete tail: {len(b)%24} bytes")
for row in images[:12]:print(f"VU{row['unit']} {row['hash']} {row['calls']} calls, {len(row['entries'])} entry/resume PCs, {row['generation_count']} generations")
