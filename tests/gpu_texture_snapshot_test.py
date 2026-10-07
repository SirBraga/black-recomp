#!/usr/bin/env python3
"""Verify the offline decoder against a known palette captured from the real GPU."""
from pathlib import Path
import json
import os
import subprocess
import tempfile
import struct
root=Path(__file__).resolve().parents[2]
for targetPSM in (19,20):
    with tempfile.TemporaryDirectory() as directory:
        p=Path(directory);trigger=p/'snapshot';trigger.touch()
        env=os.environ.copy()
        env.update(PS2X_PARALLEL_SNAPSHOT_PSM=str(targetPSM),PS2X_PARALLEL_STREAM_CAPTURE=str(p/'stream.bin'),PS2X_PARALLEL_SNAPSHOT_REQUEST=str(trigger),PS2X_PARALLEL_SNAPSHOT_ON_TEX0='1')
        subprocess.run([str(root/'recomp/gpu/build/black-parallel-texture-test'),str(root/'recomp/gpu/build/libblack-parallel-gs.so'),str(root/'recomp/gpu/moltenvk/libMoltenVK.dylib')],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True)
        state=json.loads((p/'snapshot.registers.json').read_text())
        assert state['stage']=='tex0' and state['trigger_context']==0 and not trigger.exists()
        assert state['capture_current_event'] and state['capture_events']>0 and state['capture_bytes']>0 and state['trigger_packet_bytes']==32
        subprocess.run(['python3',str(root/'ps2recomp/diagnostics/cut_stream_at_snapshot.py'),str(p/'stream.bin'),str(p/'snapshot.registers.json'),str(p/'prefix.bin')],stdout=subprocess.PIPE,check=True)
        data=(p/'prefix.bin').read_bytes()
        assert struct.unpack('<QQ',data[-16:])==(int(state['contexts'][0]['tex0'],16),6)
        planes=struct.unpack('<512H',(p/'snapshot.clut.bin').read_bytes())
        clut=tuple(planes[i]|(planes[i+256]<<16) for i in range(256))
        assert clut==tuple(0x80000000|i|((255-i)<<8)|((i^0x5a)<<16) for i in range(256))
        subprocess.run(['clang++','-std=c++20','-O2','-I'+str(root/'tools/PS2Recomp/ps2xRuntime/include'),str(root/'ps2recomp/diagnostics/gpu_texture_snapshot.cpp'),str(root/'tools/PS2Recomp/ps2xRuntime/src/lib/gs/ps2_gs_memory.cpp'),'-o',str(p/'decode')],stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True)
        subprocess.run([str(p/'decode'),str(p/'snapshot.vram.bin'),state['contexts'][0]['tex0'],str(p/'texture')],stdout=subprocess.PIPE,check=True)
        image=(p/'texture.ppm').read_bytes();header=b'P6\n64 64\n255\n'
        assert image.startswith(header)
        modulus=16 if targetPSM==20 else 256
        expected=bytes(v for i in range(4096) for v in (i%modulus,255-i%modulus,(i%modulus)^0x5a))
        assert image[len(header):]==expected
        subprocess.run([str(p/'decode'),str(p/'snapshot.vram.bin'),state['contexts'][0]['tex0'],str(p/'cached'),str(p/'snapshot.clut.bin')],stdout=subprocess.PIPE,check=True)
        assert (p/'cached.ppm').read_bytes()==image
        assert (p/'texture.pgm').read_bytes()==b'P5\n64 64\n255\n'+bytes((i%modulus)*(17 if targetPSM==20 else 1) for i in range(4096))
        print('PASS: TEX0-triggered GPU snapshot decodes all 4096 RGB pixels and indices correctly')
