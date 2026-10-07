#!/usr/bin/env python3
"""List valid GIFtag EOP boundaries inside one captured raw GIF event."""
import argparse
import json
import struct


def event_payload(path, number):
    with open(path, 'rb') as stream:
        for index in range(1, number + 1):
            header = stream.read(12)
            if len(header) != 12:
                raise ValueError('event outside capture')
            kind, gif_path, size = struct.unpack('<III', header)
            if index != number:
                stream.seek(size, 1)
                continue
            payload = stream.read(size)
            if len(payload) != size or kind != 1:
                raise ValueError('event is not a complete raw GIF transfer')
            return gif_path, payload


def boundaries(payload):
    offset = 0
    tags = 0
    eops = []
    while offset < len(payload):
        if offset + 16 > len(payload):
            raise ValueError(f'partial GIFtag at byte {offset}')
        lo, _ = struct.unpack_from('<QQ', payload, offset)
        loops = lo & 0x7fff
        fmt = (lo >> 58) & 3
        registers = (lo >> 60) & 15 or 16
        qwords = loops * registers if fmt == 0 else (loops * registers + 1) // 2 if fmt == 1 else loops
        end = offset + 16 + qwords * 16
        if end > len(payload):
            raise ValueError(f'GIFtag at byte {offset} exceeds event')
        tags += 1
        if (lo >> 15) & 1:
            eops.append(end)
        offset = end
    return {'bytes': len(payload), 'tags': tags, 'eop_end_offsets': eops}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stream')
    parser.add_argument('event', type=int)
    args = parser.parse_args()
    if args.event < 1:
        parser.error('event must be positive')
    gif_path, payload = event_payload(args.stream, args.event)
    print(json.dumps({'event': args.event, 'path': gif_path,
                      **boundaries(payload)}, indent=2))


if __name__ == '__main__':
    main()
