// BLACK_TICK_RATE=<n> (60, or another multiple of 30): run the game's logic at n ticks per second.
// The game is tick based: 0x27F730(rate) stores the tick length (1/rate s, 1000/rate ms) that the
// subsystems receive as dt, and its vblank handler (0x2B2BA8) presents a frame every `interval` vblanks,
// set by 0x2B4DD0(interval) (2 on the console: 30 frames per second). Both are scaled here, so at 60 the
// game produces one new frame per vblank with half-length ticks. Experimental: code that assumes 30 Hz
// (a few hard-coded 1/30 and 30.0 constants) is not adjusted.
#include "ps2_runtime.h"
#include "game_overrides.h"
#include "runtime/ps2_wait_stats.h"

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

    // The tick length follows what really happened: the game's loop function (0x102BD0) advances its logic by one
    // tick per call, and a call does not always take one vertical blank. The level-start menu presents every frame
    // twice (one call per two blanks), and in play a call now and then needs a second blank; with a fixed 1/60 tick
    // those stretches ran at half speed. Before each call the tick counter is advanced by the blanks the previous call
    // took beyond the first, so game time follows real time.
    // BLACK_TICK_ADAPTIVE=0 keeps the fixed tick.
    constexpr uint32_t kGameLoop = 0x00102BD0u, kTickCounter = 0x003C0E04u;
    PS2Runtime::RecompiledFunction g_gameLoop = nullptr;

    void gameLoopThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kGameLoop)
        {
            static uint64_t s_blanks = g_ps2xVblanks.load(std::memory_order_relaxed);
            const uint64_t now = g_ps2xVblanks.load(std::memory_order_relaxed);
            // Blanks not yet turned into ticks are carried over, so a long call followed by a short one (which sees no
            // blank at all, yet must advance by at least one tick) does not count the same blank twice.
            static int64_t s_owed = 0;
            s_owed += static_cast<int64_t>(now - s_blanks);
            s_blanks = now;
            const uint32_t taken = static_cast<uint32_t>(std::min<int64_t>(std::max<int64_t>(s_owed, 1), 4));
            s_owed = std::max<int64_t>(s_owed - taken, -1);
            // The game's timers measure time as a count of ticks (the counter the loop raises by one per call) times
            // a tick length each timer copied when it was created, so the length cannot be changed on the fly: the
            // blanks beyond the first are added to the counter instead.
            if (taken > 1u)
            {
                uint32_t ticks;
                std::memcpy(&ticks, rdram + kTickCounter, sizeof(ticks));
                ticks += taken - 1u;
                std::memcpy(rdram + kTickCounter, &ticks, sizeof(ticks));
            }
        }
        g_gameLoop(rdram, ctx, runtime);
    }

    // The video player (0x271F10) shows frame number time / frame duration. When the game does not give it the time,
    // it advances the time itself by a fixed step on every call, and both the step and the frame duration were made
    // from the tick length: one video frame per call, which is the film's 30 frames a second only at the console's 30
    // calls a second. The step is set from the vertical blanks that really went by since the previous call (half a
    // frame per blank), so the film keeps its speed at any call rate. BLACK_VIDEO_LOG=1 prints the player's pace.
    constexpr uint32_t kVideoDecode = 0x00271F10u;
    PS2Runtime::RecompiledFunction g_videoDecode = nullptr;

    void videoDecodeThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kVideoDecode)
        {
            static uint32_t s_player = 0;
            static uint64_t s_blanks = 0;
            const uint32_t player = getRegU32(ctx, 4);
            const uint32_t base = player & 0x1FFFFFFu;
            const uint64_t blanks = g_ps2xVblanks.load(std::memory_order_relaxed);
            const uint64_t elapsed = player == s_player ? std::min<uint64_t>(blanks - s_blanks, 4u) : 1u;
            s_player = player;
            s_blanks = blanks;
            float duration;
            std::memcpy(&duration, rdram + base + 0x38u, sizeof(duration));
            const float step = duration * 0.5f * static_cast<float>(elapsed);
            std::memcpy(rdram + base + 0x3Cu, &step, sizeof(step));

            static const bool s_log = std::getenv("BLACK_VIDEO_LOG") != nullptr;
            static unsigned s_calls = 0;
            if (s_log && (s_calls++ % 30u) == 0u)
            {
                uint32_t frame, flags;
                std::memcpy(&frame, rdram + base + 0x30u, sizeof(frame));
                std::memcpy(&flags, rdram + base + 0x54u, sizeof(flags));
                std::fprintf(stderr, "[black-tick] video: blank %llu pictures %llu frame %u flags 0x%x\n",
                             static_cast<unsigned long long>(blanks),
                             static_cast<unsigned long long>(g_ps2xMpegPictures.load()), frame, flags);
            }
        }
        g_videoDecode(rdram, ctx, runtime);
    }

    // BLACK_EMIT_LOG=1: what the particle emitter (0x1BF918: count = rate * dt) is given as dt, summed per emitter
    // over two seconds of vertical blanks. An emitter that runs the whole window should add up to 2 s.
    constexpr uint32_t kEmitterUpdate = 0x001BF918u;
    PS2Runtime::RecompiledFunction g_emitterUpdate = nullptr;

    void emitterUpdateThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kEmitterUpdate)
        {
            static uint64_t s_window = g_ps2xVblanks.load(std::memory_order_relaxed);
            static uint32_t s_calls = 0, s_first = 0, s_firstCalls = 0;
            static double s_total = 0.0, s_firstTotal = 0.0;
            static float s_last = 0.0f;
            const uint32_t emitter = getRegU32(ctx, 4);
            if (s_first == 0u)
                s_first = emitter;
            ++s_calls;
            s_total += ctx->f[12];
            if (emitter == s_first)
            {
                ++s_firstCalls;
                s_firstTotal += ctx->f[12];
            }
            s_last = ctx->f[12];
            const uint64_t blanks = g_ps2xVblanks.load(std::memory_order_relaxed);
            if (blanks - s_window >= 120u)
            {
                std::fprintf(stderr, "[black-emit] %llu blanks: %u calls, dt total %.3f s, last dt %.5f | emitter 0x%x: %u calls, dt %.3f s\n",
                             static_cast<unsigned long long>(blanks - s_window), s_calls, s_total, s_last, s_first, s_firstCalls, s_firstTotal);
                s_window = blanks;
                s_calls = s_firstCalls = 0;
                s_total = s_firstTotal = 0.0;
                s_first = 0;
            }
        }
        g_emitterUpdate(rdram, ctx, runtime);
    }

    // Same switch: live particles (0x1C0760 walks one group's list; its count is at +8) per pass of the system.
    constexpr uint32_t kParticleExpire = 0x001C0760u;
    PS2Runtime::RecompiledFunction g_particleExpire = nullptr;

    void particleExpireThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kParticleExpire)
        {
            static uint64_t s_window = g_ps2xVblanks.load(std::memory_order_relaxed);
            static uint64_t s_particles = 0, s_calls = 0, s_passes = 0;
            static uint32_t s_firstGroup = 0;
            const uint32_t group = getRegU32(ctx, 4);
            uint16_t count;
            std::memcpy(&count, rdram + ((group + 8u) & 0x1FFFFFFu), sizeof(count));
            if (s_firstGroup == 0u)
                s_firstGroup = group;
            if (group == s_firstGroup)
                ++s_passes;
            s_particles += count;
            ++s_calls;
            const uint64_t blanks = g_ps2xVblanks.load(std::memory_order_relaxed);
            if (blanks - s_window >= 120u)
            {
                std::fprintf(stderr, "[black-emit] particles: %.0f alive on average over %llu passes (%llu groups per pass)\n",
                             s_passes ? double(s_particles) / double(s_passes) : 0.0, static_cast<unsigned long long>(s_passes),
                             static_cast<unsigned long long>(s_passes ? s_calls / s_passes : 0));
                s_window = blanks;
                s_particles = s_calls = s_passes = 0;
            }
        }
        g_particleExpire(rdram, ctx, runtime);
    }

    void applyBlackTickRate(PS2Runtime &runtime)
    {
        if (std::getenv("BLACK_EMIT_LOG"))
        {
            g_emitterUpdate = runtime.lookupFunction(kEmitterUpdate);
            if (g_emitterUpdate)
                runtime.replaceFunction(kEmitterUpdate, &emitterUpdateThunk);
            g_particleExpire = runtime.lookupFunction(kParticleExpire);
            if (g_particleExpire)
                runtime.replaceFunction(kParticleExpire, &particleExpireThunk);
        }
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
        const char *adaptive = std::getenv("BLACK_TICK_ADAPTIVE");
        if (!(adaptive && adaptive[0] == '0'))
        {
            g_gameLoop = runtime.lookupFunction(kGameLoop);
            if (g_gameLoop)
                runtime.replaceFunction(kGameLoop, &gameLoopThunk);
            g_videoDecode = runtime.lookupFunction(kVideoDecode);
            if (g_videoDecode)
                runtime.replaceFunction(kVideoDecode, &videoDecodeThunk);
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("Black tick rate", "SLUS_213.76", 0x00100008u, 0u, applyBlackTickRate)
