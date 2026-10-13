// While it waits for the vblank that swaps the frame, the game's flush routine (0x2B32D8) spins on a flag
// and calls an idle callback (0x1C4D00) on every turn. Emulating that spin costs about as much host time
// as the guest time it burns, so a frame that finishes early does not leave the host any slack, and at a
// 60 Hz tick rate the game runs slower than real time in heavy scenes. Each call of the callback is
// charged a chunk of idle EE time here, so the wait reaches the vblank after a few thousand turns and the
// scheduler then sleeps until the vblank is due on the host.
// BLACK_IDLE_WAIT=0 disables; BLACK_IDLE_WAIT_CYCLES=<n> sets the chunk (default 2000 EE cycles, 6.8 us).
#include "ps2_runtime.h"
#include "game_overrides.h"
#include "runtime/ps2_wait_stats.h"

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
        // Timeline mark (diagnostics): the first turn of the wait, i.e. when the game finished its work for the frame.
        if (entry)
        {
            const uint32_t count = g_ps2xMarkCount.load(std::memory_order_relaxed);
            if (count == 0u || g_ps2xMarks[(count - 1u) & 255u].kind != 'I')
                ps2xMark('I');
            // BLACK_FPS_TIMELINE: what the wait is held by, sampled when it has been spinning for 4 ms without a
            // vertical blank: pending DMA count, queued and sent list pointers, the "frame ready" flag and the
            // blank counter against its interval.
            static const bool timeline = std::getenv("BLACK_FPS_TIMELINE") != nullptr;
            if (timeline)
            {
                static uint64_t lastSample = 0;
                const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                if (now - lastSample > 1500000u)
                {
                    lastSample = now;
                    uint32_t queued, sent;
                    std::memcpy(&queued, rdram + 0x40E668u, 4);
                    std::memcpy(&sent, rdram + 0x40E66Cu, 4);
                    ps2xMark('w', (uint32_t(rdram[0x40E660u]) << 24) | (uint32_t(queued != sent) << 16) | (uint32_t(rdram[0x40E5FFu]) << 8) | (uint32_t(rdram[0x40E671u]) << 4) | rdram[0x40DF74u]);
                }
            }
        }
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
