#include "runtime/gs/gs_cpu_backend.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

static uint32_t drawOnce(GSCpuBackend &gs, uint32_t tbp, uint32_t color)
{
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
    c.scissor = {0, 31, 0, 31};
    c.tex0.tbp0 = tbp;
    c.tex0.tbw = 1;
    c.tex0.psm = GS_PSM_CT32;
    c.tex0.tw = 3;
    c.tex0.th = 3;
    c.tex0.tfx = 1;
    c.tex0.tcc = 1;
    b.state.textureWidth = 8;
    b.state.textureHeight = 8;
    auto set = [](GSVertex &v, float x, float y, float s, float t) {
        v.x = x;
        v.y = y;
        v.s = s;
        v.t = t;
        v.q = 1.0f;
        v.a = 128;
        v.r = 128;
        v.g = 128;
        v.b = 128;
    };
    (void)color;
    set(b.vertices[0], 0, 0, 0, 0);
    set(b.vertices[1], 16, 0, 1, 0);
    set(b.vertices[2], 0, 16, 0, 1);
    gs.Submit(b);
    gs.Sync(GSSyncReason::DebugReadback);
    return gs.ReadVram(GS_PSM_CT32, 0u, 1u, 2u, 2u);
}

int main()
{
    void *allocation = std::aligned_alloc(16384, 4 * 1024 * 1024);
    assert(allocation);
    std::unique_ptr<uint8_t, decltype(&std::free)> owned(static_cast<uint8_t *>(allocation), &std::free);
    std::memset(owned.get(), 0, 4 * 1024 * 1024);
    GSCpuBackend gs;
    gs.Initialize(owned.get(), 4 * 1024 * 1024);

    constexpr uint32_t kTex = 64u;
    constexpr uint32_t kRed = 0xFF0000FFu;
    constexpr uint32_t kGreen = 0xFF00FF00u;
    for (uint32_t y = 0; y < 8u; ++y)
        for (uint32_t x = 0; x < 8u; ++x)
            gs.WriteVram(GS_PSM_CT32, kTex, 1u, x, y, kRed);

    const uint32_t first = drawOnce(gs, kTex, kRed);
    if (first != kRed)
    {
        std::cerr << "first draw 0x" << std::hex << first << "\n";
        return 1;
    }
    const uint32_t cached = drawOnce(gs, kTex, kRed);
    if (cached != kRed)
    {
        std::cerr << "cached draw 0x" << std::hex << cached << "\n";
        return 1;
    }

    gs.WriteVram(GS_PSM_CT32, kTex, 1u, 1u, 1u, kGreen);
    for (uint32_t y = 0; y < 8u; ++y)
        for (uint32_t x = 0; x < 8u; ++x)
            gs.WriteVram(GS_PSM_CT32, kTex, 1u, x, y, kGreen);
    const uint32_t after = drawOnce(gs, kTex, kGreen);
    if (after != kGreen)
    {
        std::cerr << "stale cache after texture write 0x" << std::hex << after << "\n";
        return 1;
    }

    std::puts("PASS: decoded texture cache hits and invalidates on VRAM writes");
    return 0;
}
