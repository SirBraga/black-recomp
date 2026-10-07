// Attribute a captured CT32 CLUT word to the last host-to-local GIF upload.
// Input is audit_texture_uploads.py --extract output, followed by a GPU VRAM snapshot.
#include "runtime/gs/ps2_gs_memory.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) return 2;
    FILE *in = std::fopen(argv[1], "rb");
    FILE *snapshot = std::fopen(argv[2], "rb");
    if (!in || !snapshot) return 3;
    GSMem::InitLookupTables();
    std::vector<uint8_t> vram(4u * 1024u * 1024u);
    if (std::fread(vram.data(), 1, vram.size(), snapshot) != vram.size()) return 4;
    std::fclose(snapshot);
    const uint32_t cbp = argc == 4 ? static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0)) : 11647u;
    if (cbp >= 16384u) return 7;
    std::array<uint32_t, 16> addresses{};
    std::array<uint32_t, 16> observed{};
    std::array<uint32_t, 16> latest{};
    std::array<uint32_t, 16> latestOrdinal{};
    std::array<uint32_t, 16> latestBp{};
    for (uint32_t i = 0; i < 16; ++i)
    {
        const uint32_t physical = (i & ~24u) | ((i & 8u) << 1) | ((i & 16u) >> 1);
        addresses[i] = (GSMem::PixelBitAddress(0u, cbp, 1u, physical & 15u, physical >> 4) / 8u) % vram.size();
        std::memcpy(&observed[i], vram.data() + addresses[i], 4u);
    }
    uint32_t ordinal = 0;
    while (true)
    {
        uint32_t header[8];
        if (std::fread(header, sizeof(header), 1, in) != 1) break;
        ++ordinal;
        const auto [psm, bp, bw, x, y, width, height, size] =
            std::array<uint32_t, 8>{header[0], header[1], header[2], header[3], header[4], header[5], header[6], header[7]};
        if (size > 64u * 1024u * 1024u) return 5;
        std::vector<uint8_t> data(size);
        if (std::fread(data.data(), 1, size, in) != size) return 6;
        if (psm != 0u || width == 0u || height == 0u || uint64_t(width) * height * 4u > size) continue;
        for (uint32_t yy = 0; yy < height; ++yy)
            for (uint32_t xx = 0; xx < width; ++xx)
            {
                const uint32_t addr = (GSMem::PixelBitAddress(psm, bp, bw, x + xx, y + yy) / 8u) % vram.size();
                for (uint32_t j = 0; j < 16; ++j)
                    if (addr == addresses[j])
                    {
                        const uint32_t off = (yy * width + xx) * 4u;
                        std::memcpy(&latest[j], data.data() + off, 4u);
                        latestOrdinal[j] = ordinal;
                        latestBp[j] = bp;
                    }
            }
    }
    std::fclose(in);
    for (uint32_t i = 0; i < 16; ++i)
        std::printf("clut[%02u] vram_addr=%06x observed=%08x latest_upload=%08x ordinal=%u bp=%u %s\n",
                    i, addresses[i], observed[i], latest[i], latestOrdinal[i], latestBp[i],
                    latestOrdinal[i] ? (observed[i] == latest[i] ? "MATCH" : "DIFF") : "NO_UPLOAD");
    return 0;
}
