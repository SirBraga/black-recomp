// Diagnostic: the world/occluder visibility test at 0x26DB20 (VU0 microprogram 0x880: 8 box corners
// against 6 planes; returns 0 = outside, 1 = crossing, 2 = fully inside).
// BLACK_CULL_PROBE=1 prints a histogram of results per call site; BLACK_CULL_FORCE=<n> replaces every
// result with n; BLACK_CULL_SAMPLES=<n> logs the inputs of the first n calls after 100000 calls.
#include "ps2_runtime.h"
#include "game_overrides.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    constexpr uint32_t kCullTest = 0x0026DB20u;
    PS2Runtime::RecompiledFunction g_cullTest = nullptr;
    int g_cullForce = -1;
    unsigned g_cullSamples = 0;
    unsigned long g_cullCalls = 0;
    unsigned long g_cullHist[2][4] = {};

    uint32_t cullReg(const R5900Context *ctx, int index)
    {
        uint32_t value = 0;
        std::memcpy(&value, &ctx->r[index], sizeof(value));
        return value;
    }

    // BLACK_POSE_REF=<eeMemory.bin of a PCSX2 savestate>: when <file>.trigger appears, copy the player
    // position and yaw from that RAM image (heap addresses coincide), for 30 calls in a row, so both
    // can be compared from the same point of view.
    const char *g_poseRef = nullptr;
    int g_poseLeft = -1;

    void applyReferencePose(uint8_t *rdram)
    {
        static const uint32_t kPositions[] = {0x5a8ad0u, 0x5a8b50u, 0x5a8bb0u, 0x5a8c40u, 0x5a8c70u,
                                              0x5a8cc0u, 0x5a8cd0u, 0x5a8ce0u, 0x5a8f40u};
        static const uint32_t kYaws[] = {0x5a8da0u, 0x5a8fa0u, 0x5a8fa8u};
        if (g_poseLeft < 0)
        {
            static unsigned long s_poll = 0;
            if ((++s_poll & 1023u) != 0)
                return;
            const std::string trigger = std::string(g_poseRef) + ".trigger";
            FILE *t = std::fopen(trigger.c_str(), "rb");
            if (!t)
                return;
            std::fclose(t);
            g_poseLeft = 6000;
            std::fprintf(stderr, "[black-pose] applying reference pose\n");
        }
        if (g_poseLeft == 0)
            return;
        --g_poseLeft;
        FILE *f = std::fopen(g_poseRef, "rb");
        if (!f)
            return;
        for (uint32_t addr : kPositions)
            if (std::fseek(f, addr, SEEK_SET) == 0)
                (void)std::fread(rdram + addr, 1, 12, f);
        for (uint32_t addr : kYaws)
            if (std::fseek(f, addr, SEEK_SET) == 0)
                (void)std::fread(rdram + addr, 1, 4, f);
        std::fclose(f);
    }

    void cullTestThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_poseRef)
            applyReferencePose(rdram);
        const bool entry = ctx->pc == kCullTest;
        const uint32_t ra = cullReg(ctx, 31), planes = cullReg(ctx, 4) & 0x01FFFFFFu, box = cullReg(ctx, 5) & 0x01FFFFFFu;
        float in[14][4];
        if (entry)
        {
            std::memcpy(in[0], rdram + planes, 6 * 16);
            std::memcpy(in[6], rdram + box, 8 * 16);
        }
        g_cullTest(rdram, ctx, runtime);
        if (!entry || ctx->pc != ra)
            return;
        const uint32_t result = cullReg(ctx, 2);
        const int site = ra == 0x00127E64u ? 0 : 1;
        ++g_cullHist[site][result < 3 ? result : 3];
        if (++g_cullCalls > 100000 && g_cullSamples && (!g_poseRef || g_poseLeft == 0))
        {
            --g_cullSamples;
            std::fprintf(stderr, "[black-cull] ra=%08x result=%u\n", ra, result);
            for (int i = 0; i < 14; ++i)
                std::fprintf(stderr, "[black-cull]   %s%d %.9g %.9g %.9g %.9g\n", i < 6 ? "plane" : "corner", i < 6 ? i : i - 6,
                             in[i][0], in[i][1], in[i][2], in[i][3]);
        }
        if (g_cullCalls % 20000 == 0)
            std::fprintf(stderr, "[black-cull] calls=%lu occluder 0/1/2/?=%lu/%lu/%lu/%lu frustum 0/1/2/?=%lu/%lu/%lu/%lu\n", g_cullCalls,
                         g_cullHist[0][0], g_cullHist[0][1], g_cullHist[0][2], g_cullHist[0][3], g_cullHist[1][0],
                         g_cullHist[1][1], g_cullHist[1][2], g_cullHist[1][3]);
        if (g_cullForce >= 0)
        {
            const uint64_t forced = static_cast<uint64_t>(g_cullForce);
            std::memcpy(&ctx->r[2], &forced, sizeof(forced));
        }
    }

    // BLACK_QUEUE_SITES=1: who queues draw items (0x1AF738), per call site and geometry MiB.
    PS2Runtime::RecompiledFunction g_queueAdd = nullptr;
    struct QueueSite { uint32_t ra; uint32_t mib; unsigned long count; };
    QueueSite g_queueSites[256] = {};
    unsigned long g_queueCalls = 0;

    void queueAddThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == 0x001AF738u)
        {
            const uint32_t ra = cullReg(ctx, 31), mib = cullReg(ctx, 5) >> 20;
            for (QueueSite &site : g_queueSites)
            {
                if (site.count && (site.ra != ra || site.mib != mib))
                    continue;
                site.ra = ra;
                site.mib = mib;
                ++site.count;
                break;
            }
            if (++g_queueCalls % 30000 == 0)
                for (const QueueSite &site : g_queueSites)
                    if (site.count)
                        std::fprintf(stderr, "[black-queue] calls=%lu ra=%08x geo=%02xxxxxx count=%lu\n", g_queueCalls, site.ra,
                                     site.mib, site.count);
        }
        g_queueAdd(rdram, ctx, runtime);
    }

    // BLACK_SECTION_PROBE=1: the "may need clipping" argument (a1) each world section is drawn with.
    PS2Runtime::RecompiledFunction g_sectionDraw = nullptr;
    unsigned long g_sectionCalls = 0, g_sectionA1[2] = {};
    uint32_t g_sectionRa = 0;

    void sectionDrawThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == 0x00127CF0u)
        {
            ++g_sectionA1[cullReg(ctx, 5) != 0u];
            g_sectionRa = cullReg(ctx, 31);
            if (++g_sectionCalls % 2000 == 0)
                std::fprintf(stderr, "[black-section] calls=%lu a1 zero/nonzero=%lu/%lu last ra=%08x a1=%08x\n", g_sectionCalls,
                             g_sectionA1[0], g_sectionA1[1], g_sectionRa, cullReg(ctx, 5));
        }
        g_sectionDraw(rdram, ctx, runtime);
    }

    // BLACK_TREE_PROBE=1: the frustum handed to the sphere-tree traversal (0x273A18), printed a few
    // times after the reference pose was applied: 2 planes, then two sets of 4 planes (SoA).
    PS2Runtime::RecompiledFunction g_treeCull = nullptr;
    unsigned g_treePrints = 0;

    void treeCullThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == 0x00273A18u && g_treePrints < 3 && (!g_poseRef || (g_poseLeft >= 0 && g_poseLeft < 3000)))
        {
            ++g_treePrints;
            const uint32_t frustum = cullReg(ctx, 5) & 0x01FFFFFFu;
            std::fprintf(stderr, "[black-tree] frustum=0x%08x\n", frustum);
            for (uint32_t i = 0; i < 10; ++i)
            {
                float v[4];
                std::memcpy(v, rdram + frustum + i * 16u, sizeof(v));
                std::fprintf(stderr, "[black-tree]   +%02x %.7g %.7g %.7g %.7g\n", i * 16u, v[0], v[1], v[2], v[3]);
            }
        }
        g_treeCull(rdram, ctx, runtime);
    }

    void applyBlackCullProbe(PS2Runtime &runtime)
    {
        if (std::getenv("BLACK_TREE_PROBE"))
        {
            g_treeCull = runtime.lookupFunction(0x00273A18u);
            if (g_treeCull)
                runtime.replaceFunction(0x00273A18u, &treeCullThunk);
        }
        if (std::getenv("BLACK_SECTION_PROBE"))
        {
            g_sectionDraw = runtime.lookupFunction(0x00127CF0u);
            if (g_sectionDraw)
                runtime.replaceFunction(0x00127CF0u, &sectionDrawThunk);
        }
        if (std::getenv("BLACK_QUEUE_SITES"))
        {
            g_queueAdd = runtime.lookupFunction(0x001AF738u);
            if (g_queueAdd)
                runtime.replaceFunction(0x001AF738u, &queueAddThunk);
        }
        const char *force = std::getenv("BLACK_CULL_FORCE");
        const char *samples = std::getenv("BLACK_CULL_SAMPLES");
        g_poseRef = std::getenv("BLACK_POSE_REF");
        if (!std::getenv("BLACK_CULL_PROBE") && !force && !samples && !g_poseRef)
            return;
        if (force)
            g_cullForce = std::atoi(force);
        if (samples)
            g_cullSamples = static_cast<unsigned>(std::atoi(samples));
        g_cullTest = runtime.lookupFunction(kCullTest);
        if (g_cullTest)
            runtime.replaceFunction(kCullTest, &cullTestThunk);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Black cull probe", "SLUS_213.76", 0x00100008u, 0u, applyBlackCullProbe)
