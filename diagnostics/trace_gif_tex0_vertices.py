#!/usr/bin/env python3
"""Trace packed ST/XYZ vertices after a TEX0 value in a raw GS event capture.

This is an input-stream diagnostic, not a GS primitive/rasterizer emulator.
It deliberately reports raw S,T,Q and the ADC kick bit, without deciding
whether a vertex made a visible pixel.
"""
import argparse
import collections
import json
import struct


def f32(bits):
    return struct.unpack('<f', struct.pack('<I', bits & 0xffffffff))[0]


def covers_pixel(tri, px, py):
    def edge(a, b):
        return (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])
    a, b, c = (v['local_xy'] for v in tri)
    edges = (edge(a, b), edge(b, c), edge(c, a))
    return all(e >= 0 for e in edges) or all(e <= 0 for e in edges)


def trace(path, target, max_occurrences, max_vertices, pixel=None):
    paths = collections.defaultdict(lambda: {'left': 0, 'reg': 0})
    current_tex0 = [None, None]
    prim = 0
    prmode = 0
    prmodecont = True
    xyoffset = [[0.0, 0.0], [0.0, 0.0]]
    scissor = [None, None]
    frame = [None, None]
    strip = []
    hits = collections.deque(maxlen=max_occurrences)
    stq = [0.0, 0.0, 1.0]
    results = collections.deque(maxlen=max_occurrences)
    active = None
    event = 0
    counts = collections.Counter()

    def register(addr, value, packed_hi=None):
        nonlocal prim, prmode, prmodecont, active, strip
        if addr == 0:
            prim = value & 0x7ff
            strip = []
        elif addr in (6, 7):
            context = addr - 6
            current_tex0[context] = value
            if target is None or value == target:
                strip = []
                active = {'event': event, 'path': source, 'context': context,
                          'tex0': hex(value), 'prim': hex(prim), 'vertices': []}
                results.append(active)
                counts['target_tex0_writes'] += 1
            elif active and active['context'] == context:
                active = None
                strip = []
        elif addr == 2:
            stq[0], stq[1] = f32(value), f32(value >> 32)
        elif addr == 1:
            stq[2] = f32(value >> 32)
        elif addr in (0x18, 0x19):
            xyoffset[addr - 0x18] = [(value & 0xffff) / 16.0,
                                     ((value >> 32) & 0xffff) / 16.0]
        elif addr in (0x40, 0x41):
            scissor[addr - 0x40] = [value & 0x7ff, (value >> 16) & 0x7ff,
                                     (value >> 32) & 0x7ff, (value >> 48) & 0x7ff]
        elif addr == 0x1a:
            prmodecont = bool(value & 1)
        elif addr == 0x1b:
            prmode = value & 0x7ff
        elif addr in (0x4c, 0x4d):
            frame[addr - 0x4c] = value
        elif addr in (4, 5, 12, 13) and active is not None:
            # Packed XYZF2/XYZ2 carry XY in lo[15:0]/lo[47:32] and ADC in
            # hi[47]. Reglist XYZ carries XY in a single 64-bit register.
            x = (value & 0xffff) / 16.0
            y = ((value >> (32 if packed_hi is not None else 16)) & 0xffff) / 16.0
            adc = None if packed_hi is None else (packed_hi >> 47) & 1
            effective_prim = prim if prmodecont else (prim & 7) | (prmode & ~7)
            context = (effective_prim >> 9) & 1
            if context != active['context']:
                counts['other_context_vertices'] += 1
                strip = []
                return
            local_xy = [x - xyoffset[context][0], y - xyoffset[context][1]]
            clip = scissor[context]
            vertex = {'event': event, 'path': source, 'reg': addr,
                      'xy': [x, y], 'local_xy': local_xy,
                      'scissor': clip, 'stq': stq.copy(), 'adc': adc,
                      'prim': hex(effective_prim)}
            if clip and clip[0] <= local_xy[0] <= clip[1] and clip[2] <= local_xy[1] <= clip[3]:
                counts['target_vertices_inside_scissor'] += 1
            if len(active['vertices']) < max_vertices:
                active['vertices'].append(vertex)
            counts['target_vertices'] += 1
            if (effective_prim & 7) == 4:
                strip.append(vertex)
                if len(strip) >= 3:
                    if adc == 0 and pixel is not None and covers_pixel(strip[-3:], *pixel):
                        counts['candidate_pixel_triangles'] += 1
                        hits.append({'event': event, 'tex0_event': active['event'],
                                     'tex0': active['tex0'],
                                     'frame': None if frame[context] is None else hex(frame[context]),
                                     'vertices': strip[-3:].copy()})
                    strip = strip[-2:]

    with open(path, 'rb') as stream:
        while header := stream.read(12):
            if len(header) != 12:
                break
            kind, source, size = struct.unpack('<III', header)
            data = stream.read(size)
            if len(data) != size:
                break
            event += 1
            if kind == 2 and size >= 8:
                register(source, struct.unpack_from('<Q', data)[0])
            if kind != 1:
                continue
            state = paths[source]
            for pos in range(0, size - 15, 16):
                lo, hi = struct.unpack_from('<QQ', data, pos)
                if state['left'] == 0:
                    state.update(left=lo & 0x7fff, fmt=(lo >> 58) & 3,
                                 nreg=(lo >> 60) & 15 or 16, desc=hi, reg=0)
                    stq[2] = 1.0
                    if (lo >> 46) & 1:
                        register(0, (lo >> 47) & 0x7ff)
                    continue
                if state['fmt'] == 0:
                    desc = (state['desc'] >> (state['reg'] * 4)) & 15
                    if desc == 14:
                        register(hi & 0xff, lo)
                    elif desc == 0:
                        register(0, lo)
                    elif desc == 2:
                        stq[:] = [f32(lo), f32(lo >> 32), f32(hi)]
                    elif desc in (4, 5, 12, 13):
                        register(desc, lo, hi)
                    elif desc in (6, 7):
                        register(desc, lo)
                    state['reg'] += 1
                    if state['reg'] == state['nreg']:
                        state['reg'] = 0
                        state['left'] -= 1
                elif state['fmt'] == 1:
                    for value in (lo, hi):
                        if not state['left']:
                            break
                        desc = (state['desc'] >> (state['reg'] * 4)) & 15
                        if desc in (0, 1, 2, 4, 5, 6, 7, 12, 13):
                            register(desc, value)
                        state['reg'] += 1
                        if state['reg'] == state['nreg']:
                            state['reg'] = 0
                            state['left'] -= 1
                else:
                    state['left'] -= 1
    return {'events': event, 'counts': dict(counts), 'pixel': pixel,
            'candidate_hits': list(hits), 'occurrences': list(results)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stream')
    parser.add_argument('tex0', type=lambda x: None if x == 'all' else int(x, 0))
    parser.add_argument('--occurrences', type=int, default=4)
    parser.add_argument('--vertices', type=int, default=12)
    parser.add_argument('--pixel', nargs=2, type=float, metavar=('X', 'Y'))
    args = parser.parse_args()
    if args.occurrences < 1 or args.vertices < 1:
        parser.error('limits must be positive')
    print(json.dumps(trace(args.stream, args.tex0, args.occurrences,
                           args.vertices, args.pixel), indent=2))


if __name__ == '__main__':
    main()
