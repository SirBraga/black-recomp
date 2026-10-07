#!/usr/bin/env python3
"""Compile the production dispatch/checkpoint bodies against a deterministic scheduler.
No window, ELF or copyrighted data is needed. Run from the repository root.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/PS2Recomp/ps2xRuntime/src/lib/ps2_runtime.cpp').read_text()
def body(start, end):
    return source[source.index(start):source.index(end, source.index(start))]
code = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
struct R5900Context { uint32_t pc = 0; uint32_t r[32]{}; };
uint32_t getRegU32(R5900Context* ctx, int r) { return ctx->r[r]; }
struct EeScheduler {
    static constexpr uint32_t kGuestDispatchCycles = 32;
    bool due = false;
    bool checkpointDue(uint32_t) { bool result = due; due = false; return result; }
};
thread_local uint64_t g_guestDispatchTransferSequence = 0;
struct PS2Runtime {
    enum class GuestBranchKind { DirectCall, IndirectCall, DirectJump, Return };
    enum class MissingFunctionPolicy { SkipCallDebug, ContinueToTarget, Stop };
    using RecompiledFunction = void(*)(uint8_t*, R5900Context*, PS2Runtime*);
    EeScheduler* m_eeScheduler;
    std::map<uint32_t, RecompiledFunction> functions;
    bool hasFunction(uint32_t pc) { return functions.count(pc); }
    RecompiledFunction lookupFunction(uint32_t pc) { return functions.at(pc); }
    void reportMissingFunction(uint8_t*, R5900Context*, uint32_t, uint32_t, GuestBranchKind, const char*) {}
    MissingFunctionPolicy missingFunctionPolicy() { return MissingFunctionPolicy::Stop; }
    bool isStopRequested() { return false; }
    bool dispatchGuestBranch(uint8_t*, R5900Context*, uint32_t, uint32_t, uint32_t, GuestBranchKind, const char*);
    bool eeCheckpointDue(uint32_t cycles = 32) noexcept;
};
'''
code += body('bool PS2Runtime::dispatchGuestBranch(', 'void PS2Runtime::SignalException(')
code += body('bool PS2Runtime::eeCheckpointDue(', '[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(')
code += r'''
int main() {
    EeScheduler scheduler;
    PS2Runtime runtime{&scheduler, {}};
    R5900Context ctx;
    ctx.r[29] = 0x1000;
    auto check = [&](bool ok, const char* label) {
        if (!ok) std::fprintf(stderr, "FAIL: %s\n", label);
        return ok;
    };
    bool ok = true;
    runtime.functions[0x200] = [](uint8_t*, R5900Context* c, PS2Runtime*) { c->pc = 0x300; };
    ok &= check(runtime.dispatchGuestBranch(nullptr, &ctx, 0x200, 0x100, 0x300,
                 PS2Runtime::GuestBranchKind::DirectCall, "test"), "normal return continues caller");
    // A recursive descendant pauses at the same address as the ancestor's continuation.
    runtime.functions[0x200] = [](uint8_t*, R5900Context* c, PS2Runtime* r) {
        c->r[29] -= 0x40;
        c->pc = 0x300;
        r->m_eeScheduler->due = true;
        if (r->eeCheckpointDue()) return;
    };
    ok &= check(!runtime.dispatchGuestBranch(nullptr, &ctx, 0x200, 0x100, 0x300,
                 PS2Runtime::GuestBranchKind::DirectCall, "test"), "nested yield must not become return");
    ok &= check(ctx.pc == 0x300 && ctx.r[29] == 0xfc0, "yield preserves guest continuation and stack");
    // A polling loop without a frame can pause at its own entry PC.
    runtime.functions[0x200] = [](uint8_t*, R5900Context* c, PS2Runtime* r) {
        c->pc = 0x200;
        r->m_eeScheduler->due = true;
        if (r->eeCheckpointDue()) return;
    };
    ok &= check(!runtime.dispatchGuestBranch(nullptr, &ctx, 0x200, 0x100, 0x300,
                 PS2Runtime::GuestBranchKind::DirectCall, "test"), "entry-PC yield must not become unchanged stub return");
    ok &= check(ctx.pc == 0x200, "polling entry preserved");
    runtime.functions[0x200] = [](uint8_t*, R5900Context*, PS2Runtime*) {};
    ok &= check(runtime.dispatchGuestBranch(nullptr, &ctx, 0x200, 0x100, 0x300,
                 PS2Runtime::GuestBranchKind::DirectCall, "test"), "unchanged HLE stub still returns");
    std::puts(ok ? "dispatch yield: PASS" : "dispatch yield: FAIL");
    return ok ? 0 : 1;
}
'''
with tempfile.TemporaryDirectory(prefix='black-dispatch-test-') as temp:
    test = Path(temp) / 'test.cpp'
    executable = Path(temp) / 'test'
    test.write_text(code)
    subprocess.run(['clang++', '-std=c++17', str(test), '-o', str(executable)], check=True)
    raise SystemExit(subprocess.run([str(executable)]).returncode)
