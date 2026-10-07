#!/usr/bin/env python3
"""Match complete audit_texture_uploads.py records by format, shape and bytes.

VRAM addresses are recorded, but are deliberately excluded from the content
identity because the game reuses addresses and relocates assets between frames.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def records(path):
    with open(path, 'rb') as source:
        ordinal = 0
        while header := source.read(32):
            if len(header) != 32:
                raise ValueError('partial transfer header')
            psm, bp, bw, x, y, width, height, size = struct.unpack('<8I', header)
            payload = source.read(size)
            if len(payload) != size:
                raise ValueError('partial transfer payload')
            ordinal += 1
            yield {'ordinal': ordinal, 'psm': psm, 'bp': bp, 'bw': bw,
                   'xy': [x, y], 'wh': [width, height], 'bytes': size,
                   'sha256': hashlib.sha256(payload).hexdigest()}


def identity(record):
    return record['psm'], tuple(record['wh']), record['bytes'], record['sha256']


def compare(reference, candidate, bases):
    selected = [r for r in records(reference) if r['bp'] in bases]
    matches = {identity(r): [] for r in selected}
    candidate_count = 0
    for record in records(candidate):
        candidate_count += 1
        key = identity(record)
        if key in matches:
            matches[key].append(record)
    result = [{'reference': r, 'candidate_matches': matches[identity(r)]} for r in selected]
    return {'selected_reference_uploads': len(selected), 'candidate_uploads': candidate_count,
            'matched_reference_uploads': sum(bool(r['candidate_matches']) for r in result),
            'address_is_not_content_identity': True, 'results': result}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference')
    parser.add_argument('candidate')
    parser.add_argument('--reference-bp', action='append', required=True, type=lambda value: int(value, 0))
    parser.add_argument('--output')
    args = parser.parse_args()
    result = compare(args.reference, args.candidate, set(args.reference_bp))
    if args.output:
        Path(args.output).write_text(json.dumps(result, indent=2))
    print(f"content matched {result['matched_reference_uploads']}/{result['selected_reference_uploads']} reference uploads against {result['candidate_uploads']} candidate uploads")
    for row in result['results']:
        reference = row['reference']
        positions = [(r['ordinal'], r['bp'], r['xy']) for r in row['candidate_matches']]
        print(f"reference {reference['ordinal']} BP={reference['bp']} PSM={reference['psm']} {reference['wh']}: {positions}")
