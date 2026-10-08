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
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

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
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool, uint32_t>, uint32_t> pairIds;
    std::string pairFunctions, imageFunctions, table[2], programTable;
    size_t programCount = 0;
    std::set<std::pair<uint32_t, uint64_t>> emittedImages;
    size_t totalPairs = 0, flagWriters = 0, liveWriters = 0;

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
                                      code.data(), static_cast<uint32_t>(code.size()), facts.data(), kPairs);

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

        char text[512];
        std::string body;
        uint32_t count = 0;
        for (uint32_t p = 0; p < kPairs; ++p)
        {
            if (!included[p])
                continue;
            ++count;
            const Facts &f = facts[p];
            const Facts &before = facts[previous(p)];
            const bool mayEnd = (f.bits & VU1Interpreter::PairEndBits) != 0u || (before.bits & VU1Interpreter::PairEndBits) != 0u;
            const auto key = std::make_tuple(f.lower, f.upper, uint32_t(f.bits), uint32_t(f.writtenVi), uint32_t(f.shadowReg), mayEnd, codeSize);
            auto found = pairIds.find(key);
            if (found == pairIds.end())
            {
                found = pairIds.emplace(key, static_cast<uint32_t>(pairIds.size())).first;
                std::snprintf(text, sizeof(text),
                              "    static inline __attribute__((always_inline)) R P%u(VU1Interpreter &vu, uint32_t pc, VU_BLOCK_PARAMS)\n"
                              "    { return vu.stepPair(pc, 0x%08xu, 0x%08xu, 0x%04xu, %uu, %uu, %s, 0x%xu, vuData, dataSize, gs, memory, budgetEnd); }\n",
                              found->second, f.lower, f.upper, unsigned(f.bits), unsigned(f.writtenVi), unsigned(f.shadowReg),
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
            bool unconditional = false;
            int32_t offsetPairs = 0;
            const bool delaySlot = isStaticBranch(before, unconditional, offsetPairs) || isIndirectJump(before);
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
                      "    static R %s%u_%016llx(VU1Interpreter &vu, uint32_t codeSize, VU_BLOCK_PARAMS)\n"
                      "    {\n"
                      "        if (codeSize != 0x%xu) return R::NotHandled;\n"
                      "        for (;;)\n"
                      "        {\n"
                      "            if (vu.m_cycle + %uu > budgetEnd || vu.m_stopRequested || vu.m_state.branchPending ||\n"
                      "                vu.m_state.ebit || vu.m_state.haltAfterDelaySlot)\n"
                      "                return R::NotHandled;\n"
                      "            switch (vu.m_state.pc)\n"
                      "            {\n",
                      base.c_str(), count, isProgram ? "prog" : "image", unit,
                      static_cast<unsigned long long>(hash), codeSize, maxRun);
        imageFunctions += text;
        imageFunctions += body;
        imageFunctions += "            default:\n                return R::NotHandled;\n            }\n        }\n    }\n\n";
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
    std::fputs(pairFunctions.c_str(), out);
    std::fputs("\n", out);
    std::fputs(imageFunctions.c_str(), out);
    std::fprintf(out,
                 "    static VU1Interpreter::RecompiledImage find(uint64_t hash, uint32_t unit)\n    {\n"
                 "        if (unit == 0u)\n        {\n            switch (hash)\n            {\n%s            default: return nullptr;\n            }\n        }\n"
                 "        switch (hash)\n        {\n%s        default: return nullptr;\n        }\n    }\n\n"
                 "    // Microprograms recognised by content at a fixed load address (unit, address, bytes, hash of the bytes,\n"
                 "    // first pair, code).\n"
                 "    struct Program { uint32_t unit, address, size; uint64_t hash, first; VU1Interpreter::RecompiledImage code; };\n"
                 "    static const Program *programs(size_t &count)\n    {\n"
                 "        static const Program list[] = {\n%s        {2u, 0u, 0u, 0ull, 0ull, nullptr}};\n"
                 "        count = %zuu;\n        return list;\n    }\n};\n#undef VU_BLOCK_PARAMS\n#undef VU_BLOCK_ARGS\n",
                 table[0].c_str(), table[1].c_str(), programTable.c_str(), programCount);
    std::fclose(out);
    std::printf("wrote %s: %zu images+programs (%zu programs), %zu pairs, %zu distinct pair bodies; FMAC flag writers %zu, still computing flags %zu\n",
                outputPath, emittedImages.size(), programCount, totalPairs, pairIds.size(), flagWriters, liveWriters);
    return 0;
}
