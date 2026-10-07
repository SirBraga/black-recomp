#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

static bool readExact(const char *path, void *data, size_t bytes)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(static_cast<char *>(data), static_cast<std::streamsize>(bytes));
    return f.gcount() == static_cast<std::streamsize>(bytes);
}

static bool run(SDL_GPUDevice *device, const char *vramPath, uint64_t tex0, const char *clutPath)
{
    constexpr Uint32 vramSize = 4u * 1024u * 1024u;
    std::vector<uint8_t> vram(vramSize);
    std::array<uint16_t, 512> clut{};
    if (!readExact(vramPath, vram.data(), vram.size()) || !readExact(clutPath, clut.data(), sizeof(clut))) {
        std::fprintf(stderr, "failed to read captured VRAM/CLUT\n");
        return false;
    }

    const Uint32 bp = Uint32(tex0 & 0x3FFFu);
    const Uint32 bw = std::max(1u, Uint32((tex0 >> 14u) & 0x3Fu));
    const Uint32 psm = Uint32((tex0 >> 20u) & 0x3Fu);
    const Uint32 width = 1u << Uint32((tex0 >> 26u) & 15u);
    const Uint32 height = 1u << Uint32((tex0 >> 30u) & 15u);
    const Uint32 cbp = Uint32((tex0 >> 37u) & 0x3FFFu);
    const Uint32 cpsm = Uint32((tex0 >> 51u) & 15u);
    const Uint32 csm = Uint32((tex0 >> 55u) & 1u);
    const Uint32 csa = Uint32((tex0 >> 56u) & 31u);
    if (psm != 20u || cpsm != 0u || csm != 0u || width != 128u || height != 32u) {
        std::fprintf(stderr, "unsupported TEX0 PSM=%u CPSM=%u CSM=%u %ux%u\n", psm, cpsm, csm, width, height);
        return false;
    }
    const Uint32 count = width * height;
    std::vector<Uint32> bitAddresses(count), expected(count);
    GSMem::InitLookupTables();
    GSMem::TexturePageCache cache;
    Uint32 paletteMismatches = 0;
    for (Uint32 i = 0; i < 16; ++i) {
        const Uint32 logical = i + (csa & 15u) * 16u;
        const Uint32 physical = (logical & ~24u) | ((logical & 8u) << 1u) | ((logical & 16u) >> 1u);
        const Uint32 fromVram = GSMem::ReadTexture(cache, vram.data(), 0u, cbp, 1u, physical & 15u, physical >> 4u);
        const Uint32 fromCache = Uint32(clut[logical & 255u]) | (Uint32(clut[(logical & 255u) + 256u]) << 16u);
        if (fromVram != fromCache) ++paletteMismatches;
    }
    for (Uint32 y = 0; y < height; ++y) {
        for (Uint32 x = 0; x < width; ++x) {
            const Uint32 i = y * width + x;
            bitAddresses[i] = GSMem::PixelBitAddress(psm, bp, bw, x, y);
            const Uint32 index = GSMem::ReadTexture(cache, vram.data(), psm, bp, bw, x, y) & 15u;
            const Uint32 logical = (index + (csa & 15u) * 16u) & 255u;
            expected[i] = Uint32(clut[logical]) | (Uint32(clut[logical + 256u]) << 16u);
        }
    }

    constexpr char shader[] = R"metal(
#include <metal_stdlib>
using namespace metal;
kernel void decode_t4(device const uint *vram [[buffer(0)]],
                      device const uint *bit_addresses [[buffer(1)]],
                      device const ushort *clut [[buffer(2)]],
                      device uint *output [[buffer(3)]],
                      uint gid [[thread_position_in_grid]]) {
    const uint bit = bit_addresses[gid];
    const uint index = (vram[bit >> 5u] >> (bit & 31u)) & 15u;
    output[gid] = uint(clut[index]) | (uint(clut[index + 256u]) << 16u);
}
)metal";

    SDL_GPUBufferCreateInfo vramInfo{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, vramSize, 0};
    SDL_GPUBufferCreateInfo addressInfo{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, Uint32(count * 4u), 0};
    SDL_GPUBufferCreateInfo clutInfo{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, sizeof(clut), 0};
    SDL_GPUBufferCreateInfo outputInfo{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, Uint32(count * 4u), 0};
    SDL_GPUBuffer *gpuVram = SDL_CreateGPUBuffer(device, &vramInfo);
    SDL_GPUBuffer *gpuAddresses = SDL_CreateGPUBuffer(device, &addressInfo);
    SDL_GPUBuffer *gpuClut = SDL_CreateGPUBuffer(device, &clutInfo);
    SDL_GPUBuffer *gpuOutput = SDL_CreateGPUBuffer(device, &outputInfo);
    SDL_GPUComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.code_size = sizeof(shader);
    pipelineInfo.code = reinterpret_cast<const Uint8 *>(shader);
    pipelineInfo.entrypoint = "decode_t4";
    pipelineInfo.format = SDL_GPU_SHADERFORMAT_MSL;
    pipelineInfo.num_readonly_storage_buffers = 3;
    pipelineInfo.num_readwrite_storage_buffers = 1;
    pipelineInfo.threadcount_x = 64;
    pipelineInfo.threadcount_y = pipelineInfo.threadcount_z = 1;
    SDL_GPUComputePipeline *pipeline = SDL_CreateGPUComputePipeline(device, &pipelineInfo);
    const Uint32 addressBytes = count * 4u;
    SDL_GPUTransferBufferCreateInfo uploadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, static_cast<Uint32>(vramSize + addressBytes + sizeof(clut)), 0};
    SDL_GPUTransferBufferCreateInfo downloadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, count * 4u, 0};
    SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(device, &uploadInfo);
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(device, &downloadInfo);
    if (!gpuVram || !gpuAddresses || !gpuClut || !gpuOutput || !pipeline || !upload || !download) {
        std::fprintf(stderr, "PSMT4 GPU resource creation: %s\n", SDL_GetError());
        return false;
    }
    auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(device, upload, false));
    if (!mapped) return false;
    std::memcpy(mapped, vram.data(), vramSize);
    std::memcpy(mapped + vramSize, bitAddresses.data(), addressBytes);
    std::memcpy(mapped + vramSize + addressBytes, clut.data(), sizeof(clut));
    SDL_UnmapGPUTransferBuffer(device, upload);

    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) return false;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTransferBufferLocation fromVram{upload, 0};
    SDL_GPUBufferRegion toVram{gpuVram, 0, vramSize};
    SDL_UploadToGPUBuffer(copy, &fromVram, &toVram, false);
    SDL_GPUTransferBufferLocation fromAddresses{upload, vramSize};
    SDL_GPUBufferRegion toAddresses{gpuAddresses, 0, addressBytes};
    SDL_UploadToGPUBuffer(copy, &fromAddresses, &toAddresses, false);
    SDL_GPUTransferBufferLocation fromClut{upload, vramSize + addressBytes};
    SDL_GPUBufferRegion toClut{gpuClut, 0, sizeof(clut)};
    SDL_UploadToGPUBuffer(copy, &fromClut, &toClut, false);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUBuffer *readBuffers[] = {gpuVram, gpuAddresses, gpuClut};
    SDL_GPUStorageBufferReadWriteBinding outputBinding{gpuOutput, false, 0, 0, 0};
    SDL_GPUComputePass *compute = SDL_BeginGPUComputePass(commands, nullptr, 0, &outputBinding, 1);
    if (!compute) {
        SDL_CancelGPUCommandBuffer(commands);
        return false;
    }
    SDL_BindGPUComputePipeline(compute, pipeline);
    SDL_BindGPUComputeStorageBuffers(compute, 0, readBuffers, 3);
    SDL_DispatchGPUCompute(compute, count / 64u, 1, 1);
    SDL_EndGPUComputePass(compute);
    copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUBufferRegion fromOutput{gpuOutput, 0, count * 4u};
    SDL_GPUTransferBufferLocation toDownload{download, 0};
    SDL_DownloadFromGPUBuffer(copy, &fromOutput, &toDownload);
    SDL_EndGPUCopyPass(copy);

    bool valid = SDL_SubmitGPUCommandBuffer(commands) && SDL_WaitForGPUIdle(device);
    if (valid) {
        const auto *result = static_cast<const Uint32 *>(SDL_MapGPUTransferBuffer(device, download, false));
        valid = result != nullptr;
        FILE *ppm = std::fopen("recomp/diagnostics/sdl-gpu-captured-psmt4.ppm", "wb");
        if (ppm) std::fprintf(ppm, "P6\n%u %u\n255\n", width, height);
        if (result) {
            for (Uint32 i = 0; i < count; ++i) {
                if (result[i] != expected[i]) {
                    std::fprintf(stderr, "PSMT4 pixel (%u,%u): GPU=%08x runtime=%08x\n",
                                 i % width, i / width, result[i], expected[i]);
                    valid = false;
                    break;
                }
                if (ppm) {
                    const uint8_t rgb[3] = {uint8_t(result[i]), uint8_t(result[i] >> 8u), uint8_t(result[i] >> 16u)};
                    std::fwrite(rgb, 1, sizeof(rgb), ppm);
                }
            }
            SDL_UnmapGPUTransferBuffer(device, download);
        }
        if (ppm) std::fclose(ppm);
    }
    std::printf("SDL_GPU captured PSMT4 + CLUT: %s (%ux%u, %u texels; cache/VRAM palette mismatches=%u; %s)\n",
                valid ? "PASS" : "FAIL", width, height, count, paletteMismatches, SDL_GetGPUDeviceDriver(device));
    SDL_ReleaseGPUTransferBuffer(device, download);
    SDL_ReleaseGPUTransferBuffer(device, upload);
    SDL_ReleaseGPUComputePipeline(device, pipeline);
    SDL_ReleaseGPUBuffer(device, gpuOutput);
    SDL_ReleaseGPUBuffer(device, gpuClut);
    SDL_ReleaseGPUBuffer(device, gpuAddresses);
    SDL_ReleaseGPUBuffer(device, gpuVram);
    return valid;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "usage: ps2x_sdl_gpu_psmt4_probe VRAM.bin TEX0_HEX CLUT.bin\n");
        return 2;
    }
    const uint64_t tex0 = std::strtoull(argv[2], nullptr, 16);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow("SDL_GPU PSMT4 probe", 16, 16, SDL_WINDOW_HIDDEN);
    SDL_GPUDevice *device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, true, "metal");
    if (!window || !device || !SDL_ClaimWindowForGPUDevice(device, window)) {
        std::fprintf(stderr, "SDL_GPU Metal init: %s\n", SDL_GetError());
        if (device) SDL_DestroyGPUDevice(device);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    std::printf("SDL GPU driver: %s\n", SDL_GetGPUDeviceDriver(device));
    const bool valid = run(device, argv[1], tex0, argv[3]);
    SDL_WaitForGPUIdle(device);
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return valid ? 0 : 1;
}
