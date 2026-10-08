#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_sdl_gpu_raster.h"
#include "runtime/gs/ps2_gs_memory.h"
#include <cstdlib>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

int main()
{
    constexpr uint32_t bytes = 4u * 1024u * 1024u;
    std::vector<uint8_t> vram(bytes, 0);
    try
    {
        GSMem::InitLookupTables();
        setenv("PS2X_GS_SDL_GPU", "1", 1);
        setenv("PS2X_GS_SDL_GPU_STATS", "1", 1);
        GSSDLGpuRaster::InitializeHostVideo();
        // Seed packed-format readback fixtures before the GPU owns VRAM.
        GSMem::WriteCT24(vram.data(), 1024, 1, 1, 48, 0x00332211u);
        GSMem::WriteCT16(vram.data(), 1280, 1, 1, 48, 0x0000a1b2u);
        GSMem::WriteP8(vram.data(), 1536, 1, 1, 48, 0x0000007eu);
        GSMem::WriteP4(vram.data(), 1792, 1, 2, 48, 1);
        GSMem::WriteP4(vram.data(), 1792, 1, 3, 48, 2);
        GSMem::WriteP4(vram.data(), 1792, 1, 4, 48, 3);
        GSMem::WriteCT32(vram.data(), 2048, 1, 2, 3, 0xaa112233u);
        GSMem::WriteCT16(vram.data(), 2304, 1, 2, 3, 0xabcdu);
        GSMem::WriteCT16(vram.data(), 2304, 1, 3, 3, 0x1234u);
        GSMem::WriteP8(vram.data(), 2560, 1, 2, 3, 0xaau);
        GSMem::WriteP4(vram.data(), 2816, 1, 2, 3, 1u);
        GSMem::WriteP4(vram.data(), 2816, 1, 3, 3, 2u);
        GSMem::WriteP4(vram.data(), 2816, 1, 4, 3, 3u);
        uint32_t rasterPixel = 0;
        uint32_t tinyRasterPixel = 0;
        uint32_t clearPixel = 0;
        uint32_t uploadPixel = 0;
        uint32_t ct24UploadPixel = 0;
        bool compactUploadPassed = false;
        bool overlapUploadsPassed = false;
        uint32_t localCopyPixel = 0;
        uint32_t overlapCopyPixel = 0;
        uint32_t dither16Pixel = 0;
        uint32_t gpuPresentPixel = 0;
        uint32_t gpuPresentWidth = 0;
        uint32_t gpuAsyncPresentPixel = 0;
        bool gpuAsyncPresentPassed = false;
        bool clutCachePassed = false;
        uint64_t clutCacheHits = 0;
        uint64_t clutCacheDispatches = 0;
        uint32_t gpuPresentCt24Pixel = 0;
        uint32_t gpuPresentCt16Pixel = 0;
        uint32_t gpuPresentInitialCt32Pixel = 0;
        std::vector<GSPrimitiveBatch> lineReferences;
        std::array<uint32_t, 440> gpuLineRegion{};
        bool gpuLineMatchesCpu = false;
        bool gpuAliasedDepthMatchesCpu = false;
        bool gpuDisjointDrawsMatchCpu = false;
        bool gpuTexturedDisjointDrawsMatchCpu = false;
        bool gpuRasterBatchCapacityMatchesCpu = false;
        bool gpuRasterOrderedBatchMatchesCpu = false;
        uint64_t texturedBatchedDrawCount = 0;
        uint64_t texturedRasterPassCount = 0;
        bool texturedFullVramMatchesCpu = false;
        bool gpuMipLevelMatchesCpu = false;
        bool gpuMiptbpMxlZeroBatchMatchesCpu = false;
        bool gpuMipAliasMatchesCpu = false;
        bool gpuDisjointImageChunksBatchMatchCpu = false;
        bool gpuNonfiniteMatchesExplicit = false;
        bool gpuOnlyGuardPassed = false;
        std::array<uint8_t, 16> localReadbackBytes{};
        std::array<uint8_t, 2> nibbleReadbackBytes{};
        std::array<uint8_t, 3> ct24ReadbackBytes{};
        std::array<uint8_t, 2> ct16ReadbackBytes{};
        std::array<uint8_t, 1> p8ReadbackBytes{};
        bool localReadbackStatePassed = false;
        {
            GSCpuBackend gs;
            gs.Initialize(vram.data(), bytes);
            GSPrimitiveBatch batch{};
            batch.vertexCount = 2;
            batch.state.prim.type = GS_PRIM_SPRITE;
            batch.state.context.frame.fbw = 10;
            batch.state.context.frame.psm = GS_PSM_CT32;
            batch.state.context.scissor = {0, 127, 0, 127};
            batch.state.context.zbuf.psm = 48;
            batch.state.context.zbuf.zmask = true;
            batch.state.context.test = uint64_t(1) << 17; // Z test: always.
            batch.vertices[0].x = 2; batch.vertices[0].y = 2;
            batch.vertices[1].x = 30; batch.vertices[1].y = 30;
            for (auto &vertex : batch.vertices)
            {
                vertex.r = 0x33; vertex.g = 0x66; vertex.b = 0x99; vertex.a = 0xff;
            }
            gs.Submit(batch);
            // TEXFLUSH must order later GPU work without idling the device.
            gs.TextureFlush();
            gs.FlushForPresentation();

            batch.vertices[0].x = 78; batch.vertices[0].y = 2;
            batch.vertices[1].x = 106; batch.vertices[1].y = 30;
            for (auto &vertex : batch.vertices)
            {
                vertex.r = 0xaa; vertex.g = 0x55; vertex.b = 0x11; vertex.a = 0xff;
            }
            gs.Submit(batch);

            // The normal GPU path now accepts tiny independent draws too,
            // avoiding an otherwise expensive full-VRAM CPU/GPU handoff.
            batch.vertices[0].x = 110; batch.vertices[0].y = 40;
            batch.vertices[1].x = 114; batch.vertices[1].y = 44;
            for (auto &vertex : batch.vertices)
            {
                vertex.r = 0x12; vertex.g = 0x34; vertex.b = 0x56; vertex.a = 0xff;
            }
            gs.Submit(batch);

            const std::array<std::array<int, 4>, 8> lineCases{{
                {{130, 60, 136, 63}}, {{136, 63, 130, 60}},
                {{130, 68, 133, 74}}, {{133, 74, 130, 68}},
                {{140, 60, 145, 60}}, {{140, 61, 140, 66}},
                {{145, 70, 145, 70}}, {{144, 74, 138, 71}},
            }};
            for (size_t i = 0; i < lineCases.size(); ++i)
            {
                GSPrimitiveBatch line{};
                line.vertexCount = 2;
                line.state = batch.state;
                line.state.context.scissor = {128, 159, 58, 95};
                if ((i & 2u) != 0u)
                {
                    line.state.context.zbuf.psm = GS_PSM_Z16S;
                    // Keep depth pages inside GS VRAM, immediately after the
                    // color pages used by this test.
                    line.state.context.zbuf.zbp = 40;
                    line.state.context.zbuf.zmask = false;
                }
                line.state.prim.type = (i & 1u) ? GS_PRIM_LINESTRIP : GS_PRIM_LINE;
                line.state.prim.iip = true;
                line.vertices[0].x = lineCases[i][0]; line.vertices[0].y = lineCases[i][1];
                line.vertices[1].x = lineCases[i][2]; line.vertices[1].y = lineCases[i][3];
                line.vertices[0].z = line.vertices[1].z = 1.0;
                line.vertices[0].r = static_cast<uint8_t>(i * 7u); line.vertices[0].g = 10; line.vertices[0].b = 20; line.vertices[0].a = 255;
                line.vertices[1].r = static_cast<uint8_t>(200u - i * 9u); line.vertices[1].g = 100; line.vertices[1].b = 40; line.vertices[1].a = 255;
                gs.Submit(line);
                lineReferences.push_back(line);
            }

            GSPrimitiveBatch point{};
            point.vertexCount = 1;
            point.state = batch.state;
            point.state.context.scissor = {128, 159, 58, 95};
            point.state.prim.type = GS_PRIM_POINT;
            point.vertices[0].x = 148; point.vertices[0].y = 76; point.vertices[0].z = 1.0;
            point.vertices[0].r = 0x5a; point.vertices[0].g = 0xa5; point.vertices[0].b = 0x3c; point.vertices[0].a = 255;
            gs.Submit(point);
            lineReferences.push_back(point);

            GSPrimitiveBatch dither{};
            dither.vertexCount = 2;
            dither.state = batch.state;
            dither.state.prim.type = GS_PRIM_SPRITE;
            dither.state.context.frame.fbp = 4;
            dither.state.context.frame.psm = GS_PSM_CT16S;
            dither.state.context.frame.fbmsk = 0;
            dither.state.context.zbuf.zmask = true;
            dither.state.context.scissor = {128, 159, 58, 95};
            dither.state.dthe = true;
            dither.state.dimx = 0x4444444444444444ull; // Every signed 3-bit DIMX value is -4.
            dither.vertices[0].x = 130; dither.vertices[0].y = 60;
            dither.vertices[1].x = 131; dither.vertices[1].y = 61;
            dither.vertices[0].z = dither.vertices[1].z = 1.0;
            for (auto &vertex : dither.vertices)
            {
                vertex.r = 80; vertex.g = 100; vertex.b = 120; vertex.a = 255;
            }
            gs.Submit(dither);

            GSContext clearContext = batch.state.context;
            clearContext.frame.fbp = 0;
            clearContext.frame.fbw = 10;
            clearContext.frame.psm = GS_PSM_CT32;
            clearContext.frame.fbmsk = 0;
            clearContext.scissor = {120, 123, 40, 43};
            clearContext.fba = 0;
            gs.ClearFramebuffer(clearContext, 0xff563412u);

            GSTransferCommand imageTransfer{};
            imageTransfer.bitbltbuf.dbp = 256;
            imageTransfer.bitbltbuf.dbw = 1;
            imageTransfer.bitbltbuf.dpsm = GS_PSM_CT32;
            imageTransfer.trxpos.dsax = 4;
            imageTransfer.trxpos.dsay = 48;
            imageTransfer.trxreg.rrw = 2;
            imageTransfer.trxreg.rrh = 2;
            imageTransfer.direction = 0;
            const std::array<uint32_t, 4> uploadWords{0xff102030u, 0xff405060u, 0xff708090u, 0xffa0b0c0u};
            gs.BeginTransfer(imageTransfer);
            gs.UploadImage(reinterpret_cast<const uint8_t *>(uploadWords.data()), 2u * sizeof(uint32_t));
            gs.UploadImage(reinterpret_cast<const uint8_t *>(uploadWords.data() + 2u), 2u * sizeof(uint32_t));

            GSTransferCommand ct24Upload = imageTransfer;
            ct24Upload.bitbltbuf.dbp = 2048;
            ct24Upload.bitbltbuf.dpsm = GS_PSM_CT24;
            ct24Upload.trxpos.dsax = 2;
            ct24Upload.trxpos.dsay = 3;
            ct24Upload.trxreg.rrw = ct24Upload.trxreg.rrh = 1;
            const std::array<uint8_t, 3> packed24{0x44, 0x55, 0x66};
            gs.BeginTransfer(ct24Upload);
            gs.UploadImage(packed24.data(), static_cast<uint32_t>(packed24.size()));

            GSTransferCommand compactUpload = ct24Upload;
            compactUpload.bitbltbuf.dbp = 2304;
            compactUpload.bitbltbuf.dpsm = GS_PSM_CT16;
            const std::array<uint8_t, 2> packed16{0x78, 0x56};
            gs.BeginTransfer(compactUpload);
            gs.UploadImage(packed16.data(), static_cast<uint32_t>(packed16.size()));
            compactUpload.bitbltbuf.dbp = 2560;
            compactUpload.bitbltbuf.dpsm = GS_PSM_T8;
            compactUpload.trxreg.rrw = 2;
            const std::array<uint8_t, 2> packed8{0x56, 0x78};
            gs.BeginTransfer(compactUpload);
            gs.UploadImage(packed8.data(), static_cast<uint32_t>(packed8.size()));
            compactUpload.bitbltbuf.dbp = 2816;
            compactUpload.bitbltbuf.dpsm = GS_PSM_T4;
            const std::array<uint8_t, 1> packed4{0xa5};
            gs.BeginTransfer(compactUpload);
            gs.UploadImage(packed4.data(), static_cast<uint32_t>(packed4.size()));

            GSTransferCommand overlapUpload = imageTransfer;
            overlapUpload.bitbltbuf.dbp = 3072;
            overlapUpload.trxpos.dsax = 10;
            overlapUpload.trxpos.dsay = 70;
            overlapUpload.trxreg.rrw = 2;
            overlapUpload.trxreg.rrh = 1;
            const std::array<uint32_t, 2> overlapUploadA{0xffaabbccu, 0xffddeeffu};
            const std::array<uint32_t, 2> overlapUploadB{0xff112233u, 0xff445566u};
            gs.BeginTransfer(overlapUpload);
            gs.UploadImage(reinterpret_cast<const uint8_t *>(overlapUploadA.data()), sizeof(overlapUploadA));
            gs.BeginTransfer(overlapUpload);
            gs.UploadImage(reinterpret_cast<const uint8_t *>(overlapUploadB.data()), sizeof(overlapUploadB));
            GSTransferCommand overlapUploadRead = overlapUpload;
            overlapUploadRead.bitbltbuf.sbp = overlapUpload.bitbltbuf.dbp;
            overlapUploadRead.bitbltbuf.sbw = overlapUpload.bitbltbuf.dbw;
            overlapUploadRead.bitbltbuf.spsm = GS_PSM_CT32;
            overlapUploadRead.trxpos.ssax = overlapUpload.trxpos.dsax;
            overlapUploadRead.trxpos.ssay = overlapUpload.trxpos.dsay;
            overlapUploadRead.direction = 1;
            std::array<uint8_t, 8> overlapUploadBytes{};
            gs.BeginTransfer(overlapUploadRead);
            const bool overlapUploadReadCount = gs.ConsumeLocalToHostBytes(
                overlapUploadBytes.data(), overlapUploadBytes.size()) == overlapUploadBytes.size();
            const std::array<uint8_t, 8> expectedOverlapUploadBytes{
                0x33, 0x22, 0x11, 0xff, 0x66, 0x55, 0x44, 0xff};
            overlapUploadsPassed = overlapUploadReadCount && overlapUploadBytes == expectedOverlapUploadBytes;

            GSTransferCommand localTransfer{};
            localTransfer.bitbltbuf.sbp = 256;
            localTransfer.bitbltbuf.sbw = 1;
            localTransfer.bitbltbuf.spsm = GS_PSM_CT32;
            localTransfer.bitbltbuf.dbp = 768;
            localTransfer.bitbltbuf.dbw = 1;
            localTransfer.bitbltbuf.dpsm = GS_PSM_CT32;
            localTransfer.trxpos.ssax = 4;
            localTransfer.trxpos.ssay = 48;
            localTransfer.trxpos.dsax = 4;
            localTransfer.trxpos.dsay = 48;
            localTransfer.trxreg.rrw = 2;
            localTransfer.trxreg.rrh = 2;
            localTransfer.direction = 2;
            gs.BeginTransfer(localTransfer);

            // Seed the overlap case through the production host-to-local path.
            const std::array<uint32_t, 3> overlapSeed{0xff010203u, 0xff111213u, 0xff212223u};
            GSTransferCommand seedTransfer = localTransfer;
            seedTransfer.bitbltbuf.dbp = 512;
            seedTransfer.trxreg.rrw = 3;
            seedTransfer.trxreg.rrh = 1;
            seedTransfer.direction = 0;
            gs.BeginTransfer(seedTransfer);
            gs.UploadImage(reinterpret_cast<const uint8_t *>(overlapSeed.data()), sizeof(overlapSeed));

            // An overlapping forward copy must read the values written by
            // earlier pixels, matching the guest's ordered GS transfer loop.
            GSTransferCommand overlapTransfer = localTransfer;
            overlapTransfer.bitbltbuf.sbp = 512;
            overlapTransfer.bitbltbuf.dbp = 512;
            overlapTransfer.trxpos.ssax = 4;
            overlapTransfer.trxpos.dsax = 5;
            overlapTransfer.trxreg.rrw = 3;
            overlapTransfer.trxreg.rrh = 1;
            gs.BeginTransfer(overlapTransfer);

            GSTransferCommand readbackTransfer = imageTransfer;
            readbackTransfer.bitbltbuf.sbp = 256;
            readbackTransfer.bitbltbuf.sbw = 1;
            readbackTransfer.bitbltbuf.spsm = GS_PSM_CT32;
            readbackTransfer.trxpos.ssax = 4;
            readbackTransfer.trxpos.ssay = 48;
            readbackTransfer.direction = 1;
            gs.BeginTransfer(readbackTransfer);
            const GSTransferSnapshot readbackState = gs.GetTransferSnapshot();
            const bool readbackCountMatched = readbackState.direction == 1u &&
                readbackState.localToHostPendingBytes == localReadbackBytes.size();
            if (gs.ConsumeLocalToHostBytes(localReadbackBytes.data(), localReadbackBytes.size()) != localReadbackBytes.size())
                throw std::runtime_error("SDL_GPU local-to-host CT32 byte count mismatch");
            localReadbackStatePassed = readbackCountMatched && gs.GetTransferSnapshot().localToHostPendingBytes == 0u;

            auto readbackFormat = [&](uint8_t psm, uint32_t bp, uint32_t x, uint32_t width,
                                      uint8_t *destination, uint32_t bytes) {
                GSTransferCommand transfer = readbackTransfer;
                transfer.bitbltbuf.sbp = bp;
                transfer.bitbltbuf.spsm = psm;
                transfer.trxpos.ssax = x;
                transfer.trxreg.rrw = width;
                transfer.trxreg.rrh = 1;
                gs.BeginTransfer(transfer);
                return gs.ConsumeLocalToHostBytes(destination, bytes) == bytes;
            };
            const bool ct24ReadbackCount = readbackFormat(GS_PSM_CT24, 1024, 1, 1,
                ct24ReadbackBytes.data(), ct24ReadbackBytes.size());
            const bool ct16ReadbackCount = readbackFormat(GS_PSM_CT16, 1280, 1, 1,
                ct16ReadbackBytes.data(), ct16ReadbackBytes.size());
            const bool p8ReadbackCount = readbackFormat(GS_PSM_T8, 1536, 1, 1,
                p8ReadbackBytes.data(), p8ReadbackBytes.size());
            GSTransferCommand nibbleReadback = readbackTransfer;
            nibbleReadback.bitbltbuf.sbp = 1792;
            nibbleReadback.bitbltbuf.spsm = GS_PSM_T4;
            nibbleReadback.trxpos.ssax = 2;
            nibbleReadback.trxreg.rrw = 3;
            nibbleReadback.trxreg.rrh = 1;
            gs.BeginTransfer(nibbleReadback);
            if (gs.ConsumeLocalToHostBytes(nibbleReadbackBytes.data(), nibbleReadbackBytes.size()) != nibbleReadbackBytes.size())
                throw std::runtime_error("SDL_GPU local-to-host T4 byte count mismatch");
            if (!ct24ReadbackCount || !ct16ReadbackCount || !p8ReadbackCount)
                throw std::runtime_error("SDL_GPU local-to-host packed format byte count mismatch");

            GSPresentationRequest presentation{};
            presentation.pmode = 1u; // CRT1 enabled.
            presentation.dispfb1 = 8u | (1ull << 9u) | (4ull << 32u) | (48ull << 43u);
            presentation.display1 = (63ull << 32u) | (63ull << 44u); // 64x64.
            gs.Sync(GSSyncReason::Presentation);
            const PresentationFrame presented = gs.Present(presentation);
            gpuPresentWidth = presented.width;
            if (presented.pixels.size() >= 4u)
                gpuPresentPixel = uint32_t(presented.pixels[0]) | (uint32_t(presented.pixels[1]) << 8u) |
                    (uint32_t(presented.pixels[2]) << 16u) | (uint32_t(presented.pixels[3]) << 24u);

            GSTransferCommand nextVideoFrame = imageTransfer;
            nextVideoFrame.trxreg.rrw = 1;
            nextVideoFrame.trxreg.rrh = 1;
            const uint32_t nextVideoPixel = 0xffc0ffeeu;
            gs.BeginTransfer(nextVideoFrame);
            gs.UploadImage(reinterpret_cast<const uint8_t *>(&nextVideoPixel), sizeof(nextVideoPixel));
            gs.Present(presentation); // Queue the next readback without requiring it to be ready yet.
            gs.Sync(GSSyncReason::Finish); // Explicit GS completion makes this differential check deterministic.
            const PresentationFrame nextPresented = gs.Present(presentation);
            if (nextPresented.pixels.size() >= 4u)
                gpuAsyncPresentPixel = uint32_t(nextPresented.pixels[0]) | (uint32_t(nextPresented.pixels[1]) << 8u) |
                    (uint32_t(nextPresented.pixels[2]) << 16u) | (uint32_t(nextPresented.pixels[3]) << 24u);
            gpuAsyncPresentPassed = gpuAsyncPresentPixel == nextVideoPixel;
            gs.SnapshotVram(vram);
            GSMem::TexturePageCache packedUploadCache;
            ct24UploadPixel = GSMem::ReadTexture(packedUploadCache, vram.data(), GS_PSM_CT32, 2048, 1, 2, 3);
            compactUploadPassed =
                GSMem::ReadTexture(packedUploadCache, vram.data(), GS_PSM_CT16, 2304, 1, 2, 3) == 0x5678u &&
                GSMem::ReadTexture(packedUploadCache, vram.data(), GS_PSM_CT16, 2304, 1, 3, 3) == 0x1234u &&
                GSMem::ReadTexture(packedUploadCache, vram.data(), GS_PSM_T8, 2560, 1, 2, 3) == 0x56u &&
                GSMem::ReadTexture(packedUploadCache, vram.data(), GS_PSM_T8, 2560, 1, 3, 3) == 0x78u &&
                GSMem::ReadTexture(packedUploadCache, vram.data(), GS_PSM_T4, 2816, 1, 2, 3) == 0x5u &&
                GSMem::ReadTexture(packedUploadCache, vram.data(), GS_PSM_T4, 2816, 1, 3, 3) == 0xau &&
                GSMem::ReadTexture(packedUploadCache, vram.data(), GS_PSM_T4, 2816, 1, 4, 3) == 0x3u;
            GSMem::TexturePageCache lineCache;
            size_t linePixel = 0;
            for (uint32_t y = 58; y < 78; ++y)
                for (uint32_t x = 128; x < 150; ++x)
                    gpuLineRegion[linePixel++] = GSMem::ReadTexture(lineCache, vram.data(), GS_PSM_CT32, 0, 10, x, y);
        }
        const char *gpuSelection = std::getenv("PS2X_GS_SDL_GPU");
        const std::string savedGpuSelection = gpuSelection ? gpuSelection : "";
        unsetenv("PS2X_GS_SDL_GPU");
        std::vector<uint8_t> cpuLineVram(bytes, 0);
        {
            GSCpuBackend cpuLine;
            cpuLine.Initialize(cpuLineVram.data(), bytes);
            for (const auto &line : lineReferences) cpuLine.Submit(line);
        }
        if (!savedGpuSelection.empty()) setenv("PS2X_GS_SDL_GPU", savedGpuSelection.c_str(), 1);
        GSMem::TexturePageCache cpuLineCache;
        size_t linePixel = 0;
        gpuLineMatchesCpu = true;
        for (uint32_t y = 58; y < 78; ++y)
            for (uint32_t x = 128; x < 150; ++x)
                gpuLineMatchesCpu = gpuLineMatchesCpu && gpuLineRegion[linePixel++] ==
                    GSMem::ReadTexture(cpuLineCache, cpuLineVram.data(), GS_PSM_CT32, 0, 10, x, y);
        GSPrimitiveBatch aliasedDepth{};
        aliasedDepth.vertexCount = 2;
        aliasedDepth.state.prim.type = GS_PRIM_SPRITE;
        aliasedDepth.state.context.frame.fbp = 210;
        aliasedDepth.state.context.frame.fbw = 1;
        aliasedDepth.state.context.frame.psm = GS_PSM_CT32;
        aliasedDepth.state.context.zbuf.zbp = 210;
        aliasedDepth.state.context.zbuf.psm = GS_PSM_Z24;
        aliasedDepth.state.context.zbuf.zmask = false;
        aliasedDepth.state.context.test = 1ull << 17u;
        aliasedDepth.state.context.scissor = {0, 7, 0, 7};
        aliasedDepth.vertices[0].x = 2;
        aliasedDepth.vertices[0].y = 2;
        aliasedDepth.vertices[1].x = 5;
        aliasedDepth.vertices[1].y = 5;
        for (auto &vertex : aliasedDepth.vertices)
        {
            vertex.z = 0x00123456u;
            vertex.r = 0x31; vertex.g = 0x62; vertex.b = 0x93; vertex.a = 0xff;
        }
        std::vector<uint8_t> aliasedGpuVram(bytes, 0);
        std::vector<uint8_t> aliasedCpuVram(bytes, 0);
        {
            GSCpuBackend aliasedGpu;
            aliasedGpu.Initialize(aliasedGpuVram.data(), bytes);
            aliasedGpu.Submit(aliasedDepth);
            aliasedGpu.SnapshotVram(aliasedGpuVram);
        }
        unsetenv("PS2X_GS_SDL_GPU");
        {
            GSCpuBackend aliasedCpu;
            aliasedCpu.Initialize(aliasedCpuVram.data(), bytes);
            aliasedCpu.Submit(aliasedDepth);
        }
        if (!savedGpuSelection.empty()) setenv("PS2X_GS_SDL_GPU", savedGpuSelection.c_str(), 1);
        gpuAliasedDepthMatchesCpu = aliasedGpuVram == aliasedCpuVram;

        // Two untextured sprites occupy separate GS pages. The GPU may encode
        // their dispatches in one compute pass; full-VRAM comparison guards
        // the page-range proof and preserves exact pixel output.
        GSPrimitiveBatch disjointDrawA{};
        disjointDrawA.vertexCount = 2;
        disjointDrawA.state.prim.type = GS_PRIM_SPRITE;
        disjointDrawA.state.context.frame.fbp = 100;
        disjointDrawA.state.context.frame.fbw = 10;
        disjointDrawA.state.context.frame.psm = GS_PSM_CT32;
        disjointDrawA.state.context.zbuf.psm = GS_PSM_Z24;
        disjointDrawA.state.context.zbuf.zmask = true;
        disjointDrawA.state.context.test = 1ull << 17u;
        disjointDrawA.state.context.scissor = {0, 255, 0, 63};
        disjointDrawA.vertices[0].x = 8; disjointDrawA.vertices[0].y = 4;
        disjointDrawA.vertices[1].x = 24; disjointDrawA.vertices[1].y = 18;
        disjointDrawA.vertices[0].z = disjointDrawA.vertices[1].z = 1.0;
        for (auto &vertex : disjointDrawA.vertices)
        {
            vertex.r = 0xe0; vertex.g = 0x30; vertex.b = 0x10; vertex.a = 255;
        }
        GSPrimitiveBatch disjointDrawB = disjointDrawA;
        disjointDrawB.vertices[0].x = 72; disjointDrawB.vertices[0].y = 4;
        disjointDrawB.vertices[1].x = 90; disjointDrawB.vertices[1].y = 18;
        for (auto &vertex : disjointDrawB.vertices)
        {
            vertex.r = 0x20; vertex.g = 0xb0; vertex.b = 0xf0; vertex.a = 255;
        }
        std::vector<uint8_t> disjointGpuVram(bytes, 0);
        std::vector<uint8_t> disjointCpuVram(bytes, 0);
        {
            GSCpuBackend disjointGpu;
            disjointGpu.Initialize(disjointGpuVram.data(), bytes);
            disjointGpu.Submit(disjointDrawA);
            disjointGpu.Submit(disjointDrawB);
            disjointGpu.SnapshotVram(disjointGpuVram);
        }
        unsetenv("PS2X_GS_SDL_GPU");
        {
            GSCpuBackend disjointCpu;
            disjointCpu.Initialize(disjointCpuVram.data(), bytes);
            disjointCpu.Submit(disjointDrawA);
            disjointCpu.Submit(disjointDrawB);
        }
        if (!savedGpuSelection.empty()) setenv("PS2X_GS_SDL_GPU", savedGpuSelection.c_str(), 1);
        gpuDisjointDrawsMatchCpu = disjointGpuVram == disjointCpuVram;

        // Textured draws can share an encoder only while the sampled GS page
        // is disjoint from every color/depth write. Verify full VRAM and that
        // the GPU accepted the second draw into the same pass.
        std::vector<uint8_t> texturedGpuVram(bytes, 0);
        std::vector<uint8_t> texturedCpuVram(bytes, 0);
        for (uint32_t y = 0; y < 16u; ++y)
            for (uint32_t x = 0; x < 16u; ++x)
                GSMem::WriteCT32(texturedGpuVram.data(), 300u, 1u, x, y,
                    0xff000000u | (x * 13u << 16u) | (y * 11u << 8u) | (x + y));
        texturedCpuVram = texturedGpuVram;
        const std::vector<uint8_t> texturedSeedVram = texturedGpuVram;
        std::array<uint16_t, 512> texturedClut{};
        GSPrimitiveBatch texturedA = disjointDrawA;
        texturedA.state.context.frame.fbp = 100u;
        texturedA.state.prim.tme = true;
        texturedA.state.prim.fst = true;
        texturedA.state.context.tex0.tbp0 = 300u;
        texturedA.state.context.tex0.tbw = 1u;
        texturedA.state.context.tex0.psm = GS_PSM_CT32;
        texturedA.state.context.tex0.tcc = 1u;
        texturedA.state.textureWidth = 16u;
        texturedA.state.textureHeight = 16u;
        texturedA.vertices[0].x = 8; texturedA.vertices[0].y = 4;
        texturedA.vertices[1].x = 24; texturedA.vertices[1].y = 20;
        texturedA.vertices[0].u = 0u; texturedA.vertices[0].v = 0u;
        texturedA.vertices[1].u = 16u * 16u; texturedA.vertices[1].v = 16u * 16u;
        GSPrimitiveBatch texturedB = texturedA;
        texturedB.state.context.frame.fbp = 110u;
        texturedB.vertices[0].x = 72; texturedB.vertices[0].y = 4;
        texturedB.vertices[1].x = 88; texturedB.vertices[1].y = 20;
        GSPrimitiveBatch texturedAlias = texturedB;
        texturedAlias.state.context.frame.fbp = 120u;
        texturedAlias.state.context.tex0.tbp0 = 100u * 32u; // Reads the first draw's target pages.
        {
            GSSDLGpuRaster texturedGpu(texturedGpuVram.data(), texturedClut.data());
            texturedGpu.submit(texturedA, 8, 4, 23, 19, false, false);
            texturedGpu.textureFlush();
            texturedGpu.submit(texturedB, 72, 4, 87, 19, false, false);
            texturedGpu.submit(texturedAlias, 72, 4, 87, 19, false, false);
            texturedBatchedDrawCount = texturedGpu.batchedDraws();
            texturedGpu.sync();
            texturedRasterPassCount = texturedGpu.rasterDrawPasses();
        }
        unsetenv("PS2X_GS_SDL_GPU");
        {
            GSCpuBackend texturedCpu;
            texturedCpu.Initialize(texturedCpuVram.data(), bytes);
            texturedCpu.Submit(texturedA);
            texturedCpu.TextureFlush();
            texturedCpu.Submit(texturedB);
            texturedCpu.Submit(texturedAlias);
        }
        if (!savedGpuSelection.empty()) setenv("PS2X_GS_SDL_GPU", savedGpuSelection.c_str(), 1);
        texturedFullVramMatchesCpu = texturedGpuVram == texturedCpuVram;
        gpuTexturedDisjointDrawsMatchCpu = texturedBatchedDrawCount == 1u &&
            texturedRasterPassCount == 2u && texturedFullVramMatchesCpu;

        // TEXFLUSH has no cache work in this direct-VRAM shader. These partial
        // uploads touch disjoint rows in one GS page and should stay batched.
        // Host-to-local is direction 0; the default transfer direction is 3.
        std::vector<uint8_t> imageChunkGpuVram(bytes, 0u);
        std::vector<uint8_t> imageChunkCpuVram(bytes, 0u);
        GSTransferCommand imageChunkA{};
        imageChunkA.direction = 0u;
        imageChunkA.bitbltbuf.dbp = 3200u;
        imageChunkA.bitbltbuf.dbw = 1u;
        imageChunkA.bitbltbuf.dpsm = GS_PSM_CT32;
        imageChunkA.trxreg.rrw = 16u;
        imageChunkA.trxreg.rrh = 16u;
        GSTransferCommand imageChunkB = imageChunkA;
        imageChunkB.trxpos.dsay = 2u;
        std::array<uint32_t, 16> imageChunkPixelsA{}, imageChunkPixelsB{};
        for (uint32_t i = 0; i < 16u; ++i)
        {
            imageChunkPixelsA[i] = 0xff000000u | (i * 0x010101u);
            imageChunkPixelsB[i] = 0xff000000u | (0x00ffffffu - i * 0x010101u);
        }
        uint64_t imageChunkBatchCount = 0u, imageChunkCount = 0u;
        {
            GSSDLGpuRaster imageChunkGpu(imageChunkGpuVram.data(), texturedClut.data());
            imageChunkGpu.uploadImage(imageChunkA, 0u,
                reinterpret_cast<const uint8_t *>(imageChunkPixelsA.data()), 16u);
            imageChunkGpu.textureFlush();
            imageChunkGpu.uploadImage(imageChunkB, 0u,
                reinterpret_cast<const uint8_t *>(imageChunkPixelsB.data()), 16u);
            imageChunkGpu.sync();
            imageChunkBatchCount = imageChunkGpu.imageUploadBatches();
            imageChunkCount = imageChunkGpu.imageUploadChunks();
        }
        unsetenv("PS2X_GS_SDL_GPU");
        {
            GSCpuBackend imageChunkCpu;
            imageChunkCpu.Initialize(imageChunkCpuVram.data(), bytes);
            imageChunkCpu.BeginTransfer(imageChunkA);
            imageChunkCpu.UploadImage(reinterpret_cast<const uint8_t *>(imageChunkPixelsA.data()), sizeof(imageChunkPixelsA));
            imageChunkCpu.TextureFlush();
            imageChunkCpu.BeginTransfer(imageChunkB);
            imageChunkCpu.UploadImage(reinterpret_cast<const uint8_t *>(imageChunkPixelsB.data()), sizeof(imageChunkPixelsB));
        }
        if (!savedGpuSelection.empty()) setenv("PS2X_GS_SDL_GPU", savedGpuSelection.c_str(), 1);
        gpuDisjointImageChunksBatchMatchCpu = imageChunkGpuVram == imageChunkCpuVram &&
            imageChunkBatchCount == 1u && imageChunkCount == 2u;

        // Exercise actual LOD selection in one draw: Q=.25 selects mip level 2.
        // This isolates TEX1/MIPTBP sampling from batching and feedback hazards.
        std::vector<uint8_t> mipGpuVram = texturedCpuVram;
        std::vector<uint8_t> mipCpuVram = texturedCpuVram;
        for (uint32_t y = 0; y < 4u; ++y)
            for (uint32_t x = 0; x < 4u; ++x)
            {
                const uint32_t color = 0xff000000u | (x * 31u << 16u) | (y * 29u << 8u) | (x + y);
                GSMem::WriteCT32(mipGpuVram.data(), 416u, 1u, x, y, color);
                GSMem::WriteCT32(mipCpuVram.data(), 416u, 1u, x, y, color);
            }
        GSPrimitiveBatch mipDraw = texturedA;
        mipDraw.state.context.frame.fbp = 130u;
        mipDraw.state.prim.fst = false;
        mipDraw.state.context.tex1 = 0x88u; // LCM=0, MXL=2, MMIN=2.
        mipDraw.state.context.miptbp1 = 416u | (1u << 14u) | (416u << 20u) | (1ull << 34u);
        mipDraw.vertices[0].s = 0.0f; mipDraw.vertices[0].t = 0.0f;
        mipDraw.vertices[1].s = 16.0f; mipDraw.vertices[1].t = 16.0f;
        mipDraw.vertices[0].q = mipDraw.vertices[1].q = 0.25f;
        GSPrimitiveBatch mipDrawB = mipDraw;
        mipDrawB.state.context.frame.fbp = 140u;
        mipDrawB.vertices[0].x += 64.0f; mipDrawB.vertices[1].x += 64.0f;
        uint64_t mipBatchedDraws = 0u;
        uint64_t mipRasterPasses = 0u;
        {
            GSSDLGpuRaster mipGpu(mipGpuVram.data(), texturedClut.data());
            mipGpu.submit(mipDraw, 8, 4, 23, 19, false, false);
            mipGpu.submit(mipDrawB, 72, 4, 87, 19, false, false);
            mipBatchedDraws = mipGpu.batchedDraws();
            mipGpu.sync();
            mipRasterPasses = mipGpu.rasterDrawPasses();
        }
        unsetenv("PS2X_GS_SDL_GPU");
        {
            GSCpuBackend mipCpu;
            mipCpu.Initialize(mipCpuVram.data(), bytes);
            mipCpu.Submit(mipDraw);
            mipCpu.Submit(mipDrawB);
        }
        if (!savedGpuSelection.empty()) setenv("PS2X_GS_SDL_GPU", savedGpuSelection.c_str(), 1);
        // Mip-sampled draws batch only with PS2X_GS_WIDE_BATCH=1 (their mip
        // footprints join the hazard check); only the image is asserted here.
        gpuMipLevelMatchesCpu = mipGpuVram == mipCpuVram;
        std::printf("  mip LOD: batched draws=%llu raster passes=%llu VRAM %s\n",
            static_cast<unsigned long long>(mipBatchedDraws), static_cast<unsigned long long>(mipRasterPasses),
            mipGpuVram == mipCpuVram ? "matches" : "DIFFERS");

        // Nonzero MIPTBP is unreachable for MXL=0, MMIN<2, or constant LOD=0.
        std::vector<uint8_t> mipZeroGpuVram = texturedSeedVram;
        std::vector<uint8_t> mipZeroCpuVram = texturedSeedVram;
        GSPrimitiveBatch mipZeroA = mipDraw;
        mipZeroA.state.context.tex1 = 0x80u; // LCM=0, MXL=0, MMIN=2.
        mipZeroA.state.context.miptbp1 = 416u | (1u << 14u) | (416u << 20u) | (1ull << 34u);
        mipZeroA.state.context.scissor.y1 = 127;
        mipZeroA.vertices[1].x = 72.0f; mipZeroA.vertices[1].y = 68.0f;
        GSPrimitiveBatch mipZeroB = mipZeroA;
        mipZeroB.state.context.frame.fbp = 180u;
        mipZeroB.state.context.tex1 = 0x48u; // MXL=2, MMIN=1: no mip levels.
        mipZeroB.vertices[0].x += 72.0f; mipZeroB.vertices[1].x += 72.0f;
        GSPrimitiveBatch mipZeroC = mipZeroA;
        mipZeroC.state.context.frame.fbp = 230u;
        mipZeroC.state.context.tex1 = 0x89u; // LCM=1, MXL=2, MMIN=2, K=0.
        mipZeroC.vertices[0].x += 144.0f; mipZeroC.vertices[1].x += 144.0f;
        uint64_t mipZeroBatchedDraws = 0u;
        uint64_t mipZeroRasterPasses = 0u;
        {
            GSSDLGpuRaster mipZeroGpu(mipZeroGpuVram.data(), texturedClut.data());
            mipZeroGpu.submit(mipZeroA, 8, 4, 71, 67, false, false);
            mipZeroGpu.submit(mipZeroB, 80, 4, 143, 67, false, false);
            mipZeroGpu.submit(mipZeroC, 152, 4, 215, 67, false, false);
            mipZeroBatchedDraws = mipZeroGpu.batchedDraws();
            mipZeroGpu.sync();
            mipZeroRasterPasses = mipZeroGpu.rasterDrawPasses();
        }
        unsetenv("PS2X_GS_SDL_GPU");
        {
            GSCpuBackend mipZeroCpu;
            mipZeroCpu.Initialize(mipZeroCpuVram.data(), bytes);
            mipZeroCpu.Submit(mipZeroA);
            mipZeroCpu.Submit(mipZeroB);
            mipZeroCpu.Submit(mipZeroC);
        }
        if (!savedGpuSelection.empty()) setenv("PS2X_GS_SDL_GPU", savedGpuSelection.c_str(), 1);
        gpuMiptbpMxlZeroBatchMatchesCpu = mipZeroGpuVram == mipZeroCpuVram &&
            mipZeroBatchedDraws == 2u && mipZeroRasterPasses == 1u;

        // Reproduce the formerly failing feedback sequence with nonzero mip
        // registers while LCM=1 keeps the selected level at zero.
        std::vector<uint8_t> mipAliasGpuVram = texturedSeedVram;
        std::vector<uint8_t> mipAliasCpuVram = texturedSeedVram;
        GSPrimitiveBatch mipAliasA = texturedA;
        mipAliasA.state.context.tex1 = 0x85u;
        mipAliasA.state.context.miptbp1 = 416u | (1u << 14u) | (416u << 20u) | (1ull << 34u);
        GSPrimitiveBatch mipAliasB = texturedB;
        mipAliasB.state.context.tex1 = mipAliasA.state.context.tex1;
        mipAliasB.state.context.miptbp1 = mipAliasA.state.context.miptbp1;
        GSPrimitiveBatch mipAliasRead = texturedAlias;
        mipAliasRead.state.context.tex1 = mipAliasA.state.context.tex1;
        mipAliasRead.state.context.miptbp1 = mipAliasA.state.context.miptbp1;
        uint64_t mipAliasBatchedDraws = 0u;
        uint64_t mipAliasRasterPasses = 0u;
        {
            GSSDLGpuRaster mipAliasGpu(mipAliasGpuVram.data(), texturedClut.data());
            mipAliasGpu.submit(mipAliasA, 8, 4, 23, 19, false, false);
            mipAliasGpu.submit(mipAliasB, 72, 4, 87, 19, false, false);
            mipAliasGpu.submit(mipAliasRead, 72, 4, 87, 19, false, false);
            mipAliasBatchedDraws = mipAliasGpu.batchedDraws();
            mipAliasGpu.sync();
            mipAliasRasterPasses = mipAliasGpu.rasterDrawPasses();
        }
        unsetenv("PS2X_GS_SDL_GPU");
        {
            GSCpuBackend mipAliasCpu;
            mipAliasCpu.Initialize(mipAliasCpuVram.data(), bytes);
            mipAliasCpu.Submit(mipAliasA);
            mipAliasCpu.Submit(mipAliasB);
            mipAliasCpu.Submit(mipAliasRead);
        }
        if (!savedGpuSelection.empty()) setenv("PS2X_GS_SDL_GPU", savedGpuSelection.c_str(), 1);
        // The two writers may share a pass; the feedback read must not join them.
        gpuMipAliasMatchesCpu = mipAliasGpuVram == mipAliasCpuVram &&
            mipAliasBatchedDraws <= 1u && mipAliasRasterPasses >= 2u;
        std::printf("  mip alias: batched draws=%llu raster passes=%llu VRAM %s\n",
            static_cast<unsigned long long>(mipAliasBatchedDraws), static_cast<unsigned long long>(mipAliasRasterPasses),
            mipAliasGpuVram == mipAliasCpuVram ? "matches" : "DIFFERS");

        std::vector<uint8_t> batchCapacityGpuVram(bytes, 0);
        std::vector<uint8_t> batchCapacityCpuVram(bytes, 0);
        std::array<uint16_t, 512> batchCapacityClut{};
        uint64_t capacityBatchedDraws = 0u;
        uint64_t capacityRasterPasses = 0u;
        {
            GSSDLGpuRaster batchGpu(batchCapacityGpuVram.data(), batchCapacityClut.data());
            for (uint32_t i = 0; i < 129u; ++i)
            {
                GSPrimitiveBatch draw{};
                draw.vertexCount = 2;
                draw.state.prim.type = GS_PRIM_SPRITE;
                draw.state.context.frame.fbp = 200u + i;
                draw.state.context.frame.fbw = 10u;
                draw.state.context.frame.psm = GS_PSM_CT32;
                draw.state.context.zbuf.psm = GS_PSM_Z24;
                draw.state.context.zbuf.zmask = true;
                draw.state.context.test = 1ull << 17u;
                draw.state.context.scissor = {0, 255, 0, 63};
                draw.vertices[0].x = 1; draw.vertices[0].y = 1;
                draw.vertices[1].x = 3; draw.vertices[1].y = 3;
                for (auto &vertex : draw.vertices)
                {
                    vertex.r = static_cast<uint8_t>(i);
                    vertex.g = static_cast<uint8_t>(i >> 1u);
                    vertex.b = 0x5au;
                    vertex.a = 255u;
                }
                batchGpu.submit(draw, 1, 1, 2, 2, false, false);
            }
            capacityBatchedDraws = batchGpu.batchedDraws();
            batchGpu.sync();
            capacityRasterPasses = batchGpu.rasterDrawPasses();
        }
        unsetenv("PS2X_GS_SDL_GPU");
        {
            GSCpuBackend batchCpu;
            batchCpu.Initialize(batchCapacityCpuVram.data(), bytes);
            for (uint32_t i = 0; i < 129u; ++i)
            {
                GSPrimitiveBatch draw{};
                draw.vertexCount = 2;
                draw.state.prim.type = GS_PRIM_SPRITE;
                draw.state.context.frame.fbp = 200u + i;
                draw.state.context.frame.fbw = 10u;
                draw.state.context.frame.psm = GS_PSM_CT32;
                draw.state.context.zbuf.psm = GS_PSM_Z24;
                draw.state.context.zbuf.zmask = true;
                draw.state.context.test = 1ull << 17u;
                draw.state.context.scissor = {0, 255, 0, 63};
                draw.vertices[0].x = 1; draw.vertices[0].y = 1;
                draw.vertices[1].x = 3; draw.vertices[1].y = 3;
                for (auto &vertex : draw.vertices)
                {
                    vertex.r = static_cast<uint8_t>(i);
                    vertex.g = static_cast<uint8_t>(i >> 1u);
                    vertex.b = 0x5au;
                    vertex.a = 255u;
                }
                batchCpu.Submit(draw);
            }
        }
        if (!savedGpuSelection.empty()) setenv("PS2X_GS_SDL_GPU", savedGpuSelection.c_str(), 1);
        gpuRasterBatchCapacityMatchesCpu = capacityBatchedDraws == 127u &&
            capacityRasterPasses == 2u && batchCapacityGpuVram == batchCapacityCpuVram;

        GSPrimitiveBatch orderedA{};
        orderedA.vertexCount = 2;
        orderedA.state.prim.type = GS_PRIM_SPRITE;
        orderedA.state.context.frame.fbp = 160u;
        orderedA.state.context.frame.fbw = 10u;
        orderedA.state.context.frame.psm = GS_PSM_CT32;
        orderedA.state.context.zbuf.zbp = 210u;
        orderedA.state.context.zbuf.psm = GS_PSM_Z24;
        orderedA.state.context.zbuf.zmask = false;
        orderedA.state.context.test = 1ull << 17u;
        orderedA.state.context.scissor = {0, 255, 0, 63};
        orderedA.vertices[0].x = 8; orderedA.vertices[0].y = 8;
        orderedA.vertices[1].x = 20; orderedA.vertices[1].y = 20;
        orderedA.vertices[0].z = orderedA.vertices[1].z = 10.0;
        for (auto &vertex : orderedA.vertices)
        {
            vertex.r = 0xe0; vertex.g = 0x20; vertex.b = 0x10; vertex.a = 255u;
        }
        GSPrimitiveBatch orderedB = orderedA;
        orderedB.vertices[0].x = 14; orderedB.vertices[0].y = 14;
        orderedB.vertices[1].x = 26; orderedB.vertices[1].y = 26;
        orderedB.vertices[0].z = orderedB.vertices[1].z = 20.0;
        for (auto &vertex : orderedB.vertices)
        {
            vertex.r = 0x10; vertex.g = 0x40; vertex.b = 0xd0; vertex.a = 255u;
        }
        std::vector<uint8_t> orderedGpuVram(bytes, 0), orderedCpuVram(bytes, 0);
        std::array<uint16_t, 512> orderedClut{};
        uint64_t orderedBatchedDraws = 0u, orderedRasterPasses = 0u;
        {
            GSSDLGpuRaster orderedGpu(orderedGpuVram.data(), orderedClut.data());
            orderedGpu.submit(orderedA, 8, 8, 19, 19, false, false);
            orderedGpu.submit(orderedB, 14, 14, 25, 25, false, false);
            orderedBatchedDraws = orderedGpu.batchedDraws();
            orderedGpu.sync();
            orderedRasterPasses = orderedGpu.rasterDrawPasses();
        }
        unsetenv("PS2X_GS_SDL_GPU");
        {
            GSCpuBackend orderedCpu;
            orderedCpu.Initialize(orderedCpuVram.data(), bytes);
            orderedCpu.Submit(orderedA);
            orderedCpu.Submit(orderedB);
        }
        if (!savedGpuSelection.empty()) setenv("PS2X_GS_SDL_GPU", savedGpuSelection.c_str(), 1);
        gpuRasterOrderedBatchMatchesCpu = orderedBatchedDraws == 1u && orderedRasterPasses == 1u &&
            orderedGpuVram == orderedCpuVram;

        GSPrimitiveBatch nonfiniteSprite{};
        nonfiniteSprite.vertexCount = 2;
        nonfiniteSprite.state.prim.type = GS_PRIM_SPRITE;
        nonfiniteSprite.state.prim.tme = true;
        nonfiniteSprite.state.context.frame.fbp = 16;
        nonfiniteSprite.state.context.frame.fbw = 1;
        nonfiniteSprite.state.context.frame.psm = GS_PSM_CT32;
        nonfiniteSprite.state.context.zbuf.psm = GS_PSM_Z32;
        nonfiniteSprite.state.context.zbuf.zmask = true;
        nonfiniteSprite.state.context.test = 1ull << 17u;
        nonfiniteSprite.state.context.scissor = {0, 7, 0, 7};
        nonfiniteSprite.state.context.tex0.tbp0 = 1024;
        nonfiniteSprite.state.context.tex0.tbw = 1;
        nonfiniteSprite.state.context.tex0.psm = GS_PSM_CT32;
        nonfiniteSprite.state.textureWidth = nonfiniteSprite.state.textureHeight = 4;
        nonfiniteSprite.vertices[0].x = 1;
        nonfiniteSprite.vertices[0].y = 1;
        nonfiniteSprite.vertices[0].s = 0.5f;
        nonfiniteSprite.vertices[0].t = 0.5f;
        nonfiniteSprite.vertices[0].q = 1.0f;
        nonfiniteSprite.vertices[1].x = 3;
        nonfiniteSprite.vertices[1].y = 3;
        nonfiniteSprite.vertices[1].s = 1.5f;
        nonfiniteSprite.vertices[1].t = std::numeric_limits<float>::quiet_NaN();
        nonfiniteSprite.vertices[1].q = 1.0f;
        for (auto &vertex : nonfiniteSprite.vertices)
        {
            vertex.z = 1.0f;
            vertex.r = 0x40; vertex.g = 0x80; vertex.b = 0xc0; vertex.a = 0xff;
        }
        GSPrimitiveBatch explicitSprite = nonfiniteSprite;
        explicitSprite.vertices[0].t = explicitSprite.vertices[1].t = 0.0f;
        std::vector<uint8_t> nonfiniteVram(bytes, 0), explicitVram(bytes, 0);
        {
            GSCpuBackend gpu;
            gpu.Initialize(nonfiniteVram.data(), bytes);
            gpu.Submit(nonfiniteSprite);
            gpu.SnapshotVram(nonfiniteVram);
        }
        {
            GSCpuBackend gpu;
            gpu.Initialize(explicitVram.data(), bytes);
            gpu.Submit(explicitSprite);
            gpu.SnapshotVram(explicitVram);
        }
        gpuNonfiniteMatchesExplicit = nonfiniteVram == explicitVram;
        std::vector<uint8_t> strictVram(bytes, 0);
        {
            GSCpuBackend strictGs;
            strictGs.Initialize(strictVram.data(), bytes);
            GSPrimitiveBatch unsupported{};
            unsupported.vertexCount = 2;
            unsupported.state.prim.type = GS_PRIM_SPRITE;
            unsupported.state.prim.aa1 = true; // Deliberately outside current GPU coverage.
            unsupported.state.context.frame.fbw = 10;
            unsupported.state.context.frame.psm = GS_PSM_CT32;
            unsupported.state.context.scissor = {0, 127, 0, 127};
            unsupported.state.context.zbuf.psm = 48;
            unsupported.vertices[0].x = 2; unsupported.vertices[0].y = 2;
            unsupported.vertices[1].x = 8; unsupported.vertices[1].y = 8;
            bool rejected = false;
            try { strictGs.Submit(unsupported); }
            catch (const std::runtime_error &) { rejected = true; }
            gpuOnlyGuardPassed = rejected && std::all_of(strictVram.begin(), strictVram.end(), [](uint8_t b) { return b == 0; });
        }
        GSMem::TexturePageCache cache;
        rasterPixel = GSMem::ReadTexture(cache, vram.data(), GS_PSM_CT32, 0, 10, 2, 2);
        dither16Pixel = GSMem::ReadTexture(cache, vram.data(), GS_PSM_CT16S, 128, 10, 130, 60);
        tinyRasterPixel = GSMem::ReadTexture(cache, vram.data(), GS_PSM_CT32, 0, 10, 110, 40);
        clearPixel = GSMem::ReadTexture(cache, vram.data(), GS_PSM_CT32, 0, 10, 120, 40);
        uploadPixel = GSMem::ReadTexture(cache, vram.data(), GS_PSM_CT32, 256, 1, 4, 48);
        localCopyPixel = GSMem::ReadTexture(cache, vram.data(), GS_PSM_CT32, 768, 1, 4, 48);
        overlapCopyPixel = GSMem::ReadTexture(cache, vram.data(), GS_PSM_CT32, 512, 1, 7, 48);

        constexpr uint32_t paletteBp = 512u;
        constexpr uint32_t paletteColor = 0xff336699u;
        GSMem::WriteCT32(vram.data(), paletteBp, 1, 0, 0, paletteColor);
        std::array<uint16_t, 512> clut{};
        GSTex0Reg tex{};
        tex.psm = GS_PSM_T4;
        tex.cbp = paletteBp;
        tex.cpsm = GS_PSM_CT32;
        tex.cld = 1;
        // Earlier transfer/presentation checks mutate the shared VRAM fixture.
        // Restore packed-format source texels immediately before the isolated
        // scanout conversion test so it tests CT24/CT16 decoding, not test order.
        std::vector<uint8_t> packedScanoutVram(bytes, 0u);
        GSMem::WriteCT24(packedScanoutVram.data(), 32, 1, 4, 48, 0x00332211u);
        GSMem::WriteCT16(packedScanoutVram.data(), 64, 1, 4, 48, 0x0000a1b2u);
        GSMem::WriteCT32(packedScanoutVram.data(), 0, 1, 4, 48, 0xffabcdefu);
        GSMem::WriteCT32(packedScanoutVram.data(), paletteBp, 1, 0, 0, paletteColor);
        GSMem::WriteCT32(packedScanoutVram.data(), 1024u, 1, 0, 0, 0xff123456u);
        {
            GSSDLGpuRaster gpu(packedScanoutVram.data(), clut.data());
            GSFrameReg ct32Frame{};
            ct32Frame.fbp = 0;
            ct32Frame.fbw = 1;
            ct32Frame.psm = GS_PSM_CT32;
            std::vector<uint8_t> ct32FramePixels;
            if (gpu.copyFrameToHostRgba(ct32Frame, 64, 64, ct32FramePixels, true, false, 4, 48, false))
                gpuPresentInitialCt32Pixel = uint32_t(ct32FramePixels[0]) | (uint32_t(ct32FramePixels[1]) << 8u) |
                    (uint32_t(ct32FramePixels[2]) << 16u) | (uint32_t(ct32FramePixels[3]) << 24u);
            GSFrameReg ct24Frame{};
            ct24Frame.fbp = 32;
            ct24Frame.fbw = 1;
            ct24Frame.psm = GS_PSM_CT24;
            std::vector<uint8_t> ct24FramePixels;
            if (gpu.copyFrameToHostRgba(ct24Frame, 64, 64, ct24FramePixels, true, false, 4, 48, false))
                gpuPresentCt24Pixel = uint32_t(ct24FramePixels[0]) | (uint32_t(ct24FramePixels[1]) << 8u) |
                    (uint32_t(ct24FramePixels[2]) << 16u) | (uint32_t(ct24FramePixels[3]) << 24u);

            GSFrameReg ct16Frame{};
            ct16Frame.fbp = 64;
            ct16Frame.fbw = 1;
            ct16Frame.psm = GS_PSM_CT16;
            std::vector<uint8_t> ct16FramePixels;
            if (gpu.copyFrameToHostRgba(ct16Frame, 64, 64, ct16FramePixels, true, false, 4, 48, false))
                gpuPresentCt16Pixel = uint32_t(ct16FramePixels[0]) | (uint32_t(ct16FramePixels[1]) << 8u) |
                    (uint32_t(ct16FramePixels[2]) << 16u) | (uint32_t(ct16FramePixels[3]) << 24u);
            gpu.loadClut(tex, GSTexClutReg{});
            gpu.loadClut(tex, GSTexClutReg{}); // Identical source must reuse the GPU CLUT.
            GSTex0Reg secondTex = tex;
            secondTex.cbp = 1024u;
            secondTex.csa = 1u; // A partial load must preserve the first CSA bank.
            gpu.loadClut(secondTex, GSTexClutReg{});
            gpu.loadClut(tex, GSTexClutReg{}); // A second slot keeps the first palette resident.
            GSTex0Reg overlappingTex = secondTex;
            overlappingTex.csa = 0u;
            gpu.loadClut(overlappingTex, GSTexClutReg{}); // Overwrite A's bank.
            gpu.loadClut(tex, GSTexClutReg{}); // Restore A from cache without rolling back B.
            const bool repeatedLoadReused = gpu.clutLoadCacheHits() == 3u &&
                gpu.clutLoadDispatches() == 3u;
            gpu.sync();
            const bool partialBanksPreserved = clut[0] == 0x6699u && clut[256] == 0xff33u &&
                clut[16] == 0x3456u && clut[272] == 0xff12u;
            GSMem::WriteCT32(packedScanoutVram.data(), paletteBp, 1, 0, 0, 0xffabcdefu);
            gpu.markHostDirty(); // Host upload overlaps the cached CLUT source page.
            gpu.loadClut(tex, GSTexClutReg{});
            gpu.sync();
            clutCachePassed = repeatedLoadReused && partialBanksPreserved && gpu.clutLoadCacheHits() == 3u &&
                gpu.clutLoadDispatches() == 4u && clut[0] == 0xcdefu && clut[256] == 0xffabu;
            clutCacheHits = gpu.clutLoadCacheHits();
            clutCacheDispatches = gpu.clutLoadDispatches();
        }
        // Integrated GS path: loading CSA1 must not discard CSA0. CLD0 draws
        // then consume both retained banks without another VRAM palette load.
        std::vector<uint8_t> sharedBankGpuVram(bytes, 0u);
        GSMem::WriteCT32(sharedBankGpuVram.data(), paletteBp, 1u, 0u, 0u, paletteColor);
        GSMem::WriteCT32(sharedBankGpuVram.data(), 1024u, 1u, 0u, 0u, 0xff123456u);
        std::vector<uint8_t> sharedBankCpuVram = sharedBankGpuVram;
        const auto drawSharedBanks = [&](GSCpuBackend &backend)
        {
            GSTex0Reg bankA = tex;
            bankA.tbp0 = 3000u; bankA.tbw = 1u; bankA.tw = 4u; bankA.th = 4u;
            bankA.tcc = 1u; bankA.tfx = 1u;
            GSTex0Reg bankB = bankA;
            bankB.cbp = 1024u; bankB.csa = 1u;
            backend.LoadClut(bankA, GSTexClutReg{});
            backend.LoadClut(bankB, GSTexClutReg{});
            GSPrimitiveBatch draw = texturedA;
            draw.state.context.tex0 = bankA;
            draw.state.context.tex0.cld = 0u;
            backend.LoadClut(draw.state.context.tex0, GSTexClutReg{});
            backend.Submit(draw);
            draw.state.context.frame.fbp = 110u;
            draw.state.context.tex0 = bankB;
            draw.state.context.tex0.cld = 0u;
            backend.LoadClut(draw.state.context.tex0, GSTexClutReg{});
            backend.Submit(draw);
        };
        {
            GSCpuBackend gpu;
            gpu.Initialize(sharedBankGpuVram.data(), bytes);
            drawSharedBanks(gpu);
            gpu.SnapshotVram(sharedBankGpuVram);
        }
        unsetenv("PS2X_GS_SDL_GPU");
        {
            GSCpuBackend cpu;
            cpu.Initialize(sharedBankCpuVram.data(), bytes);
            drawSharedBanks(cpu);
        }
        if (!savedGpuSelection.empty()) setenv("PS2X_GS_SDL_GPU", savedGpuSelection.c_str(), 1);
        GSMem::TexturePageCache sharedBankCache;
        const bool sharedBankDrawsPassed = sharedBankGpuVram == sharedBankCpuVram &&
            GSMem::ReadTexture(sharedBankCache, sharedBankGpuVram.data(), GS_PSM_CT32, 3200u, 10u, 10u, 6u) == paletteColor &&
            GSMem::ReadTexture(sharedBankCache, sharedBankGpuVram.data(), GS_PSM_CT32, 3520u, 10u, 10u, 6u) == 0xff123456u;
        clutCachePassed = clutCachePassed && sharedBankDrawsPassed;
        const bool rasterPassed = rasterPixel == 0xff996633u;
        const bool dither16Passed = dither16Pixel == 0xb989u;
        const bool tinyRasterPassed = tinyRasterPixel == 0xff563412u;
        const bool clearPassed = clearPixel == 0xff563412u;
        const bool uploadPassed = uploadPixel == 0xffc0ffeeu;
        const bool ct24UploadPassed = ct24UploadPixel == 0xaa665544u;
        const bool localCopyPassed = localCopyPixel == 0xff102030u;
        const bool overlapCopyPassed = overlapCopyPixel == 0xff010203u;
        const std::array<uint8_t, 16> expectedReadbackBytes{
            0x30, 0x20, 0x10, 0xff, 0x60, 0x50, 0x40, 0xff,
            0x90, 0x80, 0x70, 0xff, 0xc0, 0xb0, 0xa0, 0xff};
        const bool localReadbackPassed = localReadbackBytes == expectedReadbackBytes && localReadbackStatePassed;
        const bool packedReadbackPassed = ct24ReadbackBytes == std::array<uint8_t, 3>{0x11, 0x22, 0x33} &&
            ct16ReadbackBytes == std::array<uint8_t, 2>{0xb2, 0xa1} && p8ReadbackBytes[0] == 0x7eu;
        const bool nibbleReadbackPassed = nibbleReadbackBytes[0] == 0x21u && nibbleReadbackBytes[1] == 0x03u;
        const bool gpuPresentPassed = gpuPresentWidth == 64u && gpuPresentPixel == 0xff102030u;
        gpuAsyncPresentPassed = gpuAsyncPresentPassed && gpuAsyncPresentPixel == 0xffc0ffeeu;
        const bool gpuPresentPackedPassed = gpuPresentInitialCt32Pixel == 0xffabcdefu &&
            gpuPresentCt24Pixel == 0xff332211u && gpuPresentCt16Pixel == 0x80426b94u;
        const bool clutPassed = clutCachePassed && clut[0] == 0xcdefu && clut[256] == 0xffabu;
        std::printf("SDL_GPU GS check bits: raster=%u lines=%u strict=%u dither=%u tiny=%u clear=%u upload=%u local=%u overlap=%u overlap-upload=%u readback=%u packed=%u t4=%u scanout=%u scanout-packed=%u\n",
            rasterPassed, gpuLineMatchesCpu, gpuOnlyGuardPassed, dither16Passed, tinyRasterPassed, clearPassed, uploadPassed,
            localCopyPassed, overlapCopyPassed, overlapUploadsPassed, localReadbackPassed, packedReadbackPassed, nibbleReadbackPassed,
            gpuPresentPassed && gpuAsyncPresentPassed, gpuPresentPackedPassed);
        std::printf("SDL_GPU Metal GS raster smoke: %s (pixel=%08x expected=ff996633)\n",
            rasterPassed ? "PASS" : "FAIL", rasterPixel);
        std::printf("SDL_GPU Metal GS line/point differential: %s (440 CT32 pixels; 8 directions, LINESTRIP, POINT, and Z16S match CPU)\n",
            gpuLineMatchesCpu ? "PASS" : "FAIL");
        std::printf("SDL_GPU Metal GS aliased CT32/Z24 differential: %s (full VRAM matches CPU)\n",
            gpuAliasedDepthMatchesCpu ? "PASS" : "FAIL");
        std::printf("SDL_GPU Metal GS disjoint draw batching differential: %s (full VRAM matches CPU)\n",
            gpuDisjointDrawsMatchCpu ? "PASS" : "FAIL");
        std::printf("SDL_GPU Metal GS textured batching hazards: %s (disjoint draws batch across TEXFLUSH; texture feedback splits it; full VRAM matches CPU)\n",
            gpuTexturedDisjointDrawsMatchCpu ? "PASS" : "FAIL");
        if (!gpuTexturedDisjointDrawsMatchCpu)
                std::printf("  detail: batched draws=%llu (expected 1), raster passes=%llu (expected 2), full VRAM=%s\n",
                static_cast<unsigned long long>(texturedBatchedDrawCount),
                static_cast<unsigned long long>(texturedRasterPassCount),
                texturedFullVramMatchesCpu ? "matches" : "DIFFERS");
        std::printf("SDL_GPU Metal GS TEXFLUSH-deferred image batch: %s (2 uploads share 1 batch; full VRAM matches CPU)\n",
            gpuDisjointImageChunksBatchMatchCpu ? "PASS" : "FAIL");
        if (!gpuDisjointImageChunksBatchMatchCpu)
        {
            std::printf("  detail: batches=%llu (expected 1), chunks=%llu (expected 2), full VRAM=%s\n",
                static_cast<unsigned long long>(imageChunkBatchCount),
                static_cast<unsigned long long>(imageChunkCount),
                imageChunkGpuVram == imageChunkCpuVram ? "matches" : "DIFFERS");
            for (size_t i = 0; i < imageChunkGpuVram.size(); ++i)
                if (imageChunkGpuVram[i] != imageChunkCpuVram[i])
                {
                    std::printf("  first VRAM difference at byte %zu: GPU=%02x CPU=%02x\n", i,
                        imageChunkGpuVram[i], imageChunkCpuVram[i]);
                    break;
                }
        }
        std::printf("SDL_GPU Metal GS mip LOD differential: %s (two Q=.25 draws select MIPTBP1 level 2; full VRAM matches CPU)\n",
            gpuMipLevelMatchesCpu ? "PASS" : "FAIL");
        std::printf("SDL_GPU Metal GS base-level-only mip batching: %s (MXL=0, MMIN<2, and constant LOD=0; area >=4096; one batch and full VRAM matches CPU)\n",
            gpuMiptbpMxlZeroBatchMatchesCpu ? "PASS" : "FAIL");
        if (!gpuMiptbpMxlZeroBatchMatchesCpu)
        {
            std::printf("  detail: batched draws=%llu (expected 2), raster passes=%llu (expected 1), full VRAM=%s\n",
                static_cast<unsigned long long>(mipZeroBatchedDraws),
                static_cast<unsigned long long>(mipZeroRasterPasses),
                mipZeroGpuVram == mipZeroCpuVram ? "matches" : "DIFFERS");
            const auto mismatch = std::mismatch(mipZeroGpuVram.begin(), mipZeroGpuVram.end(), mipZeroCpuVram.begin());
            if (mismatch.first != mipZeroGpuVram.end())
            {
                const size_t offset = static_cast<size_t>(mismatch.first - mipZeroGpuVram.begin());
                std::printf("  first VRAM difference at byte %zu: GPU=%02x CPU=%02x\n", offset,
                    *mismatch.first, *mismatch.second);
            }
        }
        std::printf("SDL_GPU Metal GS TEX1/MIPTBP alias differential: %s (feedback read stays out of the writers' pass; full VRAM matches CPU)\n",
            gpuMipAliasMatchesCpu ? "PASS" : "FAIL");
        std::printf("SDL_GPU Metal GS raster batch capacity: %s (129 disjoint draws -> 2 dispatches; full VRAM matches CPU)\n",
            gpuRasterBatchCapacityMatchesCpu ? "PASS" : "FAIL");
        std::printf("SDL_GPU Metal GS ordered overlap batch: %s (overlapping same-target draws -> 1 dispatch; GS order and full VRAM match CPU)\n",
            gpuRasterOrderedBatchMatchesCpu ? "PASS" : "FAIL");
        std::printf("SDL_GPU Metal GS non-finite STQ normalization: %s (matches explicit zero T)\n",
            gpuNonfiniteMatchesExplicit ? "PASS" : "FAIL");
        std::printf("SDL_GPU GS GPU-only guard: %s (unsupported AA1 draw throws before CPU rasterization)\n",
            gpuOnlyGuardPassed ? "PASS" : "FAIL");
        std::printf("SDL_GPU Metal GS CT16S+DTHE: %s (pixel=%04x expected=b989)\n",
            dither16Passed ? "PASS" : "FAIL", dither16Pixel);
        std::printf("SDL_GPU Metal GS GPU-only passes: %s (raster=%08x tiny=%08x clear=%08x upload=%08x)\n",
            rasterPassed && tinyRasterPassed && clearPassed && uploadPassed && localCopyPassed && overlapCopyPassed ? "PASS" : "FAIL",
            rasterPixel, tinyRasterPixel, clearPixel, uploadPixel);
        std::printf("SDL_GPU Metal GS CT24 upload: %s (pixel=%08x expected=aa665544)\n",
            ct24UploadPassed ? "PASS" : "FAIL", ct24UploadPixel);
        std::printf("SDL_GPU Metal GS compact uploads: %s (CT16/T8/T4 preserve neighboring pixels)\n",
            compactUploadPassed ? "PASS" : "FAIL");
        std::printf("SDL_GPU Metal GS overlapping image uploads: %s (later write wins in GS order)\n",
            overlapUploadsPassed ? "PASS" : "FAIL");
        std::printf("SDL_GPU Metal GS local-to-local transfer: %s (copy=%08x overlap=%08x expected=ff102030:ff010203)\n",
            localCopyPassed && overlapCopyPassed ? "PASS" : "FAIL", localCopyPixel, overlapCopyPixel);
        std::printf("SDL_GPU Metal GS local-to-host transfer: %s (CT32=%s packed=%s T4=%02x%02x expected=21 03)\n",
            localReadbackPassed && packedReadbackPassed && nibbleReadbackPassed ? "PASS" : "FAIL",
            localReadbackPassed ? "16 bytes match" : "byte mismatch",
            packedReadbackPassed ? "CT24/CT16/T8 match" : "packed-byte mismatch",
            nibbleReadbackBytes[0], nibbleReadbackBytes[1]);
        std::printf("SDL_GPU Metal GS scanout conversion: %s (width=%u pixel=%08x expected=ff102030)\n",
            gpuPresentPassed ? "PASS" : "FAIL", gpuPresentWidth, gpuPresentPixel);
        std::printf("SDL_GPU Metal GS asynchronous scanout: %s (pixel=%08x expected=ffc0ffee)\n",
            gpuAsyncPresentPassed ? "PASS" : "FAIL", gpuAsyncPresentPixel);
        std::printf("SDL_GPU Metal GS scanout packed formats: %s (initial CT32=%08x CT24=%08x CT16=%08x expected=ffabcdef:ff332211:80426b94)\n",
            gpuPresentPackedPassed ? "PASS" : "FAIL", gpuPresentInitialCt32Pixel,
            gpuPresentCt24Pixel, gpuPresentCt16Pixel);
        std::printf("SDL_GPU Metal GS CLUT smoke: %s (entry=%04x:%04x expected=ffab:cdef)\n",
            clutPassed ? "PASS" : "FAIL", clut[256], clut[0]);
        std::printf("SDL_GPU Metal GS CLUT cache/invalidation: %s (shared CSA banks, CLD0 draws, full VRAM differential; hits=%llu dispatches=%llu)\n",
            clutCachePassed ? "PASS" : "FAIL", static_cast<unsigned long long>(clutCacheHits),
            static_cast<unsigned long long>(clutCacheDispatches));
        return rasterPassed && gpuLineMatchesCpu && gpuAliasedDepthMatchesCpu && gpuDisjointDrawsMatchCpu && gpuTexturedDisjointDrawsMatchCpu && gpuDisjointImageChunksBatchMatchCpu && gpuMipLevelMatchesCpu && gpuMiptbpMxlZeroBatchMatchesCpu && gpuMipAliasMatchesCpu && gpuRasterBatchCapacityMatchesCpu && gpuRasterOrderedBatchMatchesCpu && gpuNonfiniteMatchesExplicit && gpuOnlyGuardPassed && dither16Passed && tinyRasterPassed && clearPassed && uploadPassed && ct24UploadPassed && compactUploadPassed && overlapUploadsPassed && localCopyPassed && overlapCopyPassed && localReadbackPassed && packedReadbackPassed && nibbleReadbackPassed && gpuPresentPassed && gpuAsyncPresentPassed && gpuPresentPackedPassed && clutPassed ? 0 : 1;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "SDL_GPU Metal GS raster smoke: ERROR: %s\n", e.what());
        return 2;
    }
}
