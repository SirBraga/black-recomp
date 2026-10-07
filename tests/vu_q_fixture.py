#!/usr/bin/env python3
"""Scalar Q encodings: components, VF0, aliases and upper consumers of Q."""
import argparse,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output');a=p.parse_args();data=bytearray()
for special in (56,57,58,59):
 for fsf in range(4):
  for ftf in range(4):
   for fs,ft in ((0,0),(1,1),(1,2),(31,15)):
    lower=(64<<25)|(fsf<<21)|(ftf<<23)|(ft<<16)|(fs<<11)|((special&124)<<4)|60|(special&3)
    # MULq consumes the prior committed Q; WAITQ retires after upper execution.
    upper=(15<<21)|(1<<11)|(3<<6)|28
    data+=struct.pack('<II',lower,upper)
Path(a.output).write_bytes(data);print(f'{len(data)//8} synthetic scalar Q pairs')
