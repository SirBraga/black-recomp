#!/usr/bin/env python3
"""Check DMA send register preparation against Black's original SDK instructions."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'tools/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/Helpers/Support.h').read_text()
start = source.index('    uint32_t normalizeQwcFromArg(')
end = source.index('    struct ParsedDmaTag', start)
# Original sceDmaSend[ N ]: AND -13, OR 0x105 [0x101], SW QWC.
sync_start = source.index('    int32_t submitDmaSync(')
sync_end = source.index('\n    }', sync_start) + len('\n    }')
stubs = r'''
struct R5900Context { uint32_t arguments[2]; };
struct Memory {
    uint32_t chcr = 0, flushes = 0;
    uint32_t readIORegister(uint32_t) { return chcr; }
    void processPendingTransfers() { ++flushes; }
};
struct PS2Runtime { Memory mem; Memory &memory() { return mem; } };
uint32_t getRegU32(R5900Context *ctx, unsigned index) { return ctx->arguments[index - 4]; }
uint32_t resolveDmaChannelBase(uint8_t *, uint32_t arg) { return arg; }
'''
program = '#include <cstdint>\n#include <cassert>\n' + source[start:end] + stubs + source[sync_start:sync_end] + r''' 
int main() {
    for (uint32_t chcr = 0; chcr != 65536; ++chcr) {
        assert(dmaSendChcr(chcr, false) == ((chcr & 0xfffffff3u) | 0x105u));
        assert(dmaSendChcr(chcr, true) == ((chcr & 0xfffffff3u) | 0x101u));
    }
    for (uint32_t value : {0u, 1u, 65535u, 65536u, 65537u, 0xffffffffu})
        assert(normalizeQwcFromArg(value) == (value & 65535u));
    assert((dmaSendChcr(0, false) & 0x80u) == 0); // no unsolicited TIE
    assert((dmaSendChcr(0xc0, false) & 0xc0u) == 0xc0u); // preserve TIE and TTE
    assert(dmaSendChcr(0xabcd003cu, false) == 0xabcd0135u); // retain tag/ASP
    PS2Runtime runtime; R5900Context ctx{{0x10009000u, 1u}};
    assert(submitDmaSync(nullptr, &ctx, &runtime) == 0);
    runtime.mem.chcr = 0x100u;
    assert(submitDmaSync(nullptr, &ctx, &runtime) == 1);
    assert(submitDmaSync(nullptr, &ctx, &runtime) == 1);
    runtime.mem.chcr = 0;
    assert(submitDmaSync(nullptr, &ctx, &runtime) == 0);
    ctx.arguments[1] = 0;
    assert(submitDmaSync(nullptr, &ctx, &runtime) == 0);
    assert(runtime.mem.flushes == 1);
    assert(submitDmaSync(nullptr, &ctx, nullptr) == -1);
}
'''
program = program.replace('#include <cassert>', '#include <cassert>\n#include <initializer_list>')
with tempfile.TemporaryDirectory(prefix='black-dma-sdk-') as directory:
    path = pathlib.Path(directory)
    (path / 'test.cpp').write_text(program)
    subprocess.run(['clang++', '-std=c++20', '-fsanitize=address,undefined', str(path/'test.cpp'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
print('PASS: DMA MODE/DIR/STR, preserved TTE/TIE/ASP/tag bits, and QWC boundary values')
