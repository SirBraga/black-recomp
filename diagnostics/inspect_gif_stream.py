#!/usr/bin/env python3
"""Inspect bounded raw GS event captures; no rendering or game modification."""
import collections
import struct
import sys

counts = collections.Counter()
regs = collections.Counter()
paths = {}
state = {}
bad = []
unaligned_uploads = []
valid_psm = {0, 1, 2, 10, 19, 20, 27, 36, 44, 48, 49, 50, 58}

def register(addr, value, event, path):
    regs[addr] += 1
    state[addr] = value
    if addr == 0x53:
        bb = state.get(0x50, 0)
        direction = value & 3
        psm = (bb >> (24 if direction == 1 else 56)) & 63
        if direction != 3 and psm not in valid_psm:
            if len(bad) < 20:
                bad.append((event, path, direction, psm, hex(bb)))
        if direction == 0 and 0x52 in state:
            trx = state[0x52]
            width, height = trx & 4095, (trx >> 32) & 4095
            bits = {0:32,1:24,2:16,10:16,19:8,20:4,27:8,36:4,44:4,48:32,49:24,50:16,58:16}.get((bb >> 56) & 63)
            if bits and width and height:
                counts['upload_psm_%02x' % ((bb >> 56) & 63)] += 1
                total = width * height * bits
                if total % 64:
                    counts['uploads_partial_hwreg_word'] += 1
                    if len(unaligned_uploads) < 20:
                        unaligned_uploads.append((event, width, height, (bb >> 56) & 63, total))
    if addr in (6, 7):
        counts['tex_psm_%02x' % ((value >> 20) & 63)] += 1
        psm = (value >> 20) & 63
        if psm in (19, 20, 27, 36, 44):
            counts['clut_psm%02x_cpsm%02x_csm%d' % (psm, (value >> 51) & 15, (value >> 55) & 1)] += 1

with open(sys.argv[1], 'rb') as f:
    event = 0
    while h := f.read(12):
        if len(h) != 12:
            break
        kind, path, size = struct.unpack('<III', h)
        data = f.read(size)
        if len(data) != size:
            break  # active capture can end mid-write
        event += 1
        counts['kind_%d' % kind] += 1
        if kind == 2:
            register(path, struct.unpack('<Q', data)[0], event, 0)
        if kind != 1:
            continue
        p = paths.setdefault(path, {'left': 0, 'reg': 0})
        pos = 0
        while pos + 16 <= len(data):
            lo, hi = struct.unpack_from('<QQ', data, pos)
            pos += 16
            if not p['left']:
                p.update(left=lo & 32767, fmt=(lo >> 58) & 3,
                         nreg=(lo >> 60) & 15 or 16, desc=hi, reg=0)
                counts['path%d_tags_fmt%d' % (path, p['fmt'])] += 1
                continue
            if p['fmt'] == 0:
                desc = (p['desc'] >> (p['reg'] * 4)) & 15
                if desc == 14:
                    register(hi & 255, lo, event, path)
                elif desc in (6, 7):
                    register(desc, lo, event, path)
                p['reg'] += 1
                if p['reg'] == p['nreg']:
                    p['reg'] = 0
                    p['left'] -= 1
            elif p['fmt'] == 1:
                for value in (lo, hi):
                    if not p['left']:
                        break
                    desc = (p['desc'] >> (p['reg'] * 4)) & 15
                    register(desc, value, event, path)
                    p['reg'] += 1
                    if p['reg'] == p['nreg']:
                        p['reg'] = 0
                        p['left'] -= 1
            else:
                counts['path%d_image_qw' % path] += 1
                p['left'] -= 1
print('events', event)
print('counts', dict(counts))
print('registers', {hex(k): v for k, v in sorted(regs.items())})
print('invalid transfers', bad)
print('partial HWREG uploads', unaligned_uploads)
print('final transfers', {hex(k): hex(v) for k, v in state.items() if k >= 0x50})
