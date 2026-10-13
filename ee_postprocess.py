#!/usr/bin/env python3
"""Rewrites the recompiler's EE functions for speed, without changing what they compute.

  ee_postprocess.py <generated dir> <runner source dir>     (build.sh runs it after the recompiler)

The recompiler's output stores the program counter in the context before every instruction and the delay-slot
state around every branch. Nothing looks at them except code that leaves the function or a memory access that
lands on a hardware register (ps2_runtime_macros.h, "Guest memory access"). This script keeps both in a local of
the function instead (PS2X_EE_FRAME) and leaves a real store only where something can observe it:

  * `ctx->pc = X;` becomes PS2X_PC(X) when, from there to the next assignment of the program counter, the function
    only computes (registers, memory through the READ/WRITE macros). Before a call, a return, a goto, a read of
    `ctx->pc` or the end of the function it becomes PS2X_PC_SYNC(X), which stores it as before.
  * the three stores around a delay slot become PS2X_DELAY_BEGIN / PS2X_DELAY_END under the same condition.
  * the checkpoint of a backward branch becomes PS2X_EE_CHECKPOINT(), whose common outcome is decided inline.
  * `rdram` and `ctx` are declared `__restrict`: guest memory and the context never overlap, so a store to one
    does not make the compiler reload what it holds from the other.

Files that are not EE functions (overrides, the function table) are left alone.
"""
import re, sys
from pathlib import Path

SIGNATURE = re.compile(r'^void (\w+)\(uint8_t\* rdram, R5900Context\* ctx, PS2Runtime \*runtime\) \{\s*$')
PC_ASSIGN = re.compile(r'^(\s*)ctx->pc = ([^;]+);\s*$')
OBSERVES = re.compile(r'runtime->|ps2_stubs::|ps2_syscalls::|\bsub_\w+\(|\bentry_\w+\(|\breturn\b|\bgoto\b|\bthrow\b|ctx->pc|^\}')
DELAY_ON = re.compile(r'^\s*ctx->in_delay_slot = true;\s*$')
DELAY_OFF = re.compile(r'^\s*ctx->in_delay_slot = false;\s*$')
BRANCH_PC = re.compile(r'^\s*ctx->branch_pc = (0x[0-9A-Fa-f]+u);\s*$')
CONST = re.compile(r'^0x[0-9A-Fa-f]+u$')


CONDITIONAL = re.compile(r'^\s*(\}\s*)?(if\b|else\b|switch\b|for\b|while\b|do\b).*\{\s*$')


def scan(lines, i, n):
    """From the assignment at line i: is the program counter observed before it is assigned again on every path?
    Returns (observed, line of the next assignment that every path reaches). A conditional block is a path of its
    own: it is fine if it assigns before anything observes, and the scan goes on after it for the path around it."""
    j = i + 1
    while j < n:
        line = lines[j]
        if PC_ASSIGN.match(line):
            return False, j
        if OBSERVES.search(line):
            return True, j
        if CONDITIONAL.match(line):
            depth = line.count('{') - line.count('}')
            k = j + 1
            settled = False
            while k < n and depth > 0:
                inner = lines[k]
                if not settled:
                    if PC_ASSIGN.match(inner):
                        settled = True
                    elif OBSERVES.search(inner):
                        return True, k
                depth += inner.count('{') - inner.count('}')
                k += 1
            j = k
            continue
        j += 1
    return True, j


def transform(text):
    lines = text.split('\n')
    out = []
    stats = {'lazy': 0, 'kept': 0, 'delay_lazy': 0, 'delay_kept': 0}
    i, n = 0, len(lines)
    in_function = False
    while i < n:
        line = lines[i]
        m = SIGNATURE.match(line)
        if m:
            out.append('void %s(uint8_t* __restrict rdram, R5900Context* __restrict ctx, PS2Runtime *runtime) {' % m.group(1))
            out.append('    PS2X_EE_FRAME;')
            in_function = True
            i += 1
            continue
        if not in_function:
            out.append(line)
            i += 1
            continue
        if line.startswith('}'):
            in_function = False
            out.append(line)
            i += 1
            continue
        if 'runtime->eeCheckpointDue()' in line:
            out.append(line.replace('runtime->eeCheckpointDue()', 'PS2X_EE_CHECKPOINT()'))
            i += 1
            continue
        m = PC_ASSIGN.match(line)
        if not m:
            out.append(line)
            i += 1
            continue
        indent, value = m.group(1), m.group(2).strip()
        observed, j = scan(lines, i, n)
        delay = i + 2 < n and DELAY_ON.match(lines[i + 1]) and BRANCH_PC.match(lines[i + 2])
        if delay:
            # The slot ends at the store that clears the flag; it has to come before anything that observes.
            off = None
            if not observed:
                for k in range(i + 3, j):
                    if DELAY_OFF.match(lines[k]):
                        off = k
                        break
            if off is not None and CONST.match(value) and not any(DELAY_ON.match(lines[k]) or BRANCH_PC.match(lines[k]) for k in range(i + 3, j)):
                out.append('%sPS2X_DELAY_BEGIN(%s, %s);' % (indent, value, BRANCH_PC.match(lines[i + 2]).group(1)))
                for k in range(i + 3, off):
                    out.append(lines[k])
                out.append(re.match(r'^\s*', lines[off]).group(0) + 'PS2X_DELAY_END();')
                stats['delay_lazy'] += 1
                i = off + 1
                continue
            stats['delay_kept'] += 1
            out.append('%sPS2X_PC_SYNC(%s);' % (indent, value))
            i += 1
            continue
        if observed or not CONST.match(value):
            stats['kept'] += 1
            out.append('%sPS2X_PC_SYNC(%s);' % (indent, value))
        else:
            stats['lazy'] += 1
            out.append('%sPS2X_PC(%s);' % (indent, value))
        i += 1
    return '\n'.join(out), stats


def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    total = {'lazy': 0, 'kept': 0, 'delay_lazy': 0, 'delay_kept': 0}
    files = 0
    for path in sorted(list(src.glob('sub_*.cpp')) + list(src.glob('entry_*.cpp'))):
        text = path.read_text()
        if 'PS2X_EE_FRAME' in text:
            sys.exit('%s is already rewritten: the first argument must be the recompiler output' % path)
        new, stats = transform(text)
        for key in total:
            total[key] += stats[key]
        target = dst / path.name
        if not target.exists() or target.read_text() != new:
            target.write_text(new)
        files += 1
    print('ee_postprocess: %d files; program counter kept in a local at %d instructions, stored at %d; delay slots: %d local, %d stored'
          % (files, total['lazy'], total['kept'], total['delay_lazy'], total['delay_kept']))


if __name__ == '__main__':
    main()
