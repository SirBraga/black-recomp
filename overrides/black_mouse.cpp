// Mouse look for Black, as in a PC shooter: the mouse sets the view angles directly instead of driving the right
// stick (which the game turns into an accelerating turn rate). Before each pass of the main loop (0x102BD0) the
// mouse movement since the previous pass (ps2xTakeMouseLook, captured by the runtime's window loop) is added to the
// player's angles: the object the global 0x40F4D0 points to holds yaw in degrees at +0x320 (with copies at +0x520
// and +0x528 the game restores it from, so all three are written) and pitch in degrees at +0x52C (negative looks
// up). The game derives its rotation matrix and the camera object from these on the same pass.
// BLACK_MOUSE_SENS=<degrees per host point> (default 0.1), BLACK_MOUSE_INVERT=1 inverts the vertical axis.
//
// Diagnostics: BLACK_MOUSE_PROBE=1 prints, once per pass, the floats that changed in the objects the globals
// 0x40F4D0, 0x40F4F4 and 0x40F51C point to (how the fields above were found: turning with the right stick);
// BLACK_MOUSE_TEST=<first pass>,<yaw per pass>,<pitch per pass>,<passes> turns the player by fixed amounts.
#include "ps2_runtime.h"
#include "game_overrides.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace black_mouse
{
    constexpr uint32_t kGameLoop = 0x00102BD0u;
    constexpr uint32_t kObjectPointers[] = {0x0040F4D0u, 0x0040F4F4u, 0x0040F51Cu};
    constexpr uint32_t kProbeBytes = 0x600u;
    // The pitch range the right stick reaches (degrees; negative is up), measured by holding the stick.
    constexpr float kPitchMin = -70.0f, kPitchMax = 70.0f;
    PS2Runtime::RecompiledFunction g_gameLoop = nullptr;

    uint32_t read32(const uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0;
        std::memcpy(&value, rdram + (address & 0x01FFFFFFu), sizeof(value));
        return value;
    }

    void probe(const uint8_t *rdram)
    {
        static std::array<std::array<float, kProbeBytes / 4>, 3> previous{};
        static std::array<uint32_t, 3> previousBase{};
        static unsigned long pass = 0;
        ++pass;
        for (int o = 0; o < 3; ++o)
        {
            const uint32_t base = read32(rdram, kObjectPointers[o]) & 0x01FFFFFFu;
            if (base < 0x100000u || base + kProbeBytes > 0x02000000u)
                continue;
            std::array<float, kProbeBytes / 4> now{};
            std::memcpy(now.data(), rdram + base, kProbeBytes);
            if (base == previousBase[o])
                for (uint32_t i = 0; i < now.size(); ++i)
                {
                    const float a = previous[o][i], b = now[i];
                    if (a != b && std::isfinite(a) && std::isfinite(b) && std::fabs(b) < 1.0e5f && std::fabs(b) > 1.0e-6f)
                        std::fprintf(stderr, "[black-mouse] pass=%lu obj%d=%06x +0x%03x %.6g -> %.6g\n", pass, o, base, i * 4u, a, b);
                }
            previous[o] = now;
            previousBase[o] = base;
        }
    }

    float readFloat(const uint8_t *rdram, uint32_t address)
    {
        float value = 0.0f;
        std::memcpy(&value, rdram + address, sizeof(value));
        return value;
    }
    void writeFloat(uint8_t *rdram, uint32_t address, float value)
    {
        std::memcpy(rdram + address, &value, sizeof(value));
    }

    // BLACK_MOUSE_TEST=<first pass>,<yaw per pass>,<pitch per pass>,<passes>: turn the player by fixed amounts.
    void testTurn(uint8_t *rdram)
    {
        static unsigned long pass = 0, first = 0, count = 0;
        static float yaw = 0.0f, pitch = 0.0f;
        static const bool on = [] {
            const char *env = std::getenv("BLACK_MOUSE_TEST");
            return env && std::sscanf(env, "%lu,%f,%f,%lu", &first, &yaw, &pitch, &count) == 4;
        }();
        if (!on || ++pass < first || pass >= first + count)
            return;
        const uint32_t base = read32(rdram, kObjectPointers[0]) & 0x01FFFFFFu;
        if (base < 0x100000u)
            return;
        for (uint32_t field : {0x320u, 0x520u, 0x528u})
            writeFloat(rdram, base + field, readFloat(rdram, base + field) + yaw);
        writeFloat(rdram, base + 0x52Cu, readFloat(rdram, base + 0x52Cu) + pitch);
    }

    void mouseLook(uint8_t *rdram)
    {
        static const float sens = [] {
            const char *env = std::getenv("BLACK_MOUSE_SENS");
            const float v = env ? static_cast<float>(std::atof(env)) : 0.0f;
            return v > 0.0f ? v : 0.1f;
        }();
        static const float vertical = std::getenv("BLACK_MOUSE_INVERT") ? -1.0f : 1.0f;
        float dx = 0.0f, dy = 0.0f;
        if (!ps2xTakeMouseLook(dx, dy))
            return;
        const uint32_t base = read32(rdram, kObjectPointers[0]) & 0x01FFFFFFu;
        if (base < 0x100000u || base + 0x600u > 0x02000000u)
            return;
        for (uint32_t field : {0x320u, 0x520u, 0x528u})
            writeFloat(rdram, base + field, readFloat(rdram, base + field) - dx * sens);
        const float pitch = readFloat(rdram, base + 0x52Cu) + dy * sens * vertical;
        writeFloat(rdram, base + 0x52Cu, std::clamp(pitch, kPitchMin, kPitchMax));
    }

    void gameLoopThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kGameLoop)
        {
            mouseLook(rdram);
            testTurn(rdram);
            if (std::getenv("BLACK_MOUSE_PROBE"))
                probe(rdram);
        }
        g_gameLoop(rdram, ctx, runtime);
    }

    void applyBlackMouse(PS2Runtime &runtime)
    {
        g_gameLoop = runtime.lookupFunction(kGameLoop);
        if (g_gameLoop)
            runtime.replaceFunction(kGameLoop, &gameLoopThunk);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Black mouse look", "SLUS_213.76", 0x00100008u, 0u, black_mouse::applyBlackMouse)
