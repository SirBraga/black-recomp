// BLACK_WIDESCREEN=1: force the game's own 16:9 option. The game takes it from the console's screen
// setting: 0x26F2E0 returns 1 when sceScfGetAspect() says 16:9, and the video-options routine at
// 0x108BB8 reads the mode from settings+4 (1 = 16:9, otherwise 4:3) and reconfigures the cameras.
#include "ps2_runtime.h"
#include "game_overrides.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kApplyVideoOptions = 0x00108BB8u;
    PS2Runtime::RecompiledFunction g_applyVideoOptions = nullptr;

    void applyVideoOptionsThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            uint32_t settings = 0;
            std::memcpy(&settings, &ctx->r[4], sizeof(settings));
            settings &= 0x01FFFFFFu;
            const uint32_t widescreen = 1u;
            std::memcpy(rdram + settings + 4u, &widescreen, sizeof(widescreen));
        }
        g_applyVideoOptions(rdram, ctx, runtime);
    }

    void systemIsWidescreen(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint64_t yes = 1u;
        std::memcpy(&ctx->r[2], &yes, sizeof(yes));
        uint32_t ra = 0;
        std::memcpy(&ra, &ctx->r[31], sizeof(ra));
        ctx->pc = ra;
    }

    void applyBlackWidescreen(PS2Runtime &runtime)
    {
        const char *value = std::getenv("BLACK_WIDESCREEN");
        if (!value || value[0] != '1')
            return;
        runtime.replaceFunction(0x0026F2E0u, &systemIsWidescreen);
        g_applyVideoOptions = runtime.lookupFunction(kApplyVideoOptions);
        if (g_applyVideoOptions)
            runtime.replaceFunction(kApplyVideoOptions, &applyVideoOptionsThunk);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Black widescreen", "SLUS_213.76", 0x00100008u, 0u, applyBlackWidescreen)
