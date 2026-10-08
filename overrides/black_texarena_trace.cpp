// BLACK_TEXARENA_TRACE=<file>: logs the game's texture-arena (object at 0x440280) and DMA-list
// allocator calls once <file>.trigger exists. Diagnostic only; no hooks without the variable.
#include "ps2_runtime.h"
#include "game_overrides.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace
{
    constexpr uint32_t kArena = 0x00440280u;
    struct TexArenaHook
    {
        uint32_t addr;
        const char *name;
        PS2Runtime::RecompiledFunction original;
    };
    TexArenaHook g_texArenaHooks[] = {
        {0x00102BD0u, "update", nullptr},
        {0x002710D8u, "reset", nullptr},
        {0x00270FD8u, "flip", nullptr},
        {0x00270D80u, "upload", nullptr},
        {0x00270B98u, "emit", nullptr},
        {0x00271230u, "takeover", nullptr},
        {0x002711C0u, "mode", nullptr},
        {0x002B4578u, "giflist", nullptr},
        {0x002B3D88u, "alloc", nullptr},
        {0x002B42D0u, "grow", nullptr},
        {0x002B32D8u, "swap", nullptr},
        {0x002B2E20u, "flush", nullptr},
        {0x002B2638u, "pump", nullptr},
        {0x002B34F0u, "qadd", nullptr},
    };
    FILE *g_texArenaOut = nullptr;
    std::string g_texArenaTrigger;
    unsigned long g_texArenaLeft = 0;
    int g_texArenaDepth = 0;

    uint32_t texArenaRd32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t v = 0;
        addr &= 0x01FFFFFFu;
        if (addr <= PS2_RAM_SIZE - 4u)
            std::memcpy(&v, rdram + addr, 4);
        return v;
    }
    uint32_t texArenaGpr(const R5900Context *ctx, int r)
    {
        uint32_t v;
        std::memcpy(&v, &ctx->r[r], 4);
        return v;
    }
    bool texArenaActive()
    {
        if (!g_texArenaOut || !g_texArenaLeft)
            return false;
        static bool s_armed = false;
        if (!s_armed)
        {
            static unsigned s_poll = 0;
            if ((s_poll++ & 0xFFu) != 0u)
                return false;
            struct stat st;
            s_armed = ::stat(g_texArenaTrigger.c_str(), &st) == 0;
        }
        return s_armed;
    }
    void texArenaState(const uint8_t *rdram)
    {
        const uint32_t rp = texArenaRd32(rdram, 0x0040E668u);
        std::fprintf(g_texArenaOut, " | idx=%u A=%u/%x B=%u/%x f4=%u f5=%u gif=%08x vif=%08x main=%08x buf=%u busy=%u"
                                    " g0=%08x cur{t=%x v=%08x g=%08x} pend{t=%x v=%08x g=%08x slot=%08x} qw=%08x top=%08x rd=%08x wr=%08x [%x %08x %x %08x]\n",
                     texArenaRd32(rdram, kArena + 0x2C0u), texArenaRd32(rdram, kArena + 0xC0u), texArenaRd32(rdram, kArena + 0xC4u),
                     texArenaRd32(rdram, kArena + 0x1C0u), texArenaRd32(rdram, kArena + 0x1C4u), rdram[kArena + 0x2C4u],
                     rdram[kArena + 0x2C5u], texArenaRd32(rdram, 0x0040E5F4u), texArenaRd32(rdram, 0x0040E5F0u),
                     texArenaRd32(rdram, 0x0040E610u), rdram[0x0040E634u], rdram[0x0040E660u], texArenaRd32(rdram, 0x01D6DE80u),
                     texArenaRd32(rdram, 0x0040E65Cu), texArenaRd32(rdram, 0x0040E650u), texArenaRd32(rdram, 0x0040E654u),
                     texArenaRd32(rdram, 0x0040E61Cu), texArenaRd32(rdram, 0x0040E614u), texArenaRd32(rdram, 0x0040E618u),
                     texArenaRd32(rdram, 0x0040E620u), texArenaRd32(rdram, 0x0040E64Cu), texArenaRd32(rdram, 0x0040E640u),
                     rp, texArenaRd32(rdram, 0x0040E66Cu), texArenaRd32(rdram, rp), texArenaRd32(rdram, rp + 4u),
                     texArenaRd32(rdram, rp + 8u), texArenaRd32(rdram, rp + 12u));
    }

    template <size_t I>
    void texArenaThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TexArenaHook &h = g_texArenaHooks[I];
        const bool entry = ctx->pc == h.addr;
        // "alloc"/"grow" are hot: only log them while a texture list is being opened.
        const bool log = entry && texArenaActive() && (g_texArenaDepth > 0 || (h.addr != 0x002B3D88u && h.addr != 0x002B42D0u));
        const uint32_t a0 = texArenaGpr(ctx, 4), a1 = texArenaGpr(ctx, 5), ra = texArenaGpr(ctx, 31);
        if (log)
        {
            std::fprintf(g_texArenaOut, "%*s> %s a0=%x a1=%x ra=%x", g_texArenaDepth * 2, "", h.name, a0, a1, ra);
            if (h.addr == 0x002B2638u)
            {
                auto &mem = runtime->memory();
                const uint32_t rp = texArenaRd32(rdram, 0x0040E668u), g = texArenaRd32(rdram, rp + 12u), v = texArenaRd32(rdram, rp + 4u);
                std::fprintf(g_texArenaOut, " gifstat=%x vif1stat=%x vpu=%x d1=%x d2=%x mem[g]=%08x %08x mem[v]=%08x %08x",
                             mem.readIORegister(0x10003020u), mem.readIORegister(0x10003C00u), 0u,
                             mem.readIORegister(0x10009000u), mem.readIORegister(0x1000A000u),
                             texArenaRd32(rdram, g), texArenaRd32(rdram, g + 4u), texArenaRd32(rdram, v), texArenaRd32(rdram, v + 4u));
            }
            texArenaState(rdram);
            ++g_texArenaDepth;
        }
        // Overrun check: did the previous alloc's caller write past the qwords it asked for?
        static uint32_t s_granted = 0, s_grantRa = 0, s_grantA1 = 0;
        auto used = [&] { return texArenaRd32(rdram, 0x0040E610u) + (texArenaRd32(rdram, 0x0040E5F0u) - texArenaRd32(rdram, 0x0040E60Cu)); };
        if (entry && h.addr == 0x002B2E20u)
            s_granted = 0; // a flush closes the list
        if (entry && h.addr == 0x002B3D88u && g_texArenaOut && texArenaRd32(rdram, 0x0040E5F0u) != 0u)
        {
            const uint32_t now = used();
            if (s_granted && now > s_granted && now - s_granted < 0x10000u && texArenaActive())
                std::fprintf(g_texArenaOut, "OVERRUN by=%u bytes: alloc ra=%x asked a1=%x (granted end %08x, now %08x, top %08x)\n",
                             now - s_granted, s_grantRa, s_grantA1, s_granted, now, texArenaRd32(rdram, 0x0040E640u));
        }
        h.original(rdram, ctx, runtime);
        if (entry && h.addr == 0x002B3D88u)
        {
            s_granted = texArenaRd32(rdram, 0x0040E5F0u) != 0u ? used() + a1 * 16u : 0u;
            s_grantRa = ra;
            s_grantA1 = a1;
        }
        if (log)
        {
            --g_texArenaDepth;
            std::fprintf(g_texArenaOut, "%*s< %s v0=%x", g_texArenaDepth * 2, "", h.name, texArenaGpr(ctx, 2));
            if (h.addr == 0x002B2638u)
            {
                auto &mem = runtime->memory();
                const uint32_t gt = mem.readIORegister(0x1000A030u), vt = mem.readIORegister(0x10009030u);
                std::fprintf(g_texArenaOut, " D2{chcr=%x tadr=%08x} D1{chcr=%x tadr=%08x} gifstat=%x vif1stat=%x",
                             mem.readIORegister(0x1000A000u), gt, mem.readIORegister(0x10009000u), vt,
                             mem.readIORegister(0x10003020u), mem.readIORegister(0x10003C00u));
            }
            texArenaState(rdram);
            if (--g_texArenaLeft == 0)
                std::fflush(g_texArenaOut);
        }
    }

    template <size_t... I>
    void installTexArenaHooks(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        (([&] {
             TexArenaHook &h = g_texArenaHooks[I];
             h.original = runtime.lookupFunction(h.addr);
             if (h.original)
                 runtime.replaceFunction(h.addr, &texArenaThunk<I>);
         }()),
         ...);
    }

    // BLACK_TEXARENA_WATCH=1: wraps every guest function and reports which one leaves the VIF1 list
    // longer than the last func_002B3D88 reservation allowed (list writes without a reservation).
    std::vector<PS2Runtime::RecompiledFunction> g_texArenaOrig;
    uint32_t g_texArenaBudgetEnd = 0;
    bool g_texArenaBudgetReported = false;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_texArenaCulprits; // pc -> (count, max bytes)

    void texArenaWatchThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pc = ctx->pc;
        const uint32_t slot = (pc - g_ps2RecompiledFunctionTableBase) >> 2;
        const uint32_t a1 = texArenaGpr(ctx, 5);
        g_texArenaOrig[slot](rdram, ctx, runtime);
        const uint32_t vif = texArenaRd32(rdram, 0x0040E5F0u);
        if (!vif)
        {
            g_texArenaBudgetEnd = 0;
            return;
        }
        const uint32_t used = texArenaRd32(rdram, 0x0040E610u) + (vif - texArenaRd32(rdram, 0x0040E60Cu));
        if (pc == 0x002B3D88u)
        {
            g_texArenaBudgetEnd = used + a1 * 16u;
            g_texArenaBudgetReported = false;
        }
        static uint32_t s_maxOver = 0;
        if (pc != 0x002B3D88u && g_texArenaBudgetEnd && used > g_texArenaBudgetEnd && used - g_texArenaBudgetEnd < 0x4000u &&
            used - g_texArenaBudgetEnd > s_maxOver && texArenaActive())
        {
            s_maxOver = used - g_texArenaBudgetEnd;
            std::fprintf(g_texArenaOut, "MAX-OVER %u bytes past the reservation, seen leaving %06x (top-used=%d)\n", s_maxOver, pc,
                         int(texArenaRd32(rdram, 0x0040E640u) - used));
            std::fflush(g_texArenaOut);
        }
        if (pc == 0x002B3D88u)
            ;
        else if (g_texArenaBudgetEnd && used > g_texArenaBudgetEnd && !g_texArenaBudgetReported && texArenaActive())
        {
            g_texArenaBudgetReported = true;
            auto &c = g_texArenaCulprits[pc];
            ++c.first;
            c.second = std::max(c.second, used - g_texArenaBudgetEnd);
            static unsigned s_n = 0;
            if ((++s_n % 2000u) == 0u)
            {
                std::fprintf(g_texArenaOut, "UNRESERVED-WRITERS after %u events:", s_n);
                for (const auto &[addr, v] : g_texArenaCulprits)
                    std::fprintf(g_texArenaOut, " %06x x%u (max %u B)", addr, v.first, v.second);
                std::fprintf(g_texArenaOut, "\n");
                std::fflush(g_texArenaOut);
            }
        }
    }

    void applyTexArenaTrace(PS2Runtime &runtime)
    {
        const char *path = std::getenv("BLACK_TEXARENA_TRACE");
        if (!path || !*path)
            return;
        g_texArenaOut = std::fopen(path, "w");
        g_texArenaTrigger = std::string(path) + ".trigger";
        g_texArenaLeft = std::getenv("BLACK_TEXARENA_EVENTS") ? std::strtoul(std::getenv("BLACK_TEXARENA_EVENTS"), nullptr, 10) : 60000ul;
        if (std::getenv("BLACK_TEXARENA_WATCH"))
        {
            const uint32_t n = g_ps2RecompiledFunctionTableSlotCount;
            g_texArenaOrig.assign(g_ps2RecompiledFunctionTable, g_ps2RecompiledFunctionTable + n);
            for (uint32_t i = 0; i < n; ++i)
                if (g_ps2RecompiledFunctionTable[i])
                    g_ps2RecompiledFunctionTable[i] = &texArenaWatchThunk;
            return;
        }
        installTexArenaHooks(runtime, std::make_index_sequence<sizeof(g_texArenaHooks) / sizeof(g_texArenaHooks[0])>{});
    }
}

PS2_REGISTER_GAME_OVERRIDE("Black texture arena trace", "SLUS_213.76", 0x00100008u, 0u, applyTexArenaTrace)
