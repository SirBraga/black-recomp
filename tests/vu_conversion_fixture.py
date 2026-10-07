#!/usr/bin/env python3
"""Upper conversions, ABS and CLIP across masks and VF source/destination aliases."""
import argparse,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output');a=p.parse_args();data=bytearray()
for special in (*range(16,24),29,31):
 for mask in range(16):
  for fs,ft in ((0,0),(1,1),(1,2),(31,1)):
   upper=(mask<<21)|(ft<<16)|(fs<<11)|((special&124)<<4)|60|(special&3)
   data+=struct.pack('<II',0x8000033c,upper)
Path(a.output).write_bytes(data)
