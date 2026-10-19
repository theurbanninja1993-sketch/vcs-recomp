#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Replacement textures in the PCSX2 texture-pack format: files named after
// the GS texture they replace,
//   <TEX0 hash>-<format bits>.dds|png             (direct colour)
//   <TEX0 hash>-<CLUT hash>-<format bits>.dds|png (palettized)
// where the hashes are XXH3-64 of the texture's local memory / its palette
// and the bits pack TEX0.PSM/TW/TH and (16/24-bit formats) TEXA, exactly as
// PCSX2 computes them (GSTextureCache::HashCacheKey, GSTextureReplacements).
namespace gsrepl
{
    struct Key
    {
        uint64_t tex0Hash = 0;
        uint64_t clutHash = 0; // 0 for direct-colour textures
        uint32_t bits = 0;     // PSM:6 TW:4 TH:4 unused:1 TA0:8 AEM:1 TA1:8
        bool operator==(const Key &other) const
        {
            return tex0Hash == other.tex0Hash && clutHash == other.clutHash && bits == other.bits;
        }
    };

    struct KeyHash
    {
        size_t operator()(const Key &key) const
        {
            return static_cast<size_t>(key.tex0Hash ^ (key.clutHash * 0x9E3779B97F4A7C15ull) ^ (uint64_t(key.bits) << 17));
        }
    };

    inline uint32_t makeBits(uint32_t psm, uint32_t tw, uint32_t th, uint32_t ta0, uint32_t aem, uint32_t ta1)
    {
        return (psm & 0x3Fu) | ((tw & 0xFu) << 6) | ((th & 0xFu) << 10) | ((ta0 & 0xFFu) << 15) | ((aem & 1u) << 23) |
               ((ta1 & 0xFFu) << 24);
    }

    // Parses a replacement file name (without directories); region and mip
    // level variants are not supported (ignored).
    std::optional<Key> parseFileName(const std::string &fileName);

    // Size + write time of a replacement file. The renderer keeps an
    // uploaded replacement while this is unchanged, so rewriting the prompt
    // art no longer reloads every texture-pack file.
    uint64_t stampForFile(const std::string &path);

    // One replacement texture of the enabled mods.
    struct Entry
    {
        std::string path;
        uint64_t stamp = 0;
        // The prompt art: small files that load inline on the render thread at
        // the first draw that needs them (a context flip must not show the old
        // keys). Pack textures always load on the worker.
        bool inlineLoad = false;
    };

    // Every replacement texture of the enabled mods.
    struct Index
    {
        std::unordered_map<Key, Entry, KeyHash> files;
    };

    // The index in use (null or empty: no replacement). Swapped by the mod
    // manager; the renderer picks up a new one between frames.
    std::shared_ptr<const Index> current();
    void setCurrent(std::shared_ptr<const Index> index);
    uint64_t currentVersion();

    // A decoded replacement image: mip levels, either block-compressed (BCn,
    // uploaded as is) or RGBA8.
    struct Image
    {
        struct Level
        {
            uint32_t width = 0, height = 0;
            std::vector<uint8_t> data;
        };
        uint32_t glFormat = 0; // GL internal format (compressed BCn enum or GL_RGBA8)
        bool compressed = false;
        std::vector<Level> levels;
    };
    bool loadImage(const std::string &path, Image &out, std::string &error);
}
