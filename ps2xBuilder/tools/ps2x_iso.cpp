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

    // The boot file's name: SYSTEM.CNF says which, and says it in one place, so a
    // disc that does not have that file cannot be checked by name. When it cannot
    // be read the manifest's name is used instead - the hash below still decides
    // whether the disc is the right one, so this only changes how it is found.
    std::string bootFileName(ps2x::IsoImage &image, const std::string &hint, std::string &note)
    {
        const ps2x::IsoEntry *cnf = image.find("/SYSTEM.CNF");
        if (!cnf)
            note = "there is no SYSTEM.CNF in the disc's root";
        else
        {
            std::vector<uint8_t> data;
            std::string readError;
            if (!image.readFile(*cnf, data, readError))
                note = "SYSTEM.CNF could not be read: " + readError;
            else
            {
                const std::string name = ps2x::isoBootFile(image);
                if (!name.empty())
                    return name;
                note = "SYSTEM.CNF has no BOOT2 line";
            }
        }
        return hint;
    }

    bool hashBootFile(ps2x::IsoImage &image, const std::string &hint, std::string &hash, std::string &error)
    {
        std::string note;
        const std::string name = bootFileName(image, hint, note);
        if (name.empty())
        {
            error = note + ", and no boot file name was given to look for";
            return false;
        }
        if (!note.empty())
            std::printf("    %s; using the boot file name the game expects: %s\n", note.c_str(), name.c_str());
        const ps2x::IsoEntry *entry = image.find("/" + name);
        std::vector<uint8_t> data;
        std::string readError;
        if (!entry)
        {
            error = "there is no " + name + " in the disc's root";
            return false;
        }
        if (!image.readFile(*entry, data, readError))
        {
            error = "can't read " + name + ": " + readError;
            return false;
        }
        const XXH128_hash_t h = XXH3_128bits(data.data(), data.size());
        char text[40];
        std::snprintf(text, sizeof text, "%016" PRIx64 "%016" PRIx64, h.high64, h.low64);
        hash = text;
        return true;
    }

    // What is in the root, so a disc that is not the expected one says what it is.
    void printRoot(ps2x::IsoImage &image)
    {
        std::printf("    the disc's root holds:");
        size_t shown = 0;
        for (const auto &entry : image.entries())
        {
            if (entry.path.size() < 2 || entry.path.find('/', 1) != std::string::npos)
                continue;
            if (shown++ == 40)
            {
                std::printf(" ...");
                break;
            }
            std::printf(" %s%s", entry.path.substr(1).c_str(), entry.directory ? "/" : "");
        }
        std::printf("\n");
    }

    int checkIso(const std::string &source, bool wait, const std::string &boot = {})
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
            if (!hashBootFile(image, boot, hash, error))
            {
                std::printf("    %s\n", error.c_str());
                printRoot(image);
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
    {
        std::string source, boot;
        for (int i = 2; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--boot" && i + 1 < argc)
                boot = argv[++i];
            else
                source = argv[i];
        }
        return checkIso(source, false, boot);
    }
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
