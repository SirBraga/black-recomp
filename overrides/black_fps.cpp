// BLACK_FPS=1: prints game logic updates per second (Game::update @0x102BD0), with no other hooks.
#include "ps2_runtime.h"
#include "game_overrides.h"
#include "runtime/ps2_wait_stats.h"
#include "runtime/ee_scheduler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mach/mach.h>
#include <time.h>

namespace
{
    PS2Runtime::RecompiledFunction g_update = nullptr;
    // Kept apart from the shared ring of marks, which the many callback marks of a frame overrun.
    struct QueueMark { char kind; uint32_t value; std::chrono::steady_clock::time_point at; };
    QueueMark g_queueMarks[64];
    uint32_t g_queueMarkCount = 0;
    void queueMark(char kind, uint32_t value)
    {
        if (g_queueMarkCount < 64u)
            g_queueMarks[g_queueMarkCount++] = {kind, value, std::chrono::steady_clock::now()};
    }
    bool g_onlyHitches = false; // BLACK_FPS=hitch: only the windows that had a slow update are printed

    // How busy each thread is right now (the kernel's own decayed figure), busiest first: tells which part of the
    // pipeline was the limit in a slow window without attaching a profiler, which would itself slow the game.
    std::string busiestThreads()
    {
        thread_act_array_t threads = nullptr;
        mach_msg_type_number_t count = 0;
        if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
            return {};
        struct Usage { int percent; char name[64]; } top[4] = {};
        for (mach_msg_type_number_t i = 0; i < count; ++i)
        {
            thread_extended_info_data_t info{};
            mach_msg_type_number_t size = THREAD_EXTENDED_INFO_COUNT;
            if (thread_info(threads[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &size) == KERN_SUCCESS)
            {
                Usage usage{info.pth_cpu_usage / 10, {}};
                std::snprintf(usage.name, sizeof(usage.name), "%s", info.pth_name[0] ? info.pth_name : "?");
                for (Usage &slot : top)
                    if (usage.percent > slot.percent)
                        std::swap(usage, slot);
            }
            mach_port_deallocate(mach_task_self(), threads[i]);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
        std::string text;
        for (const Usage &slot : top)
            if (slot.percent > 0)
                text += " " + std::string(slot.name) + "=" + std::to_string(slot.percent) + "%";
        return text;
    }

    void updateThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Besides the rate: the longest gap between two updates in the window and how many gaps were longer than
        // 20 ms and 25 ms. A steady 60 has none; these are what is felt as a hitch.
        static auto s_t0 = std::chrono::steady_clock::now();
        static auto s_last = s_t0;
        static unsigned s_n = 0, s_over20 = 0, s_over25 = 0;
        static double s_worst = 0.0;
        const auto now = std::chrono::steady_clock::now();
        static double s_workWorst = 0.0; // longest single call of the update function in the window
        static double s_kickWorst = 0.0, s_doneWorst = 0.0, s_hitchGap = 0.0, s_hitchKick = 0.0, s_hitchDone = 0.0;
        if (ctx->pc == 0x00102BD0u)
        {
            ++s_n;
            // Within the frame that just ended: when did the game hand its last VIF1 chain to the worker, and when did
            // the worker finish the last one, both counted from the start of the frame.
            const uint64_t started = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(s_last.time_since_epoch()).count());
            const uint64_t kick = g_ps2xVif1KickNs.load(std::memory_order_relaxed), done = g_ps2xVif1DoneNs.load(std::memory_order_relaxed);
            const double kickAfter = kick > started ? double(kick - started) / 1e6 : -1.0, doneAfter = done > started ? double(done - started) / 1e6 : -1.0;
            s_kickWorst = kickAfter > s_kickWorst ? kickAfter : s_kickWorst;
            s_doneWorst = doneAfter > s_doneWorst ? doneAfter : s_doneWorst;
            const double gap = std::chrono::duration<double, std::milli>(now - s_last).count();
            s_last = now;
            s_worst = gap > s_worst ? gap : s_worst;
            s_over20 += gap > 20.0 ? 1u : 0u;
            s_over25 += gap > 25.0 ? 1u : 0u;
            // BLACK_FPS_TIMELINE=1: what happened inside the frame that just ended, for every slow frame and for one
            // normal frame in 240 (times in ms from the start of the frame; K/P carry the size in KiB).
            static const bool s_timeline = std::getenv("BLACK_FPS_TIMELINE") != nullptr;
            static unsigned s_frame = 0;
            const uint32_t marked = g_ps2xMarkCount.exchange(0u, std::memory_order_relaxed);
            struct QueueReset { ~QueueReset() { g_queueMarkCount = 0; } } queueReset;
            if (s_timeline && (gap > 25.0 || ++s_frame % 240u == 0u))
            {
                std::string line;
                for (uint32_t i = 0; i < (marked < 256u ? marked : 256u); ++i)
                {
                    const Ps2xMark &mark = g_ps2xMarks[i];
                    char text[96];
                    if (mark.kind == 'K' || mark.kind == 'P' || mark.kind == 'c' || mark.kind == 'h')
                        std::snprintf(text, sizeof(text), " %c%u@%.1f", mark.kind, mark.value / 1024u, double(int64_t(mark.ns - started)) / 1e6);
                    else if (mark.kind == 'w')
                        std::snprintf(text, sizeof(text), " w[dma=%u queued=%u ready=%u count=%u/%u]@%.1f", mark.value >> 24, (mark.value >> 16) & 1u, (mark.value >> 8) & 255u, (mark.value >> 4) & 15u, mark.value & 15u, double(int64_t(mark.ns - started)) / 1e6);
                    else
                        std::snprintf(text, sizeof(text), " %c@%.1f", mark.kind, double(int64_t(mark.ns - started)) / 1e6);
                    line += text;
                }
                std::string queue;
                for (uint32_t i = 0; i < g_queueMarkCount; ++i)
                {
                    const QueueMark &mark = g_queueMarks[i];
                    char text[96];
                    const double at = gap - std::chrono::duration<double, std::milli>(now - mark.at).count();
                    if (mark.kind == 'F')
                        std::snprintf(text, sizeof(text), " F[%u cmds vif=%u gif=%u waits=%u]@%.1f", mark.value & 255u, (mark.value >> 8) & 255u, (mark.value >> 16) & 255u, mark.value >> 24, at);
                    else if (mark.kind == 'E')
                        std::snprintf(text, sizeof(text), " E%u@%.1f", mark.value, at);
                    else
                        std::snprintf(text, sizeof(text), " %c@%.1f", mark.kind, at);
                    queue += text;
                }
                std::fprintf(stderr, "[black-queue] %.1fms:%s\n", gap, queue.c_str());

                std::fprintf(stderr, "[black-frame] %.1fms (%u marks):%s\n", gap, marked, line.c_str());
            }
            if (gap > 25.0)
            {
                // The slow frame itself.
                s_hitchGap = gap;
                s_hitchKick = kickAfter;
                s_hitchDone = doneAfter;
            }
        }
        const double dt = std::chrono::duration<double>(now - s_t0).count();
        if (dt >= 2.0)
        {
            // Cost of the game thread per unit of guest work: its CPU time over the EE cycles the generated code
            // charged in the window. Unlike the busy share, this does not move with what is on screen.
            static uint64_t s_cpuNs = 0, s_workCycles = 0;
            const uint64_t cpuNs = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID), workCycles = runtime->eeScheduler().workCycles();
            const double nsPerKiloCycle = workCycles > s_workCycles ? double(cpuNs - s_cpuNs) * 1000.0 / double(workCycles - s_workCycles) : 0.0;
            const double megaCyclesPerUpdate = s_n ? double(workCycles - s_workCycles) / 1e6 / double(s_n) : 0.0;
            const double cpuMsPerUpdate = s_n ? double(cpuNs - s_cpuNs) / 1e6 / double(s_n) : 0.0;
            s_cpuNs = cpuNs;
            s_workCycles = workCycles;
            if (!g_onlyHitches)
                std::fprintf(stderr, "[black-ee] %.1f ns per 1000 EE cycles | %.2f M cycles and %.2f ms of game thread per update\n",
                         nsPerKiloCycle, megaCyclesPerUpdate, cpuMsPerUpdate);
            // Time threads spent blocked on each other in this window, in ms: game on a full VIF1 queue, game on VIF1
            // finishing, VIF1/VU1 on a full renderer feed, game on the IOP thread.
            static uint64_t s_waited[kPs2xWaitKinds] = {};
            double waited[kPs2xWaitKinds];
            for (unsigned kind = 0; kind < kPs2xWaitKinds; ++kind)
            {
                const uint64_t total = g_ps2xWaitNs[kind].load(std::memory_order_relaxed);
                waited[kind] = double(total - s_waited[kind]) / 1e6;
                s_waited[kind] = total;
            }
            // Frames shown: vertical blanks the game answered with a new frame. This, not the spacing of the calls of
            // the game's loop function (one call can render two frames), is what a dropped frame looks like.
            static uint64_t s_blanks = 0, s_presents = 0;
            const uint64_t blanksNow = g_ps2xVblanks.load(std::memory_order_relaxed), presentsNow = g_ps2xPresents.load(std::memory_order_relaxed);
            const uint64_t blanks = blanksNow - s_blanks, presented = presentsNow - s_presents;
            s_blanks = blanksNow;
            s_presents = presentsNow;
            if (!g_onlyHitches || presented + 1u < blanks)
                std::fprintf(stderr, "[black-fps] presented %llu of %llu blanks | loop: worst=%.1fms over20=%u over25=%u vif1 kick<=%.1fms done<=%.1fms | slow frame %.1fms: kick +%.1f done +%.1f | vblank-late=%.1fms dropped=%llu waits(ms): vif1-room=%.0f vif1-done=%.0f gs-room=%.0f iop=%.0f threads:%s updates/s=%.1f\n",
                             (unsigned long long)presented, (unsigned long long)blanks, s_worst, s_over20, s_over25, s_kickWorst, s_doneWorst, s_hitchGap, s_hitchKick, s_hitchDone, double(g_ps2xVblankLateMaxNs.exchange(0u)) / 1e6, (unsigned long long)g_ps2xVblankRebased.exchange(0u),
                             waited[0], waited[1], waited[2], waited[3], busiestThreads().c_str(), s_n / dt);
            else
            {
                g_ps2xVblankLateMaxNs.store(0u);
                g_ps2xVblankRebased.store(0u);
            }
            s_workWorst = 0.0;
            s_kickWorst = s_doneWorst = s_hitchGap = s_hitchKick = s_hitchDone = 0.0;
            s_n = s_over20 = s_over25 = 0;
            s_worst = 0.0;
            s_t0 = now;
        }
        g_update(rdram, ctx, runtime);
        const double work = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count();
        s_workWorst = work > s_workWorst ? work : s_workWorst;
    }

    // Timeline marks for the game's command queue (BLACK_FPS_TIMELINE): 'F' when the frame's queue is handed over
    // (0x2B32D8), with what it holds (commands, VIF1 sends, GIF sends, waits for a vertical blank); 'f' when that
    // call returns; 'E' at the mid-frame attempt (0x2B4E80) with the "a blank went by" flag.
    PS2Runtime::RecompiledFunction g_queueFlush = nullptr, g_queueTry = nullptr;
    constexpr uint32_t kQueueFlush = 0x002B32D8u, kQueueTry = 0x002B4E80u;

    void queueFlushThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool entry = ctx->pc == kQueueFlush;
        if (entry)
        {
            uint32_t write, table[2];
            uint8_t index;
            std::memcpy(&write, rdram + 0x40E64Cu, 4);
            std::memcpy(table, rdram + 0x40E628u, 8);
            std::memcpy(&index, rdram + 0x40E634u, 1);
            const uint32_t begin = table[index & 1u];
            uint32_t commands = 0, vif = 0, gif = 0, waits = 0;
            for (uint32_t at = begin; at + 8u <= write && at < begin + 0x4000u && commands < 255u; at += 8u, ++commands)
            {
                const uint8_t code = rdram[at & 0x1FFFFFFu];
                vif += code == 0x01u;
                gif += code == 0x02u;
                waits += code == 0x40u || code == 0x41u;
            }
            queueMark('F', commands | (vif << 8) | (gif << 16) | (waits << 24));
        }
        g_queueFlush(rdram, ctx, runtime);
        if (entry)
            queueMark('f', 0u);
    }

    void queueTryThunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kQueueTry)
            queueMark('E', rdram[0x40E670u]);
        g_queueTry(rdram, ctx, runtime);
    }

    void applyBlackFps(PS2Runtime &runtime)
    {
        const char *dbg = std::getenv("BLACK_DEBUG");
        const char *mode = std::getenv("BLACK_FPS");
        if (!mode || (dbg && *dbg))
            return;
        g_onlyHitches = std::strcmp(mode, "hitch") == 0;
        g_update = runtime.lookupFunction(0x00102BD0u);
        if (g_update)
            runtime.replaceFunction(0x00102BD0u, &updateThunk);
        if (std::getenv("BLACK_FPS_TIMELINE"))
        {
            g_queueFlush = runtime.lookupFunction(kQueueFlush);
            if (g_queueFlush)
                runtime.replaceFunction(kQueueFlush, &queueFlushThunk);
            g_queueTry = runtime.lookupFunction(kQueueTry);
            if (g_queueTry)
                runtime.replaceFunction(kQueueTry, &queueTryThunk);
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("Black fps counter", "SLUS_213.76", 0x00100008u, 0u, applyBlackFps)
