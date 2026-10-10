#!/usr/bin/env python3
"""Adds our instruction patches to the recompiler configuration (build.sh runs it before recompiling).

COP2 divide latency: at 0x37EB14 and 0x37EB30 the game starts a VU0 divide and, in the very next instruction, reads Q.
On the console that read still sees the result of the PREVIOUS divide (the new one takes 7 cycles), and the code
relies on it: it is a reciprocal per component, each divide started while the previous result is collected. The
recompiled code resolves a divide at once, so the two instructions are swapped: same values as on the console. These
are the only two places in the game with a Q read inside a divide's latency without a VWAITQ (checked over all
generated code), and the same swap PCSX2 ships for this game ("COP2 Rearrangement. Fixes broken collisions").
"""
import sys
PATCHES = [(0x0037EB14, 0x4B000460), (0x0037EB18, 0x4AF103BC), (0x0037EB30, 0x4A800460), (0x0037EB34, 0x4B7103BC)]
path = sys.argv[1]
text = open(path).read()
if '[patches]' in text:
    sys.exit('the configuration already has a [patches] table: merge by hand')
text += '\n[patches]\ninstructions = [\n' + ''.join('  { address = "0x%08X", value = "0x%08X" },\n' % p for p in PATCHES) + ']\n'
open(path, 'w').write(text)
print('added %d instruction patches' % len(PATCHES))
