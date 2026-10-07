#!/usr/bin/env python3
"""Map a flattened GIF-chain byte offset back to its EE DMA source tag."""

import argparse

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("map", help="PS2X_DMA_CHAIN_MAP_ACTIVE output")
p.add_argument("offset", type=lambda value: int(value, 0), help="offset in the matching PATH3 event")
args = p.parse_args()

with open(args.map, encoding="ascii") as file:
    header = file.readline().strip()
    entries = []
    for line in file:
        address, offset, low, high = line.split()
        entries.append((int(address, 16), int(offset), int(low, 16), int(high, 16)))

size = int(next(part.split("=", 1)[1] for part in header.split() if part.startswith("bytes=")))
if not 0 <= args.offset < size:
    p.error(f"offset must be within the {size}-byte chain")

for i, (tag_address, begin, low, high) in enumerate(entries):
    end = entries[i + 1][1] if i + 1 < len(entries) else size
    if not begin <= args.offset < end:
        continue
    qwc = low & 0xFFFF
    tag_id = (low >> 28) & 7
    ref_address = (low >> 32) & 0x7FFFFFFF
    within = args.offset - begin
    if tag_id in (0, 3, 4):
        source = ref_address + within
    elif tag_id in (1, 2, 5, 6, 7):
        source = tag_address + 16 + within
    else:
        source = None
    print(f"chain_bytes={size} tag=0x{tag_address:08x} id={tag_id} qwc={qwc} "
          f"interval=[{begin},{end}) offset={args.offset} "
          f"ee_source={f'0x{source:08x}' if source is not None else 'unknown'}")
    break
