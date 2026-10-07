// Tab / DualShock Select requests the game's own video stop and cleanup path.
#include "ps2_runtime.h"
#include "game_overrides.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kVideoUpdate = 0x00109918u;
    constexpr uint32_t kVideoStop = 0x00109550u;
    constexpr uint32_t kPlaying = 28u;
    constexpr uint16_t kSelect = 0x0001u;
    PS2Runtime::RecompiledFunction g_blackVideoUpdate = nullptr;
    PS2Runtime::RecompiledFunction g_blackVideoStop = nullptr;
    bool g_blackVideoSelectDown = false;

    void videoUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kVideoUpdate)
        {
            uint8_t pad[32]{};
            const bool read = runtime->padBackend().readState(0, 0, pad, sizeof(pad));
            const uint16_t buttons = static_cast<uint16_t>(pad[2]) | (static_cast<uint16_t>(pad[3]) << 8u);
            const bool down = read && (buttons & kSelect) == 0u;
            const bool pressed = down && !g_blackVideoSelectDown;
            g_blackVideoSelectDown = down;
            const uint32_t object = GPR_U32(ctx, 4);
            if (pressed && rdram && object != 0u && object <= PS2_RAM_SIZE - 0x1BCu)
            {
                uint32_t state = 0u;
                std::memcpy(&state, rdram + object + 0x1A8u, sizeof(state));
                if (state == kPlaying)
                {
                    // Video::stop is a leaf: it clears the play request and sets
                    // the stop request. Video::update performs the real teardown,
                    // completion callback and scene transition on its normal path.
                    R5900Context stopContext = *ctx;
                    stopContext.pc = kVideoStop;
                    SET_GPR_U32(&stopContext, 31, 0u);
                    g_blackVideoStop(rdram, &stopContext, runtime);
                    std::fprintf(stderr, "[black-video] skip solicitado por Select, objeto=0x%08x\n", object);
                }
            }
        }
        g_blackVideoUpdate(rdram, ctx, runtime);
    }

    void applyVideoSkip(PS2Runtime &runtime)
    {
        const char *enabled = std::getenv("BLACK_CUTSCENE_SKIP");
        if (!enabled || std::strcmp(enabled, "0") == 0)
            return;
        g_blackVideoUpdate = runtime.lookupFunction(kVideoUpdate);
        g_blackVideoStop = runtime.lookupFunction(kVideoStop);
        if (!g_blackVideoUpdate || !g_blackVideoStop)
            return;
        g_blackVideoSelectDown = false;
        runtime.replaceFunction(kVideoUpdate, videoUpdate);
        std::fprintf(stderr, "[black-video] Tab / Select habilitado para pular videos\n");
    }
}
PS2_REGISTER_GAME_OVERRIDE("Black video skip", "SLUS_213.76", 0x00100008u, 0u, applyVideoSkip)
