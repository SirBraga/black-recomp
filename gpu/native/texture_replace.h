// Texture replacement packs in the PCSX2 layout: "<TEX0 hash>-<CLUT hash>-<bits>.dds|png".
//   TEX0 hash : XXH3_64 of the texture's pixels as the GS reads them (one byte per pixel for indexed formats)
//   CLUT hash : XXH3_64 of the 16/256 32-bit palette entries (0 when the format has no palette)
//   bits      : PSM | TW << 6 | TH << 10
// PS2X_TEXTURE_PACKS=<dir>[:<dir>...] lists the folders to index (searched recursively).
#pragma once
#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace texrepl
{
    struct Key
    {
        uint64_t tex = 0, clut = 0;
        uint32_t bits = 0;
        bool operator==(const Key &o) const { return tex == o.tex && clut == o.clut && bits == o.bits; }
    };
    struct KeyHash
    {
        size_t operator()(const Key &k) const
        {
            uint64_t h = k.tex * 0x9E3779B97F4A7C15ull;
            h ^= (k.clut + 0x7F4A7C15ull + (h << 6) + (h >> 2));
            h ^= (uint64_t(k.bits) + 0x85EBCA6Bull + (h << 6) + (h >> 2));
            return size_t(h);
        }
    };

    struct Level
    {
        uint32_t width = 0, height = 0;
        size_t offset = 0, bytes = 0, rowBytes = 0;
    };
    // A decoded replacement: compressed blocks (BC1/BC3) or RGBA8, with every mip level it came with.
    struct Image
    {
        enum Format { BC1, BC3, RGBA8 } format = RGBA8;
        std::vector<Level> levels;
        std::vector<uint8_t> data;
        size_t totalBytes() const { return data.size(); }
    };

    inline bool loadDds(const std::string &path, Image &out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return false;
        uint8_t header[128];
        f.read(reinterpret_cast<char *>(header), sizeof header);
        if (f.gcount() != sizeof header || std::memcmp(header, "DDS ", 4) != 0)
            return false;
        auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, header + at, 4); return v; };
        const uint32_t height = u32(12), width = u32(16), mips = std::max<uint32_t>(u32(28), 1u);
        const uint32_t fourcc = u32(84);
        size_t blockBytes;
        if (fourcc == 0x31545844u)      // "DXT1"
            out.format = Image::BC1, blockBytes = 8;
        else if (fourcc == 0x35545844u) // "DXT5"
            out.format = Image::BC3, blockBytes = 16;
        else
            return false;
        if (width == 0 || height == 0 || width > 8192 || height > 8192)
            return false;
        out.levels.clear();
        size_t offset = 0;
        for (uint32_t l = 0; l < mips; ++l)
        {
            const uint32_t w = std::max(width >> l, 1u), h = std::max(height >> l, 1u);
            Level lv;
            lv.width = w;
            lv.height = h;
            lv.rowBytes = size_t((w + 3) / 4) * blockBytes;
            lv.bytes = lv.rowBytes * size_t((h + 3) / 4);
            lv.offset = offset;
            offset += lv.bytes;
            out.levels.push_back(lv);
            if (w == 1 && h == 1)
                break;
        }
        out.data.resize(offset);
        f.read(reinterpret_cast<char *>(out.data.data()), std::streamsize(offset));
        if (size_t(f.gcount()) != offset)
        {
            // A file with fewer levels than its header claims: keep the ones that are there.
            size_t have = size_t(f.gcount());
            while (!out.levels.empty() && out.levels.back().offset + out.levels.back().bytes > have)
                out.levels.pop_back();
            if (out.levels.empty())
                return false;
            out.data.resize(out.levels.back().offset + out.levels.back().bytes);
        }
        return true;
    }

    // Builds an RGBA8 mip chain from the base level (2x2 box filter), for sources that come without one.
    inline void buildMips(Image &img, uint32_t width, uint32_t height, std::vector<uint8_t> base)
    {
        img.format = Image::RGBA8;
        img.levels.clear();
        img.data.clear();
        std::vector<uint8_t> cur = std::move(base);
        uint32_t w = width, h = height;
        for (;;)
        {
            Level lv;
            lv.width = w;
            lv.height = h;
            lv.rowBytes = size_t(w) * 4;
            lv.bytes = lv.rowBytes * h;
            lv.offset = img.data.size();
            img.levels.push_back(lv);
            img.data.insert(img.data.end(), cur.begin(), cur.end());
            if (w == 1 && h == 1)
                break;
            const uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
            std::vector<uint8_t> next(size_t(nw) * nh * 4);
            for (uint32_t y = 0; y < nh; ++y)
                for (uint32_t x = 0; x < nw; ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        const uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                        const uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                        const uint32_t sum = cur[(size_t(y0) * w + x0) * 4 + c] + cur[(size_t(y0) * w + x1) * 4 + c] +
                                             cur[(size_t(y1) * w + x0) * 4 + c] + cur[(size_t(y1) * w + x1) * 4 + c];
                        next[(size_t(y) * nw + x) * 4 + c] = uint8_t((sum + 2) / 4);
                    }
            cur = std::move(next);
            w = nw;
            h = nh;
        }
    }

    // Index of the files of every pack folder, by key. Nothing is read until a texture asks for its replacement.
    class Pack
    {
    public:
        void scan(const std::string &dirs)
        {
            size_t at = 0;
            while (at <= dirs.size())
            {
                const size_t end = std::min(dirs.find(':', at), dirs.size());
                if (end > at)
                    scanDir(dirs.substr(at, end - at));
                at = end + 1;
            }
        }
        size_t size() const { return files_.size(); }
        const std::string *find(const Key &k) const
        {
            const auto it = files_.find(k);
            return it == files_.end() ? nullptr : &it->second;
        }

    private:
        void scanDir(const std::string &dir)
        {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec))
            {
                std::fprintf(stderr, "[texrepl] not a folder: %s\n", dir.c_str());
                return;
            }
            size_t before = files_.size();
            for (auto it = std::filesystem::recursive_directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            {
                if (!it->is_regular_file(ec))
                    continue;
                const std::string name = it->path().filename().string();
                if (name.empty() || name[0] == '.')
                    continue;
                Key key;
                char dot = 0;
                char ext[8] = {};
                if (std::sscanf(name.c_str(), "%" SCNx64 "-%" SCNx64 "-%x%c%7s", &key.tex, &key.clut, &key.bits, &dot, ext) != 5 || dot != '.')
                    continue;
                std::string e = ext;
                for (char &c : e)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                const bool dds = e == "dds";
                if (!dds && e != "png")
                    continue;
                // A DDS wins over a PNG of the same texture: it loads faster and brings its own mipmaps.
                auto found = files_.find(key);
                if (found == files_.end() || dds)
                    files_[key] = it->path().string();
            }
            std::fprintf(stderr, "[texrepl] %s: %zu replacements\n", dir.c_str(), files_.size() - before);
        }
        std::unordered_map<Key, std::string, KeyHash> files_;
    };
}
