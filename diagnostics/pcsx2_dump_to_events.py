#!/usr/bin/env python3
"""Read an uncompressed PCSX2 GS dump and export its GIF transfers as events.

Format reference: PCSX2 GSDumpBase::AddHeader/Transfer/VSync. This exports
commands, not initial frozen GS state, and is not a standalone rendering replay.
"""
import argparse
import collections
import struct
from pathlib import Path


def convert(source, destination, view_state=False):
    data = Path(source).read_bytes()
    if len(data) < 44 or struct.unpack_from('<I', data)[0] != 0xffffffff:
        raise ValueError('expected a modern uncompressed PCSX2 GS dump')
    header_size = struct.unpack_from('<I', data, 4)[0]
    header = struct.unpack_from('<9I', data, 8)
    version, state_size = header[:2]
    if header_size < 36 or 8 + header_size + state_size + 8192 > len(data):
        raise ValueError('truncated header, state, or GS privileged registers')
    position = 8 + header_size + state_size + 8192
    counts = collections.Counter()
    paths = collections.Counter()
    payload_bytes = 0
    events = bytearray()
    if view_state:
        if version != 9 or state_size < 316:
            raise ValueError('view-state import is verified only for GS state version 9')
        state_offset = 8 + header_size
        if struct.unpack_from('<I', data, state_offset)[0] != version:
            raise ValueError('frozen state/header versions disagree')
        registers = [(0x00, 4), (0x1a, 12)]
        for context in range(2):
            base = 124 + 96 * context
            registers.extend([(0x18 + context, base), (0x40 + context, base + 48),
                              (0x4c + context, base + 80)])
        for address, offset in registers:
            events.extend(struct.pack('<IIIQ', 2, address, 8,
                struct.unpack_from('<Q', data, state_offset + offset)[0]))
    while position < len(data):
        kind = data[position]
        position += 1
        counts[kind] += 1
        if kind == 0:
            if position + 5 > len(data):
                raise ValueError('truncated transfer header')
            path, size = struct.unpack_from('<BI', data, position)
            position += 5
            if path > 3 or position + size > len(data):
                raise ValueError('invalid transfer path or truncated payload')
            # PCSX2 index 3 is its merged GS transfer stream, not hardware
            # PATH3. Use zero to retain that distinction in this analysis file.
            output_path = 0 if path == 3 else path + 1
            events.extend(struct.pack('<III', 1, output_path, size))
            events.extend(data[position:position + size])
            position += size
            paths[output_path] += 1
            payload_bytes += size
        elif kind == 1:
            if position + 1 > len(data):
                raise ValueError('truncated VSync')
            events.extend(struct.pack('<IIII', 4, 0, 4, data[position]))
            position += 1
        elif kind == 2:
            position += 4
        elif kind == 3:
            position += 8192
        else:
            raise ValueError(f'unknown packet kind {kind} at {position - 1}')
        if position > len(data):
            raise ValueError('truncated packet')
    Path(destination).write_bytes(events)
    return {'state_version': version, 'packets': dict(counts),
            'paths': dict(paths), 'gif_bytes': payload_bytes,
            'initial_state_exported': False, 'initial_view_registers_exported': view_state}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump')
    parser.add_argument('events')
    parser.add_argument('--view-state', action='store_true', help='Import initial PRIM/PRMODECONT, XYOFFSET, SCISSOR and FRAME for vertex analysis')
    args = parser.parse_args()
    print(convert(args.dump, args.events, args.view_state))
