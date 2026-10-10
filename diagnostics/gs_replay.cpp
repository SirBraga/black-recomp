// gs_replay: feeds a GS dump (PS2X_GS_DUMP of the paraLLEl-GS module) to a renderer module and saves what
// it scans out, so that two renderers can be compared on the very same input, frame by frame.
//
//   gs_replay DUMP --module LIB [--moltenvk LIB] [--out DIR] [--every N] [--frames N] [--loops N]
//   gs_replay --diff DIR_A DIR_B [--out DIR] [--tolerance T] [--quantize]
//
// Replaying prints, per scanout, whether the pixels match the hash recorded when the dump was taken (only
// meaningful for the renderer that recorded it). --loops replays the dump several times and reports the time
// per scanout: a benchmark of the renderer alone. --diff compares the frames two replays saved.
#include "runtime/gs/gs_parallel_api.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <set>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{
    struct Image
    {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgb;
    };

    bool writePpm(const std::string &path, const uint8_t *rgba, uint32_t width, uint32_t height)
    {
        FILE *file = std::fopen(path.c_str(), "wb");
        if (!file)
            return false;
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        std::vector<uint8_t> row(size_t(width) * 3u);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
                std::memcpy(&row[x * 3u], rgba + (size_t(y) * width + x) * 4u, 3);
            std::fwrite(row.data(), 1, row.size(), file);
        }
        std::fclose(file);
        return true;
    }

    bool readPpm(const std::string &path, Image &image)
    {
        FILE *file = std::fopen(path.c_str(), "rb");
        if (!file)
            return false;
        unsigned width = 0, height = 0, maximum = 0;
        const bool ok = std::fscanf(file, "P6 %u %u %u", &width, &height, &maximum) == 3 && maximum == 255u && std::fgetc(file) != EOF;
        if (ok)
        {
            image.width = width;
            image.height = height;
            image.rgb.resize(size_t(width) * height * 3u);
            if (std::fread(image.rgb.data(), 1, image.rgb.size(), file) != image.rgb.size())
                image.rgb.clear();
        }
        std::fclose(file);
        return ok && !image.rgb.empty();
    }

    int diffDirectories(const std::string &a, const std::string &b, const std::string &out, int tolerance, bool quantize)
    {
        unsigned frames = 0, different = 0;
        double worst = 0.0;
        std::set<unsigned> indices;
        for (const std::string &directory : {a, b})
            if (DIR *dir = opendir(directory.c_str()))
            {
                unsigned index;
                while (const dirent *entry = readdir(dir))
                    if (std::sscanf(entry->d_name, "frame_%u.ppm", &index) == 1)
                        indices.insert(index);
                closedir(dir);
            }
        for (const unsigned index : indices)
        {
            char name[64];
            std::snprintf(name, sizeof(name), "/frame_%05u.ppm", index);
            Image left, right;
            const bool hasLeft = readPpm(a + name, left), hasRight = readPpm(b + name, right);
            ++frames;
            if (!hasLeft || !hasRight || left.width != right.width || left.height != right.height)
            {
                ++different;
                std::printf("frame %5u: %s\n", index, !hasLeft ? "missing in A" : !hasRight ? "missing in B" : "different size");
                continue;
            }
            const size_t pixels = size_t(left.width) * left.height;
            size_t over = 0;
            uint64_t sum = 0;
            int maximum = 0;
            std::vector<uint8_t> map(out.empty() ? 0u : pixels * 4u);
            for (size_t i = 0; i < pixels; ++i)
            {
                int pixelMax = 0;
                for (unsigned c = 0; c < 3u; ++c)
                {
                    // --quantize: both sides reduced to 5 bits per channel first (the game's display buffer is 16-bit).
                    const int d = quantize ? std::abs(int(left.rgb[i * 3u + c] >> 3) - int(right.rgb[i * 3u + c] >> 3)) * 8 : std::abs(int(left.rgb[i * 3u + c]) - int(right.rgb[i * 3u + c]));
                    sum += uint64_t(d);
                    pixelMax = std::max(pixelMax, d);
                }
                maximum = std::max(maximum, pixelMax);
                if (pixelMax > tolerance)
                    ++over;
                if (!map.empty())
                {
                    // Error map: grey copy of A, differing pixels in red scaled by the error.
                    const uint8_t grey = uint8_t((left.rgb[i * 3u] + left.rgb[i * 3u + 1u] + left.rgb[i * 3u + 2u]) / 6u);
                    map[i * 4u] = pixelMax > tolerance ? uint8_t(std::min(255, 96 + pixelMax * 4)) : grey;
                    map[i * 4u + 1u] = pixelMax > tolerance ? 0u : grey;
                    map[i * 4u + 2u] = pixelMax > tolerance ? 0u : grey;
                }
            }
            const double percent = 100.0 * double(over) / double(pixels);
            worst = std::max(worst, percent);
            if (over != 0u)
                ++different;
            std::printf("frame %5u: %6.2f%% pixels over tolerance, mean error %.3f, max %d\n", index, percent, double(sum) / double(pixels * 3u), maximum);
            if (!map.empty() && over != 0u)
                writePpm(out + name, map.data(), left.width, left.height);
        }
        std::printf("%u frames compared, %u different, worst %.2f%%\n", frames, different, worst);
        return different == 0u && frames != 0u ? 0 : 1;
    }
}

int main(int argc, char **argv)
{
    std::string dumpPath, modulePath, moltenvk, out, diffA, diffB;
    unsigned every = 1, maxFrames = ~0u, loops = 1;
    int tolerance = 0;
    bool quantize = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--module")
            modulePath = next();
        else if (arg == "--moltenvk")
            moltenvk = next();
        else if (arg == "--out")
            out = next();
        else if (arg == "--every")
            every = std::max(1, std::atoi(next()));
        else if (arg == "--frames")
            maxFrames = unsigned(std::atoi(next()));
        else if (arg == "--loops")
            loops = unsigned(std::max(1, std::atoi(next())));
        else if (arg == "--tolerance")
            tolerance = std::atoi(next());
        else if (arg == "--quantize")
            quantize = true;
        else if (arg == "--diff")
        {
            diffA = next();
            diffB = next();
        }
        else
            dumpPath = arg;
    }
    if (!out.empty())
        mkdir(out.c_str(), 0755);
    if (!diffA.empty())
        return diffDirectories(diffA, diffB, out, tolerance, quantize);
    if (dumpPath.empty() || modulePath.empty())
    {
        std::fprintf(stderr, "usage: gs_replay DUMP --module LIB [--moltenvk LIB] [--out DIR] [--every N] [--frames N] [--loops N]\n"
                             "       gs_replay --diff DIR_A DIR_B [--out DIR] [--tolerance T]\n");
        return 2;
    }

    // Mapped, not read: a dump that covers a level load is several gigabytes.
    const int descriptor = open(dumpPath.c_str(), O_RDONLY);
    struct stat info{};
    if (descriptor < 0 || fstat(descriptor, &info) != 0)
    {
        std::perror(dumpPath.c_str());
        return 2;
    }
    const size_t dumpSize = size_t(info.st_size);
    const uint8_t *dump = static_cast<const uint8_t *>(mmap(nullptr, dumpSize, PROT_READ, MAP_PRIVATE, descriptor, 0));
    if (dump == MAP_FAILED)
    {
        std::perror("mmap");
        return 2;
    }

    void *library = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library)
    {
        std::fprintf(stderr, "%s\n", dlerror());
        return 2;
    }
    const auto get = reinterpret_cast<const GSParallelAPI *(*)()>(dlsym(library, "black_parallel_gs_api"));
    const GSParallelAPI *api = get ? get() : nullptr;
    if (!api || api->version != 6u)
    {
        std::fprintf(stderr, "module ABI mismatch\n");
        return 2;
    }

    struct Event
    {
        uint32_t kind, path, size;
        const uint8_t *data;
    };
    std::vector<Event> events;
    for (size_t at = 0; at + 12u <= dumpSize;)
    {
        Event event{};
        std::memcpy(&event, &dump[at], 12);
        event.data = &dump[at + 12u];
        at += 12u + event.size;
        if (at > dumpSize)
            break;
        events.push_back(event);
    }
    if (events.empty() || events[0].kind != 5u || events[0].path != 0u || events[0].size != 4u * 1024u * 1024u)
    {
        std::fprintf(stderr, "not a GS dump (it must start with the whole VRAM)\n");
        return 2;
    }

    void *gpu = api->create(moltenvk.c_str(), events[0].data);
    if (!gpu)
    {
        std::fprintf(stderr, "renderer did not start\n");
        return 2;
    }
    std::vector<uint8_t> pixels(size_t(2560) * 2048u * 4u);
    unsigned matched = 0, compared = 0, frame = 0;
    double seconds = 0.0;
    for (unsigned loop = 0; loop < loops; ++loop)
    {
        frame = 0;
        const auto begin = std::chrono::steady_clock::now();
        for (size_t index = loop == 0 ? 1u : 0u; index < events.size() && frame < maxFrames; ++index)
        {
            const Event &event = events[index];
            switch (event.kind)
            {
            case 1: api->gif(gpu, event.path, event.data, event.size); break;
            case 2:
            {
                uint64_t value;
                std::memcpy(&value, event.data, 8);
                api->reg(gpu, uint8_t(event.path), value);
                break;
            }
            case 3: api->image(gpu, event.data, event.size); break;
            case 4:
            {
                uint32_t a[9];
                std::memcpy(a, event.data, sizeof(a));
                api->clear(gpu, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
                break;
            }
            case 5: api->write(gpu, event.path, event.data, event.size); break;
            case 6: api->reset(gpu); break;
            case 7:
            {
                uint64_t payload[11];
                std::memcpy(payload, event.data, sizeof(payload));
                GSParallelScanout request{};
                request.pmode = payload[0];
                request.smode1 = payload[1];
                request.smode2 = payload[2];
                request.dispfb1 = payload[3];
                request.display1 = payload[4];
                request.dispfb2 = payload[5];
                request.display2 = payload[6];
                request.bgcolor = payload[7];
                request.vsyncTick = payload[8];
                request.rgba = pixels.data();
                request.stride = 2560u * 4u;
                request.maxHeight = 2048u;
                const bool shown = api->scanout(gpu, &request) != 0 && request.width != 0u && request.height != 0u;
                if (loop == 0)
                {
                    uint64_t hash = 0xcbf29ce484222325ull;
                    for (size_t i = 0, n = shown ? size_t(request.width) * request.height : 0u; i < n; ++i)
                    {
                        uint32_t pixel;
                        std::memcpy(&pixel, &pixels[i * 4u], 4);
                        hash = (hash ^ pixel) * 0x100000001b3ull;
                    }
                    ++compared;
                    const bool same = shown && hash == payload[10] && (uint64_t(request.width) | (uint64_t(request.height) << 32)) == payload[9];
                    matched += same ? 1u : 0u;
                    if (!out.empty() && shown && frame % every == 0u)
                    {
                        char name[64];
                        std::snprintf(name, sizeof(name), "/frame_%05u.ppm", frame);
                        writePpm(out + name, pixels.data(), request.width, request.height);
                    }
                }
                ++frame;
                break;
            }
            default: break;
            }
        }
        api->wait(gpu);
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    }
    std::printf("%u scanouts replayed; %u of %u identical to the recording\n", frame, matched, compared);
    std::printf("%.2f ms per scanout over %u loop(s)\n", frame ? seconds * 1000.0 / double(frame * loops) : 0.0, loops);
    api->destroy(gpu);
    return 0;
}
