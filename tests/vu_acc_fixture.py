#!/usr/bin/env python3
"""ACC arithmetic forms and every destination mask; shared input registers create dependencies."""
import argparse,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output');a=p.parse_args();data=bytearray()
for special in (*range(16),*range(24,29),30,*range(32,43),44,45,46):
 for mask in range(16):
  upper=(mask<<21)|(2<<16)|(1<<11)|((special&124)<<4)|60|(special&3)
  data+=struct.pack('<II',0x8000033c,upper)
Path(a.output).write_bytes(data);print(f'{len(data)//8} ACC pairs')
