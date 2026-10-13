#!/usr/bin/env python3
"""List the primitives of a GS stream, from either one of our dumps (gs_replay format) or a PCSX2 GS dump (.gs,
decompressed with zstd -d), to compare what two pipelines sent the GS for the same scene.

    gs_prims.py DUMP [--frame N] [--tbp TBP] [--alpha AAAA]

One line per draw kick: context, PRIM type, TEX0.TBP0, ALPHA (A B C D as digits), then each vertex as
x,y (pixels, XYOFFSET not removed) z, s,t,q (or u,v when FST) and RGBA. --frame picks the frame (counting vsyncs /
scanouts from 0); --tbp and --alpha filter draws."""
import struct
import sys

def args():
    a = sys.argv[1:]
    opts = {'frame': None, 'tbp': None, 'alpha': None}
    path = a[0]
    for i, v in enumerate(a):
        if v in ('--frame', '--tbp', '--alpha'):
            opts[v[2:]] = a[i + 1]
    return path, opts


class GS:
    def __init__(self, out):
        self.out = out
        self.prim = 0
        self.prmode = 0
        self.prmodecont = 1
        self.tex0 = [0, 0]
        self.alpha = [0, 0]
        self.xyoffset = [0, 0]
        self.rgbaq = (0x80, 0x80, 0x80, 0x80, 1.0)
        self.st = (0.0, 0.0)
        self.uv = (0, 0)
        self.queue = []

    def ctx(self):
        p = self.prim if self.prmodecont else self.prmode
        return (p >> 9) & 1

    def reg(self, a, v):
        if a == 0x00:
            self.prim = v; self.queue = []
        elif a == 0x1A:
            self.prmodecont = v & 1
        elif a == 0x1B:
            self.prmode = v
        elif a == 0x01:
            q = struct.unpack('<f', struct.pack('<I', (v >> 32) & 0xFFFFFFFF))[0]
            self.rgbaq = (v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF, q)
        elif a == 0x02:
            s = struct.unpack('<f', struct.pack('<I', v & 0xFFFFFFFF))[0]
            t = struct.unpack('<f', struct.pack('<I', (v >> 32) & 0xFFFFFFFF))[0]
            self.st = (s, t)
        elif a == 0x03:
            self.uv = (v & 0x3FFF, (v >> 16) & 0x3FFF)
        elif a in (0x06, 0x07):
            self.tex0[a - 0x06] = v
        elif a in (0x42, 0x43):
            self.alpha[a - 0x42] = v
        elif a in (0x18, 0x19):
            self.xyoffset[a - 0x18] = v
        elif a in (0x04, 0x05, 0x0C, 0x0D):  # XYZF2, XYZ2, XYZF3, XYZ3
            x = (v & 0xFFFF) / 16.0
            y = ((v >> 16) & 0xFFFF) / 16.0
            z = (v >> 32) & (0xFFFFFF if a in (0x04, 0x0C) else 0xFFFFFFFF)
            self.vertex(x, y, z, kick=a in (0x04, 0x05))

    def vertex(self, x, y, z, kick):
        self.queue.append((x, y, z, self.st, self.rgbaq, self.uv))
        kind = self.prim & 7
        need = {0: 1, 1: 2, 2: 2, 3: 3, 4: 3, 5: 3, 6: 2}.get(kind, 3)
        if len(self.queue) < need:
            return
        verts = self.queue[-need:]
        if kind in (4, 5):  # strip/fan keep vertices
            self.queue = ([self.queue[0]] + self.queue[-1:]) if kind == 5 else self.queue[-2:]
        else:
            self.queue = []
        if kick:
            self.out(self, verts)


def gif(gs, data):
    """Run GIF packets (bytes, 16-byte aligned) through the register model."""
    pos = 0
    n = len(data)
    state = getattr(gs, '_gif', None)
    while pos + 16 <= n:
        if state is None:
            lo, hi = struct.unpack_from('<QQ', data, pos)
            pos += 16
            nloop = lo & 0x7FFF
            pre = (lo >> 46) & 1
            flg = (lo >> 58) & 3
            nreg = (lo >> 60) & 0xF or 16
            regs = [(hi >> (4 * i)) & 0xF for i in range(nreg)]
            if pre and flg == 0:
                gs.reg(0x00, (lo >> 47) & 0x7FF)
            if flg == 2:
                pos += nloop * 16
                continue
            if nloop == 0:
                continue
            state = [flg, nloop, regs, 0]
            continue
        flg, nloop, regs, i = state
        if flg == 0:  # PACKED
            lo, hi = struct.unpack_from('<QQ', data, pos)
            pos += 16
            r = regs[i % len(regs)]
            if r == 0x0E:
                gs.reg(hi & 0xFF, lo)
            elif r == 0x01:
                gs.reg(0x01, (lo & 0xFF) | ((lo >> 32) & 0xFF) << 8 | (hi & 0xFF) << 16 | ((hi >> 32) & 0xFF) << 24 |
                       (struct.unpack('<I', struct.pack('<f', gs.rgbaq[4]))[0] << 32))
            elif r == 0x02:
                gs.reg(0x02, lo)
                gs.rgbaq = gs.rgbaq[:4] + (struct.unpack('<f', struct.pack('<I', hi & 0xFFFFFFFF))[0],)
            elif r == 0x03:
                gs.reg(0x03, (lo & 0x3FFF) | ((lo >> 32) & 0x3FFF) << 16)
            elif r in (0x04, 0x05):
                x = lo & 0xFFFF; y = (lo >> 32) & 0xFFFF
                adc = (hi >> 47) & 1
                z = (hi >> 4) & (0xFFFFFF if r == 0x04 else 0xFFFFFFFF)
                gs.vertex(x / 16.0, y / 16.0, z, kick=not adc)
            elif r in (0x0F,):
                pass
            else:
                gs.reg(r, lo)
            i += 1
            if i == nloop * len(regs):
                state = None
            else:
                state[3] = i
        else:  # REGLIST: two registers per qword
            for half in range(2):
                if i >= nloop * len(regs):
                    break
                v = struct.unpack_from('<Q', data, pos + 8 * half)[0]
                gs.reg(regs[i % len(regs)], v)
                i += 1
            pos += 16
            if i >= nloop * len(regs):
                state = None
            else:
                state[3] = i
    gs._gif = state


def frames_ours(path):
    """Yield ('gif', bytes) / ('reg', addr, value) / ('frame',) events from one of our dumps."""
    with open(path, 'rb') as f:
        while True:
            h = f.read(12)
            if len(h) < 12:
                return
            kind, p, size = struct.unpack('<III', h)
            data = f.read(size)
            if kind == 1:
                yield ('gif', data)
            elif kind == 2:
                yield ('reg', p, struct.unpack('<Q', data)[0])
            elif kind == 7:
                yield ('frame',)


def frames_pcsx2(path):
    with open(path, 'rb') as f:
        d = f.read()
    pos = 4
    header_size = struct.unpack_from('<I', d, pos)[0]
    pos += 4
    state_size = struct.unpack_from('<I', d, pos + 4)[0]
    pos += header_size
    pos += state_size + 8192
    n = len(d)
    while pos < n:
        t = d[pos]; pos += 1
        if t == 0:
            pos += 1
            size = struct.unpack_from('<I', d, pos)[0]; pos += 4
            yield ('gif', d[pos:pos + size]); pos += size
        elif t == 1:
            pos += 1
            yield ('frame',)
        elif t == 2:
            pos += 4
        elif t == 3:
            pos += 8192
        else:
            return


def main():
    path, opts = args()
    events = frames_pcsx2(path) if path.endswith('.gs') else frames_ours(path)
    want = int(opts['frame']) if opts['frame'] is not None else None
    tbp = int(opts['tbp']) if opts['tbp'] else None
    frame = 0

    def out(gs, verts):
        if want is not None and frame != want:
            return
        c = gs.ctx()
        t0 = gs.tex0[c]
        a = gs.alpha[c]
        alpha = '%d%d%d%d' % (a & 3, (a >> 2) & 3, (a >> 4) & 3, (a >> 6) & 3)
        if tbp is not None and (t0 & 0x3FFF) != tbp:
            return
        if opts['alpha'] and alpha != opts['alpha']:
            return
        ox = (gs.xyoffset[c] & 0xFFFF) / 16.0
        oy = ((gs.xyoffset[c] >> 32) & 0xFFFF) / 16.0
        fst = (gs.prim >> 8) & 1
        parts = []
        for x, y, z, st, rgbaq, uv in verts:
            tc = 'uv=%d,%d' % uv if fst else 'stq=%.4f,%.4f,%.4f' % (st[0], st[1], rgbaq[4])
            parts.append('(%.1f,%.1f z=%x %s rgba=%02x%02x%02x%02x)' % (x - ox, y - oy, z, tc, *rgbaq[:4]))
        print('f%d ctx%d prim=%d tbp=%d alpha=%s %s' % (frame, c, gs.prim & 7, t0 & 0x3FFF, alpha, ' '.join(parts)))

    gs = GS(out)
    for e in events:
        if e[0] == 'gif':
            gif(gs, e[1])
        elif e[0] == 'reg':
            gs.reg(e[1], e[2])
        else:
            frame += 1
            if want is not None and frame > want:
                break


if __name__ == '__main__':
    main()
