#include "runtime/gs/gs_frontend.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
uint64_t f32(float v)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    return bits;
}

void xyz(GS &gs, int x, int y)
{
    const uint64_t packed = static_cast<uint32_t>(x * 16) | (static_cast<uint64_t>(y * 16) << 16) | (1ull << 32);
    gs.writeRegister(GS_REG_XYZ2, packed);
}
}

int main()
{
    std::vector<uint8_t> ram(4u * 1024u * 1024u);
    GS gs;
    gs.init(ram.data(), static_cast<uint32_t>(ram.size()));

    gs.writeRegister(GS_REG_FRAME_1, 1u << 16);
    gs.writeRegister(GS_REG_ZBUF_1, 1ull << 32);
    gs.writeRegister(GS_REG_TEST_1, 0x20003ull);
    gs.writeRegister(GS_REG_SCISSOR_1, (63ull << 16) | (63ull << 48));
    gs.writeRegister(GS_REG_XYOFFSET_1, 0);
    gs.writeRegister(GS_REG_CLAMP_1, 1ull | (1ull << 2));

    const uint32_t red = 0x000000FFu;
    gs.uploadImageNative((32ull << 32) | (1ull << 48), 0, 1ull | (1ull << 32), 0,
                         reinterpret_cast<const uint8_t *>(&red), 4);
    gs.writeRegister(GS_REG_TEX0_1, 32ull | (1ull << 14) | (1ull << 34) | (1ull << 35));
    gs.writeRegister(GS_REG_PRIM, 3u | (1u << 4));
    gs.writeRegister(GS_REG_RGBAQ, 0x80808080ull | (f32(1.0f) << 32));

    gs.writeRegister(GS_REG_ST, f32(0.0f));
    xyz(gs, 0, 0);
    gs.writeRegister(GS_REG_ST, f32(1.0f));
    xyz(gs, 16, 0);
    gs.writeRegister(GS_REG_ST, f32(0.0f) | (f32(1.0f) << 32));
    xyz(gs, 0, 16);

    const uint32_t mapped = gs.ReadVram(0, 0, 1, 2, 2);
    if ((mapped & 0x00FFFFFFu) != red)
    {
        std::fprintf(stderr, "mapped pixel %08x\n", mapped);
        return 1;
    }

    gs.writeRegister(GS_REG_ST, f32(0.0f));
    xyz(gs, 40, 40);
    gs.writeRegister(GS_REG_ST, f32(1.0f));
    xyz(gs, 56, 40);
    gs.writeRegister(GS_REG_ST, f32(0.0f) | (f32(1.0f) << 32));
    xyz(gs, 40, 56);

    const uint32_t blue = 0x00FF0000u;
    gs.uploadImageNative((32ull << 32) | (1ull << 48), 0, 1ull | (1ull << 32), 0,
                         reinterpret_cast<const uint8_t *>(&blue), 4);
    gs.writeRegister(GS_REG_ST, f32(0.21f) | (f32(0.2f) << 32));
    xyz(gs, 16, 0);
    gs.writeRegister(GS_REG_ST, f32(0.2f) | (f32(0.21f) << 32));
    xyz(gs, 0, 16);
    gs.writeRegister(GS_REG_ST, f32(0.2f) | (f32(0.2f) << 32));
    xyz(gs, 0, 0);

    const uint32_t kept = gs.ReadVram(0, 0, 1, 2, 2);
    if ((kept & 0x00FFFFFFu) != red)
    {
        std::fprintf(stderr, "flat redraw replaced the wall %08x\n", kept);
        return 2;
    }

    gs.writeRegister(GS_REG_ST, f32(0.0f));
    xyz(gs, 20, 0);
    gs.writeRegister(GS_REG_ST, f32(1.0f));
    xyz(gs, 36, 0);
    gs.writeRegister(GS_REG_ST, f32(0.0f) | (f32(1.0f) << 32));
    xyz(gs, 20, 16);
    const uint32_t other = gs.ReadVram(0, 0, 1, 22, 2);
    if ((other & 0x00FFFFFFu) != blue)
    {
        std::fprintf(stderr, "a new triangle did not draw %08x\n", other);
        return 3;
    }

    std::puts("PASS: flat redraw of the same triangle keeps the mapped texture");
    return 0;
}
