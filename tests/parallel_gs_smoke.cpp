// Standalone backend probe: no game assets, no window.
#include "gs_interface.hpp"
#include "context.hpp"
#include <dlfcn.h>
#include <cstring>
#include <iostream>
int main(int argc, char **argv) {
 if (argc != 2) return 2;
 void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
 if (!library) { std::cerr << dlerror() << "\n"; return 2; }
 auto loader = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
 if (!Vulkan::Context::init_loader(loader)) return 3;
 Vulkan::Context context;
 if (!context.init_instance_and_device(nullptr, 0, nullptr, 0)) return 4;
 Vulkan::Device device; device.set_context(context);
 ParallelGS::GSInterface gs;
 if (!gs.init(&device, {})) { std::cerr << "GS init failed\n"; return 5; }
 auto *memory = static_cast<unsigned char *>(gs.map_vram_write(0, 4096));
 if (!memory) return 6;
 for (unsigned i = 0; i < 4096; i++) memory[i] = (i * 37 + 19) & 255;
 gs.end_vram_write(0, 4096); gs.flush();
 auto *result = static_cast<const unsigned char *>(gs.map_vram_read(0, 4096));
 if (!result) return 7;
 for (unsigned i = 0; i < 4096; i++) if (result[i] != ((i * 37 + 19) & 255)) return 8;
 using R = ParallelGS::RegisterAddr;
 gs.write_register(R::PRMODECONT, uint64_t(1));
 gs.write_register(R::FRAME_1, uint64_t(1) << 16);
 gs.write_register(R::ZBUF_1, uint64_t(1) << 32);
 gs.write_register(R::XYOFFSET_1, uint64_t(0));
 gs.write_register(R::SCISSOR_1, (uint64_t(63) << 16) | (uint64_t(31) << 48));
 gs.write_register(R::TEST_1, uint64_t(0));
 gs.write_register(R::COLCLAMP, uint64_t(1));
 gs.write_register(R::PRIM, uint64_t(6));
 constexpr uint32_t color = 0x80402010;
 gs.write_register(R::RGBAQ, (uint64_t(0x3f800000) << 32) | color);
 gs.write_register(R::XYZ2, uint64_t(0));
 gs.write_register(R::XYZ2, uint64_t(64 * 16) | (uint64_t(32 * 16) << 16));
 gs.flush();
 uint32_t pixel = 0;
 std::memcpy(&pixel, gs.map_vram_read(0, 4), 4);
 if (pixel != color) { std::cerr << "GPU raster mismatch: " << std::hex << pixel << "\n"; return 9; }
 device.wait_idle(); std::cout << "PASS: real GS initialized, 4096-byte VRAM transfer and GPU sprite pixel verified\n";
 return 0;
}
