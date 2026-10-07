#!/usr/bin/env python3
"""Acha alvos de codigo alcancaveis so por ponteiro no ELF do Black.

O PS2Recomp so conhece inicios de funcao que o analyzer/Ghidra descobriu. Callbacks,
entradas de vtable (GCC 2.95: {s16 delta, s16 idx, u32 pfn}) e handlers registrados
em runtime ficam de fora e viram "missing-target" no JALR. Este script coleta:

  1. pares lui/addiu (e lui/ori) cujo valor cai em .text;
  2. palavras de 32 bits alinhadas nas secoes de dados que apontam para .text;

e mantem so os que parecem inicio de funcao (logo apos um `jr ra` + delay slot,
opcionalmente seguido de nops de alinhamento, ou comecando com `addiu sp,sp,-N`).

Saida: lista de "sub_XXXXXXXX@0xXXXXXXXX" pronta para general.entry_points.
Uso: find_code_pointers.py <elf> [--toml <config.toml> para gravar in-place]
"""
from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

JR_RA = 0x03E00008
NOP = 0x00000000


def load_sections(elf: bytes):
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 1:
        raise SystemExit("esperado ELF32 little-endian")
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
    raw = [struct.unpack_from("<IIIIIIIIII", elf, shoff + i * shentsize) for i in range(shnum)]
    strtab_off = raw[shstrndx][4]
    sections = []
    for name_off, sh_type, flags, addr, off, size, *_ in raw:
        end = elf.index(b"\0", strtab_off + name_off)
        name = elf[strtab_off + name_off:end].decode("latin1")
        if sh_type == 1 and addr and size:  # PROGBITS carregada
            sections.append((name, addr, off, size, flags))
    return sections


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    elf = Path(sys.argv[1]).read_bytes()
    sections = load_sections(elf)
    text = next(s for s in sections if s[0] == ".text")
    _, t_addr, t_off, t_size, _ = text
    words = struct.unpack_from(f"<{t_size // 4}I", elf, t_off)

    def word_at(addr: int) -> int | None:
        i = (addr - t_addr) >> 2
        return words[i] if 0 <= i < len(words) else None

    def in_text(v: int) -> bool:
        return t_addr <= v < t_addr + t_size and v % 4 == 0

    def looks_like_start(v: int) -> bool:
        first = word_at(v)
        if first is None or first == NOP:
            return False
        if (first & 0xFFFF8000) == 0x27BD8000:  # addiu sp,sp,-N
            return True
        # recua sobre nops de alinhamento ate o delay slot do jr ra anterior
        p = v - 4
        for _ in range(4):
            w = word_at(p)
            if w is None:
                return False
            if word_at(p - 4) == JR_RA:
                return True
            if w != NOP:
                return False
            p -= 4
        return False

    candidates: set[int] = set()

    # 1. lui/addiu|ori no .text (janela curta, rastreando o registrador do lui)
    hi: dict[int, tuple[int, int]] = {}
    for i, w in enumerate(words):
        op = w >> 26
        rs, rt, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 0x0F:  # lui
            hi[rt] = (imm, i)
            continue
        if op in (0x09, 0x0D) and rs in hi and i - hi[rs][1] <= 16:  # addiu / ori
            lo = imm - 0x10000 if (op == 0x09 and imm & 0x8000) else imm
            v = ((hi[rs][0] << 16) + lo) & 0xFFFFFFFF
            if in_text(v):
                candidates.add(v)

    # 2. ponteiros em dados
    for name, addr, off, size, flags in sections:
        if name in (".text", ".vutext") or flags & 0x4:  # pula codigo executavel
            continue
        for j in range(0, size - 3, 4):
            v, = struct.unpack_from("<I", elf, off + j)
            if in_text(v):
                candidates.add(v)

    starts = sorted(v for v in candidates if looks_like_start(v))
    hints = [f"sub_{v:08X}@0x{v:08X}" for v in starts]
    print(f"[find_code_pointers] {len(candidates)} ponteiros para .text, {len(hints)} parecem inicio de funcao",
          file=sys.stderr)

    if len(sys.argv) >= 4 and sys.argv[2] == "--toml":
        cfg = Path(sys.argv[3])
        body = cfg.read_text()
        # Mescla com as entradas que o analyzer ja gerou (nao descartar nenhuma).
        existing: list[str] = []
        m = re.search(r"(?ms)^entry_points = \[(.*?)^\]\n", body)
        if m:
            existing = re.findall(r'"([^"]+)"', m.group(1))
            body = body[:m.start()] + body[m.end():]
        known_addrs = {e.rsplit("@", 1)[-1].lower() for e in existing}
        merged = existing + [h for h in hints if h.rsplit("@", 1)[-1].lower() not in known_addrs]
        block = "entry_points = [\n" + "".join(f'  "{h}",\n' for h in merged) + "]\n"
        body = body.replace("[general]\n", "[general]\n" + block, 1)
        print(f"[find_code_pointers] {len(existing)} entradas do analyzer + {len(merged) - len(existing)} novas",
              file=sys.stderr)
        cfg.write_text(body)
        print(f"[find_code_pointers] gravado em {cfg}", file=sys.stderr)
    else:
        print("\n".join(hints))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
