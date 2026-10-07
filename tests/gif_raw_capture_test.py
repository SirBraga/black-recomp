#!/usr/bin/env python3
"""Verify raw GIF capture starts at a texture upload without changing dispatch."""
from pathlib import Path
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
program = r'''
#include "runtime/gs/ps2_gif_arbiter.h"
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
int main(int argc,char** argv){
 assert(argc==2);setenv("PS2X_GIF_RAW_CAPTURE",argv[1],1);
 unsigned callbacks=0;
 GifArbiter arbiter([&](const uint8_t*,uint32_t,GifPathId,bool){++callbacks;});
 std::array<uint64_t,4> upload{(1ull<<60)|1u,0xeu,0u,0x50u};
 arbiter.submit(GifPathId::Path3,reinterpret_cast<const uint8_t*>(upload.data()),32);arbiter.drain();
 upload[2]=12000ull<<32;
 arbiter.submit(GifPathId::Path3,reinterpret_cast<const uint8_t*>(upload.data()),32);arbiter.drain();
 std::array<uint64_t,2> draw{0x8000u,0xfu};
 arbiter.submit(GifPathId::Path1,reinterpret_cast<const uint8_t*>(draw.data()),16);arbiter.drain();
 std::array<uint64_t,4> image{(2ull<<58)|2u,0u,0x1234u,0x5678u};
 arbiter.submit(GifPathId::Path2,reinterpret_cast<const uint8_t*>(image.data()),32);arbiter.drain();
 image={(2ull<<58)|1u,0u,0x9abcu,0xdef0u};
 arbiter.submit(GifPathId::Path2,reinterpret_cast<const uint8_t*>(image.data()),32,false,true);arbiter.drain();
 assert(callbacks==5);
}
'''
with tempfile.TemporaryDirectory(prefix='black-gif-raw-') as directory:
    path = Path(directory)
    (path/'test.cpp').write_text(program)
    subprocess.run(['clang++', '-std=c++20', '-fsanitize=address,undefined',
                    '-I', str(root/'tools/PS2Recomp/ps2xRuntime/include'),
                    str(path/'test.cpp'), str(root/'tools/PS2Recomp/ps2xRuntime/src/lib/gs/ps2_gif_arbiter.cpp'),
                    '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test'), str(path/'capture.bin')], check=True)
    data = (path/'capture.bin').read_bytes()
    expected = (struct.pack('<III4Q', 1, 3, 32, (1 << 60) | 1, 14, 12000 << 32, 0x50)
                + struct.pack('<III2Q', 1, 1, 16, 0x8000, 15)
                + struct.pack('<III4Q', 1, 2, 32, (2 << 58) | 2, 0, 0x1234, 0x5678)
                + struct.pack('<III2Q', 1, 2, 16, 0x9abc, 0xdef0))
    assert data == expected, 'capture changed bytes, order, or start boundary'
print('PASS: raw GIF capture retains hardware bytes/path/order, strips only synthetic DIRECT headers, and preserves renderer callbacks')
