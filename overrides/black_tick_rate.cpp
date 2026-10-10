// BLACK_TICK_RATE=<n> (60, or another multiple of 30): run the game's logic at n ticks per second.
// The game is tick based: 0x27F730(rate) stores the tick length (1/rate s, 1000/rate ms) that the
// subsystems receive as dt, and its vblank handler (0x2B2BA8) presents a frame every `interval` vblanks,
// set by 0x2B4DD0(interval) (2 on the console: 30 frames per second). Both are scaled here, so at 60 the
// game produces one new frame per vblank with half-length ticks. Experimental: code that assumes 30 Hz
// (a few hard-coded 1/30 and 30.0 constants) is not adjusted.
#include "ps2_runtime.h"
#include "game_overrides.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kSetTickRate = 0x0027F730u, kSetVsyncInterval = 0x002B4DD0u;
    PS2Runtime::RecompiledFunction g_setTickRate = nullptr, g_setVsyncInterval = nullptr;
    uint32_t g_factor = 1u;

    uint32_t argument(const R5900Context *ctx)
    {
        uint32_t value = 0;
        std::memcpy(&value, &ctx->r[4], sizeof(value));
        return value;
    }
    void setArgument(R5900Context *ctx, uint32_t value)
    {
        const uint64_t wide = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
        std::memcpy(&ctx->r[4], &wide, sizeof(wide));
    }

    void setTickRateThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kSetTickRate)
        {
            const uint32_t rate = argument(ctx);
            setArgument(ctx, rate * g_factor);
            std::fprintf(stderr, "[black-tick] tick rate %u -> %u\n", rate, rate * g_factor);
        }
        g_setTickRate(rdram, ctx, runtime);
    }

    void setVsyncIntervalThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kSetVsyncInterval)
        {
            const uint32_t interval = argument(ctx) & 0xFFu;
            const uint32_t scaled = interval == 0u ? 0u : std::max(1u, interval / g_factor);
            setArgument(ctx, scaled);
            std::fprintf(stderr, "[black-tick] vblanks per frame %u -> %u\n", interval, scaled);
        }
        g_setVsyncInterval(rdram, ctx, runtime);
    }

    void applyBlackTickRate(PS2Runtime &runtime)
    {
        const char *value = std::getenv("BLACK_TICK_RATE");
        const int rate = value ? std::atoi(value) : 0;
        if (rate < 60 || rate % 30 != 0)
            return;
        g_factor = static_cast<uint32_t>(rate / 30);
        g_setTickRate = runtime.lookupFunction(kSetTickRate);
        g_setVsyncInterval = runtime.lookupFunction(kSetVsyncInterval);
        if (g_setTickRate)
            runtime.replaceFunction(kSetTickRate, &setTickRateThunk);
        if (g_setVsyncInterval)
            runtime.replaceFunction(kSetVsyncInterval, &setVsyncIntervalThunk);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Black tick rate", "SLUS_213.76", 0x00100008u, 0u, applyBlackTickRate)
