// BLACK_FPS=1: prints game logic updates per second (Game::update @0x102BD0), with no other hooks.
#include "ps2_runtime.h"
#include "game_overrides.h"
#include "runtime/ps2_wait_stats.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mach/mach.h>

namespace
{
    PS2Runtime::RecompiledFunction g_update = nullptr;
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
            if (!g_onlyHitches || s_over25 != 0u)
                std::fprintf(stderr, "[black-fps] worst=%.1fms over20=%u over25=%u vif1 kick<=%.1fms done<=%.1fms | slow frame %.1fms: kick +%.1f done +%.1f | vblank-late=%.1fms dropped=%llu waits(ms): vif1-room=%.0f vif1-done=%.0f gs-room=%.0f iop=%.0f threads:%s updates/s=%.1f\n",
                             s_worst, s_over20, s_over25, s_kickWorst, s_doneWorst, s_hitchGap, s_hitchKick, s_hitchDone, double(g_ps2xVblankLateMaxNs.exchange(0u)) / 1e6, (unsigned long long)g_ps2xVblankRebased.exchange(0u),
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
    }
}

PS2_REGISTER_GAME_OVERRIDE("Black fps counter", "SLUS_213.76", 0x00100008u, 0u, applyBlackFps)
