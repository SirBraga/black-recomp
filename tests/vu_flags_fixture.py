#!/usr/bin/env python3
"""VU flags/transfers, masks/components, immediate edges and VI0/source aliases."""
import argparse,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output');a=p.parse_args();pairs=[]
for op in (16,17,18,19):
 for imm in (0,0xfff,0x800000,0xffffff):pairs.append((op<<25|imm,0x2ff))
for op in (20,21,22,23):
 for imm in (0,0x7ff,0x800,0xfff):
  for dst in (0,1,15):pairs.append((op<<25|dst<<16|((imm>>11&1)<<21)|(imm&2047),0x2ff))
for op in (24,26,27,28):
 for fs,ft in ((0,0),(1,1),(15,1),(1,15)):pairs.append((op<<25|ft<<16|fs<<11,0x2ff))
for special in (60,61):
 for mask in range(16):
  for fs,ft in ((0,0),(1,1),(31,15),(15,31)):
   pairs.append((64<<25|mask<<21|ft<<16|fs<<11|(special&124)<<4|60|(special&3),0x2ff))
# Mix flag producers into sequences: FMAC and CLIP use the same source registers.
for mask in range(16):
 pairs.append((0x8000033c,mask<<21|2<<16|1<<11|3<<6|40))
 pairs.append((0x8000033c,mask<<21|2<<16|1<<11|0x1f<<4|63))
for mask in range(16):
 for imm in (0,0x7ff,0x800,0xfff):
  fmac=mask<<21|2<<16|1<<11|3<<6|40
  clip=mask<<21|2<<16|1<<11|0x1f<<4|63
  pairs.append((21<<25|((imm>>11&1)<<21)|(imm&2047),fmac))
  pairs.append((17<<25|imm,clip))
Path(a.output).write_bytes(b''.join(struct.pack('<II',*pair) for pair in pairs));print(len(pairs),'fixture pairs')
