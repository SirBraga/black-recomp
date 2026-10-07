#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "runtime/gs/gs_cpu_backend.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

struct TextureTransfer
{
    Uint32 bp = 11616u;
    Uint32 bw = 2u;
    Uint32 x = 0u;
    Uint32 y = 0u;
    Uint32 width = 128u;
    Uint32 height = 128u;
    std::vector<Uint32> pixels;
};

static bool loadCapturedCT32(const char *path, Uint32 targetBp, TextureTransfer &out)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "Cannot open captured transfer bundle: %s\n", path);
        return false;
    }
    for (;;) {
        Uint32 record[8]{};
        input.read(reinterpret_cast<char *>(record), sizeof(record));
        if (input.eof()) break;
        if (!input) return false;
        const Uint32 psm = record[0], bp = record[1], bw = record[2];
        const Uint32 x = record[3], y = record[4], width = record[5], height = record[6], bytes = record[7];
        if (bytes > 4u * 1024u * 1024u || width == 0 || height == 0) return false;
        if (psm == GS_PSM_CT32 && bp == targetBp && width == 128u && height == 128u && x == 0u && y == 0u && bytes == width * height * 4u) {
            out.bp = bp;
            out.bw = bw;
            out.width = width;
            out.height = height;
            out.x = x;
            out.y = y;
            out.pixels.resize(width * height);
            input.read(reinterpret_cast<char *>(out.pixels.data()), bytes);
            if (!input) return false;
            std::fprintf(stderr, "Loaded captured Black CT32 upload: BP=%u BW=%u %ux%u (%u bytes)\n",
                         bp, bw, width, height, bytes);
            return true;
        }
        input.seekg(bytes, std::ios::cur);
        if (!input) return false;
    }
    std::fprintf(stderr, "No full 128x128 CT32 upload at BP=%u found in %s\n", targetBp, path);
    return false;
}

static bool testSwizzledPSMCT32(SDL_GPUDevice *device, const TextureTransfer &source)
{
    constexpr Uint32 vramBytes = 4u * 1024u * 1024u;
    const Uint32 width = source.width, height = source.height, texels = width * height;
    if (width != 128u || (height % 32u) != 0u || source.x != 0u || source.y != 0u || source.bw < 2u) {
        std::fprintf(stderr, "Unsupported captured CT32 region: %ux%u at %u,%u BW=%u\n",
                     width, height, source.x, source.y, source.bw);
        return false;
    }
    std::vector<uint8_t> guestVram(vramBytes, 0u);
    const std::vector<Uint32> &expected = source.pixels;

    GSCpuBackend gs;
    gs.Initialize(guestVram.data(), vramBytes);
    GSTransferCommand transfer{};
    transfer.bitbltbuf.dbp = source.bp;
    transfer.bitbltbuf.dbw = static_cast<uint8_t>(source.bw);
    transfer.bitbltbuf.dpsm = GS_PSM_CT32;
    transfer.trxpos.dsax = static_cast<uint16_t>(source.x);
    transfer.trxpos.dsay = static_cast<uint16_t>(source.y);
    transfer.trxreg.rrw = width;
    transfer.trxreg.rrh = height;
    transfer.direction = 0u; // Host-to-local image upload (GS TRXDIR).
    gs.BeginTransfer(transfer);
    gs.UploadImage(reinterpret_cast<const uint8_t *>(expected.data()),
                   Uint32(expected.size() * sizeof(Uint32)));
    const GSTransferSnapshot transferState = gs.GetTransferSnapshot();
    if (transferState.copiedPixels != texels) {
        std::fprintf(stderr, "CPU GS CT32 upload copied %u/%u pixels\n", transferState.copiedPixels, texels);
        return false;
    }
    gs.SnapshotVram(guestVram);

    // GPU calculates the PS2's CT32 page/block/column swizzle. The input VRAM
    // was produced by the runtime's actual GSCpuBackend::UploadImage path.
    constexpr char shader[] = R"metal(
#include <metal_stdlib>
using namespace metal;
constant uint blockTable32[32] = {
     0,  1,  4,  5, 16, 17, 20, 21,
     2,  3,  6,  7, 18, 19, 22, 23,
     8,  9, 12, 13, 24, 25, 28, 29,
    10, 11, 14, 15, 26, 27, 30, 31
};
constant uint columnTable32[64] = {
     0,  1,  4,  5,  8,  9, 12, 13,
     2,  3,  6,  7, 10, 11, 14, 15,
    16, 17, 20, 21, 24, 25, 28, 29,
    18, 19, 22, 23, 26, 27, 30, 31,
    32, 33, 36, 37, 40, 41, 44, 45,
    34, 35, 38, 39, 42, 43, 46, 47,
    48, 49, 52, 53, 56, 57, 60, 61,
    50, 51, 54, 55, 58, 59, 62, 63
};
struct Params { uint baseWord; uint pagesPerRow; };
kernel void fetch_ct32(constant Params &params [[buffer(0)]],
                       device const uint *vram [[buffer(1)]],
                       device uint *output [[buffer(2)]],
                       uint gid [[thread_position_in_grid]]) {
    const uint x = gid % 128u;
    const uint y = gid / 128u;
    const uint page = (y >> 5u) * params.pagesPerRow + (x >> 6u);
    const uint block = blockTable32[((y >> 3u) & 3u) * 8u + ((x >> 3u) & 7u)];
    const uint column = columnTable32[(y & 7u) * 8u + (x & 7u)];
    const uint wordAddress = params.baseWord + page * 2048u + (block & 31u) * 64u + column;
    output[gid] = vram[wordAddress];
}
)metal";

    SDL_GPUBufferCreateInfo vramInfo{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, vramBytes, 0};
    SDL_GPUBufferCreateInfo outputInfo{SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
        Uint32(expected.size() * sizeof(Uint32)), 0};
    SDL_GPUBuffer *vram = SDL_CreateGPUBuffer(device, &vramInfo);
    SDL_GPUBuffer *output = SDL_CreateGPUBuffer(device, &outputInfo);
    SDL_GPUComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.code_size = sizeof(shader);
    pipelineInfo.code = reinterpret_cast<const Uint8 *>(shader);
    pipelineInfo.entrypoint = "fetch_ct32";
    pipelineInfo.format = SDL_GPU_SHADERFORMAT_MSL;
    pipelineInfo.num_uniform_buffers = 1;
    pipelineInfo.num_readonly_storage_buffers = 1;
    pipelineInfo.num_readwrite_storage_buffers = 1;
    pipelineInfo.threadcount_x = 64;
    pipelineInfo.threadcount_y = pipelineInfo.threadcount_z = 1;
    SDL_GPUComputePipeline *pipeline = SDL_CreateGPUComputePipeline(device, &pipelineInfo);
    SDL_GPUTransferBufferCreateInfo uploadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, vramBytes, 0};
    SDL_GPUTransferBufferCreateInfo downloadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        Uint32(expected.size() * sizeof(Uint32)), 0};
    SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(device, &uploadInfo);
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(device, &downloadInfo);
    if (!vram || !output || !pipeline || !upload || !download) {
        std::fprintf(stderr, "PSMCT32 resource creation: %s\n", SDL_GetError());
        return false;
    }

    auto *mappedUpload = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(device, upload, false));
    if (!mappedUpload) {
        std::fprintf(stderr, "PSMCT32 upload map: %s\n", SDL_GetError());
        return false;
    }
    std::memcpy(mappedUpload, guestVram.data(), vramBytes);
    SDL_UnmapGPUTransferBuffer(device, upload);

    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
    if (!commands) return false;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTransferBufferLocation fromVram{upload, 0};
    SDL_GPUBufferRegion toVram{vram, 0, vramBytes};
    SDL_UploadToGPUBuffer(copy, &fromVram, &toVram, false);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUBuffer *readonlyBuffers[] = {vram};
    SDL_GPUStorageBufferReadWriteBinding outputBinding{output, false, 0, 0, 0};
    SDL_GPUComputePass *compute = SDL_BeginGPUComputePass(commands, nullptr, 0, &outputBinding, 1);
    if (!compute) {
        std::fprintf(stderr, "PSMCT32 compute pass: %s\n", SDL_GetError());
        SDL_CancelGPUCommandBuffer(commands);
        return false;
    }
    struct Params { Uint32 baseWord; Uint32 pagesPerRow; } params{source.bp * 64u, source.bw};
    SDL_PushGPUComputeUniformData(commands, 0, &params, sizeof(params));
    SDL_BindGPUComputePipeline(compute, pipeline);
    SDL_BindGPUComputeStorageBuffers(compute, 0, readonlyBuffers, 1);
    SDL_DispatchGPUCompute(compute, texels / 64u, 1, 1);
    SDL_EndGPUComputePass(compute);

    copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUBufferRegion fromOutput{output, 0, Uint32(expected.size() * sizeof(Uint32))};
    SDL_GPUTransferBufferLocation toDownload{download, 0};
    SDL_DownloadFromGPUBuffer(copy, &fromOutput, &toDownload);
    SDL_EndGPUCopyPass(copy);

    bool valid = SDL_SubmitGPUCommandBuffer(commands) && SDL_WaitForGPUIdle(device);
    if (valid) {
        const auto *result = static_cast<const Uint32 *>(SDL_MapGPUTransferBuffer(device, download, false));
        valid = result != nullptr;
        if (result) {
            for (Uint32 i = 0; i < texels; ++i) {
                if (result[i] != expected[i]) {
                    std::fprintf(stderr, "CT32 texel (%u,%u): got 0x%08x expected 0x%08x\n",
                                 i % width, i / width, result[i], expected[i]);
                    valid = false;
                    break;
                }
            }
            SDL_UnmapGPUTransferBuffer(device, download);
        }
    }
    std::printf("SDL_GPU GS CT32 upload + swizzle shader: %s (BP=%u, %ux%u, %u texels, %s)\n",
                valid ? "PASS" : "FAIL", source.bp, width, height, texels, SDL_GetGPUDeviceDriver(device));
    SDL_ReleaseGPUTransferBuffer(device, download);
    SDL_ReleaseGPUTransferBuffer(device, upload);
    SDL_ReleaseGPUComputePipeline(device, pipeline);
    SDL_ReleaseGPUBuffer(device, output);
    SDL_ReleaseGPUBuffer(device, vram);
    return valid;
}

int main(int argc, char **argv)
{
    TextureTransfer transfer{};
    if (argc > 1) {
        const Uint32 bp = argc > 2 ? static_cast<Uint32>(std::strtoul(argv[2], nullptr, 0)) : 11616u;
        if (!loadCapturedCT32(argv[1], bp, transfer)) return 1;
    } else {
        transfer.pixels.resize(transfer.width * transfer.height);
        for (Uint32 y = 0; y < transfer.height; ++y)
            for (Uint32 x = 0; x < transfer.width; ++x)
                transfer.pixels[y * transfer.width + x] = 0xA0000000u | (y << 16u) | x;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow("SDL_GPU Metal GS probe", 640, 448, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GPUDevice *device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, true, "metal");
    if (!device || !SDL_ClaimWindowForGPUDevice(device, window)) {
        std::fprintf(stderr, "SDL_GPU Metal init: %s\n", SDL_GetError());
        if (device) SDL_DestroyGPUDevice(device);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    std::printf("SDL GPU driver: %s\n", SDL_GetGPUDeviceDriver(device));
    bool running = testSwizzledPSMCT32(device, transfer);
    unsigned frames = 0;
    while (running && frames < 120) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) if (event.type == SDL_EVENT_QUIT) running = false;
        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUTexture *swapchain = nullptr;
        if (!commands || !SDL_AcquireGPUSwapchainTexture(commands, window, &swapchain, nullptr, nullptr)) {
            std::fprintf(stderr, "swapchain acquire: %s\n", SDL_GetError());
            if (commands) SDL_CancelGPUCommandBuffer(commands);
            running = false;
            break;
        }
        if (swapchain) {
            SDL_GPUColorTargetInfo target{};
            target.texture = swapchain;
            target.clear_color = SDL_FColor{0.08f, 0.42f, 0.18f, 1.0f};
            target.load_op = SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);
            if (!pass) {
                std::fprintf(stderr, "SDL_BeginGPURenderPass: %s\n", SDL_GetError());
                SDL_CancelGPUCommandBuffer(commands);
                running = false;
                break;
            }
            SDL_EndGPURenderPass(pass);
            ++frames;
        }
        if (!SDL_SubmitGPUCommandBuffer(commands)) running = false;
    }
    SDL_WaitForGPUIdle(device);
    std::printf("Presented %u SDL_GPU frames.\n", frames);
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return running ? 0 : 1;
}
