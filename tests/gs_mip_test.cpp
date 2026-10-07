#include "runtime/gs/gs_cpu_backend.h"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

int main()
{
    void *allocation = std::aligned_alloc(16384, 4 * 1024 * 1024);
    assert(allocation);
    std::unique_ptr<uint8_t, decltype(&std::free)> owned(static_cast<uint8_t *>(allocation), &std::free);
    std::memset(owned.get(), 0, 4 * 1024 * 1024);
    GSCpuBackend gs;
    gs.Initialize(owned.get(), 4 * 1024 * 1024);

    constexpr uint32_t kBase = 64u;
    constexpr uint32_t kMip1 = 256u;
    constexpr uint32_t kMagenta = 0xFFFF00FFu;
    constexpr uint32_t kGreen = 0xFF00FF00u;
    constexpr uint32_t kBlue = 0xFFFF0000u;
    constexpr uint32_t kMip2 = 400u;
    for (uint32_t y = 0; y < 64u; ++y)
        for (uint32_t x = 0; x < 64u; ++x)
            gs.WriteVram(GS_PSM_CT32, kBase, 1u, x, y, kMagenta);
    for (uint32_t y = 0; y < 32u; ++y)
        for (uint32_t x = 0; x < 32u; ++x)
            gs.WriteVram(GS_PSM_CT32, kMip1, 1u, x, y, kGreen);
    for (uint32_t y = 0; y < 32u; ++y)
        for (uint32_t x = 0; x < 32u; ++x)
            gs.WriteVram(GS_PSM_CT32, kMip2, 1u, x, y, kBlue);

    auto draw = [&](float span, float uvSpan, float q, int texels, uint64_t tex1, uint64_t miptbp1) {
        GSPrimitiveBatch b{};
        b.vertexCount = 3;
        b.state.prim.type = GS_PRIM_TRIANGLE;
        b.state.prim.tme = true;
        auto &c = b.state.context;
        c.frame.fbw = 1;
        c.frame.psm = GS_PSM_CT32;
        c.zbuf.zmask = true;
        c.zbuf.psm = GS_PSM_Z32;
        c.zbuf.zbp = 800;
        c.test = 1ull << 17;
        c.scissor = {0, 63, 0, 63};
        c.tex0.tbp0 = kBase;
        c.tex0.tbw = 1;
        c.tex0.psm = GS_PSM_CT32;
        c.tex0.tw = 6;
        c.tex0.th = 6;
        c.tex0.tfx = 1;
        c.tex0.tcc = 1;
        c.tex1 = tex1;
        c.miptbp1 = miptbp1;
        b.state.textureWidth = texels;
        b.state.textureHeight = texels;
        auto set = [](GSVertex &v, float x, float y, float s, float t, float qv) {
            v.x = x;
            v.y = y;
            v.s = s;
            v.t = t;
            v.q = qv;
            v.a = 128;
        };
        set(b.vertices[0], 0, 0, 0, 0, q);
        set(b.vertices[1], span, 0, uvSpan * q, 0, q);
        set(b.vertices[2], 0, span, 0, uvSpan * q, q);
        gs.Submit(b);
        gs.Sync(GSSyncReason::DebugReadback);
        return gs.ReadVram(GS_PSM_CT32, 0u, 1u, 2u, 2u);
    };

    // MXL=3, MMAG=1, MMIN=4, K=0. A 32px triangle covering the whole 64px
    // texture is 2 texels per pixel, so the sample comes from mip 1.
    const uint64_t tex1 = (3u << 2) | (1u << 5) | (4u << 6);
    const uint64_t mip1 = kMip1 | (1ull << 14);
    const uint32_t far = draw(32.0f, 1.0f, 1.0f, 64, tex1, mip1);
    if (far != kGreen)
    {
        std::cerr << "density 2 sampled 0x" << std::hex << far << " instead of the mip\n";
        return 1;
    }
    // The same texture across 64px is one texel per pixel: stay on the base.
    const uint32_t near = draw(64.0f, 1.0f, 1.0f, 64, tex1, mip1);
    if (near != kMagenta)
    {
        std::cerr << "density 1 sampled 0x" << std::hex << near << " instead of the base\n";
        return 1;
    }
    // A few pixels of a wall, Q=0.02 and K=-3.125. The old Q-only LOD picked
    // mip 2 and flattened the triangle. The UV span stays on the base.
    const uint64_t tex1Wall = tex1 | (0xFCEull << 32);
    const uint64_t mipWall = mip1 | ((kMip2 | (1ull << 14)) << 20);
    const uint32_t wall = draw(8.0f, 0.04f, 0.02f, 128, tex1Wall, mipWall);
    if (wall != kMagenta)
    {
        std::cerr << "wall triangle sampled 0x" << std::hex << wall << " instead of the base\n";
        return 1;
    }
    std::puts("PASS: mip level follows texel density and MIPTBP1");
    return 0;
}
