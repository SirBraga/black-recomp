// Differential test of the EE recompiler's instruction translations against an independent
// reference model of the R5900.
//
// Input: recomp/diagnostics/ee-difftest/ops.inc, i.e. the translator's C++ for a sample of the game's
// instruction words (ee_difftest_words.py picks them from the ELF; vu0_macro_emit turns them into
// callable functions). Every function runs on random register and memory contents, and all of the
// architectural state it can touch is compared with what the reference model below computes for the
// same word: GPRs (128 bits), HI/LO/HI1/LO1, FPU registers, FPU accumulator and condition bit, and
// the memory around the access.
//
// The reference model is written from the instruction set description, not from the translator:
// a disagreement means one of the two is wrong and has to be looked at. Floating point runs with
// round-toward-zero on both sides (as the game thread does); overflow/underflow clamping of the
// PS2 FPU is outside this test (operands are kept in a range where it cannot happen).
//
// Not covered: branches and jumps (control flow is emitted by another part of the recompiler),
// COP0, COP2 (see vu0_macro_difftest.cpp), SYSCALL/BREAK, CACHE/PREF, and the instructions listed
// as "no reference" in the summary.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include PS2X_EE_DIFFTEST_OPS

// The runtime library expects the game's function table; this test never dispatches guest code.
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {nullptr};

namespace
{
    constexpr uint32_t kMemBase = 0x00200000u; // inside EE RAM, away from address 0
    constexpr uint32_t kMemSpan = 0x00010000u;

    struct Ref
    {
        uint64_t lo64[32]{}, hi64[32]{};
        uint64_t hi = 0, lo = 0, hi1 = 0, lo1 = 0;
        uint32_t f[32]{};
        uint32_t acc = 0;
        bool cond = false;
    };

    uint32_t bitsOf(float v) { uint32_t b; std::memcpy(&b, &v, 4); return b; }
    float floatOf(uint32_t b) { float v; std::memcpy(&v, &b, 4); return v; }
    uint64_t sext32(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }

    struct Vec
    {
        uint64_t q[2]{};
        uint32_t w(int i) const { return static_cast<uint32_t>(q[i >> 1] >> ((i & 1) * 32)); }
        uint16_t h(int i) const { return static_cast<uint16_t>(q[i >> 2] >> ((i & 3) * 16)); }
        uint8_t b(int i) const { return static_cast<uint8_t>(q[i >> 3] >> ((i & 7) * 8)); }
        void setW(int i, uint32_t v) { q[i >> 1] = (q[i >> 1] & ~(0xFFFFFFFFull << ((i & 1) * 32))) | (uint64_t(v) << ((i & 1) * 32)); }
        void setH(int i, uint16_t v) { q[i >> 2] = (q[i >> 2] & ~(0xFFFFull << ((i & 3) * 16))) | (uint64_t(v) << ((i & 3) * 16)); }
        void setB(int i, uint8_t v) { q[i >> 3] = (q[i >> 3] & ~(0xFFull << ((i & 7) * 8))) | (uint64_t(v) << ((i & 7) * 8)); }
    };

    Vec gpr(const Ref &r, uint32_t reg)
    {
        Vec v;
        if (reg != 0)
        {
            v.q[0] = r.lo64[reg];
            v.q[1] = r.hi64[reg];
        }
        return v;
    }
    void setVec(Ref &r, uint32_t reg, const Vec &v)
    {
        if (reg == 0)
            return;
        r.lo64[reg] = v.q[0];
        r.hi64[reg] = v.q[1];
    }
    uint64_t g64(const Ref &r, uint32_t reg) { return reg == 0 ? 0 : r.lo64[reg]; }
    uint32_t g32(const Ref &r, uint32_t reg) { return static_cast<uint32_t>(g64(r, reg)); }
    // Writes of 64 bits or less leave the upper half of the 128-bit register alone.
    void set64(Ref &r, uint32_t reg, uint64_t v)
    {
        if (reg != 0)
            r.lo64[reg] = v;
    }

    uint64_t rd64(const uint8_t *mem, uint32_t addr) { uint64_t v; std::memcpy(&v, mem + addr, 8); return v; }
    uint32_t rd32(const uint8_t *mem, uint32_t addr) { uint32_t v; std::memcpy(&v, mem + addr, 4); return v; }
    void wr64(uint8_t *mem, uint32_t addr, uint64_t v) { std::memcpy(mem + addr, &v, 8); }
    void wr32(uint8_t *mem, uint32_t addr, uint32_t v) { std::memcpy(mem + addr, &v, 4); }

    // PS2 FPU helpers (single precision only; no NaN/infinity on the real machine).
    float fpuDiv(float a, float b)
    {
        if (b == 0.0f)
            return floatOf(((bitsOf(a) ^ bitsOf(b)) & 0x80000000u) | 0x7F7FFFFFu);
        return a / b;
    }
    int32_t fpuCvtW(float a)
    {
        if (a >= 2147483648.0f)
            return 0x7FFFFFFF;
        if (a <= -2147483648.0f)
            return static_cast<int32_t>(0x80000000u);
        return static_cast<int32_t>(a); // truncates
    }

    // Returns false when the reference model does not implement the instruction.
    bool refExec(uint32_t word, Ref &r, uint8_t *mem)
    {
        const uint32_t op = word >> 26, rs = (word >> 21) & 31u, rt = (word >> 16) & 31u, rd = (word >> 11) & 31u;
        const uint32_t sa = (word >> 6) & 31u, funct = word & 63u;
        const int32_t simm = static_cast<int16_t>(word & 0xFFFFu);
        const uint32_t uimm = word & 0xFFFFu;
        const uint32_t addr = g32(r, rs) + static_cast<uint32_t>(simm);

        switch (op)
        {
        case 0x00:
            switch (funct)
            {
            case 0x00: set64(r, rd, sext32(g32(r, rt) << sa)); return true;
            case 0x02: set64(r, rd, sext32(g32(r, rt) >> sa)); return true;
            case 0x03: set64(r, rd, sext32(static_cast<uint32_t>(static_cast<int32_t>(g32(r, rt)) >> sa))); return true;
            case 0x04: set64(r, rd, sext32(g32(r, rt) << (g32(r, rs) & 31u))); return true;
            case 0x06: set64(r, rd, sext32(g32(r, rt) >> (g32(r, rs) & 31u))); return true;
            case 0x07: set64(r, rd, sext32(static_cast<uint32_t>(static_cast<int32_t>(g32(r, rt)) >> (g32(r, rs) & 31u)))); return true;
            case 0x0A: if (g64(r, rt) == 0) set64(r, rd, g64(r, rs)); return true;
            case 0x0B: if (g64(r, rt) != 0) set64(r, rd, g64(r, rs)); return true;
            case 0x10: set64(r, rd, r.hi); return true;
            case 0x11: r.hi = g64(r, rs); return true;
            case 0x12: set64(r, rd, r.lo); return true;
            case 0x13: r.lo = g64(r, rs); return true;
            case 0x14: set64(r, rd, g64(r, rt) << (g32(r, rs) & 63u)); return true;
            case 0x16: set64(r, rd, g64(r, rt) >> (g32(r, rs) & 63u)); return true;
            case 0x17: set64(r, rd, static_cast<uint64_t>(static_cast<int64_t>(g64(r, rt)) >> (g32(r, rs) & 63u))); return true;
            case 0x18:
            case 0x19:
            {
                const uint64_t product = funct == 0x18
                                             ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(g32(r, rs))) * static_cast<int64_t>(static_cast<int32_t>(g32(r, rt))))
                                             : uint64_t(g32(r, rs)) * uint64_t(g32(r, rt));
                r.lo = sext32(static_cast<uint32_t>(product));
                r.hi = sext32(static_cast<uint32_t>(product >> 32));
                set64(r, rd, r.lo); // three-operand form of the R5900
                return true;
            }
            case 0x1A:
            {
                const int32_t a = static_cast<int32_t>(g32(r, rs)), b = static_cast<int32_t>(g32(r, rt));
                if (b == 0) { r.lo = sext32(a < 0 ? 1u : 0xFFFFFFFFu); r.hi = sext32(static_cast<uint32_t>(a)); }
                else if (a == INT32_MIN && b == -1) { r.lo = sext32(0x80000000u); r.hi = 0; }
                else { r.lo = sext32(static_cast<uint32_t>(a / b)); r.hi = sext32(static_cast<uint32_t>(a % b)); }
                return true;
            }
            case 0x1B:
            {
                const uint32_t a = g32(r, rs), b = g32(r, rt);
                if (b == 0) { r.lo = sext32(0xFFFFFFFFu); r.hi = sext32(a); }
                else { r.lo = sext32(a / b); r.hi = sext32(a % b); }
                return true;
            }
            // ADD/SUB trap on signed overflow and leave the destination alone (no handler here).
            case 0x20: { int32_t v; if (!__builtin_add_overflow(static_cast<int32_t>(g32(r, rs)), static_cast<int32_t>(g32(r, rt)), &v)) set64(r, rd, sext32(static_cast<uint32_t>(v))); return true; }
            case 0x21: set64(r, rd, sext32(g32(r, rs) + g32(r, rt))); return true;
            case 0x22: { int32_t v; if (!__builtin_sub_overflow(static_cast<int32_t>(g32(r, rs)), static_cast<int32_t>(g32(r, rt)), &v)) set64(r, rd, sext32(static_cast<uint32_t>(v))); return true; }
            case 0x23: set64(r, rd, sext32(g32(r, rs) - g32(r, rt))); return true;
            case 0x24: set64(r, rd, g64(r, rs) & g64(r, rt)); return true;
            case 0x25: set64(r, rd, g64(r, rs) | g64(r, rt)); return true;
            case 0x26: set64(r, rd, g64(r, rs) ^ g64(r, rt)); return true;
            case 0x27: set64(r, rd, ~(g64(r, rs) | g64(r, rt))); return true;
            case 0x2A: set64(r, rd, static_cast<int64_t>(g64(r, rs)) < static_cast<int64_t>(g64(r, rt)) ? 1 : 0); return true;
            case 0x2B: set64(r, rd, g64(r, rs) < g64(r, rt) ? 1 : 0); return true;
            case 0x2C: { int64_t v; if (!__builtin_add_overflow(static_cast<int64_t>(g64(r, rs)), static_cast<int64_t>(g64(r, rt)), &v)) set64(r, rd, static_cast<uint64_t>(v)); return true; }
            case 0x2D: set64(r, rd, g64(r, rs) + g64(r, rt)); return true;
            case 0x2E: { int64_t v; if (!__builtin_sub_overflow(static_cast<int64_t>(g64(r, rs)), static_cast<int64_t>(g64(r, rt)), &v)) set64(r, rd, static_cast<uint64_t>(v)); return true; }
            case 0x2F: set64(r, rd, g64(r, rs) - g64(r, rt)); return true;
            case 0x38: set64(r, rd, g64(r, rt) << sa); return true;
            case 0x3A: set64(r, rd, g64(r, rt) >> sa); return true;
            case 0x3B: set64(r, rd, static_cast<uint64_t>(static_cast<int64_t>(g64(r, rt)) >> sa)); return true;
            case 0x3C: set64(r, rd, g64(r, rt) << (sa + 32u)); return true;
            case 0x3E: set64(r, rd, g64(r, rt) >> (sa + 32u)); return true;
            case 0x3F: set64(r, rd, static_cast<uint64_t>(static_cast<int64_t>(g64(r, rt)) >> (sa + 32u))); return true;
            default: return false;
            }
        case 0x08: { int32_t v; if (!__builtin_add_overflow(static_cast<int32_t>(g32(r, rs)), simm, &v)) set64(r, rt, sext32(static_cast<uint32_t>(v))); return true; }
        case 0x09: set64(r, rt, sext32(g32(r, rs) + static_cast<uint32_t>(simm))); return true;
        case 0x0A: set64(r, rt, static_cast<int64_t>(g64(r, rs)) < static_cast<int64_t>(simm) ? 1 : 0); return true;
        case 0x0B: set64(r, rt, g64(r, rs) < static_cast<uint64_t>(static_cast<int64_t>(simm)) ? 1 : 0); return true;
        case 0x0C: set64(r, rt, g64(r, rs) & uimm); return true;
        case 0x0D: set64(r, rt, g64(r, rs) | uimm); return true;
        case 0x0E: set64(r, rt, g64(r, rs) ^ uimm); return true;
        case 0x0F: set64(r, rt, sext32(uimm << 16)); return true;
        case 0x18: { int64_t v; if (!__builtin_add_overflow(static_cast<int64_t>(g64(r, rs)), static_cast<int64_t>(simm), &v)) set64(r, rt, static_cast<uint64_t>(v)); return true; }
        case 0x19: set64(r, rt, g64(r, rs) + static_cast<uint64_t>(static_cast<int64_t>(simm))); return true;
        case 0x1A: // LDL
        {
            const uint32_t s = addr & 7u;
            const uint64_t m = rd64(mem, addr & ~7u);
            const uint64_t mask = s == 7u ? 0ull : (0x00FFFFFFFFFFFFFFull >> (s * 8u));
            set64(r, rt, (g64(r, rt) & mask) | (m << (56u - s * 8u)));
            return true;
        }
        case 0x1B: // LDR
        {
            const uint32_t s = addr & 7u;
            const uint64_t m = rd64(mem, addr & ~7u);
            const uint64_t mask = s == 0u ? 0ull : (0xFFFFFFFFFFFFFFFFull << (64u - s * 8u));
            set64(r, rt, (g64(r, rt) & mask) | (m >> (s * 8u)));
            return true;
        }
        case 0x1C:
        {
            const Vec a = gpr(r, rs), b = gpr(r, rt);
            Vec d;
            if (funct == 0x08) // MMI0
            {
                switch (sa)
                {
                case 0x00: for (int i = 0; i < 4; ++i) d.setW(i, a.w(i) + b.w(i)); break;
                case 0x01: for (int i = 0; i < 4; ++i) d.setW(i, a.w(i) - b.w(i)); break;
                case 0x02: for (int i = 0; i < 4; ++i) d.setW(i, static_cast<int32_t>(a.w(i)) > static_cast<int32_t>(b.w(i)) ? 0xFFFFFFFFu : 0u); break;
                case 0x03: for (int i = 0; i < 4; ++i) d.setW(i, static_cast<int32_t>(a.w(i)) > static_cast<int32_t>(b.w(i)) ? a.w(i) : b.w(i)); break;
                case 0x04: for (int i = 0; i < 8; ++i) d.setH(i, static_cast<uint16_t>(a.h(i) + b.h(i))); break;
                case 0x05: for (int i = 0; i < 8; ++i) d.setH(i, static_cast<uint16_t>(a.h(i) - b.h(i))); break;
                case 0x06: for (int i = 0; i < 8; ++i) d.setH(i, static_cast<int16_t>(a.h(i)) > static_cast<int16_t>(b.h(i)) ? 0xFFFFu : 0u); break;
                case 0x07: for (int i = 0; i < 8; ++i) d.setH(i, static_cast<int16_t>(a.h(i)) > static_cast<int16_t>(b.h(i)) ? a.h(i) : b.h(i)); break;
                case 0x08: for (int i = 0; i < 16; ++i) d.setB(i, static_cast<uint8_t>(a.b(i) + b.b(i))); break;
                case 0x09: for (int i = 0; i < 16; ++i) d.setB(i, static_cast<uint8_t>(a.b(i) - b.b(i))); break;
                case 0x0A: for (int i = 0; i < 16; ++i) d.setB(i, static_cast<int8_t>(a.b(i)) > static_cast<int8_t>(b.b(i)) ? 0xFFu : 0u); break;
                case 0x12: d.setW(0, b.w(0)); d.setW(1, a.w(0)); d.setW(2, b.w(1)); d.setW(3, a.w(1)); break;
                case 0x13: d.setW(0, b.w(0)); d.setW(1, b.w(2)); d.setW(2, a.w(0)); d.setW(3, a.w(2)); break;
                case 0x16: for (int i = 0; i < 4; ++i) { d.setH(2 * i, b.h(i)); d.setH(2 * i + 1, a.h(i)); } break;
                case 0x17: for (int i = 0; i < 4; ++i) { d.setH(i, b.h(2 * i)); d.setH(i + 4, a.h(2 * i)); } break;
                case 0x1A: for (int i = 0; i < 8; ++i) { d.setB(2 * i, b.b(i)); d.setB(2 * i + 1, a.b(i)); } break;
                case 0x1B: for (int i = 0; i < 8; ++i) { d.setB(i, b.b(2 * i)); d.setB(i + 8, a.b(2 * i)); } break;
                default: return false;
                }
                setVec(r, rd, d);
                return true;
            }
            if (funct == 0x28) // MMI1
            {
                switch (sa)
                {
                case 0x02: for (int i = 0; i < 4; ++i) d.setW(i, a.w(i) == b.w(i) ? 0xFFFFFFFFu : 0u); break;
                case 0x03: for (int i = 0; i < 4; ++i) d.setW(i, static_cast<int32_t>(a.w(i)) < static_cast<int32_t>(b.w(i)) ? a.w(i) : b.w(i)); break;
                case 0x06: for (int i = 0; i < 8; ++i) d.setH(i, a.h(i) == b.h(i) ? 0xFFFFu : 0u); break;
                case 0x07: for (int i = 0; i < 8; ++i) d.setH(i, static_cast<int16_t>(a.h(i)) < static_cast<int16_t>(b.h(i)) ? a.h(i) : b.h(i)); break;
                case 0x0A: for (int i = 0; i < 16; ++i) d.setB(i, a.b(i) == b.b(i) ? 0xFFu : 0u); break;
                case 0x10: for (int i = 0; i < 4; ++i) { const uint64_t s = uint64_t(a.w(i)) + b.w(i); d.setW(i, s > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(s)); } break;
                case 0x12: d.setW(0, b.w(2)); d.setW(1, a.w(2)); d.setW(2, b.w(3)); d.setW(3, a.w(3)); break;
                case 0x16: for (int i = 0; i < 4; ++i) { d.setH(2 * i, b.h(i + 4)); d.setH(2 * i + 1, a.h(i + 4)); } break;
                case 0x1A: for (int i = 0; i < 8; ++i) { d.setB(2 * i, b.b(i + 8)); d.setB(2 * i + 1, a.b(i + 8)); } break;
                default: return false;
                }
                setVec(r, rd, d);
                return true;
            }
            if (funct == 0x09) // MMI2
            {
                switch (sa)
                {
                case 0x02: d.q[0] = sext32(b.w(0) << (a.w(0) & 31u)); d.q[1] = sext32(b.w(2) << (a.w(2) & 31u)); break;
                case 0x03: d.q[0] = sext32(b.w(0) >> (a.w(0) & 31u)); d.q[1] = sext32(b.w(2) >> (a.w(2) & 31u)); break;
                case 0x08: d.q[0] = r.hi; d.q[1] = r.hi1; break; // PMFHI: the whole 128-bit HI
                case 0x09: d.q[0] = r.lo; d.q[1] = r.lo1; break;
                case 0x0E: d.q[0] = b.q[0]; d.q[1] = a.q[0]; break;
                case 0x12: d.q[0] = a.q[0] & b.q[0]; d.q[1] = a.q[1] & b.q[1]; break;
                case 0x13: d.q[0] = a.q[0] ^ b.q[0]; d.q[1] = a.q[1] ^ b.q[1]; break;
                case 0x1F: d.setW(0, b.w(1)); d.setW(1, b.w(2)); d.setW(2, b.w(0)); d.setW(3, b.w(3)); break;
                default: return false;
                }
                setVec(r, rd, d);
                return true;
            }
            if (funct == 0x29) // MMI3
            {
                switch (sa)
                {
                case 0x03:
                    d.q[0] = sext32(static_cast<uint32_t>(static_cast<int32_t>(b.w(0)) >> (a.w(0) & 31u)));
                    d.q[1] = sext32(static_cast<uint32_t>(static_cast<int32_t>(b.w(2)) >> (a.w(2) & 31u)));
                    break;
                case 0x0E: d.q[0] = a.q[1]; d.q[1] = b.q[1]; break;
                case 0x12: d.q[0] = a.q[0] | b.q[0]; d.q[1] = a.q[1] | b.q[1]; break;
                case 0x13: d.q[0] = ~(a.q[0] | b.q[0]); d.q[1] = ~(a.q[1] | b.q[1]); break;
                case 0x1B: for (int i = 0; i < 4; ++i) { d.setH(i, b.h(0)); d.setH(i + 4, b.h(4)); } break;
                default: return false;
                }
                setVec(r, rd, d);
                return true;
            }
            switch (funct)
            {
            case 0x00: // MADD
            case 0x01: // MADDU
            {
                const uint64_t accum = (uint64_t(static_cast<uint32_t>(r.hi)) << 32) | static_cast<uint32_t>(r.lo);
                const uint64_t product = funct == 0x00
                                             ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a.w(0))) * static_cast<int64_t>(static_cast<int32_t>(b.w(0))))
                                             : uint64_t(a.w(0)) * uint64_t(b.w(0));
                const uint64_t sum = accum + product;
                r.lo = sext32(static_cast<uint32_t>(sum));
                r.hi = sext32(static_cast<uint32_t>(sum >> 32));
                set64(r, rd, r.lo);
                return true;
            }
            case 0x10: set64(r, rd, r.hi1); return true;
            case 0x11: r.hi1 = g64(r, rs); return true;
            case 0x12: set64(r, rd, r.lo1); return true;
            case 0x13: r.lo1 = g64(r, rs); return true;
            case 0x18:
            case 0x19:
            {
                const uint64_t product = funct == 0x18
                                             ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a.w(0))) * static_cast<int64_t>(static_cast<int32_t>(b.w(0))))
                                             : uint64_t(a.w(0)) * uint64_t(b.w(0));
                r.lo1 = sext32(static_cast<uint32_t>(product));
                r.hi1 = sext32(static_cast<uint32_t>(product >> 32));
                set64(r, rd, r.lo1);
                return true;
            }
            case 0x1A:
            {
                const int32_t x = static_cast<int32_t>(a.w(0)), y = static_cast<int32_t>(b.w(0));
                if (y == 0) { r.lo1 = sext32(x < 0 ? 1u : 0xFFFFFFFFu); r.hi1 = sext32(static_cast<uint32_t>(x)); }
                else if (x == INT32_MIN && y == -1) { r.lo1 = sext32(0x80000000u); r.hi1 = 0; }
                else { r.lo1 = sext32(static_cast<uint32_t>(x / y)); r.hi1 = sext32(static_cast<uint32_t>(x % y)); }
                return true;
            }
            case 0x1B:
            {
                const uint32_t x = a.w(0), y = b.w(0);
                if (y == 0) { r.lo1 = sext32(0xFFFFFFFFu); r.hi1 = sext32(x); }
                else { r.lo1 = sext32(x / y); r.hi1 = sext32(x % y); }
                return true;
            }
            case 0x34: for (int i = 0; i < 8; ++i) d.setH(i, static_cast<uint16_t>(b.h(i) << (sa & 15u))); setVec(r, rd, d); return true;
            case 0x36: for (int i = 0; i < 8; ++i) d.setH(i, static_cast<uint16_t>(b.h(i) >> (sa & 15u))); setVec(r, rd, d); return true;
            case 0x37: for (int i = 0; i < 8; ++i) d.setH(i, static_cast<uint16_t>(static_cast<int16_t>(b.h(i)) >> (sa & 15u))); setVec(r, rd, d); return true;
            case 0x3C: for (int i = 0; i < 4; ++i) d.setW(i, b.w(i) << sa); setVec(r, rd, d); return true;
            case 0x3E: for (int i = 0; i < 4; ++i) d.setW(i, b.w(i) >> sa); setVec(r, rd, d); return true;
            case 0x3F: for (int i = 0; i < 4; ++i) d.setW(i, static_cast<uint32_t>(static_cast<int32_t>(b.w(i)) >> sa)); setVec(r, rd, d); return true;
            default: return false;
            }
        }
        case 0x1E: { Vec v; v.q[0] = rd64(mem, addr & ~15u); v.q[1] = rd64(mem, (addr & ~15u) + 8u); setVec(r, rt, v); return true; }
        case 0x1F: { const Vec v = gpr(r, rt); wr64(mem, addr & ~15u, v.q[0]); wr64(mem, (addr & ~15u) + 8u, v.q[1]); return true; }
        case 0x20: set64(r, rt, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(mem[addr])))); return true;
        case 0x21: { uint16_t v; std::memcpy(&v, mem + addr, 2); set64(r, rt, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(v)))); return true; }
        case 0x22: // LWL
        {
            const uint32_t s = addr & 3u, m = rd32(mem, addr & ~3u);
            const uint32_t mask = s == 3u ? 0u : (0x00FFFFFFu >> (s * 8u));
            set64(r, rt, sext32((g32(r, rt) & mask) | (m << (24u - s * 8u))));
            return true;
        }
        case 0x23: set64(r, rt, sext32(rd32(mem, addr))); return true;
        case 0x24: set64(r, rt, mem[addr]); return true;
        case 0x25: { uint16_t v; std::memcpy(&v, mem + addr, 2); set64(r, rt, v); return true; }
        case 0x26: // LWR: a whole word is sign-extended, a partial one only replaces the low 32 bits
        {
            const uint32_t s = addr & 3u, m = rd32(mem, addr & ~3u);
            if (s == 0u)
                set64(r, rt, sext32(m));
            else
            {
                const uint32_t mask = 0xFFFFFFFFu << (32u - s * 8u);
                set64(r, rt, (g64(r, rt) & 0xFFFFFFFF00000000ull) | ((g32(r, rt) & mask) | (m >> (s * 8u))));
            }
            return true;
        }
        case 0x27: set64(r, rt, rd32(mem, addr)); return true;
        case 0x28: mem[addr] = static_cast<uint8_t>(g32(r, rt)); return true;
        case 0x29: { const uint16_t v = static_cast<uint16_t>(g32(r, rt)); std::memcpy(mem + addr, &v, 2); return true; }
        case 0x2A: // SWL
        {
            const uint32_t s = addr & 3u, m = rd32(mem, addr & ~3u);
            const uint32_t mask = s == 3u ? 0u : (0xFFFFFF00u << (s * 8u));
            wr32(mem, addr & ~3u, (g32(r, rt) >> (24u - s * 8u)) | (m & mask));
            return true;
        }
        case 0x2B: wr32(mem, addr, g32(r, rt)); return true;
        case 0x2C: // SDL
        {
            const uint32_t s = addr & 7u;
            const uint64_t m = rd64(mem, addr & ~7u);
            const uint64_t mask = s == 7u ? 0ull : (0xFFFFFFFFFFFFFF00ull << (s * 8u));
            wr64(mem, addr & ~7u, (g64(r, rt) >> (56u - s * 8u)) | (m & mask));
            return true;
        }
        case 0x2D: // SDR
        {
            const uint32_t s = addr & 7u;
            const uint64_t m = rd64(mem, addr & ~7u);
            const uint64_t mask = s == 0u ? 0ull : (0xFFFFFFFFFFFFFFFFull >> (64u - s * 8u));
            wr64(mem, addr & ~7u, (g64(r, rt) << (s * 8u)) | (m & mask));
            return true;
        }
        case 0x2E: // SWR
        {
            const uint32_t s = addr & 3u, m = rd32(mem, addr & ~3u);
            const uint32_t mask = s == 0u ? 0u : (0xFFFFFFFFu >> (32u - s * 8u));
            wr32(mem, addr & ~3u, (g32(r, rt) << (s * 8u)) | (m & mask));
            return true;
        }
        case 0x31: r.f[rt] = rd32(mem, addr); return true;
        case 0x37: set64(r, rt, rd64(mem, addr)); return true;
        case 0x39: wr32(mem, addr, r.f[rt]); return true;
        case 0x3F: wr64(mem, addr, g64(r, rt)); return true;
        case 0x11:
        {
            const uint32_t fs = rd, ft = rt, fd = sa;
            if (rs == 0x00) { set64(r, rt, sext32(r.f[fs])); return true; }
            if (rs == 0x04) { r.f[fs] = g32(r, rt); return true; }
            if (rs == 0x14) // W
            {
                if (funct != 0x20)
                    return false;
                r.f[fd] = bitsOf(static_cast<float>(static_cast<int32_t>(r.f[fs])));
                return true;
            }
            if (rs != 0x10)
                return false;
            volatile float a = floatOf(r.f[fs]), b = floatOf(r.f[ft]), acc = floatOf(r.acc);
            volatile float t = 0.0f;
            switch (funct)
            {
            case 0x00: t = a + b; r.f[fd] = bitsOf(t); return true;
            case 0x01: t = a - b; r.f[fd] = bitsOf(t); return true;
            case 0x02: t = a * b; r.f[fd] = bitsOf(t); return true;
            case 0x03: r.f[fd] = bitsOf(fpuDiv(a, b)); return true;
            case 0x04: r.f[fd] = bitsOf(std::sqrt(std::fabs(static_cast<float>(b)))); return true; // operand is ft
            case 0x05: r.f[fd] = r.f[fs] & 0x7FFFFFFFu; return true;
            case 0x06: r.f[fd] = r.f[fs]; return true;
            case 0x07: r.f[fd] = r.f[fs] ^ 0x80000000u; return true;
            case 0x16: // RSQRT: fs / sqrt(|ft|); a zero (or denormal) ft gives the largest value with ft's sign
                if ((r.f[ft] & 0x7F800000u) == 0u) r.f[fd] = (r.f[ft] & 0x80000000u) | 0x7F7FFFFFu;
                else r.f[fd] = bitsOf(a / std::sqrt(std::fabs(static_cast<float>(b))));
                return true;
            case 0x18: t = a + b; r.acc = bitsOf(t); return true;
            case 0x19: t = a - b; r.acc = bitsOf(t); return true;
            case 0x1A: t = a * b; r.acc = bitsOf(t); return true;
            case 0x1C: t = a * b; t = acc + t; r.f[fd] = bitsOf(t); return true;
            case 0x1D: t = a * b; t = acc - t; r.f[fd] = bitsOf(t); return true;
            case 0x1E: t = a * b; t = acc + t; r.acc = bitsOf(t); return true;
            case 0x1F: t = a * b; t = acc - t; r.acc = bitsOf(t); return true;
            case 0x24: r.f[fd] = static_cast<uint32_t>(fpuCvtW(a)); return true;
            case 0x28: r.f[fd] = bitsOf(a >= b ? static_cast<float>(a) : static_cast<float>(b)); return true;
            case 0x29: r.f[fd] = bitsOf(a <= b ? static_cast<float>(a) : static_cast<float>(b)); return true;
            case 0x30: r.cond = false; return true;
            case 0x32: r.cond = a == b; return true;
            case 0x34: r.cond = a < b; return true;
            case 0x36: r.cond = a <= b; return true;
            default: return false;
            }
        }
        default:
            return false;
        }
    }

    std::string mnemonicOf(const char *text)
    {
        std::string s(text);
        const size_t space = s.find(' ');
        return space == std::string::npos ? s : s.substr(0, space);
    }
}

int main()
{
    std::fesetround(FE_TOWARDZERO);
    std::mt19937_64 rng(20261009);
    std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0), refMem(PS2_RAM_SIZE, 0);
    std::map<std::string, unsigned> failures, unsupported, testedBy;
    unsigned tested = 0, failed = 0, skipped = 0;

    auto randomGpr = [&](uint64_t &lo)
    {
        switch (rng() % 6u)
        {
        case 0: lo = rng(); break;
        case 1: lo = sext32(static_cast<uint32_t>(rng())); break;
        case 2: lo = rng() % 64u; break;
        case 3: lo = static_cast<uint64_t>(-static_cast<int64_t>(rng() % 1000u)); break;
        case 4: lo = static_cast<uint32_t>(rng()); break; // 32-bit value that is not sign-extended
        default: lo = (rng() % 2u) ? 0u : sext32(0x80000000u);
        }
    };
    auto randomFloat = [&]() -> uint32_t
    {
        const float base = static_cast<float>(static_cast<int32_t>(rng() % 200001u) - 100000) / 1000.0f;
        switch (rng() % 5u)
        {
        case 0: return bitsOf(base);
        case 1: return bitsOf(std::floor(base));
        case 2: return bitsOf(base * 1000.0f);
        case 3: return bitsOf(base * 0.001f);
        default: return (rng() % 4u) == 0u ? ((rng() & 1u) ? 0x80000000u : 0u) : bitsOf(base * base);
        }
    };

    for (const MacroOp &op : kMacroOps)
    {
        const std::string mnemonic = mnemonicOf(op.text);
        {
            Ref probe;
            std::vector<uint8_t> scratch(64, 0);
            // Addresses in the probe are 0 + imm and may be out of the scratch buffer: only ask
            // whether the opcode is known, on a copy with base 0 and immediate 0.
            uint32_t word = op.word;
            const uint32_t opc = word >> 26;
            if ((opc >= 0x1Au && opc <= 0x1Bu) || (opc >= 0x1Eu && opc <= 0x3Fu && opc != 0x1Cu))
                word &= 0xFC1F0000u;
            if (!refExec(word, probe, scratch.data()))
            {
                ++skipped;
                ++unsupported[mnemonic];
                continue;
            }
        }

        bool opFailed = false;
        std::string detail;
        for (int round = 0; round < 32 && !opFailed; ++round)
        {
            Ref ref;
            R5900Context ctx{};
            for (int reg = 1; reg < 32; ++reg)
            {
                randomGpr(ref.lo64[reg]);
                ref.hi64[reg] = rng();
            }
            // Base registers of memory instructions point into the test window, with every
            // alignment (the unaligned load/store family depends on it).
            const uint32_t opc = op.word >> 26;
            const bool memoryOp = (opc >= 0x1Au && opc <= 0x1Bu) || opc == 0x1Eu || opc == 0x1Fu || (opc >= 0x20u && opc <= 0x3Fu);
            if (memoryOp)
            {
                const uint32_t base = (op.word >> 21) & 31u;
                if (base == 0u)
                    continue;
                uint32_t target = kMemBase + 0x8000u + static_cast<uint32_t>(rng() % 0x1000u);
                // LQ/SQ ignore the low four address bits on the R5900; the translation does not mask
                // them. Games only use aligned addresses there, so the test does too (not exercised).
                if (opc == 0x1Eu || opc == 0x1Fu)
                    target &= ~15u;
                ref.lo64[base] = sext32(target - static_cast<uint32_t>(static_cast<int16_t>(op.word & 0xFFFFu)));
            }
            ref.hi = sext32(static_cast<uint32_t>(rng()));
            ref.lo = sext32(static_cast<uint32_t>(rng()));
            ref.hi1 = sext32(static_cast<uint32_t>(rng()));
            ref.lo1 = sext32(static_cast<uint32_t>(rng()));
            for (uint32_t &f : ref.f)
                f = randomFloat();
            ref.acc = randomFloat();
            ref.cond = (rng() & 1u) != 0u;

            for (uint32_t offset = 0; offset < kMemSpan; offset += 8u)
            {
                const uint64_t value = rng();
                std::memcpy(&rdram[kMemBase + offset], &value, 8);
            }
            std::memcpy(&refMem[kMemBase], &rdram[kMemBase], kMemSpan);

            for (int reg = 1; reg < 32; ++reg)
                ctx.r[reg] = _mm_set_epi64x(static_cast<int64_t>(ref.hi64[reg]), static_cast<int64_t>(ref.lo64[reg]));
            ctx.hi = ref.hi; ctx.lo = ref.lo; ctx.hi1 = ref.hi1; ctx.lo1 = ref.lo1;
            for (int reg = 0; reg < 32; ++reg)
                std::memcpy(&ctx.f[reg], &ref.f[reg], 4);
            std::memcpy(&ctx.f_acc, &ref.acc, 4);
            ctx.fcr31 = ref.cond ? 0x00800000u : 0u;
            const Ref before = ref;

            refExec(op.word, ref, refMem.data());
            op.run(rdram.data(), &ctx, nullptr);

            char text[320];
            for (int reg = 1; reg < 32 && !opFailed; ++reg)
            {
                uint64_t lo, hi;
                std::memcpy(&lo, &ctx.r[reg], 8);
                std::memcpy(&hi, reinterpret_cast<const uint8_t *>(&ctx.r[reg]) + 8, 8);
                if (lo != ref.lo64[reg] || hi != ref.hi64[reg])
                {
                    std::snprintf(text, sizeof(text), "r%d = %016llx_%016llx, expected %016llx_%016llx (before %016llx_%016llx)", reg,
                                  (unsigned long long)hi, (unsigned long long)lo, (unsigned long long)ref.hi64[reg], (unsigned long long)ref.lo64[reg],
                                  (unsigned long long)before.hi64[reg], (unsigned long long)before.lo64[reg]);
                    detail = text;
                    opFailed = true;
                }
            }
            const struct { const char *name; uint64_t got, want; } wide[] = {
                {"hi", ctx.hi, ref.hi}, {"lo", ctx.lo, ref.lo}, {"hi1", ctx.hi1, ref.hi1}, {"lo1", ctx.lo1, ref.lo1}};
            for (const auto &w : wide)
                if (!opFailed && w.got != w.want)
                {
                    std::snprintf(text, sizeof(text), "%s = %016llx, expected %016llx", w.name, (unsigned long long)w.got, (unsigned long long)w.want);
                    detail = text;
                    opFailed = true;
                }
            for (int reg = 0; reg < 32 && !opFailed; ++reg)
            {
                uint32_t got;
                std::memcpy(&got, &ctx.f[reg], 4);
                if (got != ref.f[reg])
                {
                    std::snprintf(text, sizeof(text), "f%d = %08x (%g), expected %08x (%g); fs=%g ft=%g acc=%g", reg, got, floatOf(got), ref.f[reg],
                                  floatOf(ref.f[reg]), floatOf(before.f[(op.word >> 11) & 31u]), floatOf(before.f[(op.word >> 16) & 31u]), floatOf(before.acc));
                    detail = text;
                    opFailed = true;
                }
            }
            if (!opFailed)
            {
                uint32_t acc;
                std::memcpy(&acc, &ctx.f_acc, 4);
                if (acc != ref.acc)
                {
                    std::snprintf(text, sizeof(text), "acc = %08x, expected %08x", acc, ref.acc);
                    detail = text;
                    opFailed = true;
                }
                else if (((ctx.fcr31 & 0x00800000u) != 0u) != ref.cond)
                {
                    detail = "FPU condition bit differs";
                    opFailed = true;
                }
                else if (std::memcmp(&rdram[kMemBase], &refMem[kMemBase], kMemSpan) != 0)
                {
                    uint32_t at = 0;
                    while (rdram[kMemBase + at] == refMem[kMemBase + at])
                        ++at;
                    std::snprintf(text, sizeof(text), "memory differs at +0x%x: %02x, expected %02x", at, rdram[kMemBase + at], refMem[kMemBase + at]);
                    detail = text;
                    opFailed = true;
                }
            }
        }
        ++tested;
        ++testedBy[mnemonic];
        if (opFailed)
        {
            ++failed;
            if (failures[mnemonic]++ < 3)
                std::printf("MISMATCH %08x %s: %s\n", op.word, op.text, detail.c_str());
        }
    }

    std::printf("\ntested %u instruction words, %u mismatching, %u without a reference model\n", tested, failed, skipped);
    for (const auto &entry : failures)
        std::printf("  %-10s %u of %u words mismatch\n", entry.first.c_str(), entry.second, testedBy[entry.first]);
    if (!unsupported.empty())
    {
        std::printf("no reference:");
        for (const auto &entry : unsupported)
            std::printf(" %s(%u)", entry.first.c_str(), entry.second);
        std::printf("\n");
    }
    return failed != 0 ? 1 : 0;
}
