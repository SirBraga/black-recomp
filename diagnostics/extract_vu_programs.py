#!/usr/bin/env python3
"""Extracts VU microprograms from a PS2 executable by following VIF MPG commands.

usage: extract_vu_programs.py ELF OUTDIR [--verify IMAGE.bin ...]

A program is a run of MPG commands (VIFcode 0x4A) whose load addresses are contiguous. Each one is
written as OUTDIR/prog<unit>-<addr>-<fnv64>.bin (raw instruction pairs; <addr> is the load address in
bytes, hex). The unit comes from the largest address: anything that reaches past 0x1000 is VU1; the
rest is written for VU1 as well, because Black only uploads VU0 code through other paths.
--verify checks that every non-zero pair of the given captured 16 KiB VU1 images is covered by an
extracted program placed at its address.
The output contains game code: keep it local, never commit it.
"""
import struct, sys, os

def fnv64(data):
    h = 14695981039346656037
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

def chains(blob):
    n = len(blob) // 4
    words = struct.unpack('<%dI' % n, blob[:n * 4])
    found = []
    i = 0
    while i < n:
        w = words[i]
        if (w >> 24) & 0x7F != 0x4A:
            i += 1
            continue
        start = i
        addr = w & 0xFFFF
        first = addr
        data = bytearray()
        j = i
        while j < n:
            w = words[j]
            if (w >> 24) & 0x7F != 0x4A or (w & 0xFFFF) != addr:
                # NOP words (alignment) may sit between two MPG commands of one program.
                k = j
                while k < n and k - j < 4 and words[k] == 0:
                    k += 1
                if k < n and k != j and (words[k] >> 24) & 0x7F == 0x4A and (words[k] & 0xFFFF) == addr:
                    j = k
                    continue
                break
            count = (w >> 16) & 0xFF or 256
            if j + 1 + count * 2 > n:
                break
            data += blob[(j + 1) * 4:(j + 1 + count * 2) * 4]
            addr += count
            j += 1 + count * 2
        if len(data) >= 16 * 8 and first * 8 + len(data) <= 0x4000:
            found.append((first * 8, bytes(data), start * 4))
            i = j
        else:
            i += 1
    return found

def plausible(data):
    # Upper instructions of real code are mostly valid opcodes; reject data that merely looks like MPG.
    uppers = struct.unpack('<%dI' % (len(data) // 8), b''.join(data[o + 4:o + 8] for o in range(0, len(data), 8)))
    nops = sum(1 for u in uppers if (u & 0x7FF) == 0x2FF or (u & 0x3F) < 0x30 or (u & 0x3F) >= 0x3C)
    return nops >= len(uppers) * 0.95

def main():
    elf, outdir = sys.argv[1], sys.argv[2]
    verify = sys.argv[4:] if len(sys.argv) > 3 and sys.argv[3] == '--verify' else []
    blob = open(elf, 'rb').read()
    programs = {}
    for addr, data, offset in chains(blob):
        if plausible(data):
            programs.setdefault((addr, data), offset)
    os.makedirs(outdir, exist_ok=True)
    for (addr, data), offset in sorted(programs.items(), key=lambda item: item[1]):
        name = 'prog1-%04x-%016x.bin' % (addr, fnv64(data))
        open(os.path.join(outdir, name), 'wb').write(data)
        print('%s  %5d pairs  file offset %#x' % (name, len(data) // 8, offset))
    print('%d programs' % len(programs))
    for path in verify:
        image = open(path, 'rb').read()
        covered = bytearray(len(image) // 8)
        for (addr, data) in programs:
            if image[addr:addr + len(data)] == data:
                for p in range(addr // 8, (addr + len(data)) // 8):
                    covered[p] = 1
        missing = [p for p in range(len(image) // 8) if not covered[p] and image[p * 8:p * 8 + 8] != bytes(8)]
        print('%s: %d non-zero pairs not covered by a whole program%s' % (os.path.basename(path), len(missing),
              '' if not missing else ' (first at pc %#x)' % (missing[0] * 8)))

if __name__ == '__main__':
    main()
