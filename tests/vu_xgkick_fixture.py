#!/usr/bin/env python3
"""Synthetic XGKICK across VI sources and unused mask/destination bits."""
import argparse,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output');a=p.parse_args();data=bytearray()
for src in range(16):
 for dst in (0,15):
  for mask in (0,1,8,15):
   lower=(64<<25)|(mask<<21)|(dst<<16)|(src<<11)|(108&124)<<4|60
   data+=struct.pack('<II',lower,0x2ff)
Path(a.output).write_bytes(data);print(f'{len(data)//8} synthetic XGKICK pairs')
