// While it waits for the vblank that swaps the frame, the game's flush routine (0x2B32D8) spins on a flag
// and calls an idle callback (0x1C4D00) on every turn. Emulating that spin costs about as much host time
// as the guest time it burns, so a frame that finishes early does not leave the host any slack, and at a
// 60 Hz tick rate the game runs slower than real time in heavy scenes. Each call of the callback is
// charged a chunk of idle EE time here, so the wait reaches the vblank after a few thousand turns and the
// scheduler then sleeps until the vblank is due on the host.
// BLACK_IDLE_WAIT=0 disables; BLACK_IDLE_WAIT_CYCLES=<n> sets the chunk (default 2000 EE cycles, 6.8 us).
#include "ps2_runtime.h"
#include "game_overrides.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace
{
    constexpr uint32_t kIdleCallback = 0x001C4D00u;
    PS2Runtime::RecompiledFunction g_idleCallback = nullptr;
    uint32_t g_idleCycles = 2000u;

    void idleCallbackThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool entry = ctx->pc == kIdleCallback; // not a resume in the middle of the function
        g_idleCallback(rdram, ctx, runtime);
        if (entry)
            runtime->eeChargeIdleCycles(g_idleCycles);
    }

    void applyBlackIdleWait(PS2Runtime &runtime)
    {
        const char *value = std::getenv("BLACK_IDLE_WAIT");
        if (value && value[0] == '0')
            return;
        if (const char *cycles = std::getenv("BLACK_IDLE_WAIT_CYCLES"))
            g_idleCycles = static_cast<uint32_t>(std::max(1, std::atoi(cycles)));
        g_idleCallback = runtime.lookupFunction(kIdleCallback);
        if (g_idleCallback)
            runtime.replaceFunction(kIdleCallback, &idleCallbackThunk);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Black idle wait", "SLUS_213.76", 0x00100008u, 0u, applyBlackIdleWait)
