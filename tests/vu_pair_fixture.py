#!/usr/bin/env python3
"""Emit synthetic VU pairs exercising VF read/write conflicts and all lane masks."""
import argparse,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output');a=p.parse_args();data=bytearray()
for special in (48,49):
 for upper_mask in range(16):
  for lower_mask in range(16):
   for source,target,fd in ((3,4,3),(4,3,3),(3,3,3),(0,0,0)):
    upper=(upper_mask<<21)|(2<<16)|(1<<11)|(fd<<6)|40
    # Lower1 opcode encodes selector as (word&3)|((word>>4)&124).
    lower=(64<<25)|(lower_mask<<21)|(target<<16)|(source<<11)|((special&124)<<4)|60|(special&3)
    data+=struct.pack('<II',lower,upper)
Path(a.output).write_bytes(data)
print(f'{len(data)//8} synthetic pairs')
