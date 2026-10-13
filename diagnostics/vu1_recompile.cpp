// Static recompiler for VU0/VU1 microcode images (file names vu0-*.bin / vu1-*.bin).
//
//   ps2x_vu1_recompile out.inc [--coverage coverage.bin] image1.bin image2.bin ...
//
// Every image (16 KiB snapshot of VU1 code memory) becomes one C++ function in
// which each reachable instruction pair is a call to VU1Interpreter::stepPair()
// with literal arguments. stepPair() and the instruction bodies are inline, so
// the compiler folds the decode and keeps only that pair's arithmetic; control
// flow inside the image is a switch on the program counter with fallthrough.
// The pair facts come from VU1Interpreter::describeImage(), i.e. from the same
// decoder and flag analysis the runtime uses.
//
// --coverage (PS2X_VU_CATALOG coverage.bin) seeds reachability with the PCs the
// game executed; without it every pair that decodes is emitted.
#include "runtime/ps2_vu1.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "vu1_direct_emit.inl"

namespace
{
    using Facts = VU1Interpreter::PairFacts;

    std::vector<uint8_t> readFile(const char *path)
    {
        std::vector<uint8_t> bytes;
        if (std::FILE *f = std::fopen(path, "rb"))
        {
            std::fseek(f, 0, SEEK_END);
            bytes.resize(static_cast<size_t>(std::ftell(f)));
            std::fseek(f, 0, SEEK_SET);
            if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size())
                bytes.clear();
            std::fclose(f);
        }
        return bytes;
    }

    uint64_t catalogHash(const std::vector<uint8_t> &bytes)
    {
        uint64_t hash = 14695981039346656037ull;
        for (uint8_t byte : bytes) { hash ^= byte; hash *= 1099511628211ull; }
        return hash;
    }

    bool isStaticBranch(const Facts &f, bool &unconditional, int32_t &offsetPairs)
    {
        if ((f.bits & VU1Interpreter::PairIBit) != 0u)
            return false;
        const uint32_t op = (f.lower >> 25) & 0x7Fu;
        const bool branch = op == 0x20u || op == 0x21u || op == 0x28u || op == 0x29u || (op >= 0x2Cu && op <= 0x2Fu);
        if (!branch)
            return false;
        unconditional = op == 0x20u || op == 0x21u;
        offsetPairs = static_cast<int32_t>(static_cast<int32_t>(f.lower << 21) >> 21);
        return true;
    }

    bool isIndirectJump(const Facts &f)
    {
        if ((f.bits & VU1Interpreter::PairIBit) != 0u)
            return false;
        const uint32_t op = (f.lower >> 25) & 0x7Fu;
        return op == 0x24u || op == 0x25u;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s out.inc [--coverage coverage.bin] image.bin...\n", argv[0]);
        return 2;
    }
    const char *outputPath = argv[1];
    std::map<std::pair<uint32_t, uint64_t>, std::set<uint32_t>> coverage; // (unit, catalog hash) -> executed pcs
    std::vector<std::string> imagePaths;
    for (int i = 2; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--coverage") && i + 1 < argc)
        {
            const std::vector<uint8_t> bytes = readFile(argv[++i]);
            for (size_t offset = 0; offset + 16u <= bytes.size(); offset += 16u)
            {
                uint32_t unit, pc;
                uint64_t hash;
                std::memcpy(&unit, &bytes[offset], 4);
                std::memcpy(&pc, &bytes[offset + 4], 4);
                std::memcpy(&hash, &bytes[offset + 8], 8);
                if (unit <= 1u && pc < 0x4000u)
                    coverage[{unit, hash}].insert(pc & ~7u);
            }
        }
        else
            imagePaths.emplace_back(argv[i]);
    }

    // (lower, upper, bits, writtenVi, shadowReg, mayEnd) -> pair function id
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool, uint32_t, uint64_t, uint32_t>, uint32_t> pairIds;
    // PS2X_VU_RECOMPILE_PROFILE=<file>[:<file>...]: execution counts of the pairs of recorded traces
    // (ps2x_vu1_trace_replay with PS2X_VU_PC_HISTOGRAM; lines "unit hash pc count cycles lower upper bits"). A pair
    // is hot when it accounts for at least 1/2000 of its unit's executed pairs.
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, uint64_t> profile;
    uint64_t hotCountOf[2] = {~0ull, ~0ull};
    if (const char *paths = std::getenv("PS2X_VU_RECOMPILE_PROFILE"))
    {
        uint64_t total[2] = {};
        std::string list = paths;
        for (size_t at = 0; at <= list.size();)
        {
            const size_t end = std::min(list.find(':', at), list.size());
            if (std::FILE *file = std::fopen(list.substr(at, end - at).c_str(), "r"))
            {
                unsigned unitOf, pc, lower, upper, bits;
                unsigned long long hash, count, cycles;
                while (std::fscanf(file, "%u %llx %x %llu %llu %x %x %x", &unitOf, &hash, &pc, &count, &cycles, &lower, &upper, &bits) == 8)
                {
                    profile[std::make_tuple(unitOf & 1u, pc, lower, upper)] += count;
                    total[unitOf & 1u] += count;
                }
                std::fclose(file);
            }
            at = end + 1u;
        }
        for (uint32_t which = 0; which < 2u; ++which)
            hotCountOf[which] = std::max<uint64_t>(total[which] / 2000u, 1u);
    }
    std::string pairFunctions, declarations, table[2], programTable;
    std::vector<std::pair<uint32_t, std::string>> functions; // pairs, text of one image/program function
    size_t programCount = 0;
    std::set<std::pair<uint32_t, uint64_t>> emittedImages;
    size_t totalPairs = 0, flagWriters = 0, liveWriters = 0, directBlocks = 0, directPairs = 0;

    for (const std::string &path : imagePaths)
    {
        std::vector<uint8_t> code = readFile(path.c_str());
        // The file name prefix (vu0-/vu1-) selects the unit.
        const std::string base = path.substr(path.find_last_of('/') + 1);
        // prog<unit>-<addr hex>-<id>.bin: one microprogram (extract_vu_programs.py), compiled on its own
        // and recognised at run time wherever it sits in VU memory at that address, whatever surrounds it.
        const bool isProgram = base.rfind("prog", 0) == 0 && base.size() > 6;
        const uint32_t unit = isProgram ? (base[4] == '0' ? 0u : 1u) : (base.rfind("vu0-", 0) == 0 ? 0u : 1u);
        const uint32_t codeSize = unit == 0u ? 0x1000u : 0x4000u;
        uint32_t programAddr = 0u, programSize = 0u;
        uint64_t programHash = 0u, programFirst = 0u;
        if (isProgram)
        {
            programAddr = static_cast<uint32_t>(std::strtoul(base.c_str() + 6, nullptr, 16));
            programSize = static_cast<uint32_t>(code.size());
            if ((programAddr & 7u) != 0u || (programSize & 7u) != 0u || programSize < 8u || programAddr + programSize > codeSize)
            {
                std::fprintf(stderr, "skip %s: not a program that fits VU%u memory\n", path.c_str(), unit);
                continue;
            }
            programHash = VU1Interpreter::imageHash(code.data(), programSize);
            std::memcpy(&programFirst, code.data(), 8);
            // Everything around the program reads the MAC flags, so a result that can leave the program
            // with its flags still pending keeps computing them.
            std::vector<uint8_t> image(codeSize);
            const uint32_t reader[2] = {(0x1Au << 25) | (1u << 16) | (1u << 11), 0x000002FFu};
            for (uint32_t offset = 0; offset < codeSize; offset += 8u)
                std::memcpy(&image[offset], reader, 8);
            std::memcpy(&image[programAddr], code.data(), programSize);
            code.swap(image);
        }
        if (code.size() != codeSize)
        {
            std::fprintf(stderr, "skip %s: not a %u-byte VU%u code image\n", path.c_str(), codeSize, unit);
            continue;
        }
        const uint32_t kPairs = codeSize / 8u;
        const uint64_t hash = isProgram ? (programHash ^ (uint64_t{programAddr} << 48) ^ 0x9E3779B97F4A7C15ull)
                                        : VU1Interpreter::imageHash(code.data(), static_cast<uint32_t>(code.size()));
        if (!emittedImages.insert({unit, hash}).second)
            continue;
        std::vector<Facts> facts(kPairs);
        VU1Interpreter::describeImage(unit == 0u ? VU1Interpreter::Unit::VU0 : VU1Interpreter::Unit::VU1,
                                      code.data(), static_cast<uint32_t>(code.size()), facts.data(), kPairs,
                                      isProgram ? programAddr : 0u, isProgram ? programSize : 0u);

        auto compilable = [&](uint32_t p)
        {
            return (facts[p].bits & (VU1Interpreter::PairReserved | VU1Interpreter::PairDBit | VU1Interpreter::PairTBit)) == 0u;
        };
        auto next = [kPairs](uint32_t p) { return p + 1u < kPairs ? p + 1u : 0u; };
        auto previous = [kPairs](uint32_t p) { return p != 0u ? p - 1u : kPairs - 1u; };

        // Reachability over pair indices from the executed PCs.
        std::vector<bool> included(kPairs, false);
        std::vector<uint32_t> work;
        const auto seeds = isProgram ? coverage.end() : coverage.find({unit, catalogHash(code)});
        if (isProgram)
        {
            // No reachability: the whole program is compiled, nothing outside it.
            for (uint32_t p = programAddr / 8u; p < (programAddr + programSize) / 8u; ++p)
                included[p] = compilable(p);
        }
        else if (seeds != coverage.end())
        {
            for (uint32_t pc : seeds->second)
                if (pc / 8u < kPairs)
                    work.push_back(pc / 8u);
        }
        else
        {
            // Image captured without coverage: emit every pair that decodes.
            for (uint32_t p = 0; p < kPairs; ++p) work.push_back(p);
        }
        while (!work.empty())
        {
            const uint32_t p = work.back();
            work.pop_back();
            if (included[p] || !compilable(p))
                continue;
            included[p] = true;
            const Facts &before = facts[previous(p)];
            bool unconditional = false;
            int32_t offsetPairs = 0;
            const bool delaySlot = isStaticBranch(before, unconditional, offsetPairs);
            if (delaySlot)
                work.push_back(static_cast<uint32_t>(static_cast<int32_t>(previous(p)) + 1 + offsetPairs) & (kPairs - 1u));
            const bool endsHere = (before.bits & VU1Interpreter::PairEBit) != 0u;
            if (!(delaySlot && unconditional) && !isIndirectJump(before) && !endsHere)
                work.push_back(next(p));
        }

        // Longest straight run that executes without returning to the dispatch loop.
        uint32_t maxRun = 1u, run = 0u;
        for (uint32_t p = 0; p < kPairs; ++p)
        {
            run = included[p] ? run + 1u : 0u;
            maxRun = std::max(maxRun, run);
        }

        // Direct blocks (vu1_direct_emit.inl): blockLength[p] != 0 at the first pair of a block, inBlock[p]
        // for the pairs it covers. PS2X_VU_RECOMPILE_NO_DIRECT=1 keeps every pair on stepPair().
        std::vector<uint32_t> blockLength(kPairs, 0u);
        std::vector<bool> inBlock(kPairs, false), blockBranches(kPairs, false), blockEnds(kPairs, false);
        if (!std::getenv("PS2X_VU_RECOMPILE_NO_DIRECT"))
        {
            std::vector<bool> target(kPairs, false), afterCall(kPairs, false);
            for (uint32_t p = 0; p < kPairs; ++p)
            {
                if (!included[p])
                    continue;
                bool unconditional = false;
                int32_t offsetPairs = 0;
                if (isStaticBranch(facts[p], unconditional, offsetPairs))
                    target[static_cast<uint32_t>(static_cast<int32_t>(p) + 1 + offsetPairs) & (kPairs - 1u)] = true;
                // A subroutine comes back to the pair after the call's delay slot through JR; the next four
                // pairs then need the full hazard check (m_hazardUnknown), which blocks do not make.
                const uint32_t op = (facts[p].lower >> 25) & 0x7Fu;
                if ((facts[p].bits & VU1Interpreter::PairIBit) == 0u && (op == 0x21u || op == 0x25u))
                    for (uint32_t q = p + 2u; q < p + 6u && q < kPairs; ++q)
                        afterCall[q] = true;
            }
            const auto endsProgram = [&](uint32_t p)
            {
                return (facts[p].bits & VU1Interpreter::PairEndBits) != 0u || (facts[previous(p)].bits & VU1Interpreter::PairEndBits) != 0u;
            };
            const auto afterBranch = [&](uint32_t p)
            {
                return (facts[previous(p)].bits & (VU1Interpreter::PairBranch | VU1Interpreter::PairIndirect)) != 0u;
            };
            const auto plain = [&](uint32_t p) // a pair a block can hold, the branch rules aside
            {
                return p < kPairs && included[p] && direct::pairIsDirect(facts[p], endsProgram(p));
            };
            const uint32_t minBlock = std::getenv("PS2X_VU_RECOMPILE_MIN_BLOCK") ? uint32_t(std::atoi(std::getenv("PS2X_VU_RECOMPILE_MIN_BLOCK"))) : 1u;
            uint32_t start = kPairs, length = 0u;
            const auto close = [&](bool branches, bool ends = false)
            {
                if (start < kPairs && length >= minBlock)
                {
                    blockLength[start] = length;
                    blockBranches[start] = branches;
                    blockEnds[start] = ends;
                    for (uint32_t q = start; q < start + length; ++q)
                        inBlock[q] = true;
                }
                start = kPairs;
                length = 0u;
            };
            // Superblocks (PS2X_VU_RECOMPILE_SUPERBLOCKS=0 for the old layout): a block no longer stops where some
            // branch lands. It runs on (up to 128 pairs) and the landing pair starts a block of its own as well, so
            // the code after it exists twice; in exchange a loop whose body has such landing pairs is one block from
            // its head to its closing branch, and its registers stay in machine registers from one turn to the next.
            const bool superblocks = !(std::getenv("PS2X_VU_RECOMPILE_SUPERBLOCKS") && std::getenv("PS2X_VU_RECOMPILE_SUPERBLOCKS")[0] == '0') && !profile.empty();
            // Doing it everywhere makes the generated code five times larger: only landing pairs that the recorded
            // traces show to be hot (PS2X_VU_RECOMPILE_PROFILE, see below) are run through.
            const uint64_t hotCount = hotCountOf[unit & 1u];
            const auto hot = [&](uint32_t p)
            {
                const auto found = profile.find(std::make_tuple(unit, p * 8u, facts[p].lower, facts[p].upper));
                return found != profile.end() && found->second >= hotCount;
            };
            // single: only the block that starts at `begin` (a landing pair inside an existing block).
            const auto scan = [&](uint32_t begin, bool single)
            {
            bool done = false;
            for (uint32_t p = begin; p < kPairs && !done; ++p)
            {
                if ((target[p] && !(superblocks && hot(p))) || (superblocks && length >= 128u))
                {
                    close(false);
                    done = single;
                    if (done)
                        break;
                }
                // The E-bit pair and the pair after it, when both are otherwise ordinary, close the block they are
                // in: it returns Ended. (Every microprogram call ends this way; left to stepPair() these two pairs
                // cost as much as a few dozen direct ones.)
                if (p + 1u < kPairs && included[p] && included[p + 1u] && (facts[p].bits & VU1Interpreter::PairEBit) != 0u &&
                    direct::pairIsOrdinary(facts[p]) && !direct::pairIsBranch(facts[p]) && !(afterBranch(p) && length == 0u) &&
                    (facts[p + 1u].bits & VU1Interpreter::PairEndBits) == 0u && direct::pairIsOrdinary(facts[p + 1u]) &&
                    !direct::pairIsBranch(facts[p + 1u]) && !std::getenv("PS2X_VU_RECOMPILE_NO_END_BLOCKS"))
                {
                    if (length == 0u)
                        start = p;
                    length += 2u;
                    close(false, true);
                    done = single;
                    ++p;
                    continue;
                }
                const bool branch = plain(p) && direct::pairIsBranch(facts[p]);
                // A block may start at a branch (the value it tests comes from the hidden-register state when the pair
                // before it wrote the register), and a run of one or two pairs is still a block: both used to be left to
                // stepPair(), about ten times slower per pair.
                if (!plain(p) || (afterBranch(p) && length == 0u) || (branch && (!plain(p + 1u) || direct::pairIsBranch(facts[p + 1u]) || target[p + 1u])))
                {
                    close(false);
                    done = single;
                    continue;
                }
                if (length == 0u)
                    start = p;
                ++length;
                if (branch)
                {
                    ++length; // the delay slot
                    ++p;
                    // The block goes on after a conditional branch (it leaves through a side exit when taken)
                    // unless the next pair must start a block of its own.
                    const uint32_t op = (facts[p - 1u].lower >> 25) & 0x7Fu;
                    if (op == 0x20u || direct::pairIsIndirect(facts[p - 1u]) || !plain(p + 1u) || (target[p + 1u] && !(superblocks && hot(p + 1u))) || length >= 96u)
                    {
                        close(true);
                        done = single;
                    }
                }
            }
            close(false);
            };
            scan(0u, false);
            if (superblocks)
                for (uint32_t q = 0; q < kPairs; ++q)
                    if (target[q] && included[q] && blockLength[q] == 0u)
                        scan(q, true);
            // PS2X_VU_RECOMPILE_NONDIRECT=<file>: the pairs left to stepPair() and why (appended, one line per pair:
            // pc, lower, upper, bits, reason), to be joined with a PS2X_VU_PC_HISTOGRAM of a trace.
            if (const char *path = std::getenv("PS2X_VU_RECOMPILE_NONDIRECT"))
                if (std::FILE *file = std::fopen(path, "a"))
                {
                    for (uint32_t p = 0; p < kPairs; ++p)
                    {
                        if (!included[p] || inBlock[p])
                            continue;
                        const Facts &f = facts[p];
                        const char *reason = "short-run";
                        if (!direct::pairIsDirect(f, endsProgram(p)))
                        {
                            const bool iBit = (f.bits & VU1Interpreter::PairIBit) != 0u;
                            reason = endsProgram(p) ? "program-end" : (f.bits & VU1Interpreter::PairEfu) ? "efu" : (f.bits & VU1Interpreter::PairIndirect) ? "indirect-jump"
                                   : (f.bits & (VU1Interpreter::PairReserved | VU1Interpreter::PairDBit | VU1Interpreter::PairTBit)) ? "reserved/d/t"
                                   : direct::decodeUpper(f.upper).kind == direct::Upper::None ? "upper-op" : direct::decodeLower(f.lower, iBit).kind == direct::Lower::None ? "lower-op" : "other";
                        }
                        else if (afterBranch(p))
                            reason = "after-branch";
                        else if (direct::pairIsBranch(f))
                            reason = "branch";
                        std::fprintf(file, "%04x %08x %08x %05x %s\n", p * 8u, f.lower, f.upper, unsigned(f.bits), reason);
                    }
                    std::fclose(file);
                }
        }

        char text[1024];
        std::string body;
        uint32_t count = 0;
        // PS2X_VU_RECOMPILE_SHARED=0: every block loads and stores its own registers, as before.
        const bool sharedRegisters = !(std::getenv("PS2X_VU_RECOMPILE_SHARED") && std::getenv("PS2X_VU_RECOMPILE_SHARED")[0] == '0');
        direct::SharedRegisters shared;
        for (uint32_t p = 0; p < kPairs; ++p)
        {
            if (!included[p])
                continue;
            ++count;
            if (blockLength[p] != 0u)
            {
                ++directBlocks;
                directPairs += blockLength[p];
                // PS2X_VU_RECOMPILE_BLOCKS=<file>: every block (pc, lower and upper word of its first pair, length).
                if (const char *path = std::getenv("PS2X_VU_RECOMPILE_BLOCKS"))
                    if (std::FILE *file = std::fopen(path, "a"))
                    {
                        std::fprintf(file, "%04x %08x %08x %u\n", p * 8u, facts[p].lower, facts[p].upper, blockLength[p]);
                        std::fclose(file);
                    }
                if (sharedRegisters)
                    std::snprintf(text, sizeof(text), "            case 0x%04xu:\n#if defined(__aarch64__)\n                if (direct && vu.directBlockAllowed(0x%04xu))\n                {\n"
                                                      "                    VU_LOAD\n                    goto B%04x;\n                }\n#endif\n                return R::NotHandled;\n"
                                                      "#if defined(__aarch64__)\n            B%04x:\n                {\n",
                                  p * 8u, p * 8u, p * 8u, p * 8u);
                else
                    std::snprintf(text, sizeof(text), "            case 0x%04xu:\n#if defined(__aarch64__)\n            B%04x:\n                if (direct && vu.directBlockAllowed(0x%04xu))\n                {\n",
                                  p * 8u, p * 8u, p * 8u);
                body += text;
                // Continuing at another block of this function skips the dispatch switch; the loop's own
                // entry conditions that a block can change (the cycle budget) are tested here instead.
                const auto chain = [&](uint32_t pc) -> std::string
                {
                    const char *leave = sharedRegisters ? "VU_STORE continue;" : "continue;";
                    if (pc / 8u >= kPairs || blockLength[pc / 8u] == 0u)
                        return leave;
                    char jump[240];
                    if (sharedRegisters)
                        std::snprintf(jump, sizeof(jump), "if (vu.m_cycle + %uu <= budgetEnd && !vu.m_stopRequested && vu.directBlockAllowed(0x%04xu)) goto B%04x; %s", maxRun, pc, pc, leave);
                    else
                        std::snprintf(jump, sizeof(jump), "if (vu.m_cycle + %uu <= budgetEnd && !vu.m_stopRequested) goto B%04x; %s", maxRun, pc, leave);
                    return jump;
                };
                char again[200];
                std::snprintf(again, sizeof(again), "c + %uu <= budgetEnd && !vu.m_stopRequested && vu.directBlockAllowed(0x%04xu)", maxRun + blockLength[p], p * 8u);
                body += direct::emitBlock(facts, p, blockLength[p], codeSize - 1u, chain, again, nullptr, blockEnds[p], sharedRegisters ? &shared : nullptr);
                body += sharedRegisters ? "                }\n#endif\n" : "                }\n#endif\n                return R::NotHandled;\n";
            }
            if (inBlock[p])
                continue;
            const Facts &f = facts[p];
            const Facts &before = facts[previous(p)];
            const bool mayEnd = (f.bits & VU1Interpreter::PairEndBits) != 0u || (before.bits & VU1Interpreter::PairEndBits) != 0u;
            bool unconditional = false;
            int32_t offsetPairs = 0;
            const bool delaySlot = isStaticBranch(before, unconditional, offsetPairs) || isIndirectJump(before);
            // A pair is only entered from the dispatch loop (which leaves pending branches to the interpreter)
            // or by falling through from the pair before it.
            // (A branch pair itself goes through the pending-branch step: that is where its delay counts down.)
            const bool branches = (f.bits & (VU1Interpreter::PairBranch | VU1Interpreter::PairIndirect)) != 0u;
            const uint32_t bits = uint32_t(f.bits) | (delaySlot || branches ? 0u : uint32_t(VU1Interpreter::PairNoPendingBranch));
            // The hazard masks depend on the pairs around this one, so equal instruction words can need different bodies.
            const auto key = std::make_tuple(f.lower, f.upper, bits, uint32_t(f.writtenVi), uint32_t(f.shadowReg), mayEnd, codeSize,
                                             f.hazardReads, f.hazardWrites);
            auto found = pairIds.find(key);
            if (found == pairIds.end())
            {
                found = pairIds.emplace(key, static_cast<uint32_t>(pairIds.size())).first;
                std::snprintf(text, sizeof(text),
                              "    static inline __attribute__((always_inline)) R P%u(VU1Interpreter &vu, uint32_t pc, VU_BLOCK_PARAMS)\n"
                              "    { return vu.stepPairCall(pc, 0x%08xu, 0x%08xu, 0x%05xu, %uu, %uu, 0x%llxull, 0x%xu, %s, 0x%xu, vuData, dataSize, gs, memory, budgetEnd); }\n",
                              found->second, f.lower, f.upper, bits, unsigned(f.writtenVi), unsigned(f.shadowReg),
                              static_cast<unsigned long long>(f.hazardReads), unsigned(f.hazardWrites),
                              mayEnd ? "true" : "false", codeSize);
                pairFunctions += text;
            }
            {
                // Same test as the runtime's writer classification (DEST != 0 and an FMAC opcode).
                uint32_t op = f.upper & 0x3Fu;
                const bool special = op >= 0x3Cu;
                if (special) op = (f.upper & 3u) | ((f.upper >> 4) & 0x7Cu);
                const bool writer = ((f.upper >> 21) & 0xFu) != 0u && (special || op < 0x30u) &&
                                    (op <= 0x0Fu || (op >= 0x18u && op <= 0x1Cu) || op == 0x1Eu || (op >= 0x20u && op <= 0x2Au) || (op >= 0x2Cu && op <= 0x2Eu));
                if (writer) { ++flagWriters; if ((f.bits & VU1Interpreter::PairFlagsDead) == 0u) ++liveWriters; }
            }
            const bool stall = (f.bits & VU1Interpreter::PairStall) != 0u;
            const bool checkResult = mayEnd || stall;
            const uint32_t following = next(p);
            const bool lastOfRun = !included[following] || following == 0u;
            std::snprintf(text, sizeof(text), "            case 0x%04xu:\n", p * 8u);
            body += text;
            if (checkResult)
                std::snprintf(text, sizeof(text), "                { const R r = P%u(vu, 0x%04xu, VU_BLOCK_ARGS); if (r != R::Continue) return r; }\n", found->second, p * 8u);
            else
                std::snprintf(text, sizeof(text), "                (void)P%u(vu, 0x%04xu, VU_BLOCK_ARGS);\n", found->second, p * 8u);
            body += text;
            if (lastOfRun || stall)
                body += "                continue;\n";
            else
            {
                if (delaySlot)
                {
                    std::snprintf(text, sizeof(text), "                if (vu.m_state.pc != 0x%04xu) continue;\n", following * 8u);
                    body += text;
                }
                body += "                [[fallthrough]];\n";
            }
        }
        totalPairs += count;
        std::snprintf(text, sizeof(text),
                      "    // %s: %u pairs\n"
                      "    VuRecompiled::R VuRecompiled::%s%u_%016llx(VU1Interpreter &vu, uint32_t codeSize, VU_BLOCK_PARAMS)\n"
                      "    {\n"
                      "        if (codeSize != 0x%xu) return R::NotHandled;\n"
                      "        const bool direct = vu.directBlocksUsable(vuData, dataSize); (void)direct;\n"
                      "        VU_REGS\n"
                      "        for (;;)\n"
                      "        {\n"
                      "            if (vu.m_cycle + %uu > budgetEnd || vu.m_stopRequested || vu.m_state.branchPending ||\n"
                      "                vu.m_state.ebit || vu.m_state.haltAfterDelaySlot)\n"
                      "                return R::NotHandled;\n"
                      "            switch (vu.m_state.pc)\n"
                      "            {\n",
                      base.c_str(), count, isProgram ? "prog" : "image", unit,
                      static_cast<unsigned long long>(hash), codeSize, maxRun);
        std::string function;
        {
            // The registers of the function's direct blocks (see direct::SharedRegisters): declared once, loaded
            // when a block is entered from code that works on the VU state, stored when such code comes next.
            std::string regs, load, store;
            char one[96];
            for (uint32_t reg = 1; reg <= 32u; ++reg)
            {
                const std::string name = reg == 32u ? "acc" : "f" + std::to_string(reg);
                const std::string where = reg == 32u ? "vu.m_state.acc" : "vu.m_state.vf[" + std::to_string(reg) + "]";
                if (shared.vfUsed[reg])
                {
                    regs += "V " + name + " = vdupq_n_f32(0.0f); ";
                    load += name + " = vld1q_f32(" + where + "); ";
                }
                if (shared.vfWritten[reg])
                    store += "vst1q_f32(" + where + ", " + name + "); ";
            }
            for (uint32_t reg = 1; reg < 16u; ++reg)
            {
                if (shared.viUsed[reg])
                {
                    std::snprintf(one, sizeof(one), "int32_t i%u = 0; ", reg);
                    regs += one;
                    std::snprintf(one, sizeof(one), "i%u = vu.m_state.vi[%u]; ", reg, reg);
                    load += one;
                }
                if (shared.viWritten[reg])
                {
                    std::snprintf(one, sizeof(one), "vu.m_state.vi[%u] = i%u; ", reg, reg);
                    store += one;
                }
            }
            function += "#if defined(__aarch64__)\n#define VU_REGS " + regs + "\n#define VU_LOAD " + load + "\n#define VU_STORE " + store +
                              "\n#else\n#define VU_REGS\n#define VU_LOAD\n#define VU_STORE\n#endif\n";
        }
        function += text;
        function += body;
        function += "            default:\n                return R::NotHandled;\n            }\n        }\n    }\n"
                          "#undef VU_REGS\n#undef VU_LOAD\n#undef VU_STORE\n\n";
        {
            char declaration[160];
            std::snprintf(declaration, sizeof(declaration), "    static R %s%u_%016llx(VU1Interpreter &vu, uint32_t codeSize, VU_BLOCK_PARAMS);\n",
                          isProgram ? "prog" : "image", unit, static_cast<unsigned long long>(hash));
            declarations += declaration;
            functions.emplace_back(count, std::move(function));
        }
        if (isProgram)
        {
            std::snprintf(text, sizeof(text), "        {%uu, 0x%04xu, 0x%04xu, 0x%016llxull, 0x%016llxull, &prog%u_%016llx},\n",
                          unit, programAddr, programSize, static_cast<unsigned long long>(programHash),
                          static_cast<unsigned long long>(programFirst), unit, static_cast<unsigned long long>(hash));
            programTable += text;
            ++programCount;
            continue;
        }
        std::snprintf(text, sizeof(text), "            case 0x%016llxull: return &image%u_%016llx;\n",
                      static_cast<unsigned long long>(hash), unit, static_cast<unsigned long long>(hash));
        table[unit] += text;
    }

    std::FILE *out = std::fopen(outputPath, "wb");
    if (!out)
    {
        std::fprintf(stderr, "cannot write %s\n", outputPath);
        return 1;
    }
    std::fprintf(out,
                 "// Generated by ps2x_vu1_recompile from captured VU1 microcode images. Do not edit.\n"
                 "// %zu images, %zu instruction pairs, %zu distinct pair bodies.\n"
                 "#define PS2X_VU_RECOMPILED_PROGRAMS 1\n"
                 "#define VU_BLOCK_PARAMS uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint64_t budgetEnd\n"
                 "#define VU_BLOCK_ARGS vuData, dataSize, gs, memory, budgetEnd\n"
                 "struct VuRecompiled\n{\n    using R = VU1Interpreter::StepResult;\n\n",
                 emittedImages.size(), totalPairs, pairIds.size());
    std::fputs(direct::runtimeHelpers(), out);
    std::fputs(pairFunctions.c_str(), out);
    std::fputs("\n    // One function per image or program; they are defined in black_vu1_recompiled_part<N>.cpp.\n", out);
    std::fputs(declarations.c_str(), out);
    std::fprintf(out,
                 "\n    static VU1Interpreter::RecompiledImage find(uint64_t hash, uint32_t unit);\n"
                 "    // Microprograms recognised by content at a fixed load address (unit, address, bytes, hash of the bytes,\n"
                 "    // first pair, code).\n"
                 "    struct Program { uint32_t unit, address, size; uint64_t hash, first; VU1Interpreter::RecompiledImage code; };\n"
                 "    static const Program *programs(size_t &count);\n};\n\n"
                 "#if !defined(PS2X_VU_RECOMPILED_PART)\n"
                 "inline VU1Interpreter::RecompiledImage VuRecompiled::find(uint64_t hash, uint32_t unit)\n{\n"
                 "    if (unit == 0u)\n    {\n        switch (hash)\n        {\n%s        default: return nullptr;\n        }\n    }\n"
                 "    switch (hash)\n    {\n%s    default: return nullptr;\n    }\n}\n\n"
                 "inline const VuRecompiled::Program *VuRecompiled::programs(size_t &count)\n{\n"
                 "    static const Program list[] = {\n%s        {2u, 0u, 0u, 0ull, 0ull, nullptr}};\n"
                 "    count = %zuu;\n    return list;\n}\n#undef VU_BLOCK_PARAMS\n#undef VU_BLOCK_ARGS\n#endif\n",
                 table[0].c_str(), table[1].c_str(), programTable.c_str(), programCount);
    std::fclose(out);
    {
        // The functions, largest first, each to the part that is lightest so far.
        constexpr size_t kParts = 8u;
        std::string parts[kParts];
        uint64_t weight[kParts] = {};
        std::stable_sort(functions.begin(), functions.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
        for (const auto &function : functions)
        {
            const size_t part = static_cast<size_t>(std::min_element(weight, weight + kParts) - weight);
            parts[part] += function.second;
            weight[part] += function.first;
        }
        std::string base = outputPath;
        const size_t dot = base.rfind('.');
        if (dot != std::string::npos)
            base.erase(dot);
        for (size_t part = 0; part < kParts; ++part)
        {
            const std::string path = base + "_part" + std::to_string(part) + ".cpp";
            std::FILE *file = std::fopen(path.c_str(), "wb");
            if (!file)
            {
                std::fprintf(stderr, "cannot write %s\n", path.c_str());
                return 1;
            }
            std::fputs("// Generated by ps2x_vu1_recompile from captured VU microcode. Do not edit.\n"
                       "#include \"ps2_vu1_recompiled_part.h\"\n#define PS2X_VU_RECOMPILED_PART 1\n#include \"black_vu1_recompiled.inc\"\n\n", file);
            std::fputs(parts[part].c_str(), file);
            std::fclose(file);
        }
    }
    std::printf("wrote %s: %zu images+programs (%zu programs), %zu pairs, %zu distinct pair bodies; FMAC flag writers %zu, still computing flags %zu; "
                "%zu direct blocks covering %zu pairs\n",
                outputPath, emittedImages.size(), programCount, totalPairs, pairIds.size(), flagWriters, liveWriters, directBlocks, directPairs);
    return 0;
}
