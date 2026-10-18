// ps2x_iso: inspect / extract a PS2 disc image (test tool for the builder).
//   <image> is an .iso, a raw .bin, a .cue sheet naming one of those, or a .chd -
//   sniffed by content, so a sheet renamed to .iso still reads.
//   ps2x_iso drives                    optical drives (/dev/sr0, D:) usable as <image>
//   ps2x_iso info <image>              volume, boot file, file count, size
//   ps2x_iso list <image>              every entry
//   ps2x_iso hash <image> [path]       XXH3-128 of the boot file (or path)
//   ps2x_iso hashfile <path>           XXH3-128 of a file on disk (a disc already
//                                      extracted to a folder, most often)
//   ps2x_iso extract <image> <outdir>  every file, with progress
#include "ps2x_iso9660.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    // XXH3-128 of a file on disk. The same value the disc path reports for the boot
    // file, so a disc and a folder of files extracted from it check against each
    // other - without needing xxhsum, which is often not installed.
    int hashFile(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            std::fprintf(stderr, "can't read %s\n", path.c_str());
            return 1;
        }
        const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const XXH128_hash_t h = XXH3_128bits(data.data(), data.size());
        std::printf("%016" PRIx64 "%016" PRIx64 "  %s\n", h.high64, h.low64, path.c_str());
        return 0;
    }

    // The knows-good boot files, so a disc can be named rather than just hashed.
    std::string gameForHash(const std::string &hash)
    {
        if (hash == "7f69b961cc427e3dcfd6582e25362be8")
            return "Grand Theft Auto: Vice City Stories";
        if (hash == "3d2280cadf1a74ce39e121d1ea6941b2")
            return "Grand Theft Auto: Liberty City Stories";
        return {};
    }

    bool hashBootFile(ps2x::IsoImage &image, std::string &hash, std::string &error)
    {
        const std::string path = "/" + ps2x::isoBootFile(image);
        const ps2x::IsoEntry *entry = image.find(path);
        std::vector<uint8_t> data;
        if (!entry || !image.readFile(*entry, data, error))
        {
            error = "can't read " + path;
            return false;
        }
        const XXH128_hash_t h = XXH3_128bits(data.data(), data.size());
        char text[40];
        std::snprintf(text, sizeof text, "%016" PRIx64 "%016" PRIx64, h.high64, h.low64);
        hash = text;
        return true;
    }

    int checkIso(const std::string &source, bool wait)
    {
        std::printf("ps2x_iso - disc check\n=====================\n\n");

        std::vector<std::string> sources;
        if (source.empty())
        {
            sources = ps2x::opticalDrives();
            if (sources.empty())
            {
                std::printf("No optical drive found.\n\n"
                            "No CD/DVD device was reported. Worth checking:\n"
                            "  - the drive is plugged in and switched on;\n"
                            "  - a disc is in it (give it a few seconds to spin up);\n"
                            "  - the system shows the drive (Explorer with a letter,\n"
                            "    or /dev/sr* on Linux).\n");
            }
            else
            {
                std::printf("Optical drive%s found:", sources.size() > 1 ? "s" : "");
                for (const std::string &drive : sources)
                    std::printf(" %s", drive.c_str());
                std::printf("\n");
            }
        }
        else
        {
            sources.push_back(source);
        }

        for (const std::string &item : sources)
        {
            std::printf("\n--- %s\n", item.c_str());
            ps2x::IsoImage image;
            std::string error;
            if (!image.open(item, error))
            {
                std::printf("    %s\n", error.c_str());
                continue;
            }
            size_t files = 0;
            for (const auto &entry : image.entries())
                files += entry.directory ? 0u : 1u;
            std::printf("    volume: %s\n    boot:   %s\n    files:  %zu\n    bytes:  %" PRIu64 "\n",
                        image.volumeId().c_str(), ps2x::isoBootFile(image).c_str(), files, image.totalFileBytes());

            std::string hash;
            if (!hashBootFile(image, hash, error))
            {
                std::printf("    %s\n", error.c_str());
                continue;
            }
            std::printf("    boot hash: %s\n", hash.c_str());
            const std::string game = gameForHash(hash);
            std::printf("    %s\n", game.empty() ? "This is a PS2 disc, but not one of the two games this is for."
                                                 : ("Recognised: " + game).c_str());
        }

        if (wait)
        {
            std::printf("\nDone. Press Enter to close.\n");
            std::fflush(stdout);
            std::getchar();
        }
        return 0;
    }
}

int main(int argc, char **argv)
{
    // No arguments at all: check whatever disc is in the drive and wait at the
    // end, so double-clicking it from Explorer is enough.
    if (argc < 2)
        return checkIso({}, true);
    if (argc >= 2 && std::string(argv[1]) == "check")
        return checkIso(argc > 2 ? argv[2] : std::string{}, false);
    if (argc >= 2 && std::string(argv[1]) == "hashfile")
        return hashFile(argc > 2 ? argv[2] : std::string{});
    if (argc >= 2 && std::string(argv[1]) == "drives")
    {
        for (const std::string &drive : ps2x::opticalDrives())
            std::printf("%s\n", drive.c_str());
        return 0;
    }
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s info|list|hash|extract <image> [path|outdir]\n", argv[0]);
        return 2;
    }
    const std::string command = argv[1];
    ps2x::IsoImage image;
    std::string error;
    if (!image.open(argv[2], error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (command == "info")
    {
        size_t files = 0;
        for (const auto &entry : image.entries())
            files += entry.directory ? 0u : 1u;
        std::printf("volume: %s\nboot: %s\nfiles: %zu\nbytes: %" PRIu64 "\n", image.volumeId().c_str(),
                    ps2x::isoBootFile(image).c_str(), files, image.totalFileBytes());
        return 0;
    }
    if (command == "list")
    {
        for (const auto &entry : image.entries())
            std::printf("%s%s  %" PRIu64 "\n", entry.path.c_str(), entry.directory ? "/" : "", entry.size);
        return 0;
    }
    if (command == "hash")
    {
        const std::string path = argc > 3 ? argv[3] : "/" + ps2x::isoBootFile(image);
        const ps2x::IsoEntry *entry = image.find(path);
        std::vector<uint8_t> data;
        if (!entry || !image.readFile(*entry, data, error))
        {
            std::fprintf(stderr, "can't read %s\n", path.c_str());
            return 1;
        }
        const XXH128_hash_t h = XXH3_128bits(data.data(), data.size());
        std::printf("%016" PRIx64 "%016" PRIx64 "  %s\n", h.high64, h.low64, path.c_str());
        return 0;
    }
    if (command == "extract" && argc > 3)
    {
        const std::filesystem::path out = argv[3];
        const uint64_t total = image.totalFileBytes();
        uint64_t before = 0;
        int lastPercent = -1;
        for (const auto &entry : image.entries())
        {
            const auto dest = out / entry.path.substr(1);
            if (entry.directory)
            {
                std::error_code ec;
                std::filesystem::create_directories(dest, ec); // empty ones too (FRONTEND/EN/MC1, ...)
                continue;
            }
            if (!image.extractFile(entry, dest.string(), error,
                                   [&](uint64_t done)
                                   {
                                       const int percent = total ? static_cast<int>((before + done) * 100u / total) : 100;
                                       if (percent != lastPercent)
                                       {
                                           lastPercent = percent;
                                           std::fprintf(stderr, "\r%3d%%", percent);
                                       }
                                   }))
            {
                std::fprintf(stderr, "\n%s\n", error.c_str());
                return 1;
            }
            before += entry.size;
        }
        std::fprintf(stderr, "\rdone\n");
        return 0;
    }
    std::fprintf(stderr, "unknown command %s\n", command.c_str());
    return 2;
}
