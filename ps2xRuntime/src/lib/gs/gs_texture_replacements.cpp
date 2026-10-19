#include "runtime/gs/gs_texture_replacements.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace gsrepl
{
    namespace
    {
        std::mutex g_mutex;
        std::shared_ptr<const Index> g_index;
        std::atomic<uint64_t> g_version{0};

        // GL internal formats (glext.h names).
        constexpr uint32_t kGlRgba8 = 0x8058;
        constexpr uint32_t kGlBc1 = 0x83F1; // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
        constexpr uint32_t kGlBc2 = 0x83F2; // GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
        constexpr uint32_t kGlBc3 = 0x83F3; // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
        constexpr uint32_t kGlBc7 = 0x8E8C; // GL_COMPRESSED_RGBA_BPTC_UNORM

        uint32_t readU32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

        bool hasSuffix(const std::string &s, const char *suffix)
        {
            const size_t n = std::strlen(suffix);
            if (s.size() < n)
                return false;
            for (size_t i = 0; i < n; ++i)
                if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i])
                    return false;
            return true;
        }

        bool loadDds(const std::string &path, Image &out, std::string &error)
        {
            std::ifstream in(path, std::ios::binary);
            std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (file.size() < 128 || std::memcmp(file.data(), "DDS ", 4) != 0)
            {
                error = "not a DDS file";
                return false;
            }
            const uint8_t *h = file.data();
            const uint32_t height = readU32(h + 12), width = readU32(h + 16);
            const uint32_t mipCount = std::max<uint32_t>(1u, readU32(h + 28));
            const uint32_t pfFlags = readU32(h + 80), fourCC = readU32(h + 84), rgbBits = readU32(h + 88);
            const uint32_t rMask = readU32(h + 92);
            size_t offset = 128;
            uint32_t blockBytes = 0;
            if (pfFlags & 0x4u) // DDPF_FOURCC
            {
                auto cc = [](const char *s) { return uint32_t(s[0]) | uint32_t(s[1]) << 8 | uint32_t(s[2]) << 16 | uint32_t(s[3]) << 24; };
                if (fourCC == cc("DXT1"))
                    out.glFormat = kGlBc1, blockBytes = 8;
                else if (fourCC == cc("DXT2") || fourCC == cc("DXT3"))
                    out.glFormat = kGlBc2, blockBytes = 16;
                else if (fourCC == cc("DXT4") || fourCC == cc("DXT5"))
                    out.glFormat = kGlBc3, blockBytes = 16;
                else if (fourCC == cc("DX10"))
                {
                    if (file.size() < 148)
                    {
                        error = "truncated DX10 header";
                        return false;
                    }
                    const uint32_t dxgi = readU32(h + 128);
                    offset = 148;
                    if (dxgi == 70 || dxgi == 71 || dxgi == 72)
                        out.glFormat = kGlBc1, blockBytes = 8;
                    else if (dxgi == 73 || dxgi == 74 || dxgi == 75)
                        out.glFormat = kGlBc2, blockBytes = 16;
                    else if (dxgi == 76 || dxgi == 77 || dxgi == 78)
                        out.glFormat = kGlBc3, blockBytes = 16;
                    else if (dxgi == 97 || dxgi == 98 || dxgi == 99)
                        out.glFormat = kGlBc7, blockBytes = 16;
                    else if (dxgi == 28 || dxgi == 29 || dxgi == 87 || dxgi == 88 || dxgi == 91)
                        out.glFormat = kGlRgba8; // R8G8B8A8 / B8G8R8A8
                    else
                    {
                        error = "unsupported DXGI format " + std::to_string(dxgi);
                        return false;
                    }
                    if (out.glFormat == kGlRgba8)
                    {
                        const bool bgra = dxgi == 87 || dxgi == 88 || dxgi == 91;
                        out.compressed = false;
                        uint32_t w = width, ht = height;
                        for (uint32_t level = 0; level < mipCount && w && ht; ++level)
                        {
                            const size_t bytes = size_t(w) * ht * 4u;
                            if (offset + bytes > file.size())
                                break;
                            Image::Level l{w, ht, std::vector<uint8_t>(file.begin() + offset, file.begin() + offset + bytes)};
                            if (bgra)
                                for (size_t i = 0; i < bytes; i += 4)
                                    std::swap(l.data[i], l.data[i + 2]);
                            out.levels.push_back(std::move(l));
                            offset += bytes;
                            w = std::max(1u, w / 2u);
                            ht = std::max(1u, ht / 2u);
                        }
                        return !out.levels.empty();
                    }
                }
                else
                {
                    error = "unsupported FourCC";
                    return false;
                }
                out.compressed = true;
                uint32_t w = width, ht = height;
                for (uint32_t level = 0; level < mipCount; ++level)
                {
                    const size_t bytes = size_t(std::max(1u, (w + 3u) / 4u)) * std::max(1u, (ht + 3u) / 4u) * blockBytes;
                    if (offset + bytes > file.size())
                        break;
                    out.levels.push_back({w, ht, std::vector<uint8_t>(file.begin() + offset, file.begin() + offset + bytes)});
                    offset += bytes;
                    if (w == 1u && ht == 1u)
                        break;
                    w = std::max(1u, w / 2u);
                    ht = std::max(1u, ht / 2u);
                }
                return !out.levels.empty();
            }
            if ((pfFlags & 0x40u) && (rgbBits == 32u || rgbBits == 24u)) // DDPF_RGB
            {
                // Uncompressed: converted to RGBA8 (alpha 0x80 = opaque on the GS scale).
                out.glFormat = kGlRgba8;
                out.compressed = false;
                const bool bgr = rMask == 0x00FF0000u;
                const bool alpha = (pfFlags & 0x1u) != 0u && rgbBits == 32u;
                const uint32_t bpp = rgbBits / 8u;
                uint32_t w = width, ht = height;
                for (uint32_t level = 0; level < mipCount && w && ht; ++level)
                {
                    const size_t bytes = size_t(w) * ht * bpp;
                    if (offset + bytes > file.size())
                        break;
                    Image::Level l{w, ht, std::vector<uint8_t>(size_t(w) * ht * 4u)};
                    for (size_t i = 0, o = 0; i < bytes; i += bpp, o += 4)
                    {
                        const uint8_t *p = file.data() + offset + i;
                        l.data[o] = bgr ? p[2] : p[0];
                        l.data[o + 1] = p[1];
                        l.data[o + 2] = bgr ? p[0] : p[2];
                        l.data[o + 3] = alpha ? p[3] : 0x80u;
                    }
                    out.levels.push_back(std::move(l));
                    offset += bytes;
                    w = std::max(1u, w / 2u);
                    ht = std::max(1u, ht / 2u);
                }
                return !out.levels.empty();
            }
            error = "unsupported DDS pixel format";
            return false;
        }

        bool loadPng(const std::string &path, Image &out, std::string &error)
        {
            ::Image image = LoadImage(path.c_str());
            if (!image.data)
            {
                error = "cannot decode PNG";
                return false;
            }
            ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            out.glFormat = kGlRgba8;
            out.compressed = false;
            Image::Level level{static_cast<uint32_t>(image.width), static_cast<uint32_t>(image.height), {}};
            level.data.assign(static_cast<uint8_t *>(image.data),
                              static_cast<uint8_t *>(image.data) + size_t(image.width) * image.height * 4u);
            UnloadImage(image);
            out.levels.push_back(std::move(level));
            return true;
        }
    }

    std::optional<Key> parseFileNameRaw(const std::string &fileName);

    std::optional<Key> parseFileName(const std::string &fileName)
    {
        auto parsed = parseFileNameRaw(fileName);
        if (parsed)
            parsed->bits &= ~(1u << 14); // bit 14 was TCC in older PCSX2 dumps; unused (PCSX2 RemoveUnusedBits)
        return parsed;
    }

    std::optional<Key> parseFileNameRaw(const std::string &fileName)
    {
        if (!hasSuffix(fileName, ".dds") && !hasSuffix(fileName, ".png"))
            return std::nullopt;
        Key key{};
        char extensionDot = 0;
        // Formats from PCSX2 GSTextureReplacements (palettized first).
        if (std::sscanf(fileName.c_str(), "%" SCNx64 "-%" SCNx64 "-%08x%c", &key.tex0Hash, &key.clutHash, &key.bits,
                        &extensionDot) == 4 &&
            extensionDot == '.')
            return key;
        key = {};
        extensionDot = 0;
        if (std::sscanf(fileName.c_str(), "%" SCNx64 "-%08x%c", &key.tex0Hash, &key.bits, &extensionDot) == 3 &&
            extensionDot == '.')
        {
            key.clutHash = 0;
            return key;
        }
        return std::nullopt;
    }

    std::shared_ptr<const Index> current()
    {
        std::lock_guard lock(g_mutex);
        return g_index;
    }

    uint64_t stampForFile(const std::string &path)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec)
            return 0u;
        const auto time = std::filesystem::last_write_time(path, ec);
        const uint64_t t = ec ? 0u : static_cast<uint64_t>(time.time_since_epoch().count());
        return (t * 1000003ull) ^ static_cast<uint64_t>(size);
    }

    void setCurrent(std::shared_ptr<const Index> index)
    {
        {
            std::lock_guard lock(g_mutex);
            g_index = std::move(index);
        }
        g_version.fetch_add(1u, std::memory_order_release);
    }

    uint64_t currentVersion()
    {
        return g_version.load(std::memory_order_acquire);
    }

    bool loadImage(const std::string &path, Image &out, std::string &error)
    {
        out = {};
        if (hasSuffix(path, ".dds"))
            return loadDds(path, out, error);
        return loadPng(path, out, error);
    }
}
