#!/usr/bin/env python3
"""Generates the statically recompiled IOP module code for the runtime.

usage: iop_recompile.py OUT.inc iop-<base>-<hash>.bin ...

Inputs are the relocated text of each IRX module as the runner saves it with
PS2X_IOP_RECOMP_CAPTURE=<dir> (name: iop-<load address>-<FNV-1a 64 of the bytes>.bin). They contain
game code: keep them and the generated file local, never commit them.

Each module becomes one small function per basic block (straight-line run of instructions; each
instruction is a call to Impl::stepRecompiled with the instruction word as a literal) and a table with
one entry per instruction word that points at the block starting there. The emulator asks
findRecompiledModule() for that table when a module is loaded.
Blocks start at every static branch/jump target, after every delay slot and call, and wherever a block
had to stop; they end after a delay slot, before an import stub, or after MAX_BLOCK instructions.
Code entered anywhere else (computed jumps into the middle of a block) is interpreted until the next
block start. Import stubs (jr ra / addiu zero,zero,n) are left to the interpreter, which resolves them.
"""
import os, struct, sys

MAX_BLOCK = 24

def control_transfer(word):
    """(is_branch_or_jump, static_target_or_None) for an instruction at `pc` (target computed by caller)."""
    op = word >> 26
    if op == 0:
        funct = word & 0x3F
        return (funct in (0x08, 0x09)), None  # jr / jalr
    if op == 1 or 4 <= op <= 7:
        return True, 'branch'
    if op in (2, 3):
        return True, 'jump'
    return False, None

def main():
    out_path, paths = sys.argv[1], sys.argv[2:]
    functions, tables, finders, total, block_total = [], [], [], 0, 0
    for path in sorted(paths):
        name = os.path.basename(path)
        parts = name[:-4].split('-')
        if len(parts) != 3 or parts[0] != 'iop':
            print('skip %s: unexpected name' % name, file=sys.stderr)
            continue
        base, digest = int(parts[1], 16), parts[2]
        data = open(path, 'rb').read()
        size = len(data) & ~3
        count = size // 4
        words = struct.unpack('<%dI' % count, data[:size])
        def is_stub(index):
            return (words[index] == 0x03E00008 and index + 1 < count
                    and (words[index + 1] & 0xFFFF0000) == 0x24000000)
        leaders = {0}
        for index, word in enumerate(words):
            transfer, kind = control_transfer(word)
            if not transfer:
                continue
            pc = base + index * 4
            if index + 2 < count:
                leaders.add(index + 2)  # after the delay slot (fall-through, or return address of a call)
            target = None
            if kind == 'branch':
                offset = word & 0xFFFF
                offset -= 0x10000 if offset & 0x8000 else 0
                target = pc + 4 + offset * 4
            elif kind == 'jump':
                target = ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            if target is not None and base <= target < base + size and target % 4 == 0:
                leaders.add((target - base) // 4)
        starts = [None] * count
        index = 0
        pending = sorted(leaders)
        done = set()
        while pending:
            index = pending.pop(0)
            if index in done or index >= count or is_stub(index):
                continue
            done.add(index)
            body, at, ended = [], index, False
            while at < count and len(body) < MAX_BLOCK and not is_stub(at) and (at == index or at not in leaders):
                body.append(at)
                transfer, _ = control_transfer(words[at])
                at += 1
                if transfer:
                    if at < count and not is_stub(at):
                        body.append(at)  # delay slot
                        at += 1
                    ended = True
                    break
            if not ended and at < count and at not in leaders and not is_stub(at):
                leaders.add(at)  # block was cut by MAX_BLOCK: continue in a new one
                pending.append(at)
                pending.sort()
            fn = 'iop_%s_%x' % (digest, base + index * 4)
            starts[index] = fn
            lines = ['    static bool %s(Impl &impl, CpuState &cpu, const uint64_t start, const uint32_t budget)' % fn,
                     '    {', '        bool progress = false;']
            for i in body:
                pc = base + i * 4
                lines.append('        IOP_STEP(0x%08xu, 0x%08xu, 0x%08xu)' % (pc, words[i], pc + 4))
            lines += ['        return true;', '    }', '']
            functions.append('\n'.join(lines))
            total += len(body)
            block_total += 1
        table = 'iop_%s_blocks' % digest
        tables.append('    static const RecompiledBlock *%s()\n    {\n        static const RecompiledBlock blocks[] = {\n%s        };\n        return blocks;\n    }\n'
                      % (table, ''.join('            %s,\n' % ('&' + s if s else 'nullptr') for s in starts)))
        finders.append('        if (base == 0x%08xu && textSize == 0x%xu && hash == 0x%sull)\n            return %s();\n' % (base, size, digest, table))
    with open(out_path, 'w') as out:
        out.write('// Generated by ps2recomp/diagnostics/iop_recompile.py from captured IRX module text. Do not edit.\n')
        out.write('// %d modules, %d basic blocks, %d instructions. Included inside IopEmulator::Impl.\n\n' % (len(tables), block_total, total))
        out.write('\n'.join(functions))
        out.write('\n')
        out.write('\n'.join(tables))
        out.write('\n    static const RecompiledBlock *findRecompiledModule(uint32_t base, uint32_t textSize, uint64_t hash)\n    {\n')
        out.write(''.join(finders))
        out.write('        return nullptr;\n    }\n')
    print('wrote %s: %d modules, %d basic blocks, %d instructions' % (out_path, len(tables), block_total, total))

if __name__ == '__main__':
    main()
