// Game fix: the display-list allocator at 0x2B3D88 keeps 4 spare qwords (the list terminator written by
// the flush at 0x2B2E20), but several renderers write up to 5 qwords more than they reserve (e.g.
// 0x1CDD98 asks for 3 and writes 8). When a frame fills the 290 KiB VIF1 list of a DMA buffer, the
// terminator then lands on the first qwords of the texture-upload (GIF) list that sits right above it:
// the whole frame draws without its uploads (Level_00 gameplay, every other frame).
// Asking the allocator for 8 more qwords than the caller wants makes it flush that much earlier.
// BLACK_DMA_GUARD=0 disables the fix.
#include "ps2_runtime.h"
#include "game_overrides.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kDmaListAlloc = 0x002B3D88u;
    constexpr uint32_t kDmaGuardQwords = 8u;
    PS2Runtime::RecompiledFunction g_dmaListAlloc = nullptr;

    void dmaListAllocThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kDmaListAlloc) // not a resume after the allocator blocked on a buffer swap
        {
            uint64_t a1 = 0;
            std::memcpy(&a1, &ctx->r[5], sizeof(a1));
            a1 = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(a1) + kDmaGuardQwords)));
            std::memcpy(&ctx->r[5], &a1, sizeof(a1));
        }
        g_dmaListAlloc(rdram, ctx, runtime);
    }

    void applyBlackDmaGuard(PS2Runtime &runtime)
    {
        const char *value = std::getenv("BLACK_DMA_GUARD");
        if (value && value[0] == '0')
            return;
        g_dmaListAlloc = runtime.lookupFunction(kDmaListAlloc);
        if (g_dmaListAlloc)
            runtime.replaceFunction(kDmaListAlloc, &dmaListAllocThunk);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Black DMA list guard", "SLUS_213.76", 0x00100008u, 0u, applyBlackDmaGuard)
