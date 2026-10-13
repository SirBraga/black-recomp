// Differential test of the IOP CPU core (ps2xIOP iop_cpu_exec.inl, which the recompiled IRX code also runs)
// against an independent reference model of the R3000A written from the instruction set description.
//
//   iop_cpu_difftest words.txt [runs per word]
//
// words.txt: one instruction word per line (iop_cpu_difftest_words.py takes every distinct word of the
// game's recompiled IOP modules). Every word runs on random registers, HI/LO, a random load still in flight
// (the R3000's load delay slot) and random memory; the core and the model must then agree on all GPRs,
// HI/LO, the load left in flight, the branch decision, the exception raised and the memory around the access.
// Not covered: COP0 beyond MFC0/MTC0/RFE, SYSCALL/BREAK (exceptions only checked as "raised").
#include "iop_cpu.h"
#include "iop_memory.h"
#include "iop_cpu_exec.inl"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace ps2x::iop::detail;

namespace
{
    constexpr uint32_t kBase = 0x00100000u; // test window in IOP RAM
    constexpr uint32_t kSpan = 0x00002000u;

    struct Ref
    {
        uint32_t gpr[32]{};
        uint32_t hi = 0, lo = 0;
        bool pending = false;
        uint32_t pendingReg = 0, pendingValue = 0;
        bool branch = false;
        uint32_t branchTarget = 0;
        bool exception = false;
        std::vector<uint8_t> mem;
    };

    uint32_t rd32(const Ref &r, uint32_t a) { uint32_t v; std::memcpy(&v, &r.mem[a - kBase], 4); return v; }
    void wr32(Ref &r, uint32_t a, uint32_t v) { std::memcpy(&r.mem[a - kBase], &v, 4); }

    // One instruction on the reference model. Returns false for words it does not model.
    bool reference(Ref &r, uint32_t w, uint32_t pc)
    {
        const uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31, fn = w & 63;
        const uint32_t imm = w & 0xFFFF;
        const int32_t simm = static_cast<int16_t>(imm);
        const uint32_t s = r.gpr[rs], t = r.gpr[rt];
        uint32_t written = 0;
        bool load = false;
        uint32_t loadReg = 0, loadValue = 0;
        auto set = [&](uint32_t reg, uint32_t v) { if (reg) { r.gpr[reg] = v; written = reg; } };
        auto ld = [&](uint32_t reg, uint32_t v) { if (reg) { load = true; loadReg = reg; loadValue = v; } };
        // The register value a LWL/LWR merges into: the one still being loaded into it, if any.
        auto mergeBase = [&](uint32_t reg) { return r.pending && r.pendingReg == reg ? r.pendingValue : r.gpr[reg]; };
        auto takeBranch = [&](bool c) { if (c) { r.branch = true; r.branchTarget = pc + 4 + (static_cast<uint32_t>(simm) << 2); } };
        r.branch = false;
        r.exception = false;
        switch (op)
        {
        case 0x00:
            switch (fn)
            {
            case 0x00: set(rd, t << sa); break;
            case 0x02: set(rd, t >> sa); break;
            case 0x03: set(rd, static_cast<uint32_t>(static_cast<int32_t>(t) >> sa)); break;
            case 0x04: set(rd, t << (s & 31)); break;
            case 0x06: set(rd, t >> (s & 31)); break;
            case 0x07: set(rd, static_cast<uint32_t>(static_cast<int32_t>(t) >> (s & 31))); break;
            case 0x08: r.branch = true; r.branchTarget = s; break;
            case 0x09: set(rd, pc + 8); r.branch = true; r.branchTarget = s; break;
            case 0x0C: case 0x0D: r.exception = true; break;
            case 0x10: set(rd, r.hi); break;
            case 0x11: r.hi = s; break;
            case 0x12: set(rd, r.lo); break;
            case 0x13: r.lo = s; break;
            case 0x18: { const int64_t p = int64_t(int32_t(s)) * int64_t(int32_t(t)); r.lo = uint32_t(p); r.hi = uint32_t(uint64_t(p) >> 32); break; }
            case 0x19: { const uint64_t p = uint64_t(s) * t; r.lo = uint32_t(p); r.hi = uint32_t(p >> 32); break; }
            case 0x1A:
                if (t == 0) { r.lo = int32_t(s) >= 0 ? 0xFFFFFFFFu : 1u; r.hi = s; }
                else if (s == 0x80000000u && t == 0xFFFFFFFFu) { r.lo = s; r.hi = 0; }
                else { r.lo = uint32_t(int32_t(s) / int32_t(t)); r.hi = uint32_t(int32_t(s) % int32_t(t)); }
                break;
            case 0x1B: if (t == 0) { r.lo = 0xFFFFFFFFu; r.hi = s; } else { r.lo = s / t; r.hi = s % t; } break;
            case 0x20: { int32_t v; if (__builtin_add_overflow(int32_t(s), int32_t(t), &v)) r.exception = true; else set(rd, uint32_t(v)); break; }
            case 0x21: set(rd, s + t); break;
            case 0x22: { int32_t v; if (__builtin_sub_overflow(int32_t(s), int32_t(t), &v)) r.exception = true; else set(rd, uint32_t(v)); break; }
            case 0x23: set(rd, s - t); break;
            case 0x24: set(rd, s & t); break;
            case 0x25: set(rd, s | t); break;
            case 0x26: set(rd, s ^ t); break;
            case 0x27: set(rd, ~(s | t)); break;
            case 0x2A: set(rd, int32_t(s) < int32_t(t)); break;
            case 0x2B: set(rd, s < t); break;
            default: return false;
            }
            break;
        case 0x01:
            if (rt == 0x10 || rt == 0x11) set(31, pc + 8);
            if (rt == 0x00 || rt == 0x10) takeBranch(int32_t(s) < 0);
            else if (rt == 0x01 || rt == 0x11) takeBranch(int32_t(s) >= 0);
            else return false;
            break;
        case 0x02: case 0x03:
            if (op == 0x03) set(31, pc + 8);
            r.branch = true; r.branchTarget = ((pc + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
            break;
        case 0x04: takeBranch(s == t); break;
        case 0x05: takeBranch(s != t); break;
        case 0x06: takeBranch(int32_t(s) <= 0); break;
        case 0x07: takeBranch(int32_t(s) > 0); break;
        case 0x08: { int32_t v; if (__builtin_add_overflow(int32_t(s), simm, &v)) r.exception = true; else set(rt, uint32_t(v)); break; }
        case 0x09: set(rt, s + uint32_t(simm)); break;
        case 0x0A: set(rt, int32_t(s) < simm); break;
        case 0x0B: set(rt, s < uint32_t(simm)); break;
        case 0x0C: set(rt, s & imm); break;
        case 0x0D: set(rt, s | imm); break;
        case 0x0E: set(rt, s ^ imm); break;
        case 0x0F: set(rt, imm << 16); break;
        case 0x20: case 0x24: { const uint8_t b = r.mem[s + simm - kBase]; ld(rt, op == 0x20 ? uint32_t(int32_t(int8_t(b))) : b); break; }
        case 0x21: case 0x25:
        {
            const uint32_t a = s + simm;
            if (a & 1) { r.exception = true; break; }
            uint16_t h; std::memcpy(&h, &r.mem[a - kBase], 2);
            ld(rt, op == 0x21 ? uint32_t(int32_t(int16_t(h))) : h);
            break;
        }
        case 0x23: { const uint32_t a = s + simm; if (a & 3) { r.exception = true; break; } ld(rt, rd32(r, a)); break; }
        case 0x22: case 0x26:
        {
            // LWL fills the register from its top byte down to the addressed byte; LWR from the addressed byte up to
            // its bottom (little endian), keeping the other bytes.
            const uint32_t a = s + simm, word = rd32(r, a & ~3u), base = mergeBase(rt), k = a & 3;
            uint32_t v = base;
            if (op == 0x22) { const uint32_t keep = k == 3 ? 0u : (0xFFFFFFFFu >> (8 * (k + 1))); v = (base & keep) | (word << (8 * (3 - k))); }
            else { const uint32_t keep = k == 0 ? 0u : ~(0xFFFFFFFFu >> (8 * k)); v = (base & keep) | (word >> (8 * k)); }
            ld(rt, v);
            break;
        }
        case 0x28: r.mem[s + simm - kBase] = uint8_t(t); break;
        case 0x29: { const uint32_t a = s + simm; if (a & 1) { r.exception = true; break; } const uint16_t h = uint16_t(t); std::memcpy(&r.mem[a - kBase], &h, 2); break; }
        case 0x2B: { const uint32_t a = s + simm; if (a & 3) { r.exception = true; break; } wr32(r, a, t); break; }
        case 0x2A: case 0x2E:
        {
            const uint32_t a = s + simm, k = a & 3, old = rd32(r, a & ~3u);
            uint32_t v;
            if (op == 0x2A) { const uint32_t keep = k == 3 ? 0u : ~(0xFFFFFFFFu >> (8 * (3 - k))); v = (old & keep) | (t >> (8 * (3 - k))); }
            else { const uint32_t keep = k == 0 ? 0u : (0xFFFFFFFFu >> (8 * (4 - k))); v = (old & keep) | (t << (8 * k)); }
            wr32(r, a & ~3u, v);
            break;
        }
        default: return false;
        }
        if (r.exception)
            return true; // nothing committed after an exception; only "raised" is compared
        // The load issued by the previous instruction lands now, unless this instruction wrote that register.
        if (r.pending && r.pendingReg != written)
            r.gpr[r.pendingReg] = r.pendingValue;
        r.pending = load;
        r.pendingReg = loadReg;
        r.pendingValue = loadValue;
        r.gpr[0] = 0;
        return true;
    }

    bool isMemoryOp(uint32_t op) { return (op >= 0x20 && op <= 0x26) || (op >= 0x28 && op <= 0x2B) || op == 0x2E; }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s words.txt [runs]\n", argv[0]);
        return 2;
    }
    const int runs = argc > 2 ? std::atoi(argv[2]) : 64;
    std::vector<uint32_t> words;
    if (std::FILE *f = std::fopen(argv[1], "r"))
    {
        unsigned w;
        while (std::fscanf(f, "%x", &w) == 1)
            words.push_back(w);
        std::fclose(f);
    }
    IopMemory memory;
    IopCpuCore core(memory);
    std::mt19937 rng(1234);
    size_t tested = 0, failed = 0, unmodelled = 0;
    std::map<uint32_t, size_t> unmodelledOps, failedOps;
    for (const uint32_t w : words)
    {
        const uint32_t op = w >> 26;
        if (op == 0x10 || op == 0x12) // COP0 / COP2: outside this test
        {
            ++unmodelled; ++unmodelledOps[op];
            continue;
        }
        bool wordFailed = false, modelled = true;
        for (int run = 0; run < runs && !wordFailed && modelled; ++run)
        {
            Ref ref;
            ref.mem.resize(kSpan);
            for (auto &b : ref.mem) b = uint8_t(rng());
            for (int i = 1; i < 32; ++i)
                switch (rng() % 5) { case 0: ref.gpr[i] = rng() & 0xFF; break; case 1: ref.gpr[i] = uint32_t(-int32_t(rng() & 0xFF)); break;
                                     case 2: ref.gpr[i] = 0x80000000u ^ (rng() & 0xF); break; default: ref.gpr[i] = rng(); }
            ref.hi = rng(); ref.lo = rng();
            if (rng() % 2) { ref.pending = true; ref.pendingReg = 1 + rng() % 31; ref.pendingValue = rng(); }
            const uint32_t rs = (w >> 21) & 31, rt = (w >> 16) & 31;
            const int32_t simm = static_cast<int16_t>(w & 0xFFFF);
            if (isMemoryOp(op))
            {
                // Point the base register into the test window; usually aligned for the access size.
                uint32_t target = kBase + 0x800 + (rng() % (kSpan - 0x1000));
                if (rng() % 8) target &= (op == 0x21 || op == 0x25 || op == 0x29) ? ~1u : (op == 0x23 || op == 0x2B) ? ~3u : ~0u;
                if (rs == 0) continue; // absolute address: not in the window
                ref.gpr[rs] = target - uint32_t(simm);
                if (ref.pending && ref.pendingReg == rs) ref.pending = false; // keep the address register settled
                (void)rt;
            }
            IopCpuState cpu{};
            for (int i = 0; i < 32; ++i) cpu.gpr[i] = ref.gpr[i];
            cpu.hi = ref.hi; cpu.lo = ref.lo; cpu.pc = 0x00020000u;
            cpu.pendingLoad = ref.pending; cpu.pendingLoadReg = ref.pendingReg; cpu.pendingLoadValue = ref.pendingValue;
            for (uint32_t i = 0; i < kSpan; i += 4)
            {
                uint32_t v; std::memcpy(&v, &ref.mem[i], 4); memory.write32(kBase + i, v);
            }
            if (!reference(ref, w, cpu.pc)) { modelled = false; break; }
            (void)core.executeDecoded(cpu, w);
            std::string why;
            char buf[160];
            if (cpu.exception != ref.exception) { std::snprintf(buf, sizeof buf, "exception %d vs %d", cpu.exception, ref.exception); why = buf; }
            else if (!ref.exception)
            {
                for (int i = 1; i < 32 && why.empty(); ++i)
                    if (cpu.gpr[i] != ref.gpr[i]) { std::snprintf(buf, sizeof buf, "r%d = %08x, expected %08x", i, cpu.gpr[i], ref.gpr[i]); why = buf; }
                if (why.empty() && (cpu.hi != ref.hi || cpu.lo != ref.lo)) { std::snprintf(buf, sizeof buf, "hi/lo %08x/%08x, expected %08x/%08x", cpu.hi, cpu.lo, ref.hi, ref.lo); why = buf; }
                if (why.empty() && (cpu.pendingLoad != ref.pending || (ref.pending && (cpu.pendingLoadReg != ref.pendingReg || cpu.pendingLoadValue != ref.pendingValue))))
                { std::snprintf(buf, sizeof buf, "load in flight r%u=%08x (%d), expected r%u=%08x (%d)", cpu.pendingLoadReg, cpu.pendingLoadValue, cpu.pendingLoad, ref.pendingReg, ref.pendingValue, ref.pending); why = buf; }
                if (why.empty() && (cpu.branchPending != ref.branch || (ref.branch && cpu.branchTarget != ref.branchTarget)))
                { std::snprintf(buf, sizeof buf, "branch %d->%08x, expected %d->%08x", cpu.branchPending, cpu.branchTarget, ref.branch, ref.branchTarget); why = buf; }
                for (uint32_t i = 0; i < kSpan && why.empty(); i += 4)
                {
                    uint32_t want; std::memcpy(&want, &ref.mem[i], 4);
                    const uint32_t got = memory.read32(kBase + i);
                    if (got != want) { std::snprintf(buf, sizeof buf, "memory %08x = %08x, expected %08x", kBase + i, got, want); why = buf; }
                }
            }
            if (!why.empty())
            {
                wordFailed = true;
                ++failedOps[op == 0 ? 0x100 | (w & 63) : op];
                if (failed < 30)
                    std::printf("MISMATCH %08x (op %02x funct %02x): %s\n", w, op, w & 63, why.c_str());
            }
        }
        if (!modelled) { ++unmodelled; ++unmodelledOps[op == 0 ? 0x100 | (w & 63) : op]; continue; }
        ++tested;
        failed += wordFailed;
    }
    std::printf("\ntested %zu instruction words, %zu mismatching, %zu not modelled\n", tested, failed, unmodelled);
    for (const auto &[k, n] : failedOps) std::printf("  failing %s %02x: %zu\n", k & 0x100 ? "funct" : "op", k & 0xFF, n);
    for (const auto &[k, n] : unmodelledOps) std::printf("  not modelled %s %02x: %zu\n", k & 0x100 ? "funct" : "op", k & 0xFF, n);
    return failed ? 1 : 0;
}
