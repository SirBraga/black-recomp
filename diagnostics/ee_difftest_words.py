#!/usr/bin/env python3
"""Pick a sample of the game's EE instruction words for ee_difftest (one hex word per line).

usage: ee_difftest_words.py SLUS_213.76 words.txt [per_class]
Takes up to per_class (default 24) distinct words of every instruction class found in the executable
sections, skipping what the test does not cover (control flow, COP0, COP2, SYSCALL/BREAK, CACHE/PREF).
"""
import collections, random, struct, sys

elf = open(sys.argv[1], 'rb').read()
per_class = int(sys.argv[3]) if len(sys.argv) > 3 else 24
shoff, = struct.unpack_from('<I', elf, 0x20)
shentsize, shnum = struct.unpack_from('<HH', elf, 0x2E)
classes = collections.defaultdict(set)
for i in range(shnum):
    name, kind, flags, addr, offset, size = struct.unpack_from('<6I', elf, shoff + i * shentsize)
    if kind != 1 or not flags & 4:  # SHT_PROGBITS with SHF_EXECINSTR
        continue
    for o in range(0, size & ~3, 4):
        w, = struct.unpack_from('<I', elf, offset + o)
        op, rs, rt, funct = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 63
        if op == 0:
            if funct in (0x08, 0x09, 0x0C, 0x0D, 0x0F) or 0x30 <= funct <= 0x36:
                continue
            key = 'special_%02x' % funct
        elif op == 0x1C:
            key = 'mmi_%02x_%02x' % (funct, (w >> 6) & 31) if funct in (0x08, 0x09, 0x28, 0x29) else 'mmi_%02x' % funct
        elif op == 0x11:
            if rs == 0x08 or rs in (0x02, 0x06):
                continue
            key = 'cop1_%02x_%02x' % (rs, funct) if rs in (0x10, 0x14) else 'cop1_%02x' % rs
        elif op in (0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x12, 0x14, 0x15, 0x16, 0x17, 0x2F, 0x33, 0x36, 0x3E):
            continue
        else:
            key = 'op_%02x' % op
        classes[key].add(w)
random.seed(1)
words = []
for key in sorted(classes):
    pool = sorted(classes[key])
    words += random.sample(pool, min(per_class, len(pool)))
open(sys.argv[2], 'w').write(''.join('%08x\n' % w for w in words))
print('%d classes, %d words' % (len(classes), len(words)))
