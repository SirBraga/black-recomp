#!/usr/bin/env python3
"""Summarize the opt-in PS2X_GIF_TIMELINE_TRACE text log."""
import collections
import re
import sys

if len(sys.argv) != 2:
    raise SystemExit(f"usage: {sys.argv[0]} TIMELINE.events")

pattern = re.compile(
    r"\[gif-timeline\] seq=(\d+) event=(\S+) a=([0-9a-fA-F]+) "
    r"b=([0-9a-fA-F]+) c=([0-9a-fA-F]+)"
)
counts = collections.Counter()
dma_starts = collections.Counter()
active_channels = set()
overlapping_dma_starts = 0
max_active_channels = 0
eops = 0
eops_with_vif1_str = 0
eops_with_gif_str = 0
services_with_both_queued = 0
services_with_gif_queued = 0
services_with_vif1_queued = 0
last_sequence = -1
malformed = 0
vif1_next_offset = None
vif1_prefix_errors = 0
vif1_prefix_bytes = 0

with open(sys.argv[1], encoding="utf-8", errors="replace") as source:
    for line in source:
        match = pattern.search(line)
        if not match:
            malformed += 1
            continue
        sequence = int(match[1])
        event = match[2]
        a, b, c = (int(match[i], 16) for i in (3, 4, 5))
        last_sequence = max(last_sequence, sequence)
        counts[event] += 1
        if event == "DMA_CHAIN_READY" and a == 0x10009000:
            vif1_next_offset = 0
        elif event == "VIF1_PREFIX":
            if b == 0 or b > c or (vif1_next_offset is not None and a != vif1_next_offset):
                vif1_prefix_errors += 1
            vif1_next_offset = a + b
            vif1_prefix_bytes += b
        if event == "DMA_START":
            dma_starts[a] += 1
            if b & 0x100:
                if active_channels and a not in active_channels:
                    overlapping_dma_starts += 1
                active_channels.add(a)
                max_active_channels = max(max_active_channels, len(active_channels))
        elif event == "DMA_COMPLETE":
            active_channels.discard(a)
        elif event == "SERVICE_BEGIN":
            services_with_gif_queued += a > 0
            services_with_vif1_queued += b > 0
            services_with_both_queued += a > 0 and b > 0
        elif event == "PATH3_EOP":
            eops += 1
            gif_chcr = c >> 32
            vif1_chcr = c & 0xFFFFFFFF
            eops_with_gif_str += bool(gif_chcr & 0x100)
            eops_with_vif1_str += bool(vif1_chcr & 0x100)

print(f"events={sum(counts.values())} last_sequence={last_sequence} malformed_lines={malformed}")
for event, count in sorted(counts.items()):
    print(f"{event}={count}")
print("DMA_START per channel: " + ", ".join(f"0x{channel:08x}={count}" for channel, count in sorted(dma_starts.items())))
print(f"SERVICE_BEGIN with queues GIF/VIF1/both={services_with_gif_queued}/{services_with_vif1_queued}/{services_with_both_queued}")
print(f"DMA active overlap starts={overlapping_dma_starts}; max active channels={max_active_channels}; active at capture end={[f'0x{channel:08x}' for channel in sorted(active_channels)]}")
print(f"PATH3_EOP with GIF_STR={eops_with_gif_str}/{eops}; VIF1_STR={eops_with_vif1_str}/{eops}")
print(f"VIF1_PREFIX bytes={vif1_prefix_bytes}; boundary/continuity errors={vif1_prefix_errors}")
