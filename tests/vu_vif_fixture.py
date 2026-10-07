#!/usr/bin/env python3
"""XTOP/XITOP encodings across VI destinations and unused source/mask bits."""
import argparse,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output');a=p.parse_args();data=bytearray()
for special in (104,105):
 for dst in range(16):
  for source in (0,1,15,31):
   for mask in (0,1,8,15):
    lower=(64<<25)|(mask<<21)|(dst<<16)|(source<<11)|((special&124)<<4)|60|(special&3)
    data+=struct.pack('<II',lower,0x2ff)
Path(a.output).write_bytes(data);print(f'{len(data)//8} synthetic VIF register pairs')
