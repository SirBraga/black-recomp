// GS front end of the native renderer: register state, GIF packet decoding, the vertex queue and host
// transfers into the CPU copy of VRAM. Everything that draws is handed to a Sink, which owns the GPU side.
#pragma once

#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <arm_acle.h>
#include <bitset>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gsn
{
    // Where a pixel of each format lives in VRAM, as one table lookup: the position inside a page does not depend
    // on the buffer, only the page does. Built from (and checked against) the runtime's swizzle tables.
    class Swizzle
    {
    public:
        uint32_t bits = 0; // per pixel: 32, 24, 16, 8 or 4

        static const Swizzle &of(uint32_t psm)
        {
            static Swizzle all[64];
            Swizzle &entry = all[psm & 63u];
            if (entry.table.empty())
                entry.build(psm & 63u);
            return entry;
        }

        uint32_t bitAddress(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const
        {
            const uint32_t page = (y >> heightShift) * (wide ? std::max(bw >> 1, 1u) : bw) + (x >> widthShift);
            return (((bp + (page << 5)) << 11) + table[((y & heightMask) << widthShift) | (x & widthMask)]) & 0x1FFFFFFu;
        }

        // Table entries for the pixels from (x, y) to the end of their 8-pixel cell: same page, same block.
        const uint32_t *cell(uint32_t x, uint32_t y) const { return &table[((y & heightMask) << widthShift) | (x & widthMask)]; }

        uint32_t read(const uint8_t *vram, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const
        {
            const uint32_t at = bitAddress(bp, bw, x, y);
            const uint8_t *p = vram + (at >> 3);
            switch (bits)
            {
            case 32: { uint32_t v; std::memcpy(&v, p, 4); return v; }
            case 24: return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
            case 16: { uint16_t v; std::memcpy(&v, p, 2); return v; }
            case 8: return *p;
            default: return (*p >> (at & 4u)) & 15u;
            }
        }

        // Returns whether the pixel changed.
        bool write(uint8_t *vram, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) const
        {
            const uint32_t at = bitAddress(bp, bw, x, y);
            uint8_t *p = vram + (at >> 3);
            switch (bits)
            {
            case 32: { uint32_t v; std::memcpy(&v, p, 4); if (v == value) return false; std::memcpy(p, &value, 4); return true; }
            case 24:
                if (p[0] == uint8_t(value) && p[1] == uint8_t(value >> 8) && p[2] == uint8_t(value >> 16)) return false;
                p[0] = uint8_t(value); p[1] = uint8_t(value >> 8); p[2] = uint8_t(value >> 16);
                return true;
            case 16: { uint16_t v; std::memcpy(&v, p, 2); if (v == uint16_t(value)) return false; v = uint16_t(value); std::memcpy(p, &v, 2); return true; }
            case 8: if (*p == uint8_t(value)) return false; *p = uint8_t(value); return true;
            default:
            {
                const uint32_t shift = at & 4u;
                const uint8_t merged = uint8_t((*p & ~(15u << shift)) | ((value & 15u) << shift));
                if (merged == *p) return false;
                *p = merged;
                return true;
            }
            }
        }

    private:
        std::vector<uint32_t> table;
        uint32_t widthShift = 6, heightShift = 5, widthMask = 63, heightMask = 31;
        bool wide = false;

        void build(uint32_t psm)
        {
            const bool narrow = psm == 0x02u || psm == 0x0Au || psm == 0x32u || psm == 0x3Au;
            bits = psm == 0x13u ? 8u : psm == 0x14u ? 4u : psm == 0x1Bu ? 8u : psm == 0x24u || psm == 0x2Cu ? 4u : narrow ? 16u : psm == 0x01u || psm == 0x31u ? 24u : 32u;
            wide = psm == 0x13u || psm == 0x14u;
            widthShift = wide ? 7u : 6u;
            heightShift = psm == 0x14u ? 7u : narrow || psm == 0x13u ? 6u : 5u;
            widthMask = (1u << widthShift) - 1u;
            heightMask = (1u << heightShift) - 1u;
            table.resize(size_t(1u) << (widthShift + heightShift));
            for (uint32_t y = 0; y <= heightMask; ++y)
                for (uint32_t x = 0; x <= widthMask; ++x)
                    table[(y << widthShift) | x] = GSMem::PixelBitAddress(psm, 0u, 2u, x, y);
            // Any disagreement with the reference tables would silently corrupt textures: check a spread of cases.
            uint32_t seed = 12345u + psm;
            for (uint32_t i = 0; i < 20000u; ++i)
            {
                seed = seed * 1664525u + 1013904223u;
                const uint32_t bp = (seed >> 8) & 0x3FFFu, bw = wide ? 2u * (1u + ((seed >> 4) & 7u)) : 1u + ((seed >> 4) & 15u);
                seed = seed * 1664525u + 1013904223u;
                const uint32_t x = (seed >> 6) & 1023u, y = (seed >> 18) & 1023u;
                if (x >= bw * 64u)
                    continue;
                if (bitAddress(bp, bw, x, y) != (GSMem::PixelBitAddress(psm, bp, bw, x, y) & 0x1FFFFFFu))
                {
                    std::fprintf(stderr, "[native-gs] swizzle table mismatch: psm %02x bp %u bw %u at %u,%u\n", psm, bp, bw, x, y);
                    std::abort();
                }
            }
        }
    };

    struct Context
    {
        uint64_t tex0 = 0, tex1 = 0, clamp = 0, xyoffset = 0, miptbp1 = 0, miptbp2 = 0, scissor = 0, alpha = 0, test = 0, fba = 0, frame = 0, zbuf = 0;
    };

    struct Registers
    {
        Context ctx[2];
        uint64_t prim = 0, prmode = 0, prmodecont = 1, rgbaq = 0, uv = 0, fog = 0, texclut = 0, texa = 0, fogcol = 0, dimx = 0, dthe = 0, colclamp = 1,
                 pabe = 0, scanmsk = 0, bitbltbuf = 0, trxpos = 0, trxreg = 0, trxdir = 3;
        float s = 0.0f, t = 0.0f, q = 1.0f;

        // PRIM with the mode bits taken from PRMODE when PRMODECONT says so.
        uint32_t mode() const { return (prmodecont & 1u) != 0u ? uint32_t(prim) & 0x7FFu : (uint32_t(prim) & 7u) | (uint32_t(prmode) & 0x7F8u); }
    };

    struct Vertex
    {
        int32_t x, y; // 12.4 fixed point, window coordinates (XYOFFSET not yet removed)
        uint32_t z;
        uint32_t rgba;
        float s, t, q;
        uint16_t u, v; // 12.4 texel coordinates when FST
        uint8_t fog;
    };

    class Sink
    {
    public:
        virtual ~Sink() = default;
        // A register that takes part in drawing is about to change: what was queued with the old value has to go out.
        virtual void stateChanging() = 0;
        virtual void triangle(const Vertex &a, const Vertex &b, const Vertex &c) = 0;
        virtual void sprite(const Vertex &a, const Vertex &b) = 0;
        virtual void line(const Vertex &a, const Vertex &b) = 0;
        virtual void point(const Vertex &a) = 0;
        // Pixels were changed in VRAM by a host transfer; blocks are 256 bytes.
        virtual void vramWritten(const std::vector<uint16_t> &blocks) = 0;
        // A host transfer ended: where it went, a hash of what was sent and the blocks it covers (changed or not).
        virtual void uploadFinished(uint32_t dbp, bool atOrigin, uint64_t hash, uint64_t rectKey, std::vector<uint16_t> &blocks) = 0;
        // The same bytes are about to be sent to the same rectangle again: true when VRAM still holds them there
        // (nothing wrote or drew over it since), so the transfer can be dropped.
        virtual bool uploadUnchanged(uint32_t dbp, bool atOrigin, uint64_t hash, uint64_t rectKey) = 0;
    };

    class Frontend
    {
    public:
        Registers regs;
        uint8_t *vram = nullptr;
        Sink *sink = nullptr;

        void reset()
        {
            regs = Registers{};
            queued = 0;
            for (Path &path : paths)
                path = Path{};
            transferActive = false;
        }

        void writeRegister(uint32_t address, uint64_t value)
        {
            switch (address)
            {
            case 0x00: // PRIM
                if ((regs.prim ^ value) & 0x7FFu)
                    sink->stateChanging();
                regs.prim = value & 0x7FFu;
                queued = 0;
                break;
            case 0x01: regs.rgbaq = value; std::memcpy(&regs.q, reinterpret_cast<const uint8_t *>(&value) + 4, 4); break;
            case 0x02: std::memcpy(&regs.s, &value, 4); std::memcpy(&regs.t, reinterpret_cast<const uint8_t *>(&value) + 4, 4); break;
            case 0x03: regs.uv = value; break;
            case 0x04: kick(uint32_t(value) & 0xFFFFu, uint32_t(value >> 16) & 0xFFFFu, uint32_t(value >> 32) & 0xFFFFFFu, uint8_t(value >> 56), true); break;
            case 0x05: kick(uint32_t(value) & 0xFFFFu, uint32_t(value >> 16) & 0xFFFFu, uint32_t(value >> 32), uint8_t(regs.fog >> 56), true); break;
            case 0x06: case 0x07: set(regs.ctx[address - 0x06].tex0, value); break;
            case 0x08: case 0x09: set(regs.ctx[address - 0x08].clamp, value); break;
            case 0x0A: regs.fog = value; break;
            case 0x0C: kick(uint32_t(value) & 0xFFFFu, uint32_t(value >> 16) & 0xFFFFu, uint32_t(value >> 32) & 0xFFFFFFu, uint8_t(value >> 56), false); break;
            case 0x0D: kick(uint32_t(value) & 0xFFFFu, uint32_t(value >> 16) & 0xFFFFu, uint32_t(value >> 32), uint8_t(regs.fog >> 56), false); break;
            case 0x14: case 0x15: set(regs.ctx[address - 0x14].tex1, value); break;
            case 0x16: case 0x17: // TEX2: the format and palette fields of TEX0
            {
                const uint64_t mask = (0x3Full << 20) | (0x7FFFFFFull << 37);
                uint64_t &tex0 = regs.ctx[address - 0x16].tex0;
                set(tex0, (tex0 & ~mask) | (value & mask));
                break;
            }
            case 0x18: case 0x19: set(regs.ctx[address - 0x18].xyoffset, value); break;
            case 0x1A: set(regs.prmodecont, value & 1u); break;
            case 0x1B: set(regs.prmode, value & 0x7F8u); break;
            case 0x1C: set(regs.texclut, value); break;
            case 0x22: set(regs.scanmsk, value); break;
            case 0x34: case 0x35: set(regs.ctx[address - 0x34].miptbp1, value); break;
            case 0x36: case 0x37: set(regs.ctx[address - 0x36].miptbp2, value); break;
            case 0x3B: set(regs.texa, value); break;
            case 0x3D: set(regs.fogcol, value); break;
            case 0x3F: break; // TEXFLUSH: textures are validated against VRAM page generations instead
            case 0x40: case 0x41: set(regs.ctx[address - 0x40].scissor, value); break;
            case 0x42: case 0x43: set(regs.ctx[address - 0x42].alpha, value); break;
            case 0x44: set(regs.dimx, value); break;
            case 0x45: set(regs.dthe, value); break;
            case 0x46: set(regs.colclamp, value); break;
            case 0x47: case 0x48: set(regs.ctx[address - 0x47].test, value); break;
            case 0x49: set(regs.pabe, value); break;
            case 0x4A: case 0x4B: set(regs.ctx[address - 0x4A].fba, value); break;
            case 0x4C: case 0x4D: set(regs.ctx[address - 0x4C].frame, value); break;
            case 0x4E: case 0x4F: set(regs.ctx[address - 0x4E].zbuf, value); break;
            case 0x50: regs.bitbltbuf = value; break;
            case 0x51: regs.trxpos = value; break;
            case 0x52: regs.trxreg = value; break;
            case 0x53: regs.trxdir = value & 3u; beginTransfer(); break;
            case 0x54: transferData(reinterpret_cast<const uint8_t *>(&value), 8); break;
            default: break;
            }
        }

        void gif(uint32_t pathIndex, const uint8_t *data, uint32_t size)
        {
            Path &path = paths[pathIndex & 3u];
            for (uint32_t at = 0; at + 16u <= size;)
            {
                if (path.left == 0u)
                {
                    uint64_t lo, hi;
                    std::memcpy(&lo, data + at, 8);
                    std::memcpy(&hi, data + at + 8u, 8);
                    at += 16u;
                    const uint32_t nloop = uint32_t(lo) & 0x7FFFu;
                    path.flg = uint32_t(lo >> 58) & 3u;
                    path.nreg = uint32_t(lo >> 60) & 15u;
                    if (path.nreg == 0u)
                        path.nreg = 16u;
                    path.regs = hi;
                    path.index = 0;
                    if (nloop == 0u)
                        continue;
                    if (((lo >> 46) & 1u) != 0u && path.flg != 3u)
                        writeRegister(0x00, (lo >> 47) & 0x7FFu);
                    path.left = path.flg < 2u ? nloop * path.nreg : nloop;
                    if (path.flg == 0u)
                        packedQ = 1.0f;
                    continue;
                }
                if (path.flg == 0u)
                {
                    // PACKED: one register per quadword.
                    uint32_t count = std::min(path.left, (size - at) / 16u);
                    path.left -= count;
                    for (; count != 0u; --count, at += 16u)
                    {
                        const uint32_t descriptor = uint32_t(path.regs >> (4u * path.index)) & 15u;
                        if (++path.index == path.nreg)
                            path.index = 0;
                        uint64_t lo, hi;
                        std::memcpy(&lo, data + at, 8);
                        std::memcpy(&hi, data + at + 8u, 8);
                        switch (descriptor)
                        {
                        case 0x1: // RGBAQ, Q as latched by the last ST
                            regs.rgbaq = (lo & 0xFFu) | ((lo >> 24) & 0xFF00u) | ((hi & 0xFFu) << 16) | (((hi >> 32) & 0xFFu) << 24);
                            regs.q = packedQ;
                            break;
                        case 0x2:
                            std::memcpy(&regs.s, data + at, 4);
                            std::memcpy(&regs.t, data + at + 4u, 4);
                            std::memcpy(&packedQ, data + at + 8u, 4);
                            break;
                        case 0x3: regs.uv = (lo & 0x3FFFu) | (((lo >> 32) & 0x3FFFu) << 16); break;
                        case 0x4: kick(uint32_t(lo) & 0xFFFFu, uint32_t(lo >> 32) & 0xFFFFu, uint32_t(hi >> 4) & 0xFFFFFFu, uint8_t(hi >> 36), ((hi >> 47) & 1u) == 0u); break;
                        case 0x5: kick(uint32_t(lo) & 0xFFFFu, uint32_t(lo >> 32) & 0xFFFFu, uint32_t(hi), uint8_t(regs.fog >> 56), ((hi >> 47) & 1u) == 0u); break;
                        case 0xA: regs.fog = (hi >> 36 & 0xFFu) << 56; break;
                        case 0xC: kick(uint32_t(lo) & 0xFFFFu, uint32_t(lo >> 32) & 0xFFFFu, uint32_t(hi >> 4) & 0xFFFFFFu, uint8_t(hi >> 36), false); break;
                        case 0xD: kick(uint32_t(lo) & 0xFFFFu, uint32_t(lo >> 32) & 0xFFFFu, uint32_t(hi), uint8_t(regs.fog >> 56), false); break;
                        case 0xE: writeRegister(uint32_t(hi) & 0xFFu, lo); break;
                        case 0xF: break;
                        default: writeRegister(descriptor, lo); break;
                        }
                    }
                }
                else if (path.flg == 1u)
                {
                    // REGLIST: two 64-bit register values per quadword.
                    for (uint32_t half = 0; half < 2u && path.left != 0u; ++half, --path.left)
                    {
                        const uint32_t descriptor = uint32_t(path.regs >> (4u * path.index)) & 15u;
                        if (++path.index == path.nreg)
                            path.index = 0;
                        uint64_t value;
                        std::memcpy(&value, data + at + 8u * half, 8);
                        if (descriptor != 0xEu && descriptor != 0xFu)
                            writeRegister(descriptor, value);
                    }
                    at += 16u;
                }
                else
                {
                    const uint32_t count = std::min(path.left, (size - at) / 16u);
                    if (path.flg == 2u)
                        transferData(data + at, count * 16u);
                    path.left -= count;
                    at += count * 16u;
                }
            }
        }

        // The transfer is gathered whole before it touches VRAM: the game sends its textures again every frame, and
        // most of the time the same bytes go to the same place, which is recognized by hash and skipped.
        void transferData(const uint8_t *data, uint32_t bytes)
        {
            if (!transferActive)
                return;
            const size_t take = std::min<size_t>(bytes, transferExpected - pendingTransfer.size());
            pendingTransfer.insert(pendingTransfer.end(), data, data + take);
            if (pendingTransfer.size() < transferExpected)
                return;
            commitTransfer();
        }

    private:
        uint64_t transferRectKey() const
        {
            return ((regs.bitbltbuf >> 32) * 0x9E3779B97F4A7C15ull) ^ ((regs.trxpos >> 32) * 0xD6E8FEB86659FD93ull) ^ regs.trxreg;
        }

        void commitTransfer()
        {
            if (pendingTransfer.empty())
            {
                transferActive = false;
                return;
            }
            // Two interleaved CRC32C lanes (hardware instruction): megabytes of texture data go through this every frame.
            uint32_t lane0 = uint32_t(transferHash), lane1 = uint32_t(transferHash >> 32);
            size_t at = 0;
            for (; at + 16u <= pendingTransfer.size(); at += 16u)
            {
                uint64_t first, second;
                std::memcpy(&first, pendingTransfer.data() + at, 8);
                std::memcpy(&second, pendingTransfer.data() + at + 8u, 8);
                lane0 = __builtin_arm_crc32cd(lane0, first);
                lane1 = __builtin_arm_crc32cd(lane1, second);
            }
            for (; at < pendingTransfer.size(); ++at)
                lane0 = __builtin_arm_crc32cb(lane0, pendingTransfer[at]);
            const uint64_t hash = (uint64_t(lane1) << 32 | lane0) ^ (uint64_t(pendingTransfer.size()) * 0x9E3779B97F4A7C15ull);
            const uint32_t dbp = uint32_t(regs.bitbltbuf >> 32) & 0x3FFFu;
            const bool atOrigin = ((regs.trxpos >> 32) & 0x7FFu) == 0u && ((regs.trxpos >> 48) & 0x7FFu) == 0u;
            if (pendingTransfer.size() == transferExpected && sink->uploadUnchanged(dbp, atOrigin, hash, transferRectKey()))
                transferActive = false;
            else
            {
                transferHash = hash;
                writeTransfer(pendingTransfer.data(), uint32_t(pendingTransfer.size()));
                transferActive = false;
            }
            pendingTransfer.clear();
        }

        void writeTransfer(const uint8_t *data, uint32_t bytes)
        {
            const uint32_t dbp = uint32_t(regs.bitbltbuf >> 32) & 0x3FFFu, dbw = std::max<uint32_t>(uint32_t(regs.bitbltbuf >> 48) & 0x3Fu, 1u), psm = uint32_t(regs.bitbltbuf >> 56) & 0x3Fu;
            const uint32_t dx = uint32_t(regs.trxpos >> 32) & 0x7FFu, dy = uint32_t(regs.trxpos >> 48) & 0x7FFu;
            const uint32_t width = uint32_t(regs.trxreg) & 0xFFFu, height = uint32_t(regs.trxreg >> 32) & 0xFFFu;
            // Exactly the pages written: a narrow image in a wide buffer skips pages between its rows.
            std::vector<uint16_t> &changed = changedBlocks;
            changed.clear();
            // The game sends its textures again every frame: pixels that already hold the value are left alone, so
            // that nothing downstream (decoded textures, render targets) is invalidated by an upload that changes nothing.
            const Swizzle &layout = Swizzle::of(psm);
            uint32_t cellX = ~0u, cellY = ~0u, block = 0;
            bool cellChanged = false;
            const auto put = [&](uint32_t value)
            {
                const uint32_t x = (dx + transferX) & 2047u, y = (dy + transferY) & 2047u;
                // Blocks are at least 8x8 pixels in every format.
                if ((x >> 3) != cellX || (y >> 3) != cellY)
                {
                    cellX = x >> 3;
                    cellY = y >> 3;
                    cellChanged = false;
                    block = layout.bitAddress(dbp, dbw, x, y) >> 11;
                    coveredWords[block >> 6] |= 1ull << (block & 63u);
                }
                if (layout.write(vram, dbp, dbw, x, y, value) && !cellChanged)
                {
                    cellChanged = true;
                    if (changed.empty() || changed.back() != block)
                        changed.push_back(uint16_t(block));
                }
                if (++transferX == width)
                {
                    transferX = 0;
                    if (++transferY == height)
                        transferActive = false;
                }
            };
            const uint32_t bits = psm == 0x00u || psm == 0x30u ? 32u : psm == 0x01u || psm == 0x31u ? 24u : psm == 0x02u || psm == 0x0Au || psm == 0x32u || psm == 0x3Au ? 16u
                                : psm == 0x13u || psm == 0x1Bu ? 8u : psm == 0x14u || psm == 0x24u || psm == 0x2Cu ? 4u : 0u;
            if (bits == 0u || width == 0u || height == 0u)
            {
                transferActive = false;
                return;
            }
            if (bits == 24u)
            {
                for (uint32_t i = 0; i < bytes && transferActive; ++i)
                {
                    carry[carried++] = data[i];
                    if (carried == 3u)
                    {
                        put(uint32_t(carry[0]) | (uint32_t(carry[1]) << 8) | (uint32_t(carry[2]) << 16));
                        carried = 0;
                    }
                }
            }
            else if (bits == 4u)
            {
                for (uint32_t i = 0; i < bytes && transferActive; ++i)
                {
                    put(data[i] & 15u);
                    if (transferActive)
                        put(data[i] >> 4);
                }
            }
            else if (bits == 32u)
                copyCells<uint32_t>(layout, data, bytes, dbp, dbw, dx, dy, width, height, changed);
            else if (bits == 16u)
                copyCells<uint16_t>(layout, data, bytes, dbp, dbw, dx, dy, width, height, changed);
            else
                copyCells<uint8_t>(layout, data, bytes, dbp, dbw, dx, dy, width, height, changed);
            if (!changed.empty())
                sink->vramWritten(changed);
            // Sorted and unique straight from the bitmap (the pixel order visits blocks out of order and many times).
            coveredBlocks.clear();
            for (uint32_t word = 0; word < 256u; ++word)
                for (uint64_t bits = coveredWords[word]; bits != 0u; bits &= bits - 1u)
                    coveredBlocks.push_back(uint16_t(word * 64u + uint32_t(__builtin_ctzll(bits))));
            std::memset(coveredWords, 0, sizeof(coveredWords));
            sink->uploadFinished(dbp, dx == 0u && dy == 0u, transferHash, transferRectKey(), coveredBlocks);
            coveredBlocks.clear();
        }

    private:
        struct Path
        {
            uint64_t regs = 0;
            uint32_t left = 0, flg = 0, nreg = 0, index = 0;
        };
        Path paths[4];
        Vertex queue[3];
        uint32_t queued = 0;
        float packedQ = 1.0f;
        bool transferActive = false;
        std::vector<uint8_t> pendingTransfer;
        size_t transferExpected = 0;
        uint64_t transferHash = 0;
        std::vector<uint16_t> coveredBlocks, changedBlocks;
        uint64_t coveredWords[256] = {};
        uint32_t transferX = 0, transferY = 0, carried = 0;
        uint8_t carry[3] = {};

        // Whole-pixel formats, up to eight pixels of a row at a time: they share a block, so the address is one table
        // lookup away from the first one and the change tracking is per group instead of per pixel.
        template <typename Pixel>
        void copyCells(const Swizzle &layout, const uint8_t *data, uint32_t bytes, uint32_t dbp, uint32_t dbw, uint32_t dx, uint32_t dy, uint32_t width, uint32_t height,
                       std::vector<uint16_t> &changed)
        {
            for (uint32_t at = 0; transferActive && at + sizeof(Pixel) <= bytes;)
            {
                const uint32_t x = (dx + transferX) & 2047u, y = (dy + transferY) & 2047u;
                const uint32_t count = std::min({width - transferX, uint32_t((bytes - at) / sizeof(Pixel)), 8u - (x & 7u)});
                const uint32_t *offsets = layout.cell(x, y);
                const uint32_t first = layout.bitAddress(dbp, dbw, x, y), base = first - offsets[0];
                const uint16_t block = uint16_t(first >> 11);
                coveredWords[block >> 6] |= 1ull << (block & 63u);
                bool different = false;
                for (uint32_t i = 0; i < count; ++i)
                {
                    uint8_t *destination = vram + (((base + offsets[i]) & 0x1FFFFFFu) >> 3);
                    Pixel incoming, present;
                    std::memcpy(&incoming, data + at + i * sizeof(Pixel), sizeof(Pixel));
                    std::memcpy(&present, destination, sizeof(Pixel));
                    if (incoming != present)
                    {
                        std::memcpy(destination, &incoming, sizeof(Pixel));
                        different = true;
                    }
                }
                if (different && (changed.empty() || changed.back() != block))
                    changed.push_back(block);
                at += count * uint32_t(sizeof(Pixel));
                transferX += count;
                if (transferX == width)
                {
                    transferX = 0;
                    if (++transferY == height)
                        transferActive = false;
                }
            }
        }

        void set(uint64_t &reg, uint64_t value)
        {
            if (reg != value)
            {
                sink->stateChanging();
                reg = value;
            }
        }

        void beginTransfer()
        {
            // A transfer cut short by the next one still wrote what it had.
            if (transferActive && !pendingTransfer.empty())
                commitTransfer();
            pendingTransfer.clear();
            {
                const uint32_t psm = uint32_t(regs.bitbltbuf >> 56) & 0x3Fu;
                const uint64_t bits = psm == 0x00u || psm == 0x30u ? 32u : psm == 0x01u || psm == 0x31u ? 24u : psm == 0x02u || psm == 0x0Au || psm == 0x32u || psm == 0x3Au ? 16u
                                    : psm == 0x13u || psm == 0x1Bu ? 8u : psm == 0x14u || psm == 0x24u || psm == 0x2Cu ? 4u : 0u;
                transferExpected = size_t(((regs.trxreg & 0xFFFu) * ((regs.trxreg >> 32) & 0xFFFu) * bits + 7u) / 8u);
            }
            transferActive = regs.trxdir == 0u && transferExpected != 0u;
            transferX = transferY = carried = 0;
            coveredBlocks.clear();
            transferHash = (regs.bitbltbuf >> 56) * 0x100000001b3ull ^ regs.trxreg;
            // Textures may be about to change under what is queued.
            if (regs.trxdir != 3u)
                sink->stateChanging();
        }

        void kick(uint32_t x, uint32_t y, uint32_t z, uint8_t fog, bool draw)
        {
            Vertex &vertex = queue[queued < 3u ? queued : 2u];
            if (queued == 3u)
            {
                queue[0] = queue[1];
                queue[1] = queue[2];
            }
            else
                ++queued;
            vertex.x = int32_t(x);
            vertex.y = int32_t(y);
            vertex.z = z;
            vertex.rgba = uint32_t(regs.rgbaq);
            vertex.s = regs.s;
            vertex.t = regs.t;
            vertex.q = regs.q;
            vertex.u = uint16_t(regs.uv & 0x3FFFu);
            vertex.v = uint16_t((regs.uv >> 16) & 0x3FFFu);
            vertex.fog = fog;
            switch (uint32_t(regs.prim) & 7u)
            {
            case 3: // triangle
                if (queued == 3u)
                {
                    if (draw)
                        sink->triangle(queue[0], queue[1], queue[2]);
                    queued = 0;
                }
                break;
            case 4: // strip: the last three vertices
                if (queued == 3u && draw)
                    sink->triangle(queue[0], queue[1], queue[2]);
                break;
            case 5: // fan: the first vertex stays
                if (queued == 3u)
                {
                    if (draw)
                        sink->triangle(queue[0], queue[1], queue[2]);
                    queue[1] = queue[2];
                    queued = 2;
                }
                break;
            case 6: // sprite
                if (queued == 2u)
                {
                    if (draw)
                        sink->sprite(queue[0], queue[1]);
                    queued = 0;
                }
                break;
            case 0: // point
                if (draw)
                    sink->point(queue[0]);
                queued = 0;
                break;
            case 1: // line
                if (queued == 2u)
                {
                    if (draw)
                        sink->line(queue[0], queue[1]);
                    queued = 0;
                }
                break;
            case 2: // line strip
                if (queued == 2u)
                {
                    if (draw)
                        sink->line(queue[0], queue[1]);
                    queue[0] = queue[1];
                    queued = 1;
                }
                break;
            default:
                queued = 0;
                break;
            }
        }
    };
}
