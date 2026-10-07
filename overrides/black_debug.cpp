// Overrides de diagnostico para Black (SLUS_213.76) no PS2Recomp.
// Envolve funcoes do jogo por endereco para logar estado do guest na entrada,
// sem alterar o comportamento (sempre chama a funcao recompilada original).
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ps2_pad.h"

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
#include <cstdlib>
#include <chrono>
#include <atomic>
#include <tuple>
#include "runtime/gs/ps2_gs_memory.h"
#include <algorithm>
#include <string>
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_cpu_backend.h"

namespace
{
    constexpr uint32_t kGameObjGlobal = 0x0040F0E0; // lw a0,-0xF20(0x410000) em 0x264018

    uint32_t readU32(const uint8_t *rdram, uint32_t addr)
    {
        addr &= 0x01FFFFFFu;
        if (addr > PS2_RAM_SIZE - 4u)
            return 0xDEADBEEFu;
        uint32_t v;
        std::memcpy(&v, rdram + addr, sizeof(v));
        return v;
    }

    uint32_t reg(const R5900Context *ctx, int r)
    {
        return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[r], 0));
    }

    std::atomic<double> g_guestNowCached{0.0}; // guest seconds, refreshed on every Game::update

    struct Hook
    {
        uint32_t addr;
        const char *name;
        PS2Runtime::RecompiledFunction original;
        unsigned hits;
        unsigned maxLogs;
    };

    Hook g_hooks[] = {
        {0x00264018u, "main", nullptr, 0, 4},
        {0x00387E50u, "operator_new?", nullptr, 0, 8},
        {0x001020C0u, "Game::ctor?", nullptr, 0, 4},
        {0x00102930u, "Game::init_step?", nullptr, 0, 4},
        {0x00102BD0u, "Game::update?", nullptr, 0, 4},
        {0x001036F0u, "Game::swapPending?", nullptr, 0, 8},
        {0x001031F8u, "Game::frame?", nullptr, 0, 2},
        {0x0027CAB8u, "RwFsOpen?", nullptr, 0, 0},
        {0x0036AF20u, "sceSifBindRpc", nullptr, 0, 40},
        {0x002697E0u, "GtsCmd4", nullptr, 0, 20},
        {0x003240D0u, "RwaDispatch", nullptr, 0, 0},
        {0x00324E48u, "RwaStreamDone", nullptr, 0, 0},
        {0x001EF548u, "SndLoad?", nullptr, 0, 3},
        {0x00280160u, "SndFindSlot", nullptr, 0, 3},
        {0x00280A38u, "SndLoad.a?", nullptr, 0, 3},
        {0x0027FF78u, "SndLoad.b?", nullptr, 0, 3},
        {0x00293F60u, "sceMpegCreate", nullptr, 0, 5},
        {0x00294A08u, "sceMpegInit", nullptr, 0, 5},
        {0x00294AC8u, "sceMpegGetPicture", nullptr, 0, 5},
        {0x0036BD20u, "sceOpen", nullptr, 0, 30},
        {0x00269820u, "GtsCmd5", nullptr, 0, 20},
        {0x0036B100u, "sceSifCallRpc", nullptr, 0, 60},
        {0x0036A528u, "_sceSifSendCmd", nullptr, 0, 12},
        {0x0036A660u, "sceSifSendCmd", nullptr, 0, 12},
        {0x0036A460u, "sceSifAddCmdHandler", nullptr, 0, 12},
        {0x00324660u, "RwaEeCmdHandler", nullptr, 0, 12},
        {0x003251B8u, "RwaEventCb", nullptr, 0, 12},
        {0x0029B868u, "sceDbcCreateSocket", nullptr, 0, 0},
        {0x0029B998u, "sceDbcDeleteSocket", nullptr, 0, 0},
        {0x0029BA58u, "dbc_29ba58", nullptr, 0, 0},
        {0x0029BBC8u, "dbc_29bbc8", nullptr, 0, 0},
        {0x0029BCE0u, "sceDbcSendData2", nullptr, 0, 0},
        {0x0029BE90u, "dbc_29be90", nullptr, 0, 0},
        {0x0029BFE8u, "sceDbcReceiveData", nullptr, 0, 0},
        {0x0029B7F8u, "dbc_29b7f8", nullptr, 0, 0},
        {0x0028F408u, "ipuRingKick", nullptr, 0, 0},
        {0x0035AC60u, "mc2i_35ac60", nullptr, 0, 0},
        {0x0035B228u, "mc2i_35b228", nullptr, 0, 0},
        {0x00356878u, "mc2i_356878", nullptr, 0, 0},
        {0x0035AB40u, "mc2i_35ab40", nullptr, 0, 0},
        {0x0035C308u, "mc2i_35c308", nullptr, 0, 0},
        {0x00355278u, "mc2_355278", nullptr, 0, 0},
        {0x003554F8u, "mc2_3554f8", nullptr, 0, 0},
        {0x00355598u, "mc2_355598", nullptr, 0, 0},
        {0x00355648u, "mc2_355648", nullptr, 0, 0},
        {0x00355708u, "mc2_355708", nullptr, 0, 0},
        {0x00355808u, "mc2_355808", nullptr, 0, 0},
        {0x00355928u, "mc2_355928", nullptr, 0, 0},
        {0x00355A48u, "mc2_355a48", nullptr, 0, 0},
        {0x00355B40u, "mc2_355b40", nullptr, 0, 0},
        {0x00355C70u, "mc2_355c70", nullptr, 0, 0},
        {0x00356030u, "mc2_356030", nullptr, 0, 0},
        {0x00356058u, "mc2_356058", nullptr, 0, 0},
        // EE-side RWA stream client (BLACK_RWA_TRACE=1 logs each op compactly).
        {0x00324990u, "Rwa:open", nullptr, 0, 0},
        {0x003249E8u, "Rwa:close", nullptr, 0, 0},
        {0x00324A88u, "Rwa:op3", nullptr, 0, 0},
        {0x00324BA0u, "Rwa:read", nullptr, 0, 0},
        {0x00324CB0u, "Rwa:op4", nullptr, 0, 0},
        // Level-start corruption chain (see black-anom logs).
        {0x002B2BA8u, "RwVblankHandler", nullptr, 0, 0},
        {0x003500E0u, "Vec::growTo", nullptr, 0, 0},
        {0x0034E530u, "Expr::eval?", nullptr, 0, 0},
        // Frontend (FEMain.bin) action callbacks, registered by 0x104210 via 0x209618.
        {0x0020B678u, "FE:SetPage", nullptr, 0, 40},
        {0x0020CAD0u, "FE:PageCallback", nullptr, 0, 40},
        {0x00103990u, "FE:StartGame", nullptr, 0, 20},
        {0x00103BF8u, "FE:StartMainMenu", nullptr, 0, 20},
        {0x00103C28u, "FE:SkipIntroCredits", nullptr, 0, 20},
        {0x00103B38u, "FE:StartMainMenuSkip", nullptr, 0, 20},
        {0x00104078u, "FE:StartVideo", nullptr, 0, 20},
        {0x00103D08u, "FE:StartVideoPage", nullptr, 0, 20},
        {0x00103930u, "FE:StartNewMission", nullptr, 0, 20},
        {0x00103DB0u, "FE:fmvDoneCb", nullptr, 0, 20},
        {0x00103A28u, "FE:103a28", nullptr, 0, 20},
        {0x001034B0u, "Game::setPendingScene", nullptr, 0, 20},
        {0x001035D0u, "Game::revertScene", nullptr, 0, 20},
        {0x001094C0u, "Video::play", nullptr, 0, 20},
        {0x00128480u, "LevelDat::load", nullptr, 0, 20},
    };

    PS2Runtime *g_runtimeForTrace = nullptr;
    const auto s_bootTime = std::chrono::steady_clock::now();

    // Maquina de estados de init do Game (0x102930): estado em game+0x21078.
    void traceInitState(const uint8_t *rdram, uint32_t pc)
    {
        static uint32_t s_last = 0xFFFFFFFFu;
        static unsigned long s_calls = 0, s_frames = 0;
        static const auto s_t0 = std::chrono::steady_clock::now();
        if (pc == 0x001031F8u)
            ++s_frames;
        if (pc != 0x00102930u)
            return;
        ++s_calls;
        {
            PS2Runtime *rt = g_runtimeForTrace;
            if (rt)
            {
                rt->gs().setDebugHistoryPaused(false);
                unsigned xfer = 0, draw = 0, tags = 0;
                uint32_t lastTbp = 0, lastPsm = 0, pix = 0;
                for (const GSDebugHistoryEntry &e : rt->gs().getDebugHistory())
                {
                    if (e.kind == GSDebugEventKind::Transfer)
                    {
                        ++xfer;
                        lastTbp = e.bitbltbuf.dbp;
                        lastPsm = e.bitbltbuf.dpsm;
                        pix += e.transferPixels;
                    }
                    draw += e.kind == GSDebugEventKind::Draw;
                    tags += e.kind == GSDebugEventKind::GifTag;
                }
                if (xfer || draw || tags)
                    std::fprintf(stderr, "[black-gs] init call=%lu giftag=%u draw=%u xfer=%u pix=%u lastDbp=%u dpsm=0x%x\n",
                                 s_calls, tags, draw, xfer, pix, lastTbp, lastPsm);
                rt->gs().clearDebugHistory();
            }
        }
        const uint32_t game = readU32(rdram, kGameObjGlobal);
        const uint32_t state = game ? readU32(rdram, game + 0x21078u) : 0u;
        // Frontend loader (0x20a338): objeto em *0x40F544, estado em +0x37A0.
        // Streamer em *0x40F4C4: flags de ocupado em +0xB38/+0xB39.
        const uint32_t fe = readU32(rdram, 0x0040F544u);
        const uint32_t feState = fe ? readU32(rdram, fe + 0x37A0u) : 0u;
        const uint32_t streamer = readU32(rdram, 0x0040F4C4u);
        const uint32_t busy = streamer ? (readU32(rdram, streamer + 0xB38u) & 0xFFFFu) : 0u;
        static uint32_t s_lastFe = 0xFFFFFFFFu;
        if (state != s_last || feState != s_lastFe || (s_calls % 2000u) == 0u)
        {
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t0).count();
            std::fprintf(stderr, "[black-init] t=%.2fs calls=%lu frames=%lu state=%u fe=%u busy=0x%04x%s\n", t, s_calls,
                         s_frames, state, feState, busy,
                         state == s_last && feState == s_lastFe ? " (sem mudanca)" : "");
            s_last = state;
            s_lastFe = feState;
        }
    }

    // Resumo do caminho grafico a cada ~2s de Game::update (0x102bd0).
    // BLACK_GS_DETAIL_AT companion: for the distinct textures used by the frame's draws, dump
    // what VRAM holds now as /tmp/black_tex_<n>_idx.pgm (raw texel / CLUT index) and
    // /tmp/black_tex_<n>_rgb.ppm (decoded through the CLUT as the GS would load it).
    // Tells apart wrong texel data from a wrong palette.
    template <typename History>
    void dumpFrameTextures(PS2Runtime *runtime, const History &hist)
    {
        uint8_t *vram = runtime->memory().getGSVRAM();
        if (!vram)
            return;
        GSMem::TexturePageCache cache;
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> seen;
        int n = 0;
        for (const GSDebugHistoryEntry &e : hist)
        {
            if (e.kind != GSDebugEventKind::Draw || !e.prim.tme || n >= 16)
                continue;
            const GSTex0Reg &t = e.tex0;
            const auto key = std::make_tuple(t.tbp0, static_cast<uint32_t>(t.psm), t.cbp);
            if (std::find(seen.begin(), seen.end(), key) != seen.end())
                continue;
            seen.push_back(key);
            const uint32_t tw = 1u << std::min<uint32_t>(t.tw, 10u), th = 1u << std::min<uint32_t>(t.th, 10u);
            const bool p4 = t.psm == GS_PSM_T4 || t.psm == GS_PSM_T4HL || t.psm == GS_PSM_T4HH;
            const bool p8 = t.psm == GS_PSM_T8 || t.psm == GS_PSM_T8H;
            const bool c16 = t.cpsm == GS_PSM_CT16 || t.cpsm == GS_PSM_CT16S;
            uint32_t clut[256] = {};
            if (p4 || p8)
            {
                // CSM1: entries stored as 8x2 blocks (16x16 for 256 colors) in a 64-wide buffer; the
                // GS swaps bits 3 and 4 of the 8-bit index (entries 8-15 <-> 16-23 within each 32).
                for (uint32_t i = 0; i < (p4 ? 16u : 256u); ++i)
                {
                    uint32_t src = i;
                    if (t.csm == 0u && p8)
                        src = (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
                    const uint32_t x = t.csm == 0u ? (p4 ? (src & 7u) : (src & 0x0Fu)) : src;
                    const uint32_t y = t.csm == 0u ? (p4 ? (src >> 3) : (src >> 4)) : 0u;
                    uint32_t c = GSMem::ReadTexture(cache, vram, t.cpsm, t.cbp, 1u, x, y);
                    if (c16)
                        c = ((c & 0x1Fu) << 3) | (((c >> 5) & 0x1Fu) << 11) | (((c >> 10) & 0x1Fu) << 19) | ((c & 0x8000u) ? 0x80000000u : 0u);
                    clut[i] = c;
                }
            }
            char path[96];
            std::snprintf(path, sizeof(path), "/tmp/black_tex_%d_idx.pgm", n);
            FILE *fi = std::fopen(path, "wb");
            std::snprintf(path, sizeof(path), "/tmp/black_tex_%d_rgb.ppm", n);
            FILE *fc = std::fopen(path, "wb");
            if (fi && fc)
            {
                std::fprintf(fi, "P5\n%u %u\n255\n", tw, th);
                std::fprintf(fc, "P6\n%u %u\n255\n", tw, th);
                for (uint32_t y = 0; y < th; ++y)
                    for (uint32_t x = 0; x < tw; ++x)
                    {
                        const uint32_t v = GSMem::ReadTexture(cache, vram, t.psm, t.tbp0, std::max<uint32_t>(t.tbw, 1u), x, y);
                        const uint8_t gray = p4 ? static_cast<uint8_t>((v & 0xFu) * 17u)
                                           : p8 ? static_cast<uint8_t>(v & 0xFFu)
                                                : static_cast<uint8_t>(((v & 0xFFu) + ((v >> 8) & 0xFFu) + ((v >> 16) & 0xFFu)) / 3u);
                        std::fputc(gray, fi);
                        const uint32_t c = (p4 || p8) ? clut[v & (p4 ? 0xFu : 0xFFu)] : v;
                        const uint8_t rgb[3] = {static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c >> 16)};
                        std::fwrite(rgb, 1, 3, fc);
                    }
            }
            if (fi)
                std::fclose(fi);
            if (fc)
                std::fclose(fc);
            std::fprintf(stderr, "[black-tex] %d tbp=%u tbw=%u psm=0x%x %ux%u cbp=%u cpsm=0x%x csm=%u csa=%u cld=%u tcc=%u tfx=%u\n", n,
                         t.tbp0, t.tbw, t.psm, tw, th, t.cbp, t.cpsm, t.csm, t.csa, t.cld, t.tcc, t.tfx);
            ++n;
        }
    }

    void traceGs(PS2Runtime *runtime, uint32_t pc)
    {
        static unsigned long s_updates = 0;
        if (pc != 0x00102BD0u)
            return;
        runtime->gs().setDebugHistoryPaused(false);
        ++s_updates;
        g_guestNowCached.store(PSPadBackend::guestSeconds(), std::memory_order_relaxed);
        {
            // Level loader state machine (0x128480): state at loader+0x5AA0, logged on change.
            const uint8_t *rd = runtime->memory().getRDRAM();
            static uint32_t s_lastState = 0xFFFFFFFFu;
            if (const uint32_t loader = readU32(rd, 0x0040F4D0u))
            {
                const uint32_t st = readU32(rd, loader + 0x5AA0u);
                if (st != s_lastState)
                {
                    std::fprintf(stderr, "[black-load] g=%.2f upd=%lu loaderState=%u -> %u\n", PSPadBackend::guestSeconds(),
                                 s_updates, s_lastState, st);
                    // BLACK_LOAD_XFER_TRACE=1: log the GS uploads of the level load (from StLevel.bin on).
                    if (s_lastState == 1u && st != 1u && std::getenv("BLACK_LOAD_XFER_TRACE"))
                        g_gsTransferTraceBudget.store(20000);
                    s_lastState = st;
                }
            }
        }
        {
            // Watch the Game sound-interface vptr (game+0x14, set to 0x3e24d8 by 0x387e50).
            const uint8_t *rd = runtime->memory().getRDRAM();
            const uint32_t game = readU32(rd, kGameObjGlobal);
            static uint32_t s_lastVptr = 0xFFFFFFFFu;
            const uint32_t vptr = game ? readU32(rd, game + 0x14u) : 0u;
            if (game && vptr != s_lastVptr)
            {
                std::fprintf(stderr, "[black-watch] g=%.2f upd=%lu game+0x14=0x%08x words:", PSPadBackend::guestSeconds(),
                             s_updates, vptr);
                for (uint32_t o = 0; o < 0x40u; o += 4)
                    std::fprintf(stderr, " %08x", readU32(rd, game + o));
                std::fprintf(stderr, "\n");
                s_lastVptr = vptr;
            }
        }
        // traceInitState/main tambem chamam isto; no init (antes do 1o update) conta no hook de init_step
        static const unsigned long s_every = std::getenv("BLACK_DUMP_EVERY") ? std::strtoul(std::getenv("BLACK_DUMP_EVERY"), nullptr, 10) : 60u;
        if (s_updates > 3 && (s_updates % s_every) != 1u)
            return;
        const GSRegisters &r = runtime->memory().gs();
        {
            const uint8_t *rd = runtime->memory().getRDRAM();
            const uint32_t fe = readU32(rd, 0x0040F544u);
            const uint32_t streamer = readU32(rd, 0x0040F4C4u);
            std::fprintf(stderr, "[black-state] t=%.1f g=%.1f upd=%lu feState=%u fe+0x37a4=%u streamerBusy=0x%04x mode=%u cur=0x%08x pend=0x%08x pendState=%u\n",
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - s_bootTime).count(), PSPadBackend::guestSeconds(), s_updates,
                         fe ? readU32(rd, fe + 0x37A0u) : 0u, fe ? readU32(rd, fe + 0x37A4u) : 0u,
                         streamer ? (readU32(rd, streamer + 0xB38u) & 0xFFFFu) : 0u,
                         readU32(rd, readU32(rd, kGameObjGlobal) + 0x21084u),
                         readU32(rd, readU32(rd, kGameObjGlobal) + 0x21070u),
                         readU32(rd, readU32(rd, kGameObjGlobal) + 0x21074u),
                         readU32(rd, readU32(rd, readU32(rd, kGameObjGlobal) + 0x21074u)));
            if (fe && fe <= PS2_RAM_SIZE - 0x3950u && std::getenv("BLACK_FE_TRACE"))
                std::fprintf(stderr, "[black-fe-state] fe=%08x active=%u ready=%u page=\"%.31s\" next=\"%.31s\" request=%u/%u fade=%08x mcBoot=%u\n",
                             fe, rd[fe + 0x390fu], rd[fe + 0x3913u],
                             reinterpret_cast<const char *>(rd + fe + 0x3840u),
                             reinterpret_cast<const char *>(rd + fe + 0x3820u),
                             readU32(rd, fe + 0x3884u), readU32(rd, fe + 0x388cu),
                             readU32(rd, fe + 0x3948u), readU32(rd, 0x40f53cu));
            std::fprintf(stderr, "[black-dbcwork] %08x %08x %08x %08x %08x %08x %08x %08x | %08x %08x %08x %08x\n",
                         readU32(rd, 0x445cc0u), readU32(rd, 0x445cc4u), readU32(rd, 0x445cc8u), readU32(rd, 0x445cccu),
                         readU32(rd, 0x445cd0u), readU32(rd, 0x445cd4u), readU32(rd, 0x445cd8u), readU32(rd, 0x445cdcu),
                         readU32(rd, 0x445d40u), readU32(rd, 0x445d44u), readU32(rd, 0x445d48u), readU32(rd, 0x445d4cu));
            std::fprintf(stderr, "[black-mcrx] b00: %08x %08x %08x %08x %08x %08x | b80: %08x %08x %08x %08x %08x %08x\n",
                         readU32(rd, 0x480b00u), readU32(rd, 0x480b04u), readU32(rd, 0x480b08u), readU32(rd, 0x480b0cu),
                         readU32(rd, 0x480b10u), readU32(rd, 0x480b14u), readU32(rd, 0x480b80u), readU32(rd, 0x480b84u),
                         readU32(rd, 0x480b88u), readU32(rd, 0x480b8cu), readU32(rd, 0x480b90u), readU32(rd, 0x480b94u));
            if (const uint32_t mc = readU32(rd, 0x0040F4F0u))
            {
                // Game memory-card manager (the debug overlay prints +0xC as meMemCardState).
                std::fprintf(stderr, "[black-mc] obj=0x%08x +0=%u +4=%u +8=%u state=%u +10=%u +14=%u +18=%u +1c=%u\n", mc,
                             readU32(rd, mc), readU32(rd, mc + 4u), readU32(rd, mc + 8u), readU32(rd, mc + 0xCu),
                             readU32(rd, mc + 0x10u), readU32(rd, mc + 0x14u), readU32(rd, mc + 0x18u), readU32(rd, mc + 0x1Cu));
            }
            {
                const uint32_t req = readU32(rd, 0x00443240u);
                const uint32_t rq4 = req ? readU32(rd, req + 4u) : 0u;
                const uint32_t tbl = readU32(rd, 0x004157F0u - 30104);
                std::fprintf(stderr, "[black-snd] upd=%lu slotState=0x%x slotLoaded=%u req=0x%08x reqState=0x%x "
                             "stream=%u streamFlags=0x%x\n", s_updates, readU32(rd, 0x005A2440u + 0x103Cu),
                             readU32(rd, 0x005A2440u + 0x102Cu) >> 16 & 0xFFu, req, req ? readU32(rd, req + 0x2Cu) : 0u,
                             rq4, (tbl && rq4) ? readU32(rd, tbl + (rq4 - 1u) * 0x70u + 0x1Cu) : 0u);
            }
            const uint32_t pend = readU32(rd, readU32(rd, kGameObjGlobal) + 0x21074u);
            if (s_updates == 121)
            {
                const uint32_t mgr = readU32(rd, 0x0040F510u);
                const uint32_t obj = mgr ? readU32(rd, mgr + 0xCBD8u) : 0u;
                const uint32_t sub = obj ? readU32(rd, obj + 0x40u) : 0u;
                const uint32_t vt = sub ? readU32(rd, sub + 0x1B0u) : 0u;
                std::fprintf(stderr, "[black-state] mgr=0x%08x obj=0x%08x sub=0x%08x vt=0x%08x isReady=0x%08x subState=%u slots:",
                             mgr, obj, sub, vt, vt ? readU32(rd, vt + 0xCu) : 0u, sub ? readU32(rd, sub + 0x1B4u) : 0u);
                for (uint32_t i = 0; vt && i < 10; ++i)
                    std::fprintf(stderr, " 0x%08x", readU32(rd, vt + 8u + i * 8u + 4u));
                std::fprintf(stderr, "\n");
            }
            if (pend && s_updates == 1)
            {
                const uint32_t vt = readU32(rd, pend + 8u);
                std::fprintf(stderr, "[black-state] pend vtbl=0x%08x", vt);
                for (uint32_t i = 0; i < 12; ++i)
                    std::fprintf(stderr, " [%u]=0x%08x", i, readU32(rd, vt + 8u + i * 8u + 4u));
                std::fprintf(stderr, "\n");
            }
        }
        std::fprintf(stderr,
                     "[black-gs] upd=%lu pmode=0x%llx dispfb1=0x%llx display1=0x%llx dispfb2=0x%llx display2=0x%llx "
                     "smode2=0x%llx vsync=%llu nativeGif=%llu nativeImg=%llu\n",
                     s_updates, (unsigned long long)r.pmode, (unsigned long long)r.dispfb1,
                     (unsigned long long)r.display1, (unsigned long long)r.dispfb2, (unsigned long long)r.display2,
                     (unsigned long long)r.smode2, (unsigned long long)r.vsyncTick.load(),
                     (unsigned long long)runtime->gs().nativePackedGIFPacketCount(),
                     (unsigned long long)runtime->gs().nativeImageUploadCount());
        const std::vector<GSDebugHistoryEntry> hist = runtime->gs().getDebugHistory();
        unsigned kinds[5] = {};
        uint32_t lastFbp = 0, lastVerts = 0, presents = 0, maxFrame = 0;
        for (const GSDebugHistoryEntry &e : hist)
        {
            if (static_cast<unsigned>(e.kind) < 5u)
                ++kinds[static_cast<unsigned>(e.kind)];
            if (e.kind == GSDebugEventKind::Draw)
            {
                lastFbp = e.frame.fbp;
                lastVerts = e.vertexCount;
            }
            if (e.kind == GSDebugEventKind::Present)
                ++presents;
            maxFrame = std::max(maxFrame, e.frameIndex);
        }
        {
            static int s_detail = 0;
            // BLACK_GS_DETAIL_AT=<update>: also dump the distinct draws of one frame at that update.
            static const unsigned long s_detailAt = std::getenv("BLACK_GS_DETAIL_AT") ? std::strtoul(std::getenv("BLACK_GS_DETAIL_AT"), nullptr, 10) : 0ul;
            static bool s_detailDone = false;
            const bool detailNow = s_detailAt != 0ul && !s_detailDone && s_updates >= s_detailAt;
            if (detailNow)
                s_detailDone = true;
            if (s_detail++ < 3 || detailNow)
            {
                std::vector<std::string> seen;
                for (const GSDebugHistoryEntry &e : hist)
                {
                    if (e.kind != GSDebugEventKind::Draw && e.kind != GSDebugEventKind::Present)
                        continue;
                    char line[256];
                    if (e.kind == GSDebugEventKind::Present)
                        std::snprintf(line, sizeof(line), "present disp=%u src=%u %ux%u pref=%d", e.displayFbp,
                                      e.sourceFbp, e.width, e.height, e.usedPreferred ? 1 : 0);
                    else
                        std::snprintf(line, sizeof(line),
                                      "draw prim=%d tme=%d abe=%d fbp=%u fbw=%u psm=0x%x zbp=%u tbp=%u tpsm=0x%x v=%u "
                                      "x=[%.0f,%.0f] y=[%.0f,%.0f] a=[%u,%u] scis=[%u..%u,%u..%u]",
                                      static_cast<int>(e.prim.type), e.prim.tme, e.prim.abe, e.frame.fbp, e.frame.fbw,
                                      e.frame.psm, e.zbuf.zbp, e.tex0.tbp0, e.tex0.psm, e.vertexCount, e.xMin, e.xMax,
                                      e.yMin, e.yMax, e.aMin, e.aMax, e.scissor.x0, e.scissor.x1, e.scissor.y0,
                                      e.scissor.y1);
                    if (std::find(seen.begin(), seen.end(), line) == seen.end())
                        seen.emplace_back(line);
                }
                for (const std::string &l : seen)
                    std::fprintf(stderr, "[black-gs]     %s\n", l.c_str());
                if (detailNow)
                {
                    dumpFrameTextures(runtime, hist);
                    g_gsTextureDumpRequest.store(24); // the next frame's world textures, as sampled
                    g_gsTransferTraceBudget.store(600); // and the uploads that feed them
                }
            }
        }
        const GSDebugSnapshot snap = runtime->gs().getDebugSnapshot();
        std::fprintf(stderr, "[black-gs]   bitbltbuf dbp=%u dbw=%u dpsm=0x%x trxreg=%ux%u trxdir=%u xfer %u/%u\n",
                     snap.bitbltbuf.dbp, snap.bitbltbuf.dbw, snap.bitbltbuf.dpsm, snap.trxreg.rrw, snap.trxreg.rrh,
                     snap.trxdir, snap.transferCopiedPixels, snap.transferTotalPixels);
        std::fprintf(stderr,
                     "[black-gs]   hist=%zu giftag=%u reg=%u draw=%u xfer=%u present=%u lastDrawFbp=%u verts=%u "
                     "frame=%u | present %ux%u dispFbp=%u srcFbp=%u has=%d\n",
                     hist.size(), kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], lastFbp, lastVerts, maxFrame,
                     snap.hostPresentationWidth, snap.hostPresentationHeight, snap.hostPresentationDisplayFbp,
                     snap.hostPresentationSourceFbp, snap.hasHostPresentationFrame ? 1 : 0);
        runtime->gs().clearDebugHistory();

        // Frame apresentado ao host: estatistica + dump PPM em /tmp/black_frame_<n>.ppm
        std::vector<uint8_t> px;
        uint32_t w = 0, hgt = 0;
        if (runtime->gs().copyLatchedHostPresentationFrame(px, w, hgt) && w && hgt && px.size() >= size_t(w) * hgt * 4u)
        {
            uint64_t sum = 0;
            size_t nonBlack = 0;
            for (size_t i = 0; i < size_t(w) * hgt; ++i)
            {
                const uint32_t s = px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2];
                sum += s;
                nonBlack += s > 24u;
            }
            std::fprintf(stderr, "[black-gs]   frame %ux%u media=%.1f naoPretos=%zu\n", w, hgt,
                         double(sum) / (3.0 * w * hgt), nonBlack);
            static int s_dump = 0;
            char path[64];
            // BLACK_FRAME_KEEP=N guarda os N primeiros dumps em sequencia (padrao: ring de 4).
            static const int s_keep = std::getenv("BLACK_FRAME_KEEP") ? std::atoi(std::getenv("BLACK_FRAME_KEEP")) : 0;
            std::snprintf(path, sizeof(path), "/tmp/black_frame_%d.ppm", s_keep > 0 ? std::min(s_dump++, s_keep - 1) : s_dump++ % 4);
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fprintf(f, "P6\n%u %u\n255\n", w, hgt);
                for (size_t i = 0; i < size_t(w) * hgt; ++i)
                    std::fwrite(&px[i * 4], 1, 3, f);
                std::fclose(f);
            }
        }
        // BLACK_VRAM_DUMP=1: despeja o buffer do video (FBP 70 = BP 2240, CT32, 640x448) e o tile 64x64 (BP 11200)
        if (std::getenv("BLACK_VRAM_DUMP"))
        {
            static int s_vd = 0;
            char path[64];
            std::snprintf(path, sizeof(path), "/tmp/black_vram70_%d.ppm", s_vd % 8);
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fprintf(f, "P6\n640 448\n255\n");
                for (uint32_t y = 0; y < 448; ++y)
                    for (uint32_t x = 0; x < 640; ++x)
                    {
                        const uint32_t c = runtime->gs().ReadVram(0x00u, 2240u, 10u, x, y);
                        const uint8_t rgb[3] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16)};
                        std::fwrite(rgb, 1, 3, f);
                    }
                std::fclose(f);
            }
            std::snprintf(path, sizeof(path), "/tmp/black_tile_%d.ppm", s_vd % 8);
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fprintf(f, "P6\n64 64\n255\n");
                for (uint32_t y = 0; y < 64; ++y)
                    for (uint32_t x = 0; x < 64; ++x)
                    {
                        const uint32_t c = runtime->gs().ReadVram(0x00u, 11200u, 1u, x, y);
                        const uint8_t rgb[3] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16)};
                        std::fwrite(rgb, 1, 3, f);
                    }
                std::fclose(f);
            }
            ++s_vd;
        }
        // Textura da tela de loading (TBP 2240, PSMCT24): amostra direta da VRAM
        {
            uint64_t sum = 0;
            for (uint32_t y = 0; y < 448; y += 16)
                for (uint32_t x = 0; x < 32; x += 4)
                    sum += runtime->gs().ReadVram(0x01u, 2240u, 1u, x, y) & 0xFFFFFFu;
            std::fprintf(stderr, "[black-gs]   tex@2240 somaAmostra=%llu\n", (unsigned long long)sum);
        }
    }

    // BLACK_WATCH_WORD=<hexaddr>: report which hooked call boundary first sees the word change.
    void checkWatch(const uint8_t *rdram, const char *where, const char *name)
    {
        static const uint32_t s_addr = std::getenv("BLACK_WATCH_WORD") ? std::strtoul(std::getenv("BLACK_WATCH_WORD"), nullptr, 16) : 0u;
        if (!s_addr)
            return;
        static uint32_t s_last = 0xFFFFFFFFu;
        const uint32_t v = readU32(rdram, s_addr);
        static const bool s_verbose = std::getenv("BLACK_WATCH_WORD_VERBOSE") != nullptr;
        static unsigned s_verboseLeft = 400;
        if (s_verbose && s_verboseLeft && s_verboseLeft--)
            std::fprintf(stderr, "[black-watch]   %s %s =0x%08x\n", where, name, v);
        if (v != s_last)
        {
            std::fprintf(stderr, "[black-watch] *0x%08x=0x%08x (was 0x%08x) at %s %s g=%.2f\n", s_addr, v, s_last, where, name,
                         PSPadBackend::guestSeconds());
            s_last = v;
        }
    }

    template <size_t I>
    void hookThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        Hook &h = g_hooks[I];
        g_runtimeForTrace = runtime;
        checkWatch(rdram, "enter", h.name);
        if (ctx->pc == h.addr && h.addr == 0x002B2BA8u)
        {
            const uint32_t gp = reg(ctx, 28);
            const uint32_t slot = readU32(rdram, (gp - 29044u) & 0x01FFFFFFu);
            static unsigned s_ok = 0, s_bad = 0;
            if (gp != 0x004157F0u || slot != 0x004157F0u)
            {
                if (s_bad++ < 12)
                    std::fprintf(stderr, "[black-anom] RwVblankHandler gp=0x%08x slot=0x%08x sp=0x%08x a0=0x%x a1=0x%x g=%.2f (ok calls before: %u)\n",
                                 gp, slot, reg(ctx, 29), reg(ctx, 4), reg(ctx, 5), g_guestNowCached.load(std::memory_order_relaxed), s_ok);
            }
            else
                ++s_ok;
        }
        if (ctx->pc == h.addr && h.addr == 0x003500E0u)
        {
            const uint32_t vec = reg(ctx, 4), index = reg(ctx, 5);
            const uint32_t count = readU32(rdram, (vec + 280u) & 0x01FFFFFFu);
            static unsigned s_reports = 0;
            if ((index >= 70u || count > 70u) && s_reports++ < 12)
            {
                const uint32_t rec = reg(ctx, 16);
                std::fprintf(stderr, "[black-anom] Vec::growTo vec=0x%08x index=0x%x count=0x%x ra=0x%06x s0=0x%08x rec:", vec, index, count,
                             reg(ctx, 31), rec);
                for (uint32_t o = 0; o < 16u; o += 4u)
                    std::fprintf(stderr, " %08x", readU32(rdram, (rec + o) & 0x01FFFFFFu));
                std::fprintf(stderr, " g=%.2f\n", g_guestNowCached.load(std::memory_order_relaxed));
            }
        }
        static const bool s_rwaTrace = std::getenv("BLACK_RWA_TRACE") != nullptr;
        if (s_rwaTrace && ctx->pc == h.addr &&
            (std::strncmp(h.name, "Rwa:", 4) == 0 || h.addr == 0x00324E48u))
        {
            const double g = g_guestNowCached.load(std::memory_order_relaxed);
            if (h.addr == 0x00324990u)
            {
                char name[64] = {};
                const uint32_t p = reg(ctx, 4) & 0x01FFFFFFu;
                for (size_t i = 0; i + 1 < sizeof(name) && p + i < PS2_RAM_SIZE && rdram[p + i]; ++i)
                    name[i] = static_cast<char>(rdram[p + i]);
                std::fprintf(stderr, "[rwa] g=%.2f open \"%s\" ra=%06x\n", g, name, reg(ctx, 31));
            }
            else if (h.addr == 0x00324E48u)
            {
                const uint32_t tbl = readU32(rdram, 0x0040E258u);
                const uint32_t idx = reg(ctx, 5);
                std::fprintf(stderr, "[rwa] g=%.2f done idx=%u result=0x%x flags=0x%x op=%u\n", g, idx, reg(ctx, 4),
                             tbl ? readU32(rdram, tbl + idx * 0x70u + 0x1Cu) : 0u, tbl ? readU32(rdram, tbl + idx * 0x70u + 0x10u) : 0u);
            }
            else
                std::fprintf(stderr, "[rwa] g=%.2f %s stream=%u a1=0x%x a2=0x%x a3=0x%x t0=0x%x t1=0x%x ra=%06x\n", g, h.name + 4,
                             reg(ctx, 4), reg(ctx, 5), reg(ctx, 6), reg(ctx, 7), reg(ctx, 8), reg(ctx, 9), reg(ctx, 31));
        }
        if (h.addr == 0x002A99D8u && ctx->pc == h.addr)
            std::fprintf(stderr, "[black-rw] RwEngineStart device fn=0x%08x\n", readU32(rdram, 0x0044944Cu));
        traceInitState(rdram, h.addr);
        traceGs(runtime, h.addr);
        if (ctx->pc == h.addr && h.hits++ < h.maxLogs)
        {
            const uint32_t game = readU32(rdram, kGameObjGlobal);
            const uint32_t sub = game + 0x20000u;
            std::fprintf(stderr,
                         "[black-dbg] %s @0x%06x a0=0x%08x a1=0x%08x ra=0x%08x sp=0x%08x | game=0x%08x "
                         "state=%u cur=0x%08x pend=0x%08x\n",
                         h.name, h.addr, reg(ctx, 4), reg(ctx, 5), reg(ctx, 31), reg(ctx, 29), game,
                         game ? readU32(rdram, sub + 0x1084u) : 0u,
                         game ? readU32(rdram, sub + 0x1070u) : 0u,
                         game ? readU32(rdram, sub + 0x1074u) : 0u);
        }
        if (ctx->pc == h.addr && h.hits <= h.maxLogs && h.name[0] != '\0' &&
            (std::strncmp(h.name, "FE:", 3) == 0 || h.addr == 0x001094C0u))
        {
            // Show a0/a1 (and a0+60, the FE page-name slot) as strings when they point at text.
            auto str = [&](uint32_t va) {
                static char buf[4][48];
                static int slot = 0;
                char *out = buf[slot++ & 3];
                const uint32_t p = va & 0x01FFFFFFu;
                size_t n = 0;
                for (; n + 1 < sizeof(buf[0]) && p + n < PS2_RAM_SIZE; ++n)
                {
                    const uint8_t c = rdram[p + n];
                    if (c == 0)
                        break;
                    if (c < 0x20 || c > 0x7E)
                    {
                        n = 0;
                        break;
                    }
                    out[n] = static_cast<char>(c);
                }
                out[n] = '\0';
                return out;
            };
            std::fprintf(stderr, "[black-fe] g=%.2f %s a0=\"%s\" a1=\"%s\" a0+60=\"%s\" a2=0x%x a3=0x%x\n",
                         PSPadBackend::guestSeconds(), h.name, str(reg(ctx, 4)), str(reg(ctx, 5)), str(reg(ctx, 4) + 60u), reg(ctx, 6), reg(ctx, 7));
        }
        const bool logThis = ctx->pc == h.addr && h.hits <= h.maxLogs;
        const uint32_t s0In = reg(ctx, 16), spIn = reg(ctx, 29), raIn = reg(ctx, 31);
        if (logThis)
        {
            std::fprintf(stderr, "[black-dbg]   enter %s s0=0x%08x s1=0x%08x\n", h.name, s0In, reg(ctx, 17));
            // Slots do ctor 0x1020c0 (sp_ctor = 0x1ffffb0 - 0xa0): s0@+0x90, s1@+0x80, s4@+0x50
            const uint32_t spCtor = 0x01ffffb0u - 0xa0u;
            std::fprintf(stderr, "[black-dbg]   ctor slots: s0=%08x_%08x s1=%08x_%08x s4=%08x_%08x ra=%08x\n",
                         readU32(rdram, spCtor + 0x94), readU32(rdram, spCtor + 0x90),
                         readU32(rdram, spCtor + 0x84), readU32(rdram, spCtor + 0x80),
                         readU32(rdram, spCtor + 0x54), readU32(rdram, spCtor + 0x50),
                         readU32(rdram, spCtor + 0x00));
        }
        char path[128] = {};
        if (h.addr == 0x0027CAB8u)
        {
            const uint32_t p = reg(ctx, 5) & 0x01FFFFFFu;
            for (size_t i = 0; i + 1 < sizeof(path) && p + i < PS2_RAM_SIZE && rdram[p + i]; ++i)
                path[i] = static_cast<char>(rdram[p + i]);
        }
        uint32_t dbcArgs[8] = {};
        const uint32_t dbgPicAddr = reg(ctx, 5);
        uint32_t ringBefore[6] = {};
        const bool ringKick = h.addr == 0x0028F408u;
        const uint32_t ringPtr = reg(ctx, 4) & 0x01FFFFFFu;
        if (ringKick)
        {
            const uint32_t r = ringPtr;
            for (int i = 0; i < 6; ++i)
                ringBefore[i] = readU32(rdram, r + 4u * i);
        }
        const bool mc2Api = (h.addr >= 0x00355278u && h.addr <= 0x00356058u) || h.addr == 0x0035AC60u ||
                            h.addr == 0x0035B228u || h.addr == 0x00356878u || h.addr == 0x0035AB40u || h.addr == 0x0035C308u;
        const bool dbcGeneric = mc2Api || h.addr == 0x0029BA58u || h.addr == 0x0029BBC8u || h.addr == 0x0029BCE0u ||
                                h.addr == 0x0029BE90u || h.addr == 0x0029BFE8u || h.addr == 0x0029B7F8u;
        if (h.addr == 0x0029B868u || h.addr == 0x0029B998u || dbcGeneric)
        {
            dbcArgs[3] = reg(ctx, 7);
            dbcArgs[0] = reg(ctx, 4);
            dbcArgs[1] = reg(ctx, 5);
            dbcArgs[2] = reg(ctx, 6);
            for (int i = 0; i < 5 && h.addr == 0x0029B868u; ++i)
                dbcArgs[3 + i] = readU32(rdram, (dbcArgs[0] & 0x01FFFFFFu) + 4u * i);
        }
        h.original(rdram, ctx, runtime);
        checkWatch(rdram, "leave", h.name);
        if (ringKick && std::getenv("BLACK_RING_TRACE"))
        {
            std::fprintf(stderr, "[black-ring] kick ra=%08x before idx=%u cnt=%d pend=%x | after idx=%u cnt=%d pend=%x | d4 madr=%x tadr=%x qwc=%x chcr=%x\n",
                         raIn, ringBefore[3], (int)ringBefore[4], ringBefore[5],
                         readU32(rdram, ringPtr + 12u), (int)readU32(rdram, ringPtr + 16u), readU32(rdram, ringPtr + 20u),
                         runtime->memory().readIORegister(0x1000B410u), runtime->memory().readIORegister(0x1000B430u),
                         runtime->memory().readIORegister(0x1000B420u), runtime->memory().readIORegister(0x1000B400u));
        }
        static unsigned s_dbcLogs = 0;
        if (dbcGeneric && s_dbcLogs++ < 2000)
        {
            const uint32_t a1p = dbcArgs[1] & 0x01FFFFFFu;
            std::fprintf(stderr, "[black-dbc] %s a0=0x%08x a1=0x%x a2=0x%x a3=0x%x [a1]=%08x %08x %08x %08x ra=%08x -> v0=0x%08x\n", h.name,
                         dbcArgs[0], dbcArgs[1], dbcArgs[2], dbcArgs[3],
                         a1p < 0x2000000u ? readU32(rdram, a1p) : 0u, a1p < 0x2000000u ? readU32(rdram, a1p + 4u) : 0u,
                         a1p < 0x2000000u ? readU32(rdram, a1p + 8u) : 0u, a1p < 0x2000000u ? readU32(rdram, a1p + 12u) : 0u,
                         raIn, reg(ctx, 2));
        }
        if ((h.addr == 0x0029B868u || h.addr == 0x0029B998u) && s_dbcLogs++ < 2000)
            std::fprintf(stderr, "[black-dbc] %s a0=0x%08x a1=0x%x a2=0x%x desc=%08x %08x %08x %08x %08x -> v0=0x%08x\n", h.name,
                         dbcArgs[0], dbcArgs[1], dbcArgs[2], dbcArgs[3], dbcArgs[4], dbcArgs[5], dbcArgs[6], dbcArgs[7], reg(ctx, 2));
        if (h.addr == 0x00294AC8u && std::getenv("BLACK_PIC_DUMP"))
        {
            // Picture written by sceMpegGetPicture: RGB32, 16-wide macroblock columns of full height.
            static unsigned s_pics = 0;
            if (++s_pics >= 30u)
            {
                const uint32_t img = dbgPicAddr & 0x01FFFFFFu;
                const uint32_t w = 640, hh = 480;
                if (FILE *f = std::fopen("/tmp/black_mpeg_pic.ppm", "wb"))
                {
                    std::fprintf(f, "P6\n%u %u\n255\n", w, hh);
                    for (uint32_t y = 0; y < hh; ++y)
                        for (uint32_t x = 0; x < w; ++x)
                        {
                            const uint32_t off = img + ((x / 16u) * hh * 16u + y * 16u + (x % 16u)) * 4u;
                            std::fwrite(rdram + off, 1, 3, f);
                        }
                    std::fclose(f);
                }
            }
        }
        if (h.addr == 0x003240D0u)
        {
            static unsigned s_n = 0;
            const uint32_t m = reg(ctx, 4);
            if (s_n++ < 60)
                std::fprintf(stderr, "[black-rwa] msg type=%u +14=0x%08x +18=0x%x +1c=0x%x +20=0x%x +24=0x%x +28=0x%x +2c=0x%x\n",
                             readU32(rdram, m + 16), readU32(rdram, m + 20), readU32(rdram, m + 24),
                             readU32(rdram, m + 28), readU32(rdram, m + 32), readU32(rdram, m + 36),
                             readU32(rdram, m + 40), readU32(rdram, m + 44));
        }
        if (h.addr == 0x00280160u && h.hits <= 3)
        {
            const uint32_t slot = reg(ctx, 2);
            const uint32_t vt = slot ? readU32(rdram, slot + 0x1030u) : 0u;
            std::fprintf(stderr, "[black-snd] slot=0x%08x loaded=%u vt=0x%08x poll=0x%08x load=0x%08x\n", slot,
                         slot ? (readU32(rdram, slot + 0x102Cu) >> 16) & 0xFFu : 0u, vt,
                         vt ? readU32(rdram, vt + 0x14u) : 0u, vt ? readU32(rdram, vt + 0x0Cu) : 0u);
        }
        if (h.addr == 0x0027CAB8u)
            std::fprintf(stderr, "[black-fs] open \"%s\" -> 0x%08x\n", path, reg(ctx, 2));
        if (logThis)
            std::fprintf(stderr,
                         "[black-dbg]   leave %s pc=0x%08x (ra_in=0x%08x) s0=0x%08x (in 0x%08x) sp=0x%08x (in 0x%08x) "
                         "v0=0x%08x stack[s0 slot?]\n",
                         h.name, ctx->pc, raIn, reg(ctx, 16), s0In, reg(ctx, 29), spIn, reg(ctx, 2));
    }

    template <size_t... I>
    void installAll(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        (([&]
          {
              Hook &h = g_hooks[I];
              h.original = runtime.lookupFunction(h.addr);
              if (h.original)
                  runtime.replaceFunction(h.addr, &hookThunk<I>);
              else
                  std::fprintf(stderr, "[black-dbg] sem funcao em 0x%06x\n", h.addr);
          }()),
         ...);
    }

    // Watchpoint grosso: checa um endereco a cada entrada/retomada de funcao
    // guest (todas as chamadas passam pela tabela) e reporta a 1a mudanca.
    constexpr uint32_t kWatchAddr = 0x01ffff10u + 0x90u; // slot de s0 salvo pelo ctor 0x1020c0
    std::vector<PS2Runtime::RecompiledFunction> g_origTable;
    uint32_t g_watchLast = 0;
    bool g_watchArmed = false;
    unsigned g_watchReports = 0;
    uint32_t g_pcHistory[16] = {};
    unsigned g_pcHistoryPos = 0;

    uint32_t g_traceLo = 0, g_traceHi = 0;
    unsigned g_traceLeft = 0;

    // BLACK_PROFILE_AFTER=<guest_sec>:<dur_sec>: histogram of guest function entries in that
    // guest-time window, printed once (top 48), to see what a stalled game is spinning on.
    double g_profFrom = -1.0, g_profDur = 0.0;
    bool g_profDone = false;
    std::vector<uint32_t> g_profCounts;

    void profileEntry(uint32_t pc)
    {
        if (g_profDone || g_profFrom < 0.0)
            return;
        // Guest time cached by the Game::update hook: the scheduler clock takes a lock that can
        // already be held when guest code runs from interrupt/callback context.
        const double s_now = g_guestNowCached.load(std::memory_order_relaxed);
        if (s_now < g_profFrom)
            return;
        if (s_now >= g_profFrom + g_profDur)
        {
            g_profDone = true;
            std::vector<std::pair<uint32_t, uint32_t>> top;
            for (uint32_t i = 0; i < g_profCounts.size(); ++i)
                if (g_profCounts[i])
                    top.emplace_back(g_profCounts[i], g_ps2RecompiledFunctionTableBase + i * 4u);
            std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            std::fprintf(stderr, "[black-prof] window g=[%.1f, %.1f) functions=%zu\n", g_profFrom, g_profFrom + g_profDur,
                         top.size());
            for (size_t i = 0; i < top.size() && i < 400; ++i)
                std::fprintf(stderr, "[black-prof]   %06x %u\n", top[i].second, top[i].first);
            return;
        }
        const uint32_t slot = (pc - g_ps2RecompiledFunctionTableBase) >> 2;
        if (slot < g_profCounts.size())
            ++g_profCounts[slot];
    }

    void watchThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pc = ctx->pc;
        profileEntry(pc);
        static const double s_traceFrom = std::getenv("BLACK_TRACE_AFTER") ? std::atof(std::getenv("BLACK_TRACE_AFTER")) : 0.0;
        const bool traceEntry = g_traceLeft && pc >= g_traceLo && pc < g_traceHi &&
                                g_guestNowCached.load(std::memory_order_relaxed) >= s_traceFrom;
        if (traceEntry)
        {
            --g_traceLeft;
            std::fprintf(stderr, "[black-call] %06x a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x sp=%08x s0=%08x s1=%08x s2=%08x\n", pc, reg(ctx, 4), reg(ctx, 5),
                         reg(ctx, 6), reg(ctx, 7), reg(ctx, 31), reg(ctx, 29), reg(ctx, 16), reg(ctx, 17), reg(ctx, 18));
        }
        // BLACK_WATCH_ENTRY=<hexaddr>: check that word at every guest function entry and report
        // the first changes with the recent call history (default: the old ctor-slot watch).
        static const uint32_t s_watchAddr = std::getenv("BLACK_WATCH_ENTRY")
                                                ? std::strtoul(std::getenv("BLACK_WATCH_ENTRY"), nullptr, 16)
                                                : kWatchAddr;
        static const bool s_customWatch = std::getenv("BLACK_WATCH_ENTRY") != nullptr;
        const uint32_t cur = readU32(rdram, s_watchAddr);
        if (!g_watchArmed && (s_customWatch ? cur != 0u : cur == 0x00410000u))
        {
            g_watchArmed = true;
            std::fprintf(stderr, "[black-watch] armado em pc=0x%06x valor=0x%08x\n", pc, cur);
        }
        else if (g_watchArmed && cur != g_watchLast && g_watchReports < (s_customWatch ? 20u : 6u))
        {
            ++g_watchReports;
            std::fprintf(stderr, "[black-watch] 0x%08x: 0x%08x -> 0x%08x ao entrar em pc=0x%06x sp=0x%08x ra=0x%08x g=%.2f hist:",
                         s_watchAddr, g_watchLast, cur, pc, reg(ctx, 29), reg(ctx, 31),
                         g_guestNowCached.load(std::memory_order_relaxed));
            for (unsigned i = 0; i < 16; ++i)
                std::fprintf(stderr, " %06x", g_pcHistory[(g_pcHistoryPos + i) & 15u]);
            std::fprintf(stderr, "\n");
        }
        g_watchLast = cur;
        g_pcHistory[g_pcHistoryPos++ & 15u] = pc;

        const uint32_t slot = (pc - g_ps2RecompiledFunctionTableBase) >> 2;
        g_origTable[slot](rdram, ctx, runtime);
        if (traceEntry)
            std::fprintf(stderr, "[black-return] entry=%06x pc=%06x ra=%08x sp=%08x s0=%08x s1=%08x s2=%08x stack=%08x/%08x/%08x\n",
                         pc, ctx->pc, reg(ctx, 31), reg(ctx, 29), reg(ctx, 16), reg(ctx, 17), reg(ctx, 18),
                         readU32(rdram, reg(ctx, 29)), readU32(rdram, reg(ctx, 29) + 0x10u), readU32(rdram, reg(ctx, 29) + 0x20u));
    }

    void installWatch()
    {
        const uint32_t n = g_ps2RecompiledFunctionTableSlotCount;
        g_origTable.assign(g_ps2RecompiledFunctionTable, g_ps2RecompiledFunctionTable + n);
        uint32_t wrapped = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (g_ps2RecompiledFunctionTable[i])
            {
                g_ps2RecompiledFunctionTable[i] = &watchThunk;
                ++wrapped;
            }
        }
        std::fprintf(stderr, "[black-watch] %u slots envolvidos (base=0x%x)\n", wrapped, g_ps2RecompiledFunctionTableBase);
    }

    void applyBlackDebug(PS2Runtime &runtime)
    {
        // Diagnostics cost frame time (hooks, logs, GS history, frame dumps): opt-in only.
        if (!std::getenv("BLACK_DEBUG") || !*std::getenv("BLACK_DEBUG"))
            return;
        std::fprintf(stderr, "[black-dbg] overrides de diagnostico ativos\n");
        installAll(runtime, std::make_index_sequence<sizeof(g_hooks) / sizeof(g_hooks[0])>{});
        if (const char *range = std::getenv("BLACK_TRACE_RANGE"))
        {
            // BLACK_TRACE_RANGE=lo:hi[:count] logs guest function entries inside [lo, hi).
            unsigned count = 20000;
            std::sscanf(range, "%x:%x:%u", &g_traceLo, &g_traceHi, &count);
            g_traceLeft = count;
        }
        if (const char *prof = std::getenv("BLACK_PROFILE_AFTER"))
        {
            std::sscanf(prof, "%lf:%lf", &g_profFrom, &g_profDur);
            g_profCounts.assign(g_ps2RecompiledFunctionTableSlotCount, 0u);
        }
        if (std::getenv("BLACK_WATCH") || std::getenv("BLACK_WATCH_ENTRY") || g_traceLeft || g_profFrom >= 0.0)
            installWatch();
    }
}

PS2_REGISTER_GAME_OVERRIDE("Black debug hooks", "SLUS_213.76", 0x00100008u, 0u, applyBlackDebug)
