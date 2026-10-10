#!/usr/bin/env python3
"""sample_game_thread.py <sample.txt> [n]: where the game (EE) thread's time goes, by category and by function."""
import re, sys
txt = open(sys.argv[1]).read()
call = txt[txt.index('Call graph:'):txt.index('Total number in stack')]
t = [x for x in re.split(r'\n    (?=\d+ Thread_)', call) if 'GameThread' in x.split('\n', 1)[0]][0]
P = []
for l in t.split('\n')[1:]:
    m = re.match(r'^([ +!:|]*)(\d+) (.+?)  \(in ([^)]+)\)', l)
    if m: P.append((len(m.group(1)), int(m.group(2)), m.group(3)))
total = P[0][1]
leaf = {}
for i, (d, n, s) in enumerate(P):
    child = 0
    for d2, n2, _ in P[i + 1:]:
        if d2 <= d: break
        if d2 == d + 2: child += n2
    if n - child > 0: leaf[s] = leaf.get(s, 0) + n - child
def cat(s):
    if s.startswith(('sub_', 'func_', 'entry_')): return 'EE guest code'
    if 'VU1Interpreter' in s or 'VuRecompiled' in s or 'Vu0' in s or 'VU0' in s: return 'VU0'
    if 'iop' in s.lower(): return 'IOP'
    if any(k in s for k in ('psynch', 'semwait', 'mach_msg', 'ulock', 'semaphore_wait')): return 'waiting'
    if any(k in s for k in ('EeScheduler', 'dispatchGuestBranch', 'tlv_get_addr', 'advanceEeTimers', 'eeCheckpointDue', 'lookupFunction')): return 'EE dispatch/scheduling'
    if any(k in s for k in ('memmove', 'memcpy', 'memset', 'bzero', 'vector<', 'malloc', 'free', 'operator new', 'operator delete')): return 'copies/allocation'
    return 'other runtime'
c = {}
for s, v in leaf.items(): c[cat(s)] = c.get(cat(s), 0) + v
print('game thread, %d samples' % total)
for k, v in sorted(c.items(), key=lambda x: -x[1]): print('  %5.1f%%  %s' % (100 * v / total, k))
n = int(sys.argv[2]) if len(sys.argv) > 2 else 16
print('top functions outside the guest code:')
for s, v in sorted(leaf.items(), key=lambda x: -x[1]):
    if cat(s) not in ('EE guest code', 'waiting') and n > 0:
        print('  %4d  %s' % (v, s[:100])); n -= 1
print('top guest functions:', ', '.join('%s %d' % (s[:14], v) for s, v in sorted(leaf.items(), key=lambda x: -x[1]) if cat(s) == 'EE guest code')[:400])
