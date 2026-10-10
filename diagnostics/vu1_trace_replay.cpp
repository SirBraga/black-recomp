// Replays VU1 execute() records captured with PS2X_VU_TRACE and compares the
// fast engine against the reference per-pair loop: registers, VU data memory
// and every PATH1 packet. Also reports the recorded in-game result against the
// reference (guards the harness itself) and times both engines.
//
//   ps2x_vu1_trace_replay trace.bin [--engine fast|recompiled] [--bench N] [--bench-engine reference|fast] [--limit K] [--status-mask HEX]
//
// --status-mask masks VU1State::status in the fast-vs-reference comparison
// (default 0xC3F: the sticky Z/S/U/O bits are not maintained when no
// instruction of the image can read them).
#include "runtime/ps2_vu1.h"
#include "runtime/gs/gs_frontend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
    struct Record
    {
        uint32_t startPC = 0, top = 0, itop = 0, maxCycles = 0;
        uint32_t header[8]{}; // as stored, for --rebaseline
        bool resumed = false;
        uint32_t unit = 1;
        VU1State before{}, after{};
        std::vector<uint8_t> dataBefore, dataAfter;
        const std::vector<uint8_t> *code = nullptr; // deduplicated image (stable address)
        uint64_t codeKey = 0;
    };

    struct Packets
    {
        std::vector<std::vector<uint8_t>> list;
        static void sink(void *context, const uint8_t *data, uint32_t size)
        {
            static_cast<Packets *>(context)->list.emplace_back(data, data + size);
        }
    };

    struct Result
    {
        VU1State state{};
        std::vector<uint8_t> data;
        Packets packets;
        uint64_t pairs = 0;
    };

    std::map<uint64_t, std::unique_ptr<std::vector<uint8_t>>> &images();

    bool loadTrace(const char *path, std::vector<Record> &records, size_t limit)
    {
        std::FILE *f = std::fopen(path, "rb");
        if (!f)
            return false;
        for (;;)
        {
            uint32_t header[8];
            if (std::fread(header, sizeof(header), 1, f) != 1)
                break;
            const uint32_t kind = header[0] & 0x00FFFFFFu; // "VUT"/"VUR" + unit digit
            const uint32_t unitDigit = header[0] >> 24;
            if ((kind != 0x00545556u && kind != 0x00525556u) || (unitDigit != '0' && unitDigit != '1') || header[5] != sizeof(VU1State))
            {
                std::fprintf(stderr, "bad record header (magic=%08x state=%u, expected %zu)\n", header[0], header[5], sizeof(VU1State));
                break;
            }
            Record r;
            std::memcpy(r.header, header, sizeof(header));
            r.resumed = kind == 0x00525556u;
            r.unit = unitDigit - '0';
            r.startPC = header[1]; r.top = header[2]; r.itop = header[3]; r.maxCycles = header[4];
            std::vector<uint8_t> code(header[6]);
            r.dataBefore.resize(header[7]); r.dataAfter.resize(header[7]);
            if (std::fread(&r.before, sizeof(VU1State), 1, f) != 1 ||
                std::fread(code.data(), 1, code.size(), f) != code.size() ||
                std::fread(r.dataBefore.data(), 1, r.dataBefore.size(), f) != r.dataBefore.size() ||
                std::fread(&r.after, sizeof(VU1State), 1, f) != 1 ||
                std::fread(r.dataAfter.data(), 1, r.dataAfter.size(), f) != r.dataAfter.size())
                break; // truncated tail (runner killed mid-record)
            uint64_t key = 14695981039346656037ull;
            for (uint8_t byte : code) { key ^= byte; key *= 1099511628211ull; }
            auto &slot = images()[key];
            if (!slot) slot = std::make_unique<std::vector<uint8_t>>(std::move(code));
            r.code = slot.get(); r.codeKey = key;
            records.push_back(std::move(r));
            if (limit && records.size() >= limit)
                break;
        }
        std::fclose(f);
        return true;
    }

    std::map<uint64_t, std::unique_ptr<std::vector<uint8_t>>> &images()
    {
        static std::map<uint64_t, std::unique_ptr<std::vector<uint8_t>>> table;
        return table;
    }

    GS &dummyGs()
    {
        // Never dereferenced: packets go to the sink.
        alignas(16) static unsigned char storage[16];
        return *reinterpret_cast<GS *>(storage);
    }

    void runRecord(VU1Interpreter &vu, const Record &r, Result &out)
    {
        out.packets.list.clear();
        out.data = r.dataBefore;
        vu.state() = r.before;
        vu.setXgkickSink(&Packets::sink, &out.packets);
        vu.setExternalCodeKey(r.codeKey);
        const uint64_t pairsBefore = vu.executedPairs();
        // The interpreter only reads the code image.
        uint8_t *code = const_cast<uint8_t *>(r.code->data());
        if (r.resumed)
        {
            vu.prepareReplay(r.before);
            vu.resume(code, static_cast<uint32_t>(r.code->size()), out.data.data(),
                      static_cast<uint32_t>(out.data.size()), dummyGs(), nullptr, r.top, r.itop, r.maxCycles);
        }
        else
            vu.execute(code, static_cast<uint32_t>(r.code->size()), out.data.data(),
                       static_cast<uint32_t>(out.data.size()), dummyGs(), nullptr, r.startPC, r.top, r.itop, r.maxCycles);
        out.pairs = vu.executedPairs() - pairsBefore;
        out.state = vu.state();
    }

    // Compares the architectural state; returns a description of the first difference.
    std::string diffState(const VU1State &a, const VU1State &b, uint32_t statusMask, bool compareCycles)
    {
        char text[160];
        for (int reg = 0; reg < 32; ++reg)
            for (int c = 0; c < 4; ++c)
                if (std::memcmp(&a.vf[reg][c], &b.vf[reg][c], 4) != 0)
                {
                    uint32_t x, y; std::memcpy(&x, &a.vf[reg][c], 4); std::memcpy(&y, &b.vf[reg][c], 4);
                    std::snprintf(text, sizeof(text), "vf%d.%c %08x != %08x", reg, "xyzw"[c], x, y);
                    return text;
                }
        for (int reg = 0; reg < 16; ++reg)
            if (a.vi[reg] != b.vi[reg])
            {
                std::snprintf(text, sizeof(text), "vi%d %d != %d", reg, a.vi[reg], b.vi[reg]);
                return text;
            }
        auto bits = [](float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; };
#define CHECK_FIELD(name, x, y) \
    if ((x) != (y)) { std::snprintf(text, sizeof(text), name " %08x != %08x", unsigned(x), unsigned(y)); return text; }
        for (int c = 0; c < 4; ++c)
            CHECK_FIELD("acc", bits(a.acc[c]), bits(b.acc[c]))
        CHECK_FIELD("q", bits(a.q), bits(b.q))
        CHECK_FIELD("p", bits(a.p), bits(b.p))
        CHECK_FIELD("i", bits(a.i), bits(b.i))
        CHECK_FIELD("r", a.r, b.r)
        CHECK_FIELD("pc", a.pc, b.pc)
        CHECK_FIELD("mac", a.mac, b.mac)
        CHECK_FIELD("clip", a.clip, b.clip)
        CHECK_FIELD("status", a.status & statusMask, b.status & statusMask)
        CHECK_FIELD("ebit", a.ebit, b.ebit)
        CHECK_FIELD("haltAfterDelaySlot", a.haltAfterDelaySlot, b.haltAfterDelaySlot)
        CHECK_FIELD("stoppedByD", a.stoppedByD, b.stoppedByD)
        CHECK_FIELD("stoppedByT", a.stoppedByT, b.stoppedByT)
        CHECK_FIELD("branchPending", a.branchPending, b.branchPending)
        if (a.branchPending)
        {
            CHECK_FIELD("branchTarget", a.branchTarget, b.branchTarget)
            CHECK_FIELD("branchDelay", a.branchDelay, b.branchDelay)
        }
        if (compareCycles)
            CHECK_FIELD("cycles", uint32_t(a.cycles), uint32_t(b.cycles))
#undef CHECK_FIELD
        return {};
    }

    std::string diffResult(const Result &a, const Result &b, uint32_t statusMask)
    {
        std::string d = diffState(a.state, b.state, statusMask, true);
        if (!d.empty())
            return d;
        if (a.data != b.data)
        {
            for (size_t i = 0; i < a.data.size(); ++i)
                if (a.data[i] != b.data[i])
                {
                    // First differing qword, both versions, and how many qwords differ in all.
                    const size_t q = i & ~size_t{15};
                    uint32_t wa[4], wb[4];
                    std::memcpy(wa, &a.data[q], 16);
                    std::memcpy(wb, &b.data[q], 16);
                    size_t count = 0;
                    for (size_t j = 0; j + 16 <= a.data.size(); j += 16)
                        count += std::memcmp(&a.data[j], &b.data[j], 16) != 0;
                    char text[200];
                    std::snprintf(text, sizeof(text), "data memory differs at 0x%zx: %08x %08x %08x %08x != %08x %08x %08x %08x (%zu qwords differ)",
                                  q, wa[0], wa[1], wa[2], wa[3], wb[0], wb[1], wb[2], wb[3], count);
                    return text;
                }
        }
        if (a.packets.list.size() != b.packets.list.size())
            return "packet count " + std::to_string(a.packets.list.size()) + " != " + std::to_string(b.packets.list.size());
        for (size_t i = 0; i < a.packets.list.size(); ++i)
            if (a.packets.list[i] != b.packets.list[i])
                return "packet " + std::to_string(i) + " differs (sizes " + std::to_string(a.packets.list[i].size()) +
                       "/" + std::to_string(b.packets.list[i].size()) + ")";
        return {};
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s trace.bin [--bench N] [--limit K] [--status-mask HEX] [--rebaseline OUT]\n"
                             "  --rebaseline OUT: write the trace again with the reference engine's results as the expected\n"
                             "                    ones (after a deliberate change of the reference timing model)\n", argv[0]);
        return 2;
    }
    int bench = 0;
    const char *exportDir = nullptr;
    const char *rebaselinePath = nullptr;
    int benchEngine = -1; // --bench-engine reference|fast
    int independence = 0; // --independence K: see below
    VU1Interpreter::Engine candidate = VU1Interpreter::Engine::Fast; // --engine fast|recompiled
    size_t limit = 0;
    uint32_t statusMask = 0xC3Fu;
    for (int i = 2; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--bench") && i + 1 < argc) bench = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--bench-engine") && i + 1 < argc) benchEngine = !std::strcmp(argv[++i], "fast") ? 1 : 0;
        else if (!std::strcmp(argv[i], "--engine") && i + 1 < argc) candidate = !std::strcmp(argv[++i], "recompiled") ? VU1Interpreter::Engine::Recompiled : VU1Interpreter::Engine::Fast;
        else if (!std::strcmp(argv[i], "--export") && i + 1 < argc) exportDir = argv[++i];
        else if (!std::strcmp(argv[i], "--rebaseline") && i + 1 < argc) rebaselinePath = argv[++i];
        else if (!std::strcmp(argv[i], "--limit") && i + 1 < argc) limit = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--independence") && i + 1 < argc) independence = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--status-mask") && i + 1 < argc) statusMask = std::strtoul(argv[++i], nullptr, 16);
    }
    std::vector<Record> records;
    if (!loadTrace(argv[1], records, limit) || records.empty())
    {
        std::fprintf(stderr, "no records in %s\n", argv[1]);
        return 2;
    }

    std::unique_ptr<VU1Interpreter> references[2], candidates[2];
    for (uint32_t unit = 0; unit < 2u; ++unit)
    {
        const auto kind = unit == 0u ? VU1Interpreter::Unit::VU0 : VU1Interpreter::Unit::VU1;
        references[unit] = std::make_unique<VU1Interpreter>(kind);
        candidates[unit] = std::make_unique<VU1Interpreter>(kind);
        references[unit]->reset(); candidates[unit]->reset();
        references[unit]->setEngine(VU1Interpreter::Engine::Reference);
        candidates[unit]->setEngine(candidate);
    }

    // --independence K (the trace must be contiguous, PS2X_VU_TRACE_STRIDE=1): how much does a VU1 run depend on
    // the K runs before it? Each run is executed twice, once as recorded and once from the state it would have had
    // if those K runs had not happened: every register and data byte it inherited unchanged from the previous run
    // is put back to its value before them, while what was written in between from outside (VIF unpacks) stays.
    // A run whose packets are the same either way could have started before those runs finished; if what it writes
    // (registers, data memory) is the same too, nothing downstream could tell. This is the measurement behind the
    // question of spreading VU1 runs over several cores.
    if (independence > 0)
    {
        VU1Interpreter &vu = *candidates[1];
        const size_t distance = static_cast<size_t>(independence);
        uint64_t totalPairs = 0, outputPairs = 0, fullPairs = 0;
        size_t total = 0, outputSame = 0, fullSame = 0, shown = 0;
        size_t byRegisters = 0, byMemory = 0, byFlags = 0;
        std::map<uint32_t, std::pair<size_t, size_t>> byEntry; // PC where the run starts or continues -> runs, dependent runs
        Result real, alone;
        for (size_t n = distance; n < records.size(); ++n)
        {
            const Record &r = records[n], &previous = records[n - 1], &base = records[n - distance];
            // Black's VU1 work is one long program that stops after each batch and is continued (MSCNT) for the next:
            // nearly every record is such a continuation, and those are the batches of interest. The PC where a run
            // continues is taken as known (it is where the previous one stopped).
            bool usable = r.unit == 1u;
            for (size_t k = n - distance; k < n && usable; ++k)
                usable = records[k].unit == 1u;
            if (!usable)
                continue;
            Record counterfactual = r;
            {
                const uint32_t *now = reinterpret_cast<const uint32_t *>(&r.before), *inherited = reinterpret_cast<const uint32_t *>(&previous.after),
                               *before = reinterpret_cast<const uint32_t *>(&base.before);
                uint32_t *target = reinterpret_cast<uint32_t *>(&counterfactual.before);
                // vf, vi, acc, q, p, i, r: the registers a program computes with; then MAC, clip and status flags.
                const size_t registerWords = offsetof(VU1State, pc) / 4u;
                for (size_t w = 0; w < registerWords; ++w)
                    if (now[w] == inherited[w])
                        target[w] = before[w];
                if (r.before.mac == previous.after.mac) counterfactual.before.mac = base.before.mac;
                if (r.before.clip == previous.after.clip) counterfactual.before.clip = base.before.clip;
                if (r.before.status == previous.after.status) counterfactual.before.status = base.before.status;
                for (size_t b = 0; b < r.dataBefore.size() && b < previous.dataAfter.size(); ++b)
                    if (r.dataBefore[b] == previous.dataAfter[b])
                        counterfactual.dataBefore[b] = base.dataBefore[b];
            }
            runRecord(vu, r, real);
            runRecord(vu, counterfactual, alone);
            ++total;
            totalPairs += real.pairs;
            auto &entry = byEntry[r.resumed ? r.before.pc : r.startPC];
            ++entry.first;
            const bool sameOutput = real.packets.list == alone.packets.list;
            // What the run itself wrote has to come out the same; what it only passed through does not count.
            bool sameRegisters = true, sameFlags = true, sameMemory = true;
            {
                const uint32_t *a = reinterpret_cast<const uint32_t *>(&real.state), *b = reinterpret_cast<const uint32_t *>(&alone.state);
                const uint32_t *ia = reinterpret_cast<const uint32_t *>(&r.before), *ib = reinterpret_cast<const uint32_t *>(&counterfactual.before);
                for (size_t w = 0; w < offsetof(VU1State, pc) / 4u; ++w)
                    if ((a[w] != ia[w] || b[w] != ib[w]) && a[w] != b[w])
                        sameRegisters = false;
                const uint32_t mask = statusMask;
                if ((real.state.mac != r.before.mac || alone.state.mac != counterfactual.before.mac) && real.state.mac != alone.state.mac) sameFlags = false;
                if ((real.state.clip != r.before.clip || alone.state.clip != counterfactual.before.clip) && real.state.clip != alone.state.clip) sameFlags = false;
                if (((real.state.status ^ alone.state.status) & mask) != 0u && ((real.state.status ^ r.before.status) & mask) != 0u) sameFlags = false;
                for (size_t i = 0; i < real.data.size(); ++i)
                    if ((real.data[i] != r.dataBefore[i] || alone.data[i] != counterfactual.dataBefore[i]) && real.data[i] != alone.data[i])
                    {
                        sameMemory = false;
                        break;
                    }
            }
            if (sameOutput)
            {
                ++outputSame;
                outputPairs += real.pairs;
            }
            else
                ++entry.second;
            if (sameOutput && sameRegisters && sameFlags && sameMemory)
            {
                ++fullSame;
                fullPairs += real.pairs;
            }
            else if (sameOutput)
            {
                byRegisters += sameRegisters ? 0u : 1u;
                byFlags += sameFlags ? 0u : 1u;
                byMemory += sameMemory ? 0u : 1u;
            }
            if (!sameOutput && shown++ < 8)
                std::printf("run %zu (pc=0x%x, %llu pairs, %zu packets): output depends on the previous %zu run(s)\n", n, r.startPC,
                            static_cast<unsigned long long>(real.pairs), real.packets.list.size(), distance);
        }
        std::printf("independence from the previous %zu run(s), %zu runs, %llu pairs:\n", distance, total, static_cast<unsigned long long>(totalPairs));
        std::printf("  same packets:                    %6.2f%% of the runs, %6.2f%% of the pairs\n", 100.0 * outputSame / std::max<size_t>(total, 1), 100.0 * outputPairs / std::max<uint64_t>(totalPairs, 1));
        std::printf("  same packets and same own writes: %6.2f%% of the runs, %6.2f%% of the pairs\n", 100.0 * fullSame / std::max<size_t>(total, 1), 100.0 * fullPairs / std::max<uint64_t>(totalPairs, 1));
        std::printf("  same packets but different writes: registers %zu, flags %zu, data memory %zu runs\n", byRegisters, byFlags, byMemory);
        for (const auto &entry : byEntry)
            std::printf("  entry 0x%04x: %zu runs, %zu with dependent output\n", entry.first, entry.second.first, entry.second.second);
        return 0;
    }

    // --export DIR: write every image of the trace as DIR/vu1-<fnv>.bin plus
    // DIR/coverage.bin (PS2X_VU_CATALOG layout) for ps2x_vu1_recompile.
    std::map<std::pair<uint32_t, uint64_t>, std::vector<uint8_t>> coverageMaps;
    Result a, b;
    std::FILE *rebaseline = rebaselinePath ? std::fopen(rebaselinePath, "wb") : nullptr;
    size_t fastMismatch = 0, recordedMismatch = 0;
    uint64_t pairs = 0, packets = 0;
    for (size_t n = 0; n < records.size(); ++n)
    {
        const Record &r = records[n];
        VU1Interpreter *reference = references[r.unit].get();
        VU1Interpreter *fast = candidates[r.unit].get();
        if (exportDir)
        {
            auto &map = coverageMaps[{r.unit, r.codeKey}];
            map.resize(0x800u);
            reference->setCoverageMap(map.data());
        }
        runRecord(*reference, r, a);
        reference->setCoverageMap(nullptr);
        if (rebaseline)
        {
            std::fwrite(r.header, sizeof(r.header), 1, rebaseline);
            std::fwrite(&r.before, sizeof(VU1State), 1, rebaseline);
            std::fwrite(r.code->data(), 1, r.code->size(), rebaseline);
            std::fwrite(r.dataBefore.data(), 1, r.dataBefore.size(), rebaseline);
            std::fwrite(&a.state, sizeof(VU1State), 1, rebaseline);
            std::fwrite(a.data.data(), 1, a.data.size(), rebaseline);
        }
        runRecord(*fast, r, b);
        pairs += a.pairs; packets += a.packets.list.size();
        const std::string d = diffResult(a, b, statusMask);
        if (!d.empty() && fastMismatch++ < 12)
            std::printf("record %zu (pc=0x%x pairs=%llu): fast != reference: %s\n", n, r.startPC,
                        static_cast<unsigned long long>(a.pairs), d.c_str());
        std::string g = diffState(a.state, r.after, 0xFFFu, false);
        if (g.empty() && a.data != r.dataAfter)
            g = "data memory";
        if (!g.empty() && recordedMismatch++ < 6)
            std::printf("record %zu (pc=0x%x): reference != in-game result: %s\n", n, r.startPC, g.c_str());
    }
    if (rebaseline)
    {
        std::fclose(rebaseline);
        std::printf("rebaselined %zu records to %s\n", records.size(), rebaselinePath);
    }
    std::printf("records=%zu pairs=%llu packets=%llu fast-vs-reference mismatches=%zu reference-vs-recorded mismatches=%zu (status mask %03x)\n",
                records.size(), static_cast<unsigned long long>(pairs), static_cast<unsigned long long>(packets),
                fastMismatch, recordedMismatch, statusMask);
    for (unsigned unit = 0; unit < 2u; ++unit)
        if (candidates[unit] && candidates[unit]->directPairs() != 0u)
            std::printf("VU%u: %.1f%% of the pairs ran in direct blocks\n", unit,
                        100.0 * double(candidates[unit]->directPairs()) / double(std::max<uint64_t>(candidates[unit]->executedPairs(), 1u)));

    if (exportDir)
    {
        const std::string dir = exportDir;
        std::FILE *coverage = std::fopen((dir + "/coverage.bin").c_str(), "wb");
        for (const auto &entry : coverageMaps)
        {
            char name[64];
            std::snprintf(name, sizeof(name), "/vu%u-%016llx.bin", entry.first.first, static_cast<unsigned long long>(entry.first.second));
            const std::vector<uint8_t> &image = *images()[entry.first.second];
            if (std::FILE *f = std::fopen((dir + name).c_str(), "wb"))
            {
                std::fwrite(image.data(), 1, image.size(), f);
                std::fclose(f);
            }
            for (uint32_t pair = 0; coverage && pair < entry.second.size(); ++pair)
                if (entry.second[pair])
                {
                    const uint32_t head[2] = {entry.first.first, pair * 8u};
                    std::fwrite(head, sizeof(head), 1, coverage);
                    std::fwrite(&entry.first.second, 8, 1, coverage);
                }
        }
        if (coverage) std::fclose(coverage);
        std::printf("exported %zu images to %s\n", coverageMaps.size(), exportDir);
    }

    if (bench > 0)
    {
        // The game keeps one image active for long stretches; replaying the
        // sampled records in capture order would rebuild the decode cache on
        // almost every run and measure that instead of execution.
        std::stable_sort(records.begin(), records.end(),
                         [](const Record &x, const Record &y) { return x.codeKey < y.codeKey; });
        for (int engine = 0; engine < 2; ++engine)
        {
            if (benchEngine >= 0 && benchEngine != engine)
                continue;
            double best = 1e30;
            for (int round = 0; round < 3; ++round)
            {
                const auto t0 = std::chrono::steady_clock::now();
                for (int rep = 0; rep < bench; ++rep)
                    for (const Record &r : records)
                        runRecord(engine == 0 ? *references[r.unit] : *candidates[r.unit], r, a);
                best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
            std::printf("%s: %.3f s for %d passes, %.2f ns/pair (includes %zu-byte data reset per run), images=%zu\n",
                        engine == 0 ? "reference" : "fast", best, bench,
                        best * 1e9 / (double(pairs) * bench), records[0].dataBefore.size(), images().size());
        }
    }
    return fastMismatch == 0 ? 0 : 1;
}
