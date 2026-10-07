#!/usr/bin/env python3
"""Synthetic LQ/SQ/ILW/ISW encodings covering masks, signed offsets and register aliases."""
import argparse,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output');p.add_argument('--paired',action='store_true');p.add_argument('--incremental',action='store_true');p.add_argument('--indirect',action='store_true');a=p.parse_args();data=bytearray()
for op in (0,1,4,5):
 for mask in range(16):
  for imm in (-1024,-1,0,1023):
   for fs,ft in ((0,0),(1,1),(15,31),(31,15)):
    lower=(op<<25)|(mask<<21)|(ft<<16)|(fs<<11)|(imm&2047)
    # Upper NOP: no I bit; literal lower must execute as an instruction.
    data+=struct.pack('<II',lower,0x000002ff)
if a.paired:
 data=bytearray()
 for op in (0,1):
  for mask in range(16):
   for upperMask in (0,1,8,15):
    for fd,fs,ft in ((3,1,3),(3,3,1),(3,3,3),(0,0,0)):
     upper=(upperMask<<21)|(2<<16)|(1<<11)|(fd<<6)|40
     lower=(op<<25)|(mask<<21)|(ft<<16)|(fs<<11)
     data+=struct.pack('<II',lower,upper)
if a.incremental or a.indirect:
 data=bytearray()
 for special in ((62,63) if a.indirect else (52,53,54,55)):
  for mask in range(16):
   for fs,ft in ((0,0),(1,1),(15,31),(31,15)):
    lower=(64<<25)|(mask<<21)|(ft<<16)|(fs<<11)|((special&124)<<4)|60|(special&3)
    upper=(15<<21)|(2<<16)|(1<<11)|(3<<6)|40
    data+=struct.pack('<II',lower,upper)
Path(a.output).write_bytes(data);print(f'{len(data)//8} synthetic memory pairs')
