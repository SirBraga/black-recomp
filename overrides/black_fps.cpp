// BLACK_FPS=1: prints game logic updates per second (Game::update @0x102BD0), with no other hooks.
#include "ps2_runtime.h"
#include "game_overrides.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace
{
    PS2Runtime::RecompiledFunction g_update = nullptr;

    void updateThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static auto s_t0 = std::chrono::steady_clock::now();
        static unsigned s_n = 0;
        if (ctx->pc == 0x00102BD0u)
            ++s_n;
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t0).count();
        if (dt >= 2.0)
        {
            std::fprintf(stderr, "[black-fps] updates/s=%.1f\n", s_n / dt);
            s_n = 0;
            s_t0 = std::chrono::steady_clock::now();
        }
        g_update(rdram, ctx, runtime);
    }

    void applyBlackFps(PS2Runtime &runtime)
    {
        const char *dbg = std::getenv("BLACK_DEBUG");
        if (!std::getenv("BLACK_FPS") || (dbg && *dbg))
            return;
        g_update = runtime.lookupFunction(0x00102BD0u);
        if (g_update)
            runtime.replaceFunction(0x00102BD0u, &updateThunk);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Black fps counter", "SLUS_213.76", 0x00100008u, 0u, applyBlackFps)
