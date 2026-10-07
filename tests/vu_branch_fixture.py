#!/usr/bin/env python3
"""Synthetic VU branches: signed offsets, link/source aliases and VI0."""
import argparse,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output');a=p.parse_args();data=bytearray()
for op in (32,33,36,37,40,41,44,45,46,47):
 for imm in (-1024,-1,0,1023):
  for fs,ft in ((0,0),(1,1),(1,2),(15,1)):
   lower=(op<<25)|(ft<<16)|(fs<<11)|(imm&2047)
   upper=(15<<21)|(2<<16)|(1<<11)|(3<<6)|40
   data+=struct.pack('<II',lower,upper)
 # Interleave a VI write so the next branch can read its backup.
 data+=struct.pack('<II',(64<<25)|(1<<16)|(1<<11)|(1<<6)|50,0x2ff)
Path(a.output).write_bytes(data);print(f'{len(data)//8} synthetic branch/write pairs')
