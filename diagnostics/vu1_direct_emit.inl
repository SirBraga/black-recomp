// Direct emission of straight runs of VU1 instruction pairs (included by vu1_recompile.cpp).
//
// A "block" is a straight run of pairs made of the instructions translated here: add/sub/mul-family FMAC
// ops, MAX/MINI/ABS/ITOF/FTOI/CLIP, loads and stores, integer ops, MOVE/MR32, MTIR/MFIR/MFP, DIV/SQRT/RSQRT,
// the flag readers, XTOP/XITOP/XGKICK and, as the last two pairs, a conditional branch with its delay slot.
// Inside a block the VF/VI/ACC registers live in C++ locals (the compiler keeps them in machine
// registers), the arithmetic is plain NEON under flush-to-zero (see VU1Interpreter::execUpperInline) and
// the cycle counter, program counter and pair counter are written once at the end. A pair that may
// stall (register hazard, FDIV/EFU busy, XGKICK) tests for it and, when it does have to wait, lets the
// runtime advance the cycles; the block's cycle base `c` moves accordingly. Everything else still goes
// through VU1Interpreter::stepPair(), and so does a block whose entry conditions do not hold (the
// function returns NotHandled and the interpreted fast engine runs those pairs).
//
// The semantics are the interpreter's, instruction by instruction; ps2x_vu1_trace_replay checks them.
namespace direct
{
    using Facts = VU1Interpreter::PairFacts;

    constexpr uint32_t kAcc = 32u; // index of ACC in the per-register tables
    constexpr uint8_t kFmacAdd = 0u, kFmacSub = 1u, kFmacMul = 2u; // VU1Interpreter::FmacAdd/Sub/Mul

    struct Upper
    {
        enum Kind { None, Nop, Fmac, MaxMin, Itof, Ftoi, Abs, Clip } kind = None;
        enum Rhs { Vt, Bc, Q, I } rhs = Vt;
        uint8_t fmac = 0;      // kFmacAdd/Sub/Mul of the last stage
        bool product = false;  // last stage adds/subtracts a product to/from ACC (MADD/MSUB/OPMSUB)
        bool cross = false;    // OPMULA / OPMSUB operand permutation
        bool toAcc = false;
        bool isMax = false;
        uint32_t shift = 0;    // ITOF/FTOI fraction bits
        uint32_t dest = 0, fs = 0, ft = 0, target = 0, bc = 0;
    };

    inline Upper decodeUpper(uint32_t instr)
    {
        Upper u;
        const uint32_t op = instr & 0x3Fu;
        u.dest = (instr >> 21) & 0xFu;
        u.ft = (instr >> 16) & 0x1Fu;
        u.fs = (instr >> 11) & 0x1Fu;
        u.target = (instr >> 6) & 0x1Fu;
        const auto arithmetic = [&u](uint32_t family, Upper::Rhs rhs, uint32_t bc)
        {
            // family: 0 add, 1 sub, 2 madd, 3 msub, 4 mul
            u.kind = Upper::Fmac;
            u.rhs = rhs;
            u.bc = bc;
            u.product = family == 2u || family == 3u;
            u.fmac = family == 4u ? kFmacMul : ((family == 0u || family == 2u) ? kFmacAdd : kFmacSub);
        };
        const auto general = [&](uint32_t code) // codes shared by the normal and the ACC groups
        {
            if (code <= 0x0Fu) { arithmetic(code >> 2, Upper::Bc, code & 3u); return true; }
            if (code >= 0x18u && code <= 0x1Bu) { arithmetic(4u, Upper::Bc, code & 3u); return true; }
            switch (code)
            {
            case 0x1Cu: arithmetic(4u, Upper::Q, 0u); return true;
            case 0x1Eu: arithmetic(4u, Upper::I, 0u); return true;
            case 0x20u: arithmetic(0u, Upper::Q, 0u); return true;
            case 0x21u: arithmetic(2u, Upper::Q, 0u); return true;
            case 0x22u: arithmetic(0u, Upper::I, 0u); return true;
            case 0x23u: arithmetic(2u, Upper::I, 0u); return true;
            case 0x24u: arithmetic(1u, Upper::Q, 0u); return true;
            case 0x25u: arithmetic(3u, Upper::Q, 0u); return true;
            case 0x26u: arithmetic(1u, Upper::I, 0u); return true;
            case 0x27u: arithmetic(3u, Upper::I, 0u); return true;
            case 0x28u: arithmetic(0u, Upper::Vt, 0u); return true;
            case 0x29u: arithmetic(2u, Upper::Vt, 0u); return true;
            case 0x2Au: arithmetic(4u, Upper::Vt, 0u); return true;
            case 0x2Cu: arithmetic(1u, Upper::Vt, 0u); return true;
            case 0x2Du: arithmetic(3u, Upper::Vt, 0u); return true;
            default: return false;
            }
        };
        if (op < 0x3Cu)
        {
            if (general(op))
                return u;
            if (op >= 0x10u && op <= 0x17u) { u.kind = Upper::MaxMin; u.rhs = Upper::Bc; u.bc = op & 3u; u.isMax = op < 0x14u; return u; }
            switch (op)
            {
            case 0x1Du: u.kind = Upper::MaxMin; u.rhs = Upper::I; u.isMax = true; return u;
            case 0x1Fu: u.kind = Upper::MaxMin; u.rhs = Upper::I; u.isMax = false; return u;
            case 0x2Bu: u.kind = Upper::MaxMin; u.rhs = Upper::Vt; u.isMax = true; return u;
            case 0x2Fu: u.kind = Upper::MaxMin; u.rhs = Upper::Vt; u.isMax = false; return u;
            case 0x2Eu: // OPMSUB: ACC - cross product
                u.kind = Upper::Fmac; u.rhs = Upper::Vt; u.product = true; u.cross = true; u.fmac = kFmacSub; return u;
            default: return u; // not handled
            }
        }
        const uint32_t special = (instr & 0x3u) | ((instr >> 4) & 0x7Cu);
        if (special == 0x2Fu || special == 0x30u) { u.kind = Upper::Nop; return u; }
        if (special >= 0x10u && special <= 0x13u) { u.kind = Upper::Itof; u.target = u.ft; u.shift = special == 0x10u ? 0u : (special == 0x11u ? 4u : (special == 0x12u ? 12u : 15u)); return u; }
        if (special >= 0x14u && special <= 0x17u) { u.kind = Upper::Ftoi; u.target = u.ft; u.shift = special == 0x14u ? 0u : (special == 0x15u ? 4u : (special == 0x16u ? 12u : 15u)); return u; }
        if (special == 0x1Du) { u.kind = Upper::Abs; u.target = u.ft; return u; }
        if (special == 0x1Fu) { u.kind = Upper::Clip; return u; }
        if (special == 0x2Eu) // OPMULA
        {
            u.kind = Upper::Fmac; u.rhs = Upper::Vt; u.cross = true; u.fmac = kFmacMul; u.toAcc = true; return u;
        }
        if (special != 0x1Fu && special != 0x2Bu && general(special))
            u.toAcc = true;
        return u;
    }

    struct Lower
    {
        enum Kind { None, Nop, Immediate, Lq, Sq, Lqi, Sqi, Lqd, Sqd, Iaddiu, Isubiu, Iadd, Isub, Iaddi, Iand, Ior,
                    Move, Mr32, Mtir, Mfir, Fmeq, Fmand, Fmor, Branch,
                    Ilw, Isw, Ilwr, Iswr, Fceq, Fcand, Fcor, Fcget, Div, Sqrt, Rsqrt, Waitq, Mfp, Xtop, Xitop, Xgkick } kind = None;
        uint32_t dest = 0, ft = 0, fs = 0, it = 0, is = 0, id = 0, op = 0;
        int32_t imm = 0;
    };

    inline Lower decodeLower(uint32_t instr, bool iBit)
    {
        Lower l;
        if (iBit) { l.kind = Lower::Immediate; return l; }
        if (instr == 0u || instr == 0x8000033Cu) { l.kind = Lower::Nop; return l; }
        l.op = (instr >> 25) & 0x7Fu;
        l.dest = (instr >> 21) & 0xFu;
        l.ft = (instr >> 16) & 0x1Fu;
        l.fs = (instr >> 11) & 0x1Fu;
        l.it = (instr >> 16) & 0xFu;
        l.is = (instr >> 11) & 0xFu;
        l.id = (instr >> 6) & 0xFu;
        const int32_t imm11 = static_cast<int32_t>(static_cast<int32_t>(instr << 21) >> 21);
        switch (l.op)
        {
        case 0x00u: l.kind = Lower::Lq; l.imm = imm11; return l;
        case 0x01u: l.kind = Lower::Sq; l.imm = imm11; return l;
        case 0x08u: l.kind = Lower::Iaddiu; l.imm = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int16_t>(instr & 0x7FFu)) | ((instr >> 10) & 0x7800u)); return l;
        case 0x09u: l.kind = Lower::Isubiu; l.imm = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int16_t>(instr & 0x7FFu)) | ((instr >> 10) & 0x7800u)); return l;
        case 0x04u: l.kind = Lower::Ilw; l.imm = imm11; return l;
        case 0x05u: l.kind = Lower::Isw; l.imm = imm11; return l;
        case 0x10u: l.kind = Lower::Fceq; return l;
        case 0x12u: l.kind = Lower::Fcand; return l;
        case 0x13u: l.kind = Lower::Fcor; return l;
        case 0x1Cu: l.kind = Lower::Fcget; return l;
        case 0x18u: l.kind = Lower::Fmeq; return l;
        case 0x1Au: l.kind = Lower::Fmand; return l;
        case 0x1Bu: l.kind = Lower::Fmor; return l;
        case 0x20u: case 0x28u: case 0x29u: case 0x2Cu: case 0x2Du: case 0x2Eu: case 0x2Fu:
            l.kind = Lower::Branch; l.imm = imm11; return l;
        case 0x40u:
        {
            const uint32_t funct = instr & 0x3Fu;
            switch (funct)
            {
            case 0x30u: l.kind = Lower::Iadd; return l;
            case 0x31u: l.kind = Lower::Isub; return l;
            case 0x32u: l.kind = Lower::Iaddi; l.imm = static_cast<int32_t>(static_cast<int32_t>(((instr >> 6) & 0x1Fu) << 27) >> 27); return l;
            case 0x34u: l.kind = Lower::Iand; return l;
            case 0x35u: l.kind = Lower::Ior; return l;
            default: break;
            }
            if (funct < 0x3Cu)
                return l;
            switch ((instr & 0x3u) | ((instr >> 4) & 0x7Cu))
            {
            case 0x30u: l.kind = Lower::Move; return l;
            case 0x31u: l.kind = Lower::Mr32; return l;
            case 0x34u: l.kind = Lower::Lqi; return l;
            case 0x35u: l.kind = Lower::Sqi; return l;
            case 0x36u: l.kind = Lower::Lqd; return l;
            case 0x37u: l.kind = Lower::Sqd; return l;
            case 0x3Cu: l.kind = Lower::Mtir; return l;
            case 0x3Du: l.kind = Lower::Mfir; return l;
            case 0x38u: l.kind = Lower::Div; return l;
            case 0x39u: l.kind = Lower::Sqrt; return l;
            case 0x3Au: l.kind = Lower::Rsqrt; return l;
            case 0x3Bu: l.kind = Lower::Waitq; return l;
            case 0x3Eu: l.kind = Lower::Ilwr; return l;
            case 0x3Fu: l.kind = Lower::Iswr; return l;
            case 0x64u: l.kind = Lower::Mfp; return l;
            case 0x68u: l.kind = Lower::Xtop; return l;
            case 0x69u: l.kind = Lower::Xitop; return l;
            case 0x6Cu: l.kind = Lower::Xgkick; return l;
            default: return l;
            }
        }
        default: return l;
        }
    }

    // A pair the block emitter can translate (branches are judged with their delay slot by the caller).
    inline bool pairIsDirect(const Facts &f, bool mayEnd)
    {
        constexpr uint32_t blocking = VU1Interpreter::PairReserved | VU1Interpreter::PairEndBits | VU1Interpreter::PairEBit |
                                      VU1Interpreter::PairDBit | VU1Interpreter::PairTBit | VU1Interpreter::PairEfu |
                                      VU1Interpreter::PairIndirect;
        if ((f.bits & blocking) != 0u || mayEnd)
            return false;
        if (decodeUpper(f.upper).kind == Upper::None)
            return false;
        return decodeLower(f.lower, (f.bits & VU1Interpreter::PairIBit) != 0u).kind != Lower::None;
    }

    inline bool pairIsBranch(const Facts &f)
    {
        return decodeLower(f.lower, (f.bits & VU1Interpreter::PairIBit) != 0u).kind == Lower::Branch;
    }

    // Helpers the generated blocks use (members of struct VuRecompiled).
    inline const char *runtimeHelpers()
    {
        return R"(#if defined(__aarch64__)
    using V = float32x4_t;
    // Operand of an add/sub/mul: infinities and NaNs become the largest finite value of their sign
    // (flush-to-zero takes care of denormals).
    static inline __attribute__((always_inline)) V D_clamp(V v)
    {
        const uint32x4_t bits = vreinterpretq_u32_f32(v);
        const uint32x4_t sign = vandq_u32(bits, vdupq_n_u32(0x80000000u));
        return vreinterpretq_f32_u32(vorrq_u32(vminq_u32(vbicq_u32(bits, vdupq_n_u32(0x80000000u)), vdupq_n_u32(0x7F7FFFFFu)), sign));
    }
    // Operand of the ops that copy bits around: denormals become zero as well.
    static inline __attribute__((always_inline)) V D_norm(V v)
    {
        const uint32x4_t bits = vreinterpretq_u32_f32(v);
        const uint32x4_t exponent = vandq_u32(bits, vdupq_n_u32(0x7F800000u));
        const uint32x4_t sign = vandq_u32(bits, vdupq_n_u32(0x80000000u));
        uint32x4_t out = vbslq_u32(vceqq_u32(exponent, vdupq_n_u32(0u)), sign, bits);
        out = vbslq_u32(vceqq_u32(exponent, vdupq_n_u32(0x7F800000u)), vorrq_u32(sign, vdupq_n_u32(0x7F7FFFFFu)), out);
        return vreinterpretq_f32_u32(out);
    }
    static inline __attribute__((always_inline)) uint32x4_t D_mask(uint32_t dest)
    {
        const uint32_t lanes[4] = {dest & 8u ? ~0u : 0u, dest & 4u ? ~0u : 0u, dest & 2u ? ~0u : 0u, dest & 1u ? ~0u : 0u};
        return vld1q_u32(lanes);
    }
    static inline __attribute__((always_inline)) V D_merge(uint32_t dest, V value, V old) { return vbslq_f32(D_mask(dest), value, old); }
    static inline __attribute__((always_inline)) V D_vf0() { const float lanes[4] = {0.0f, 0.0f, 0.0f, 1.0f}; return vld1q_f32(lanes); }
    static inline __attribute__((always_inline)) V D_zeroW(V v) { return vsetq_lane_f32(0.0f, v, 3); }
    static inline __attribute__((always_inline)) V D_scalar(float value) { return vdupq_n_f32(vuNormalizeOperand(value)); }
    static inline __attribute__((always_inline)) V D_load(const uint8_t *data, uint32_t address) { return vreinterpretq_f32_u8(vld1q_u8(data + address)); }
    static inline __attribute__((always_inline)) void D_store(uint8_t *data, uint32_t address, V value) { vst1q_u8(data + address, vreinterpretq_u8_f32(value)); }
    static inline __attribute__((always_inline)) void D_push(VU1Interpreter &vu, V x, V y, uint8_t kind, uint8_t dest, uint64_t cycle)
    {
        VU1Interpreter::LazyFlags &record = vu.m_lazy[vu.m_lazyCount++ & 7u];
        vst1q_f32(record.x, x);
        vst1q_f32(record.y, y);
        record.readyCycle = cycle + VU1Interpreter::kFmacLatency;
        record.dest = dest;
        record.kind = kind;
        record.noStatus = false;
    }
    static __attribute__((noinline)) uint64_t D_commitSlow(VU1Interpreter &vu, uint64_t cycle)
    {
        vu.m_cycle = cycle;
        vu.commitReadyPipelines();
        return vu.m_nextCommitCycle;
    }
    // Something in a pipeline is due at `cycle`. Nearly always it is just the result of a DIV/SQRT/RSQRT:
    // that case is handled here, with no call (a call inside a block makes the compiler save and reload
    // every vector register the block keeps live).
    static inline __attribute__((always_inline)) uint64_t D_commit(VU1Interpreter &vu, uint64_t cycle)
    {
        if (__builtin_expect(vu.m_fdiv.valid && vu.m_fdiv.readyCycle <= cycle &&
                             (vu.m_flagPending | vu.m_storePending | vu.m_vfPending | vu.m_viPending | vu.m_accPending) == 0u &&
                             !vu.m_efu[0].valid && !vu.m_efu[1].valid, 1))
        {
            vu.m_state.q = vu.m_fdiv.value;
            const uint32_t currentDi = vu.m_fdiv.statusDi & 0x30u;
            vu.m_state.status = (vu.m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
            vu.m_fdiv.valid = false;
            vu.m_nextCommitCycle = std::numeric_limits<uint64_t>::max();
            return vu.m_nextCommitCycle;
        }
        return D_commitSlow(vu, cycle);
    }
    static __attribute__((noinline)) uint32_t D_macSlow(VU1Interpreter &vu, uint64_t cycle)
    {
        vu.m_cycle = cycle;
        vu.resolveLazyFlagsSlow();
        return vu.m_state.mac;
    }
    // MAC flags as a reader at `cycle` sees them (see VU1Interpreter::resolveLazyFlagsSlow, which this
    // repeats for the common case): the newest record out of the pipeline, when its result has no lane that
    // could have underflowed, gives sign and zero flags straight from the NEON result.
    static inline __attribute__((always_inline)) uint32_t D_mac(VU1Interpreter &vu, uint64_t cycle)
    {
        const uint32_t count = vu.m_lazyCount;
        uint32_t applied = vu.m_lazyApplied;
        if (count == applied)
            return vu.m_state.mac;
        // Only the last eight records exist (long stretches of code write results nobody reads).
        if (count - applied > 8u)
            applied = count - 8u;
        uint32_t end = count;
        while (end != applied && vu.m_lazy[(end - 1u) & 7u].readyCycle > cycle)
            --end;
        if (end == applied)
        {
            vu.m_lazyApplied = applied;
            return vu.m_state.mac;
        }
        const VU1Interpreter::LazyFlags &record = vu.m_lazy[(end - 1u) & 7u];
        if (__builtin_expect(record.noStatus, 0))
            return D_macSlow(vu, cycle);
        const V x = vld1q_f32(record.x), y = vld1q_f32(record.y);
        const uint32_t weightValues[4] = {8u, 4u, 2u, 1u};
        const uint32x4_t weights = vld1q_u32(weightValues);
        const uint32x4_t dest = vtstq_u32(vdupq_n_u32(record.dest), weights);
        uint32x4_t bits, exactZero;
        if (record.kind == VU1Interpreter::FmacMul)
        {
            bits = vreinterpretq_u32_f32(vmulq_f32(x, y));
            exactZero = vorrq_u32(vceqzq_f32(x), vceqzq_f32(y));
        }
        else if (record.kind == VU1Interpreter::FmacSub)
        {
            bits = vreinterpretq_u32_f32(vsubq_f32(x, y));
            exactZero = vceqq_f32(x, y);
        }
        else
        {
            bits = vreinterpretq_u32_f32(vaddq_f32(x, y));
            exactZero = vceqq_f32(x, vnegq_f32(y));
        }
        const uint32x4_t zero = vandq_u32(vceqzq_u32(vandq_u32(bits, vdupq_n_u32(0x7F800000u))), dest);
        // A zero that is not an exact zero is an underflow (or a product below every denormal): exact path.
        if (__builtin_expect(vmaxvq_u32(vbicq_u32(zero, exactZero)) != 0u, 0))
            return D_macSlow(vu, cycle);
        const uint32_t zeroBits = vaddvq_u32(vandq_u32(zero, weights));
        const uint32_t signBits = vaddvq_u32(vandq_u32(vandq_u32(vcltzq_s32(vreinterpretq_s32_u32(bits)), dest), weights));
        const uint32_t mac = zeroBits | (signBits << 4);
        const uint32_t current = (zeroBits != 0u ? 1u : 0u) | (signBits != 0u ? 2u : 0u);
        vu.m_state.mac = mac;
        vu.m_state.status = (vu.m_state.status & 0xFF0u) | current | (current << 6);
        vu.m_lazyApplied = end;
        return mac;
    }
    // Waits (cycle by cycle, pipelines included) until `target`; false when the budget ran out first.
    static __attribute__((noinline)) bool D_wait(VU1Interpreter &vu, uint64_t cycle, uint64_t target, uint64_t budgetEnd)
    {
        vu.m_cycle = cycle;
        vu.advanceTo(target >= budgetEnd ? budgetEnd : target);
        return vu.m_cycle < budgetEnd;
    }
    static __attribute__((noinline)) bool D_hazards(VU1Interpreter &vu, uint64_t cycle, uint32_t pc, uint64_t budgetEnd)
    {
        vu.m_cycle = cycle;
        return vu.waitHazardsOfPair(pc, budgetEnd);
    }
    static __attribute__((noinline)) bool D_stall(VU1Interpreter &vu, uint64_t cycle, uint32_t bits, uint64_t budgetEnd)
    {
        vu.m_cycle = cycle;
        return vu.waitPairStall(bits, budgetEnd);
    }
    static inline __attribute__((always_inline)) void D_queueQ(VU1Interpreter &vu, uint64_t cycle, float value, uint32_t latency, uint32_t statusDi)
    {
        vu.m_fdiv.valid = true;
        vu.m_fdiv.readyCycle = cycle + latency;
        vu.m_nextCommitCycle = std::min(vu.m_nextCommitCycle, vu.m_fdiv.readyCycle);
        vu.m_fdiv.value = vuNormalizeOperand(value);
        vu.m_fdiv.statusDi = statusDi & 0x30u;
    }
    static inline __attribute__((always_inline)) void D_div(VU1Interpreter &vu, uint64_t cycle, float numerator, float denominator)
    {
        const float num = vuNormalizeOperand(numerator), den = vuNormalizeOperand(denominator);
        uint32_t statusDi = 0u;
        float result = 0.0f;
        if (den == 0.0f)
        {
            statusDi = num == 0.0f ? 0x10u : 0x20u;
            result = std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
        }
        else
            result = num / den;
        D_queueQ(vu, cycle, result, 7u, statusDi);
    }
    static __attribute__((noinline)) void D_sqrt(VU1Interpreter &vu, uint64_t cycle, float operand)
    {
        vu.m_cycle = cycle;
        const float val = vuNormalizeOperand(operand);
        vu.queueQ(std::sqrt(std::fabs(val)), 7u, val < 0.0f ? 0x10u : 0u);
    }
    static __attribute__((noinline)) void D_rsqrt(VU1Interpreter &vu, uint64_t cycle, float numerator, float operand)
    {
        vu.m_cycle = cycle;
        const float num = vuNormalizeOperand(numerator), radicand = vuNormalizeOperand(operand);
        const float den = std::sqrt(std::fabs(radicand));
        uint32_t statusDi = radicand < 0.0f ? 0x10u : 0u;
        float result = 0.0f;
        if (den != 0.0f)
            result = num / den;
        else
        {
            statusDi = num == 0.0f ? 0x10u : 0x20u;
            result = std::signbit(num) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
        }
        uint32_t ignoredFlags = 0u;
        result = vu.normalizeResult(result, ignoredFlags);
        vu.queueQ(result, 13u, statusDi);
    }
    static inline __attribute__((always_inline)) void D_clip(VU1Interpreter &vu, uint64_t cycle, V value, V limitSource)
    {
        // |x|,|y|,|z| against |w| as integers (a zero exponent in w counts as the largest denormal), like
        // the interpreter's CLIP; blocks only run with the clip flags kept on demand.
        const uint32_t wBits = vgetq_lane_u32(vreinterpretq_u32_f32(limitSource), 3);
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        const int32x4_t bits = vreinterpretq_s32_f32(value);
        const uint32x4_t plus = vcgtq_s32(bits, vdupq_n_s32(limit));
        const uint32x4_t minus = vcgtq_s32(veorq_s32(bits, vdupq_n_s32(INT32_MIN)), vdupq_n_s32(limit));
        const uint32_t plusWeights[4] = {0x01u, 0x04u, 0x10u, 0u}, minusWeights[4] = {0x02u, 0x08u, 0x20u, 0u};
        const uint32_t flags = vaddvq_u32(vorrq_u32(vandq_u32(plus, vld1q_u32(plusWeights)), vandq_u32(minus, vld1q_u32(minusWeights))));
        vu.m_workingClip = ((vu.m_workingClip << 6) | flags) & 0xFFFFFFu;
        vu.m_lazyClip[vu.m_lazyClipCount++ & 7u] = {cycle + VU1Interpreter::kFmacLatency, vu.m_workingClip};
    }
    // Clip flags as a reader at `cycle` sees them (VU1Interpreter::resolveLazyClipSlow, in line: no call).
    static inline __attribute__((always_inline)) uint32_t D_clipFlags(VU1Interpreter &vu, uint64_t cycle)
    {
        const uint32_t count = vu.m_lazyClipCount;
        uint32_t applied = vu.m_lazyClipApplied;
        if (count != applied)
        {
            if (count - applied > 8u)
                applied = count - 8u;
            uint32_t end = count;
            while (end != applied && vu.m_lazyClip[(end - 1u) & 7u].readyCycle > cycle)
                --end;
            if (end != applied)
                vu.m_state.clip = vu.m_lazyClip[(end - 1u) & 7u].clip;
            vu.m_lazyClipApplied = end;
        }
        return vu.m_state.clip;
    }
    static __attribute__((noinline)) void D_xgkick(VU1Interpreter &vu, uint64_t cycle, int32_t address)
    {
        vu.m_cycle = cycle;
        vu.startXgkick(static_cast<uint32_t>(static_cast<uint16_t>(address)));
    }
    static inline __attribute__((always_inline)) void D_sync(VU1Interpreter &vu, uint64_t cycle)
    {
        if (__builtin_expect(vu.m_xgkick.active, 0))
        {
            vu.m_cycle = cycle;
            vu.syncXgkickSlow();
        }
    }
#endif

)";
    }

    struct Emitter
    {
        std::string body;
        bool vfRead[33]{}, vfWritten[33]{}, viRead[16]{}, viWritten[16]{};
        uint8_t normal[33]{}; // lanes (bit = lane index) known to hold a finite, non-denormal float
        char text[512];
        uint32_t suppressReg = 0; // VF register the lower instruction must not write (see emitBlock)
        // Second pass of emitBlock(): registers the block never writes. Their clamped / normalized forms are
        // computed once, before the block's body (and before its loop, if it loops).
        const bool *invariant = nullptr;
        bool hoistClamp[33]{}, hoistNorm[33]{};

        static uint8_t lanesOf(uint32_t dest)
        {
            return static_cast<uint8_t>(((dest & 8u) ? 1u : 0u) | ((dest & 4u) ? 2u : 0u) | ((dest & 2u) ? 4u : 0u) | ((dest & 1u) ? 8u : 0u));
        }
        void add(const char *format, ...) __attribute__((format(printf, 2, 3)))
        {
            va_list args;
            va_start(args, format);
            std::vsnprintf(text, sizeof(text), format, args);
            va_end(args);
            body += text;
        }
        std::string vf(uint32_t reg)
        {
            if (reg == 0u)
                return "D_vf0()";
            vfRead[reg] = true;
            return reg == kAcc ? std::string("acc") : "f" + std::to_string(reg);
        }
        std::string vi(uint32_t reg)
        {
            if (reg == 0u)
                return "0";
            viRead[reg] = true;
            return "i" + std::to_string(reg);
        }
        // Register as an operand whose `lanes` must be finite (clampOnly) or fully normalized.
        std::string operand(uint32_t reg, uint8_t lanes, bool clampOnly)
        {
            const std::string value = vf(reg);
            if (reg == 0u || (normal[reg] & lanes) == lanes)
                return value;
            if (invariant && invariant[reg])
            {
                (clampOnly ? hoistClamp : hoistNorm)[reg] = true;
                return (clampOnly ? "k" : "n") + (reg == kAcc ? std::string("acc") : std::to_string(reg));
            }
            return std::string(clampOnly ? "D_clamp(" : "D_norm(") + value + ")";
        }
        void setVf(uint32_t reg, uint32_t dest, const std::string &value, uint8_t normalLanes)
        {
            // normalLanes: which of the written lanes are known normal afterwards
            if (reg == 0u || dest == 0u || reg == suppressReg)
                return;
            const std::string name = reg == kAcc ? std::string("acc") : "f" + std::to_string(reg);
            if (dest == 0xFu)
                add("                    %s = %s;\n", name.c_str(), value.c_str());
            else
            {
                vfRead[reg] = true;
                add("                    %s = D_merge(%uu, %s, %s);\n", name.c_str(), dest, value.c_str(), name.c_str());
            }
            vfWritten[reg] = true;
            const uint8_t lanes = lanesOf(dest);
            normal[reg] = static_cast<uint8_t>((normal[reg] & ~lanes) | (normalLanes & lanes));
        }
        void setVi(uint32_t reg, const std::string &value)
        {
            if (reg == 0u)
                return;
            add("                    i%u = %s;\n", reg, value.c_str());
            viWritten[reg] = true;
        }

        // Emits the upper instruction's computation into `u` (and flag record); returns what to commit.
        struct Pending { bool write = false; uint32_t reg = 0, dest = 0; uint8_t normalLanes = 0; };
        Pending upper(const Upper &u, bool flagsDead, uint32_t k)
        {
            Pending pending;
            const uint8_t lanes = lanesOf(u.dest);
            auto rhsOperand = [&](bool clampOnly) -> std::string
            {
                switch (u.rhs)
                {
                case Upper::Q: return "D_scalar(vu.m_state.q)";
                case Upper::I: return "D_scalar(vu.m_state.i)";
                case Upper::Bc:
                {
                    const uint8_t lane = static_cast<uint8_t>(1u << u.bc);
                    if (u.ft == 0u)
                        return u.bc == 3u ? "vdupq_n_f32(1.0f)" : "vdupq_n_f32(0.0f)";
                    const std::string source = "vdupq_laneq_f32(" + vf(u.ft) + ", " + std::to_string(u.bc) + ")";
                    if ((normal[u.ft] & lane) == lane)
                        return source;
                    if (invariant && invariant[u.ft])
                    {
                        (clampOnly ? hoistClamp : hoistNorm)[u.ft] = true;
                        return "vdupq_laneq_f32(" + std::string(clampOnly ? "k" : "n") + std::to_string(u.ft) + ", " + std::to_string(u.bc) + ")";
                    }
                    return std::string(clampOnly ? "D_clamp(" : "D_norm(") + source + ")";
                }
                default: return operand(u.ft, lanes, clampOnly);
                }
            };
            switch (u.kind)
            {
            case Upper::Nop:
                return pending;
            case Upper::Fmac:
            {
                if (u.dest == 0u)
                    return pending; // no lane written, no flags
                std::string x, y;
                if (u.cross)
                {
                    const std::string s = operand(u.fs, 0x7u, true), t = operand(u.ft, 0x7u, true);
                    add("                    const V cs = D_zeroW(__builtin_shufflevector(%s, %s, 1, 2, 0, 3));\n", s.c_str(), s.c_str());
                    add("                    const V ct = D_zeroW(__builtin_shufflevector(%s, %s, 2, 0, 1, 3));\n", t.c_str(), t.c_str());
                    if (u.product) // OPMSUB
                    {
                        add("                    const V x = D_zeroW(%s);\n", operand(kAcc, 0x7u, true).c_str());
                        add("                    const V y = vmulq_f32(cs, ct);\n");
                    }
                    else
                    {
                        add("                    const V x = cs;\n");
                        add("                    const V y = ct;\n");
                    }
                }
                else if (u.product)
                {
                    const std::string s = operand(u.fs, lanes, true), t = rhsOperand(true);
                    add("                    const V x = %s;\n", operand(kAcc, lanes, true).c_str());
                    add("                    const V y = vmulq_f32(%s, %s);\n", s.c_str(), t.c_str());
                }
                else
                {
                    add("                    const V x = %s;\n", operand(u.fs, lanes, true).c_str());
                    add("                    const V y = %s;\n", rhsOperand(true).c_str());
                }
                add("                    const V u = %s(x, y);\n",
                    u.fmac == kFmacAdd ? "vaddq_f32" : (u.fmac == kFmacSub ? "vsubq_f32" : "vmulq_f32"));
                if (!flagsDead)
                    add("                    D_push(vu, x, y, %uu, %uu, c + %uu);\n", unsigned(u.fmac), u.dest, k);
                pending.write = true;
                pending.reg = u.toAcc ? kAcc : u.target;
                pending.dest = u.dest;
                pending.normalLanes = 0xFu;
                return pending;
            }
            case Upper::MaxMin:
            {
                add("                    const V x = %s;\n", operand(u.fs, lanes, false).c_str());
                add("                    const V y = %s;\n", rhsOperand(false).c_str());
                add("                    const V u = vbslq_f32(%s(x, y), x, y);\n", u.isMax ? "vcgtq_f32" : "vcltq_f32");
                pending.write = true;
                pending.reg = u.target;
                pending.dest = u.dest;
                pending.normalLanes = 0xFu;
                return pending;
            }
            case Upper::Itof:
                if (u.shift == 0u)
                    add("                    const V u = vcvtq_f32_s32(vreinterpretq_s32_f32(%s));\n", vf(u.fs).c_str());
                else
                    add("                    const V u = vcvtq_n_f32_s32(vreinterpretq_s32_f32(%s), %u);\n", vf(u.fs).c_str(), u.shift);
                pending.write = true;
                pending.reg = u.target;
                pending.dest = u.dest;
                pending.normalLanes = 0xFu;
                return pending;
            case Upper::Ftoi:
                if (u.shift == 0u)
                    add("                    const V u = vreinterpretq_f32_s32(vcvtq_s32_f32(%s));\n", operand(u.fs, lanes, true).c_str());
                else
                    add("                    const V u = vreinterpretq_f32_s32(vcvtq_n_s32_f32(%s, %u));\n", operand(u.fs, lanes, true).c_str(), u.shift);
                pending.write = true;
                pending.reg = u.target;
                pending.dest = u.dest;
                pending.normalLanes = 0u;
                return pending;
            case Upper::Clip:
                add("                    D_clip(vu, c + %uu, %s, %s);\n", k, vf(u.fs).c_str(), vf(u.ft).c_str());
                return pending;
            case Upper::Abs:
                add("                    const V u = vabsq_f32(%s);\n", operand(u.fs, lanes, false).c_str());
                pending.write = true;
                pending.reg = u.target;
                pending.dest = u.dest;
                pending.normalLanes = 0xFu;
                return pending;
            default:
                return pending;
            }
        }

        static uint8_t rotateNormal(uint8_t lanes) // MR32: result lane n takes source lane n+1
        {
            return static_cast<uint8_t>(((lanes >> 1) | ((lanes & 1u) << 3)) & 0xFu);
        }

        // `branchVi(reg)` gives the expression a branch reads for a VI register.
        void lower(const Lower &l, uint32_t lowerWord, uint32_t pc, uint32_t k, const std::function<std::string(uint32_t)> &branchVi)
        {
            switch (l.kind)
            {
            case Lower::Nop:
                return;
            case Lower::Immediate:
                add("                    { const uint32_t bits = 0x%08xu; float value; std::memcpy(&value, &bits, 4); vu.m_state.i = vuNormalizeOperand(value); }\n", lowerWord);
                return;
            case Lower::Lq:
                add("                    const V m = D_load(vuData, (static_cast<uint32_t>(%s + (%d)) * 16u) & DM);\n", vi(l.is).c_str(), l.imm);
                setVf(l.ft, l.dest, "m", 0u);
                return;
            case Lower::Lqi:
                add("                    const V m = D_load(vuData, (static_cast<uint32_t>(static_cast<uint16_t>(%s)) * 16u) & DM);\n", vi(l.is).c_str());
                setVf(l.ft, l.dest, "m", 0u);
                setVi(l.is, "static_cast<int16_t>(" + vi(l.is) + " + 1)");
                return;
            case Lower::Lqd:
                setVi(l.is, "static_cast<int16_t>(" + vi(l.is) + " - 1)");
                add("                    const V m = D_load(vuData, (static_cast<uint32_t>(static_cast<uint16_t>(%s)) * 16u) & DM);\n", vi(l.is).c_str());
                setVf(l.ft, l.dest, "m", 0u);
                return;
            case Lower::Sq:
            case Lower::Sqi:
            case Lower::Sqd:
            {
                if (l.kind == Lower::Sqd)
                    setVi(l.it, "static_cast<int16_t>(" + vi(l.it) + " - 1)");
                if (l.kind == Lower::Sq)
                    add("                    const uint32_t a = (static_cast<uint32_t>(%s + (%d)) * 16u) & DM;\n", vi(l.it).c_str(), l.imm);
                else
                    add("                    const uint32_t a = (static_cast<uint32_t>(static_cast<uint16_t>(%s)) * 16u) & DM;\n", vi(l.it).c_str());
                add("                    D_sync(vu, c + %uu);\n", k);
                if (l.dest == 0xFu)
                    add("                    D_store(vuData, a, %s);\n", vf(l.fs).c_str());
                else if (l.dest != 0u)
                    add("                    D_store(vuData, a, D_merge(%uu, %s, D_load(vuData, a)));\n", l.dest, vf(l.fs).c_str());
                if (l.kind == Lower::Sqi)
                    setVi(l.it, "static_cast<int16_t>(" + vi(l.it) + " + 1)");
                return;
            }
            case Lower::Iaddiu:
                setVi(l.it, "static_cast<int16_t>(" + vi(l.is) + " + " + std::to_string(l.imm) + ")");
                return;
            case Lower::Isubiu:
                setVi(l.it, "static_cast<int16_t>(" + vi(l.is) + " - " + std::to_string(l.imm) + ")");
                return;
            case Lower::Iadd:
                setVi(l.id, "static_cast<int16_t>(" + vi(l.is) + " + " + vi(l.it) + ")");
                return;
            case Lower::Isub:
                setVi(l.id, "static_cast<int16_t>(" + vi(l.is) + " - " + vi(l.it) + ")");
                return;
            case Lower::Iaddi:
                setVi(l.it, "static_cast<int16_t>(" + vi(l.is) + " + (" + std::to_string(l.imm) + "))");
                return;
            case Lower::Iand:
                setVi(l.id, vi(l.is) + " & " + vi(l.it));
                return;
            case Lower::Ior:
                setVi(l.id, vi(l.is) + " | " + vi(l.it));
                return;
            case Lower::Move:
            {
                const uint8_t source = l.fs == 0u ? 0xFu : normal[l.fs];
                setVf(l.ft, l.dest, vf(l.fs), source);
                return;
            }
            case Lower::Mr32:
            {
                const uint8_t source = l.fs == 0u ? 0xFu : normal[l.fs];
                const std::string s = vf(l.fs);
                setVf(l.ft, l.dest, "vextq_f32(" + s + ", " + s + ", 1)", rotateNormal(source));
                return;
            }
            case Lower::Mtir:
                setVi(l.it, "static_cast<int16_t>(vgetq_lane_u32(vreinterpretq_u32_f32(" + vf(l.fs) + "), " + std::to_string(l.dest & 3u) + ") & 0xFFFFu)");
                return;
            case Lower::Mfir:
                setVf(l.ft, l.dest, "vreinterpretq_f32_s32(vdupq_n_s32(static_cast<int16_t>(" + vi(l.is) + " & 0xFFFF)))", 0u);
                return;
            case Lower::Fmeq:
                setVi(l.it, "((D_mac(vu, c + " + std::to_string(k) + "u) & 0xFFFFu) == static_cast<uint32_t>(static_cast<uint16_t>(" + vi(l.is) + "))) ? 1 : 0");
                return;
            case Lower::Fmand:
                setVi(l.it, "static_cast<int32_t>(D_mac(vu, c + " + std::to_string(k) + "u) & static_cast<uint32_t>(static_cast<uint16_t>(" + vi(l.is) + ")))");
                return;
            case Lower::Fmor:
                setVi(l.it, "static_cast<int32_t>(D_mac(vu, c + " + std::to_string(k) + "u) | static_cast<uint32_t>(static_cast<uint16_t>(" + vi(l.is) + ")))");
                return;
            case Lower::Ilw:
            case Lower::Ilwr:
            {
                const uint32_t component = (l.dest & 8u) ? 0u : ((l.dest & 4u) ? 1u : ((l.dest & 2u) ? 2u : 3u));
                if (l.kind == Lower::Ilw)
                    add("                    const uint32_t a = (static_cast<uint32_t>(%s + (%d)) * 16u) & DM;\n", vi(l.is).c_str(), l.imm);
                else
                    add("                    const uint32_t a = (static_cast<uint32_t>(static_cast<uint16_t>(%s)) * 16u) & DM;\n", vi(l.is).c_str());
                add("                    uint32_t w; std::memcpy(&w, vuData + a + %uu, 4);\n", component * 4u);
                setVi(l.it, "static_cast<int16_t>(w & 0xFFFFu)");
                return;
            }
            case Lower::Isw:
            case Lower::Iswr:
                if (l.kind == Lower::Isw)
                    add("                    const uint32_t a = (static_cast<uint32_t>(%s + (%d)) * 16u) & DM;\n", vi(l.is).c_str(), l.imm);
                else
                    add("                    const uint32_t a = (static_cast<uint32_t>(static_cast<uint16_t>(%s)) * 16u) & DM;\n", vi(l.is).c_str());
                add("                    D_sync(vu, c + %uu);\n", k);
                add("                    D_store(vuData, a, D_merge(%uu, vreinterpretq_f32_u32(vdupq_n_u32(static_cast<uint16_t>(%s & 0xFFFF))), D_load(vuData, a)));\n",
                    l.dest, vi(l.it).c_str());
                return;
            case Lower::Fceq:
                viWritten[1] = true;
                add("                    i1 = ((D_clipFlags(vu, c + %uu) & 0xFFFFFFu) == 0x%06xu) ? 1 : 0;\n", k, lowerWord & 0xFFFFFFu);
                return;
            case Lower::Fcand:
                viWritten[1] = true;
                add("                    i1 = ((D_clipFlags(vu, c + %uu) & 0x%06xu) != 0u) ? 1 : 0;\n", k, lowerWord & 0xFFFFFFu);
                return;
            case Lower::Fcor:
                viWritten[1] = true;
                add("                    i1 = ((D_clipFlags(vu, c + %uu) | 0x%06xu) == 0xFFFFFFu) ? 1 : 0;\n", k, lowerWord & 0xFFFFFFu);
                return;
            case Lower::Fcget:
                setVi(l.it, "static_cast<int32_t>(D_clipFlags(vu, c + " + std::to_string(k) + "u) & 0x0FFFu)");
                return;
            case Lower::Div:
                add("                    D_div(vu, c + %uu, vgetq_lane_f32(%s, %u), vgetq_lane_f32(%s, %u));\n                    nc = vu.m_nextCommitCycle;\n",
                    k, vf(l.fs).c_str(), (lowerWord >> 21) & 3u, vf(l.ft).c_str(), (lowerWord >> 23) & 3u);
                return;
            case Lower::Sqrt:
                add("                    D_sqrt(vu, c + %uu, vgetq_lane_f32(%s, %u));\n                    nc = vu.m_nextCommitCycle;\n",
                    k, vf(l.ft).c_str(), (lowerWord >> 23) & 3u);
                return;
            case Lower::Rsqrt:
                add("                    D_rsqrt(vu, c + %uu, vgetq_lane_f32(%s, %u), vgetq_lane_f32(%s, %u));\n                    nc = vu.m_nextCommitCycle;\n",
                    k, vf(l.fs).c_str(), (lowerWord >> 21) & 3u, vf(l.ft).c_str(), (lowerWord >> 23) & 3u);
                return;
            case Lower::Waitq:
                return;
            case Lower::Mfp:
                setVf(l.ft, l.dest, "vdupq_n_f32(vu.m_state.p)", 0u);
                return;
            case Lower::Xtop:
                setVi(l.it, "static_cast<int32_t>(vu.m_state.top & 0x3FFu)");
                return;
            case Lower::Xitop:
                setVi(l.it, "static_cast<int32_t>(vu.m_state.itop & 0x3FFu)");
                return;
            case Lower::Xgkick:
                add("                    D_xgkick(vu, c + %uu, %s);\n", k, vi(l.is).c_str());
                return;
            case Lower::Branch:
            {
                const std::string s = "static_cast<int16_t>(" + branchVi(l.is) + ")", t = "static_cast<int16_t>(" + branchVi(l.it) + ")";
                std::string condition;
                switch (l.op)
                {
                case 0x20u: condition = "true"; break;
                case 0x28u: condition = s + " == " + t; break;
                case 0x29u: condition = s + " != " + t; break;
                case 0x2Cu: condition = s + " < 0"; break;
                case 0x2Du: condition = s + " > 0"; break;
                case 0x2Eu: condition = s + " <= 0"; break;
                default: condition = s + " >= 0"; break;
                }
                add("                    taken = %s;\n", condition.c_str());
                (void)pc;
                return;
            }
            default:
                return;
            }
        }
    };

    // Emits the block made of facts[first .. first+count). A branch inside it comes with its delay slot;
    // when taken, the block is left right after the delay slot (or, if it goes back to the first pair of
    // this very block, run again without the registers ever leaving their locals). The caller wraps the
    // text in the entry guard; `chain(pc)` gives the statement that continues at `pc` (a jump to another
    // block or back to the dispatch loop) and `again` the condition under which the block may loop.
    inline std::string emitBlock(const std::vector<Facts> &facts, uint32_t first, uint32_t count, uint32_t pcMask,
                                 const std::function<std::string(uint32_t)> &chain, const std::string &again,
                                 const bool *invariantVf = nullptr)
    {
        Emitter e;
        e.invariant = invariantVf;
        // Pairs that may have to wait: the cycle base can move there, and they look at the ready times.
        const auto waitCapable = [&](uint32_t k)
        {
            return k < 4u || facts[first + k].hazardReads != 0u || (facts[first + k].bits & VU1Interpreter::PairStall) != 0u;
        };
        // Ready times (m_vfReady / m_viReady) of the registers written so far. Nothing inside the block
        // reads them except a pair that may wait, so a write is only stored at once when such a pair lies
        // within its latency; the others are stored on the way out, if still in the pipeline by then.
        struct ReadyWrite { uint32_t pair, ready; std::string target; };
        std::vector<ReadyWrite> deferred;
        const auto pendingReady = [&](uint32_t executed) -> std::string
        {
            std::string text;
            for (const ReadyWrite &write : deferred)
                if (write.pair < executed && write.ready > executed)
                    text += write.target + " = c + " + std::to_string(write.ready) + "u; ";
            return text;
        };
        const auto readyWrite = [&](uint32_t k, uint32_t latency, const std::string &target)
        {
            bool eager = false;
            for (uint32_t m = k + 1u; m < count && m < k + latency; ++m)
                eager = eager || waitCapable(m);
            if (eager)
                e.body += "                    " + target + " = c + " + std::to_string(k + latency) + "u;\n";
            else
                deferred.push_back({k, k + latency, target});
        };
        uint32_t branchTarget = 0u;
        bool afterBranch = false; // the pair being emitted is the delay slot of the branch to branchTarget
        bool loops = false;
        int backupReg = -1; // VI register hidden from a branch in the next pair, its old value is in `old`
        const auto backupText = [](int reg) -> std::string
        {
            if (reg < 0)
                return "vu.m_viBranchBackupValid = false; ";
            return "vu.m_viBranchBackupValue = old; vu.m_viBranchBackupReg = " + std::to_string(reg) + "; vu.m_viBranchBackupValid = true; ";
        };
        for (uint32_t k = 0; k < count; ++k)
        {
            const Facts &f = facts[first + k];
            const uint32_t pc = (first + k) * 8u;
            const bool iBit = (f.bits & VU1Interpreter::PairIBit) != 0u;
            const Upper u = decodeUpper(f.upper);
            const Lower l = decodeLower(f.lower, iBit);
            e.add("                {   // %04x\n", pc);

            // Leaving the block before this pair runs (a wait hit the cycle budget, or pushed the rest of
            // the block past it): the registers go back to the state and the dispatch loop takes over.
            char leave[256];
            std::string exitText = "@@STORES@@vu.m_hazardUnknown = static_cast<uint8_t>(hu); ";
            std::snprintf(leave, sizeof(leave), "vu.m_executedPairs += %uu; vu.m_directPairs += %uu; vu.m_state.pc = 0x%04xu; ", k, k, pc);
            exitText += leave;
            if (k != 0u)
                exitText += backupText(backupReg);
            if (afterBranch)
            {
                std::snprintf(leave, sizeof(leave), "vu.m_state.branchPending = taken; vu.m_state.branchTarget = 0x%04xu; vu.m_state.branchDelay = 0; ", branchTarget);
                exitText += leave;
            }
            exitText += "continue;";
            const auto afterWait = [&]()
            {
                e.body += "                        if (!ok || vu.m_cycle + " + std::to_string(count - k) + "u > budgetEnd) { " + exitText + " }\n";
                e.add("                        c = vu.m_cycle - %uu;\n                        nc = vu.m_nextCommitCycle;\n", k);
            };
            // After an indirect jump (or at a program entry) the pairs before this one are not the ones the
            // static hazard masks assume: up to four pairs then take the full register check, as in stepPair().
            if (k < 4u)
            {
                e.add("                    if (__builtin_expect(hu != 0u, 0))\n                    {\n");
                e.add("                        const bool ok = D_hazards(vu, c + %uu, 0x%04xu, budgetEnd);\n", k, pc);
                afterWait();
                e.add("                        --hu;\n                    }\n");
            }
            if (f.hazardReads != 0u)
            {
                // Same test as stepPair(): the registers this pair reads must have left their writer's pipeline.
                e.add("                    %s{\n                        uint64_t ready = c + %uu;\n", k < 4u ? "else " : "", k);
                for (uint32_t slot = 0; slot < 4u; ++slot)
                {
                    const uint32_t access = static_cast<uint32_t>(f.hazardReads >> (9u * slot)) & 0x1FFu;
                    for (uint32_t component = 0; component < 4u; ++component)
                        if ((access & (8u >> component)) != 0u)
                            e.add("                        ready = std::max(ready, vu.m_vfReady[%u][%u]);\n", access >> 4, component);
                }
                for (uint32_t reg = 1; reg < 16u; ++reg)
                    if (((f.hazardReads >> 40u) >> reg) & 1u)
                        e.add("                        ready = std::max(ready, vu.m_viReady[%u]);\n", reg);
                // A wait with nothing due in the pipelines meanwhile, no PATH1 transfer in flight and room in
                // the cycle budget only moves the block's cycle base; otherwise the runtime steps through it.
                e.add("                        if (ready > c + %uu)\n                        {\n", k);
                e.add("                        if (ready < nc && !vu.m_xgkick.active && ready + %uu <= budgetEnd)\n                            c = ready - %uu;\n"
                      "                        else\n                        {\n", count - k, k);
                e.add("                        const bool ok = D_wait(vu, c + %uu, ready, budgetEnd);\n", k);
                afterWait();
                e.add("                        }\n                        }\n                    }\n");
            }
            if ((f.bits & VU1Interpreter::PairStall) != 0u)
            {
                // The common case (nothing to wait for) is tested here, without a call.
                std::string busy;
                if ((f.bits & (VU1Interpreter::PairWaitQ | VU1Interpreter::PairFdiv)) != 0u)
                    busy = "(vu.m_fdiv.valid && vu.m_fdiv.readyCycle > c + " + std::to_string(k) + "u)";
                if ((f.bits & VU1Interpreter::PairXgkick) != 0u)
                    busy += std::string(busy.empty() ? "" : " || ") + "vu.m_xgkick.active";
                if ((f.bits & (VU1Interpreter::PairWaitP | VU1Interpreter::PairEfu)) != 0u || busy.empty())
                    busy = "true";
                e.add("                    if (__builtin_expect(%s, 0))\n                    {\n                        const bool ok = D_stall(vu, c + %uu, 0x%04xu, budgetEnd);\n",
                      busy.c_str(), k, unsigned(f.bits));
                afterWait();
                e.add("                    }\n");
            }

            const Emitter::Pending pending = e.upper(u, (f.bits & VU1Interpreter::PairFlagsDead) != 0u, k);
            const bool hides = (f.bits & VU1Interpreter::PairDelaysBranch) != 0u && f.writtenVi != 0u;
            if (hides)
                e.add("                    old = %s;\n", e.vi(f.writtenVi).c_str());
            const int previousBackup = backupReg;
            // When both instructions write the same VF register the upper one wins, whole register.
            e.suppressReg = pending.write && pending.reg != kAcc ? pending.reg : 0u;
            e.lower(l, f.lower, pc, k, [&](uint32_t reg) -> std::string
            {
                if (reg == 0u)
                    return "0";
                if (previousBackup == static_cast<int>(reg))
                    return "old";
                return e.vi(reg);
            });
            const bool delaySlot = afterBranch;
            afterBranch = l.kind == Lower::Branch;
            if (afterBranch)
                branchTarget = (pc + 8u + static_cast<uint32_t>(l.imm * 8)) & pcMask;
            e.suppressReg = 0u;
            if (pending.write)
                e.setVf(pending.reg, pending.dest, "u", pending.normalLanes);
            for (uint32_t which = 0; which < 2u; ++which)
            {
                const uint32_t write = (f.hazardWrites >> (12u * which)) & 0xFFFu;
                if (write == 0u)
                    continue;
                for (uint32_t component = 0; component < 4u; ++component)
                    if ((write & (8u >> component)) != 0u)
                        readyWrite(k, write >> 9, "vu.m_vfReady[" + std::to_string((write >> 4) & 0x1Fu) + "][" + std::to_string(component) + "]");
            }
            if (f.writtenVi != 0u && ((f.hazardWrites >> 28u) & 7u) != 0u)
                readyWrite(k, (f.hazardWrites >> 28u) & 7u, "vu.m_viReady[" + std::to_string(unsigned(f.writtenVi)) + "]");
            e.add("                }\n");
            e.add("                if (__builtin_expect(c + %uu >= nc, 0)) nc = D_commit(vu, c + %uu);\n", k + 1u, k + 1u);
            backupReg = hides ? static_cast<int>(f.writtenVi) : -1;
            if (delaySlot)
            {
                // The branch of the pair before this one is resolved here.
                e.add("                if (taken)\n                {\n");
                if (branchTarget == first * 8u)
                {
                    loops = true;
                    e.body += "                    if (" + again + ")\n                    {\n";
                    e.body += "                        " + pendingReady(k + 1u) + "\n";
                    e.add("                        c += %uu; vu.m_executedPairs += %uu; vu.m_directPairs += %uu;\n", k + 1u, k + 1u, k + 1u);
                    e.body += "                        " + backupText(backupReg) + "\n                        goto again" + std::to_string(first) + ";\n                    }\n";
                }
                e.body += "                    @@STORES@@vu.m_hazardUnknown = static_cast<uint8_t>(hu); " + backupText(backupReg) + pendingReady(k + 1u) + "\n";
                e.add("                    vu.m_cycle = c + %uu; vu.m_executedPairs += %uu; vu.m_directPairs += %uu; vu.m_state.pc = 0x%04xu;\n",
                      k + 1u, k + 1u, k + 1u, branchTarget);
                e.body += "                    " + chain(branchTarget) + "\n                }\n";
            }
        }

        if (!invariantVf)
        {
            // First pass done: now that the registers the block leaves alone are known, emit it for real.
            bool invariantSet[33];
            for (uint32_t reg = 0; reg <= kAcc; ++reg)
                invariantSet[reg] = !e.vfWritten[reg];
            return emitBlock(facts, first, count, pcMask, chain, again, invariantSet);
        }

        std::string stores;
        char text[256];
        for (uint32_t reg = 1; reg <= kAcc; ++reg)
            if (e.vfWritten[reg])
            {
                if (reg == kAcc)
                    stores += "vst1q_f32(vu.m_state.acc, acc); ";
                else
                {
                    std::snprintf(text, sizeof(text), "vst1q_f32(vu.m_state.vf[%u], f%u); ", reg, reg);
                    stores += text;
                }
            }
        for (uint32_t reg = 1; reg < 16u; ++reg)
            if (e.viWritten[reg])
            {
                std::snprintf(text, sizeof(text), "vu.m_state.vi[%u] = i%u; ", reg, reg);
                stores += text;
            }

        std::string out;
        char mask[80];
        std::snprintf(mask, sizeof(mask), "                constexpr uint32_t DM = 0x%04xu; (void)DM;\n", pcMask & ~0xFu); // data memory, as large as code memory
        out += mask;
        out += "                uint64_t c = vu.m_cycle;\n                uint64_t nc = vu.m_nextCommitCycle;\n"
               "                bool taken = false; int32_t old = 0; (void)taken; (void)old;\n"
               "                uint32_t hu = vu.m_hazardUnknown;\n";
        for (uint32_t reg = 1; reg <= kAcc; ++reg)
            if (e.vfRead[reg] || e.vfWritten[reg])
            {
                if (reg == kAcc)
                    out += "                V acc = vld1q_f32(vu.m_state.acc);\n";
                else
                {
                    std::snprintf(text, sizeof(text), "                V f%u = vld1q_f32(vu.m_state.vf[%u]);\n", reg, reg);
                    out += text;
                }
            }
        for (uint32_t reg = 1; reg < 16u; ++reg)
            if (e.viRead[reg] || e.viWritten[reg])
            {
                std::snprintf(text, sizeof(text), "                int32_t i%u = vu.m_state.vi[%u];\n", reg, reg);
                out += text;
            }
        for (uint32_t reg = 1; reg <= kAcc; ++reg)
        {
            const std::string name = reg == kAcc ? std::string("acc") : std::to_string(reg), source = reg == kAcc ? std::string("acc") : "f" + name;
            if (e.hoistClamp[reg])
                out += "                const V k" + name + " = D_clamp(" + source + ");\n";
            if (e.hoistNorm[reg])
                out += "                const V n" + name + " = D_norm(" + source + ");\n";
        }
        if (loops)
            out += "            again" + std::to_string(first) + ":\n";
        std::string body = e.body;
        for (size_t at = body.find("@@STORES@@"); at != std::string::npos; at = body.find("@@STORES@@", at))
            body.replace(at, 10u, stores);
        out += body;
        out += "                " + stores + "\n";
        const uint32_t nextPc = ((first + count) * 8u) & pcMask;
        std::snprintf(text, sizeof(text), "                vu.m_cycle = c + %uu;\n                vu.m_executedPairs += %uu;\n                vu.m_directPairs += %uu;\n"
                                          "                vu.m_hazardUnknown = static_cast<uint8_t>(hu);\n", count, count, count);
        out += text;
        out += "                " + backupText(backupReg) + pendingReady(count) + "\n";
        std::snprintf(text, sizeof(text), "                vu.m_state.pc = 0x%04xu;\n                ", nextPc);
        out += text;
        out += chain(nextPc) + "\n";
        return out;
    }
}
