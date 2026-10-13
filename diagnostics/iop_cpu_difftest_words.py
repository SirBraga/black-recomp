#!/usr/bin/env python3
"""Every distinct instruction word of the game's recompiled IOP modules, for iop_cpu_difftest.

usage: iop_cpu_difftest_words.py black_iop_recompiled.inc words.txt"""
import re
import sys

text = open(sys.argv[1]).read()
words = sorted({int(w, 16) for w in re.findall(r'IOP_STEP\(0x[0-9a-f]+u, 0x([0-9a-f]+)u', text)})
open(sys.argv[2], 'w').write(''.join('%08x\n' % w for w in words))
print(len(words), 'distinct words')
