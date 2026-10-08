// Differential test of the EE recompiler's VU0 macro-mode (COP2) translations against the VU core.
// Every distinct COP2 instruction of the game (translated by vu0_macro_emit into
// recomp/diagnostics/vu0-macro/ops.inc) runs on random register values, and the result is compared
// with the reference VU interpreter executing the same instruction as a one-pair microprogram.
// The VU core rounds toward zero and the macro code uses host rounding, so floats may differ by a
// couple of ulps; anything larger, or any integer difference, is reported.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_vu1.h"
#include "runtime/gs/gs_frontend.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include PS2X_VU0_MACRO_OPS

namespace
{
    GS &dummyGs()
    {
        alignas(16) static unsigned char storage[16];
        return *reinterpret_cast<GS *>(storage);
    }
    uint32_t bitsOf(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        return bits;
    }
    bool closeEnough(float a, float b, bool integerPattern)
    {
        const uint32_t ua = bitsOf(a), ub = bitsOf(b);
        if (ua == ub)
            return true;
        if (integerPattern)
            return false;
        if ((ua ^ ub) & 0x80000000u)
            return (ua & 0x7FFFFFFFu) == 0u && (ub & 0x7FFFFFFFu) == 0u;
        const uint32_t diff = ua > ub ? ua - ub : ub - ua;
        return diff <= 4u;
    }
}

int main()
{
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> magnitude(-2.0f, 2.0f);
    VU1Interpreter vu(VU1Interpreter::Unit::VU0);
    vu.setEngine(VU1Interpreter::Engine::Reference);
    std::vector<uint8_t> code(0x1000), data(0x1000);
    std::map<std::string, unsigned> failuresByMnemonic;
    unsigned tested = 0, failed = 0;
    uint64_t codeKey = 1;

    for (const MacroOp &op : kMacroOps)
    {
        const uint32_t word = op.word;
        const uint32_t funct = word & 0x3Fu;
        const bool special2 = funct >= 0x3Cu;
        const uint32_t index = special2 ? (((word >> 6) & 0x1Fu) << 2) | (word & 3u) : funct;
        if (special2 && (index == 0x40u || index == 0x41u || index == 0x42u || index == 0x43u))
            continue; // R register ops: no counterpart in the EE context checked here
        if (special2 && ((index >= 0x34u && index <= 0x37u) || index == 0x3Eu || index == 0x3Fu))
            continue; // VU0 memory access
        const bool lower = special2 ? index >= 0x30u : funct >= 0x30u;
        const bool integerResult = special2 && index >= 0x14u && index <= 0x17u; // FTOI
        const uint32_t body = word & 0x01FFFFFFu;
        const uint32_t pair0[2] = {lower ? (0x80000000u | body) : 0x8000033Cu, lower ? 0x000002FFu : body};
        const uint32_t pair1[2] = {0x8000033Cu, 0x400002FFu};
        const uint32_t nopPair[2] = {0x8000033Cu, 0x000002FFu};
        for (size_t offset = 0; offset < code.size(); offset += 8)
            std::memcpy(&code[offset], nopPair, 8);
        std::memcpy(&code[0], pair0, 8);
        std::memcpy(&code[8], pair1, 8);

        bool opFailed = false;
        std::string detail;
        for (int round = 0; round < 24 && !opFailed; ++round)
        {
            R5900Context ctx{};
            VU1State state{};
            for (int reg = 0; reg < 32; ++reg)
            {
                float lanes[4];
                for (float &lane : lanes)
                {
                    // Mix of small values, larger ones and whole numbers (for FTOI and comparisons).
                    const float base = magnitude(rng);
                    const int kind = static_cast<int>(rng() % 4u);
                    lane = kind == 0 ? base : (kind == 1 ? base * 100.0f : (kind == 2 ? std::floor(base * 50.0f) : base * 0.01f + (base < 0 ? -0.01f : 0.01f)));
                }
                if (reg == 0)
                {
                    lanes[0] = lanes[1] = lanes[2] = 0.0f;
                    lanes[3] = 1.0f;
                }
                ctx.vu0_vf[reg] = _mm_loadu_ps(lanes);
                std::memcpy(state.vf[reg], lanes, sizeof(lanes));
            }
            float acc[4] = {magnitude(rng), magnitude(rng) * 10.0f, magnitude(rng), magnitude(rng)};
            ctx.vu0_acc = _mm_loadu_ps(acc);
            std::memcpy(state.acc, acc, sizeof(acc));
            ctx.vu0_q = state.q = magnitude(rng) + 3.0f;
            ctx.vu0_i = state.i = magnitude(rng) * 5.0f;
            for (int reg = 1; reg < 16; ++reg)
            {
                const uint16_t value = static_cast<uint16_t>(rng());
                ctx.vi[reg] = value;
                state.vi[reg] = static_cast<int32_t>(value);
            }
            state.vf[0][3] = 1.0f;

            op.run(nullptr, &ctx, nullptr);

            vu.state() = state;
            vu.prepareReplay(state);
            vu.setExternalCodeKey(++codeKey);
            vu.execute(code.data(), static_cast<uint32_t>(code.size()), data.data(), static_cast<uint32_t>(data.size()),
                       dummyGs(), nullptr, 0u, 0u, 0u, 4096u);
            const VU1State &ref = vu.state();

            char text[256];
            for (int reg = 1; reg < 32 && !opFailed; ++reg)
            {
                float lanes[4];
                _mm_storeu_ps(lanes, ctx.vu0_vf[reg]);
                for (int lane = 0; lane < 4; ++lane)
                    if (!closeEnough(lanes[lane], ref.vf[reg][lane], integerResult))
                    {
                        std::snprintf(text, sizeof(text), "vf%d.%c macro=%g (%08x) vu=%g (%08x)", reg, "xyzw"[lane], lanes[lane],
                                      bitsOf(lanes[lane]), ref.vf[reg][lane], bitsOf(ref.vf[reg][lane]));
                        detail = text;
                        opFailed = true;
                        break;
                    }
            }
            float accNow[4];
            _mm_storeu_ps(accNow, ctx.vu0_acc);
            for (int lane = 0; lane < 4 && !opFailed; ++lane)
                if (!closeEnough(accNow[lane], ref.acc[lane], false))
                {
                    std::snprintf(text, sizeof(text), "acc.%c macro=%g vu=%g", "xyzw"[lane], accNow[lane], ref.acc[lane]);
                    detail = text;
                    opFailed = true;
                }
            if (!opFailed && !closeEnough(ctx.vu0_q, ref.q, false))
            {
                std::snprintf(text, sizeof(text), "Q macro=%g vu=%g", ctx.vu0_q, ref.q);
                detail = text;
                opFailed = true;
            }
            for (int reg = 1; reg < 16 && !opFailed; ++reg)
                if (ctx.vi[reg] != static_cast<uint16_t>(ref.vi[reg]))
                {
                    std::snprintf(text, sizeof(text), "vi%d macro=%04x vu=%04x", reg, ctx.vi[reg], static_cast<uint16_t>(ref.vi[reg]));
                    detail = text;
                    opFailed = true;
                }
        }
        ++tested;
        if (opFailed)
        {
            ++failed;
            std::string mnemonic = op.text;
            mnemonic = mnemonic.substr(0, mnemonic.find(' '));
            if (failuresByMnemonic[mnemonic]++ < 3u)
                std::printf("MISMATCH %08x %-40s %s\n", word, op.text, detail.c_str());
        }
    }
    std::printf("%u instructions tested, %u with mismatches\n", tested, failed);
    for (const auto &[mnemonic, count] : failuresByMnemonic)
        std::printf("  %-12s %u\n", mnemonic.c_str(), count);
    return failed ? 1 : 0;
}
