#!/usr/bin/env python3
"""gs_feature_census.py <raw GIF capture>: which GS features the captured stream uses, counted per drawn
primitive. Planning aid for a native renderer (what has to be translated, what is rare)."""
import struct, sys, collections
path = sys.argv[1]
st = collections.defaultdict(int); st['prmodecont'] = 1
C = collections.Counter
prims = C(); flags = C(); tex = C(); texfn = C(); filt = C(); clampm = C(); alpha = C(); atest = C(); ztest = C(); dtest = C()
frames = C(); combos = C(); scan = C(); zbufs = C(); misc = C(); feedback = C(); fbmask = C(); regs_seen = C(); pathc = C()
nverts = 0; pending = 0
def prim_now(): return st['prim'] if st['prmodecont'] else ((st['prim'] & 7) | (st['prmode'] & ~7))
def kick():
    p = prim_now(); t = p & 7
    prims[('point', 'line', 'linestrip', 'triangle', 'tristrip', 'trifan', 'sprite', 'invalid')[t]] += 1
    ctx = (p >> 9) & 1
    for name, bit in (('IIP gouraud', 3), ('TME texture', 4), ('FGE fog', 5), ('ABE blend', 6), ('AA1', 7), ('FST uv', 8), ('CTXT 2', 9), ('FIX', 10)):
        if (p >> bit) & 1: flags[name] += 1
    fr = st['frame%d' % ctx]; zb = st['zbuf%d' % ctx]; te = st['test%d' % ctx]; al = st['alpha%d' % ctx]
    fbp = fr & 0x1FF; fpsm = (fr >> 24) & 0x3F
    frames[(fbp, (fr >> 16) & 0x3F, fpsm)] += 1
    sc = st['scissor%d' % ctx]
    combos[('fbp=%d fbw=%d psm=%d' % (fbp, (fr >> 16) & 0x3F, fpsm), 'zbp=%d zpsm=%d zmsk=%d zte=%d' % (zb & 0x1FF, (zb >> 24) & 0xF, (zb >> 32) & 1, (te >> 16) & 1), 'scissor x %d..%d y %d..%d' % (sc & 0x7FF, (sc >> 16) & 0x7FF, (sc >> 32) & 0x7FF, (sc >> 48) & 0x7FF))] += 1
    fbmask['fbmsk=%08x' % ((fr >> 32) & 0xFFFFFFFF)] += 1
    zbufs[(zb & 0x1FF, (zb >> 24) & 0xF, (zb >> 32) & 1)] += 1
    ztest[('ZTE=%d' % ((te >> 16) & 1), ('never', 'always', 'gequal', 'greater')[(te >> 17) & 3], 'zmsk=%d' % ((zb >> 32) & 1))] += 1
    if te & 1: atest[(('never', 'always', 'less', 'lequal', 'equal', 'gequal', 'greater', 'notequal')[(te >> 1) & 7], 'aref=%d' % ((te >> 4) & 0xFF), ('keep', 'fb_only', 'zb_only', 'rgb_only')[(te >> 12) & 3])] += 1
    else: atest['off'] += 1
    dtest['DATE=%d DATM=%d' % ((te >> 14) & 1, (te >> 15) & 1)] += 1
    if (p >> 6) & 1:
        n = 'CsCdZ?'  # A,B,D: 0 Cs 1 Cd 2 zero ; C: 0 As 1 Ad 2 FIX
        a, b, c, d = al & 3, (al >> 2) & 3, (al >> 4) & 3, (al >> 6) & 3
        alpha['(%s-%s)*%s+%s%s' % (('Cs', 'Cd', '0', '?')[a], ('Cs', 'Cd', '0', '?')[b], ('As', 'Ad', 'FIX', '?')[c], ('Cs', 'Cd', '0', '?')[d], ' fix=%d' % ((al >> 32) & 0xFF) if c == 2 else '')] += 1
    if (p >> 4) & 1:
        t0 = st['tex0_%d' % ctx]; t1 = st['tex1_%d' % ctx]; cl = st['clamp%d' % ctx]
        psm = (t0 >> 20) & 0x3F; tbp = t0 & 0x3FFF
        tex[({0: 'CT32', 1: 'CT24', 2: 'CT16', 0xA: 'CT16S', 0x13: 'T8', 0x14: 'T4', 0x1B: 'T8H', 0x24: 'T4HL', 0x2C: 'T4HH', 0x30: 'Z32', 0x31: 'Z24', 0x32: 'Z16', 0x3A: 'Z16S'}.get(psm, hex(psm)), '%dx%d' % (1 << ((t0 >> 26) & 15), 1 << ((t0 >> 30) & 15)))] += 1
        texfn[('TCC=%d' % ((t0 >> 34) & 1), ('modulate', 'decal', 'highlight', 'highlight2')[(t0 >> 35) & 3], 'clut ' + ({0: 'CT32', 2: 'CT16', 0xA: 'CT16S'}.get((t0 >> 51) & 0xF, '?')) if psm in (0x13, 0x14, 0x1B, 0x24, 0x2C) else 'direct')] += 1
        filt[('mag ' + ('nearest', 'linear')[(t1 >> 5) & 1], 'min ' + ('nearest', 'linear', 'n_mip_n', 'n_mip_l', 'l_mip_n', 'l_mip_l', '?', '?')[(t1 >> 6) & 7], 'mxl=%d' % ((t1 >> 2) & 7), 'lcm=%d' % (t1 & 1))] += 1
        clampm[(('repeat', 'clamp', 'region_clamp', 'region_repeat')[cl & 3], ('repeat', 'clamp', 'region_clamp', 'region_repeat')[(cl >> 2) & 3])] += 1
        # texture read from a render target: same 64-page-word block base as the current frame or depth buffer
        if tbp == fbp * 32: feedback['texture == current frame buffer'] += 1
        elif tbp == (zb & 0x1FF) * 32: feedback['texture == current depth buffer'] += 1
        elif tbp // 32 in targets: feedback['texture is an earlier render target (%d)' % (tbp // 32)] += 1
    if st['fba%d' % ctx] & 1: misc['FBA'] += 1
    if st['pabe'] & 1: misc['PABE'] += 1
    if not st['colclamp'] & 1: misc['COLCLAMP off (wrap)'] += 1
    if st['dthe'] & 1: misc['DTHE dither'] += 1
    targets.add(fbp)
targets = set()
need = {0: 1, 1: 2, 2: 2, 3: 3, 4: 3, 5: 3, 6: 2, 7: 0}
def vertex(k):
    global pending
    t = prim_now() & 7; pending += 1
    if need[t] and pending >= need[t]:
        if k: kick()
        if t in (0, 1, 3, 6): pending = 0
        else: pending = need[t] - 1
def setprim(v):
    global pending
    st['prim'] = v & 0x7FF; pending = 0
def reg(a, v):
    regs_seen[a] += 1
    if a == 0: setprim(v)
    elif a in (4, 5): vertex(True)
    elif a in (0xC, 0xD): vertex(False)
    elif a in (6, 7): st['tex0_%d' % (a - 6)] = v
    elif a in (8, 9): st['clamp%d' % (a - 8)] = v
    elif a in (0x14, 0x15): st['tex1_%d' % (a - 0x14)] = v
    elif a in (0x16, 0x17): st['tex0_%d' % (a - 0x16)] = (st['tex0_%d' % (a - 0x16)] & ~(0x7FFFF << 37 | 0x3F << 20)) | (v & (0x7FFFF << 37 | 0x3F << 20))  # TEX2
    elif a == 0x1A: st['prmodecont'] = v & 1
    elif a == 0x1B: st['prmode'] = v
    elif a in (0x42, 0x43): st['alpha%d' % (a - 0x42)] = v
    elif a in (0x47, 0x48): st['test%d' % (a - 0x47)] = v
    elif a in (0x4A, 0x4B): st['fba%d' % (a - 0x4A)] = v
    elif a in (0x4C, 0x4D): st['frame%d' % (a - 0x4C)] = v
    elif a in (0x4E, 0x4F): st['zbuf%d' % (a - 0x4E)] = v
    elif a == 0x45: st['dthe'] = v
    elif a == 0x46: st['colclamp'] = v
    elif a == 0x49: st['pabe'] = v
    elif a == 0x50: misc['BITBLTBUF (transfer setup)'] += 1; st['bitblt'] = v
    elif a == 0x53:
        d = v & 3; misc['TRXDIR ' + ('host->local upload', 'local->host readback', 'local->local copy', 'off')[d]] += 1
        if d == 2 and ((st['bitblt'] >> 0) & 0x3FFF) // 32 in targets: misc['local copy FROM a render target'] += 1
    elif a == 0x3F: misc['TEXFLUSH'] += 1
    elif a in (0x34, 0x35, 0x36, 0x37): misc['MIPTBP set'] += 1
    elif a in (0x3B,): misc['TEXA set'] += 1
    elif a in (0x40, 0x41): misc['SCISSOR set'] += 1; st['scissor%d' % (a - 0x40)] = v
    elif a in (0x60, 0x61, 0x62): misc['SIGNAL/FINISH/LABEL'] += 1
paths = {}
with open(path, 'rb') as f:
    while True:
        h = f.read(12)
        if len(h) < 12: break
        kind, p, sz = struct.unpack('<III', h)
        data = f.read(sz)
        if len(data) < sz: break
        if kind == 2 and sz == 8: reg(p, struct.unpack('<Q', data)[0]); continue
        if kind == 7:
            v = struct.unpack('<11Q', data); scan[('pmode=%x' % v[0], 'dispfb1 fbp=%d fbw=%d psm=%d dbx=%d dby=%d' % (v[3] & 0x1FF, (v[3] >> 9) & 0x3F, (v[3] >> 15) & 0x1F, (v[3] >> 32) & 0x7FF, (v[3] >> 43) & 0x7FF), 'display1=%x' % v[4],
                  'dispfb2 fbp=%d fbw=%d psm=%d dbx=%d dby=%d' % (v[5] & 0x1FF, (v[5] >> 9) & 0x3F, (v[5] >> 15) & 0x1F, (v[5] >> 32) & 0x7FF, (v[5] >> 43) & 0x7FF), 'display2=%x' % v[6])] += 1
            continue
        if kind != 1: continue
        pathc[p] += 1
        ps = paths.setdefault(p, {'left': 0}); pos = 0
        while pos + 16 <= len(data):
            if ps['left'] == 0:
                lo, hi = struct.unpack_from('<QQ', data, pos); pos += 16
                nloop = lo & 0x7FFF; pre = (lo >> 46) & 1; prim = (lo >> 47) & 0x7FF; flg = (lo >> 58) & 3; nreg = (lo >> 60) & 15 or 16
                ps.update(flg=flg, nreg=nreg, regs=[(hi >> (4 * i)) & 15 for i in range(nreg)], idx=0)
                if pre and flg != 3 and nloop: setprim(prim)
                ps['left'] = nloop * nreg if flg in (0, 1) else nloop
                if flg >= 2 and nloop: misc['IMAGE data tag'] += 1
                continue
            flg = ps['flg']
            if flg == 0:
                lo, hi = struct.unpack_from('<QQ', data, pos); pos += 16
                r = ps['regs'][ps['idx'] % ps['nreg']]; ps['idx'] += 1; ps['left'] -= 1
                if r in (4, 5): vertex(not (hi >> 47) & 1)
                elif r == 0xE: reg(hi & 0xFF, lo)
                elif r == 0: setprim(lo)
                elif r != 0xF: reg(r, lo) if r not in (1, 2, 3) else None
            elif flg == 1:
                for k in range(2):
                    if ps['left'] == 0: break
                    v = struct.unpack_from('<Q', data, pos + 8 * k)[0]
                    r = ps['regs'][ps['idx'] % ps['nreg']]; ps['idx'] += 1; ps['left'] -= 1
                    if r not in (0xE, 0xF, 1, 2, 3): reg(r, v)
                pos += 16
            else:
                pos += 16; ps['left'] -= 1
total = sum(prims.values())
def show(title, counter, n=14):
    print('\n' + title)
    for k, v in counter.most_common(n): print('  %6.2f%%  %9d  %s' % (100.0 * v / max(total, 1), v, k if isinstance(k, str) else ' '.join(map(str, k))))
print('%d primitives drawn; packets per GIF path: %s' % (total, dict(pathc)))
show('primitive types', prims); show('PRIM flags', flags); show('render targets (fbp, fbw, psm)', frames); show('frame write masks', fbmask, 6)
show('depth buffers (zbp, psm, zmsk)', zbufs, 6); show('depth test', ztest); show('alpha test', atest); show('destination alpha test', dtest)
show('blend equations (of blended primitives)', alpha); show('texture formats', tex, 20); show('texture function', texfn); show('texture filtering', filt); show('texture wrap (u, v)', clampm)
show('render-to-texture / feedback', feedback); show('other state', misc, 20)
show('target + depth + scissor', combos, 40); show('scanouts (count is not a share of primitives)', scan, 8)
