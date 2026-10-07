#!/usr/bin/env python3
"""Small terminating VU loop with a branch delay slot and dependent memory operations."""
import argparse,struct
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output');a=p.parse_args();nop=0x2ff
pairs=[((8<<25)|(1<<16)|4,nop),
 ((64<<25)|(1<<16)|(1<<11)|(31<<6)|50,(15<<21)|(2<<16)|(1<<11)|(3<<6)|40),
 ((45<<25)|(1<<11)|((-2)&2047),nop),
 (0x8000033c,(15<<21)|(2<<16)|(3<<11)|(4<<6)|42),
 ((1<<25)|(15<<21)|(3<<11),nop),
 ((15<<21)|(5<<16),nop),
 (0x8000033c,nop|0x40000000),
 (0x8000033c,nop)]
Path(a.output).write_bytes(b''.join(struct.pack('<II',*v) for v in pairs))
