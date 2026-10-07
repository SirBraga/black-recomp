// Decode a GS framebuffer from a captured VRAM image to a portable PPM.
#include "runtime/gs/ps2_gs_memory.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv)
{
    if (argc != 7)
    {
        std::fprintf(stderr, "usage: %s VRAM.bin FRAME_HEX WIDTH HEIGHT X Y > frame.ppm\n", argv[0]);
        return 2;
    }
    std::vector<uint8_t> vram(4u * 1024u * 1024u);
    FILE *in = std::fopen(argv[1], "rb");
    if (!in) return 3;
    const bool complete = std::fread(vram.data(), 1, vram.size(), in) == vram.size();
    std::fclose(in);
    if (!complete) return 4;
    const uint64_t frame = std::strtoull(argv[2], nullptr, 16);
    const unsigned width = std::strtoul(argv[3], nullptr, 0);
    const unsigned height = std::strtoul(argv[4], nullptr, 0);
    const unsigned originX = std::strtoul(argv[5], nullptr, 0);
    const unsigned originY = std::strtoul(argv[6], nullptr, 0);
    const unsigned fbp = frame & 0x1ffu;
    const unsigned fbw = (frame >> 16u) & 0x3fu;
    const unsigned psm = (frame >> 24u) & 0x3fu;
    if (!width || !height || width > 2048u || height > 2048u || !fbw ||
        (psm != 0u && psm != 1u && psm != 2u && psm != 10u))
        return 5;
    GSMem::InitLookupTables();
    GSMem::TexturePageCache cache;
    std::printf("P6\n%u %u\n255\n", width, height);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
        {
            const uint32_t pixel = GSMem::ReadTexture(cache, vram.data(), psm,
                                                       fbp * 32u, fbw, x + originX, y + originY);
            uint8_t rgb[3];
            if (psm == 2u || psm == 10u)
            {
                rgb[0] = static_cast<uint8_t>((pixel & 31u) * 255u / 31u);
                rgb[1] = static_cast<uint8_t>(((pixel >> 5u) & 31u) * 255u / 31u);
                rgb[2] = static_cast<uint8_t>(((pixel >> 10u) & 31u) * 255u / 31u);
            }
            else
            {
                rgb[0] = static_cast<uint8_t>(pixel);
                rgb[1] = static_cast<uint8_t>(pixel >> 8u);
                rgb[2] = static_cast<uint8_t>(pixel >> 16u);
            }
            std::fwrite(rgb, 1, sizeof(rgb), stdout);
        }
}
