#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "ps2x_iso9660.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <sstream>

#if defined(PS2X_ISO_CHD)
#include <libchdr/chd.h>
#endif

namespace ps2x
{
    namespace
    {
        uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

        bool seek64(std::FILE *f, uint64_t offset)
        {
#if defined(_WIN32)
            return _fseeki64(f, static_cast<long long>(offset), SEEK_SET) == 0;
#else
            return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
        }

        std::string upper(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return s;
        }

        // A CHD says 'MComprHD' first, whatever version it is.
        bool looksLikeChd(const uint8_t *head) { return std::memcmp(head, "MComprHD", 8) == 0; }

        std::string trim(const std::string &text)
        {
            const size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                return {};
            const size_t last = text.find_last_not_of(" \t\r\n");
            return text.substr(first, last - first + 1);
        }

        // A .cue is text, and small: the largest real one is a few KB. Sniffing
        // the content rather than the extension means a cue sheet renamed to .iso
        // is understood instead of being called "not an ISO9660 image", which is
        // true but no help to anyone. What makes it a sheet is a line that names a
        // file - FILE in the standard, DATAFILE in what the Windows tools write -
        // so plain text that merely mentions one is not mistaken for it.
        bool looksLikeCue(const uint8_t *head, size_t size)
        {
            if (size == 0 || size > 1024u * 1024u)
                return false;
            for (size_t i = 0; i < size; ++i)
                if (head[i] != 0 && (head[i] < 0x09 || (head[i] > 0x0D && head[i] < 0x20)))
                    return false; // control bytes: this is a binary image, not a sheet
            const std::string text(reinterpret_cast<const char *>(head), size);
            std::istringstream in(text);
            std::string line;
            while (std::getline(in, line))
            {
                line = trim(line);
                for (const char *keyword : {"FILE", "DATAFILE"})
                {
                    const size_t n = std::strlen(keyword);
                    if (line.size() > n && line.compare(0, n, keyword) == 0 &&
                        (line[n] == ' ' || line[n] == '\t' || line[n] == '"'))
                        return true;
                }
            }
            return false;
        }
    }

    IsoImage::~IsoImage()
    {
        close();
    }

    void IsoImage::close()
    {
        if (m_file)
            std::fclose(m_file);
        m_file = nullptr;
#if defined(_WIN32)
        if (m_device)
            CloseHandle(static_cast<HANDLE>(m_device));
#endif
        m_device = nullptr;
#if defined(PS2X_ISO_CHD)
        if (m_chd)
            chd_close(static_cast<chd_file *>(m_chd));
#endif
        m_chd = nullptr;
        m_chdHunk.clear();
        m_chdHunkIndex = 0xFFFFFFFFu;
        m_chdFrameBytes = 0;
        m_entries.clear();
        m_volumeId.clear();
        m_readError.clear();
    }

    // A CHD holds the disc as fixed-size units packed into hunks - 2448 bytes each
    // for a CD (2352 of sector plus 96 of subcode), or a flat 2048 for a DVD.
    // Reading one is therefore the same shape as reading a raw image: the unit for
    // sector N, with the user data at whichever offset the layout probe settled on.
    // Only the stride and the decompression differ, so everything after this point
    // (the directory walk, extraction) is unchanged.
    //
    // The hunk is cached because a hunk holds several units and libchdr decodes a
    // hunk to hand out any of them: without this a sector-by-sector walk would ask
    // for the same hunk eight times over.
    bool IsoImage::readChdSectors(uint32_t lba, uint32_t count, uint8_t *out)
    {
#if defined(PS2X_ISO_CHD)
        chd_file *chd = static_cast<chd_file *>(m_chd);
        const chd_header *head = chd_get_header(chd);
        const uint32_t framesPerHunk = head->hunkbytes / m_chdFrameBytes;
        if (framesPerHunk == 0)
        {
            m_readError = "the CHD's hunks are smaller than one of its frames";
            return false;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t frame = lba + i;
            const uint32_t hunk = frame / framesPerHunk;
            if (hunk != m_chdHunkIndex)
            {
                if (m_chdHunk.size() != head->hunkbytes)
                    m_chdHunk.resize(head->hunkbytes);
                const chd_error err = chd_read(chd, hunk, m_chdHunk.data());
                if (err != CHDERR_NONE)
                {
                    m_readError = std::string("can't decompress the CHD: ") + chd_error_string(err);
                    return false;
                }
                m_chdHunkIndex = hunk;
            }
            std::memcpy(out + size_t(i) * 2048u, m_chdHunk.data() + size_t(frame % framesPerHunk) * m_chdFrameBytes + m_dataOffset, 2048u);
        }
        return true;
#else
        (void)lba; (void)count; (void)out;
        m_readError = "this build of ps2x_iso has no CHD support";
        return false;
#endif
    }

    bool IsoImage::readSectors(uint32_t lba, uint32_t count, uint8_t *out)
    {
#if defined(_WIN32)
        if (m_device)
        {
            // Raw device reads must be whole, aligned sectors (out is sized in sectors).
            LARGE_INTEGER offset;
            offset.QuadPart = static_cast<LONGLONG>(uint64_t(lba) * 2048u);
            if (!SetFilePointerEx(static_cast<HANDLE>(m_device), offset, nullptr, FILE_BEGIN))
            {
                m_readError = "can't seek the disc (Windows error " + std::to_string(GetLastError()) + ")";
                return false;
            }
            DWORD read = 0;
            const DWORD bytes = count * 2048u;
            if (!ReadFile(static_cast<HANDLE>(m_device), out, bytes, &read, nullptr) || read != bytes)
            {
                const DWORD code = GetLastError();
                m_readError = (code == 21 || code == 1155)
                                  ? "the disc is not ready - it can need a few seconds after insertion"
                                  : "can't read the disc (Windows error " + std::to_string(code) + ")";
                return false;
            }
            return true;
        }
#endif
        if (m_chd)
            return readChdSectors(lba, count, out);

        bool ok = false;
        if (m_sectorSize == 2048u)
        {
            ok = seek64(m_file, uint64_t(lba) * 2048u) && std::fread(out, 2048u, count, m_file) == count;
        }
        else
        {
            ok = true;
            for (uint32_t i = 0; i < count && ok; ++i)
                ok = seek64(m_file, uint64_t(lba + i) * m_sectorSize + m_dataOffset) &&
                     std::fread(out + size_t(i) * 2048u, 2048u, 1, m_file) == 1;
        }
        if (!ok)
            m_readError = "can't read the image at sector " + std::to_string(lba);
        return ok;
    }

    // The primary volume descriptor, then the directories.
    //
    // A plain image is tried as 2048-byte sectors and as raw 2352-byte ones, with
    // the user data after the sync/header. A CHD is different in one way that
    // matters: its unit is a CD frame of 2448 bytes (2352 + 96 subcode) for a CD,
    // or a flat 2048 for a DVD, but what is at the *front* of the unit depends on
    // what the CHD was made from - chdman passes through 2048-byte sectors as they
    // are, so their data sits at 0 - so the three offsets are all worth trying.
    bool IsoImage::mount(bool isChd, std::string &error)
    {
        struct Layout
        {
            uint32_t stride, dataOffset;
        };
        std::vector<Layout> layouts;
        if (isChd)
        {
            const uint32_t unit = m_chdFrameBytes;
            for (const uint32_t offset : {0u, 16u, 24u})
                if (unit >= offset + 2048u)
                    layouts.push_back({unit, offset});
        }
        else
        {
            // Every way an image can lay its sectors out, and the reader tries them
            // in turn - the volume descriptor check is what decides, so trying more
            // costs nothing and a wrong guess is never accepted.
            //
            // 2048 is a plain ISO. 2064 is a raw DVD sector (12 of sync, 4 of
            // header, 2048 of data, 4 of EDC) - the console is a DVD one, so a raw
            // dump of a PlayStation 2 disc looks like this. 2352 is a raw CD sector
            // and 2448 is a CD frame with its 96 bytes of subcode, which is what
            // `chdman extractdvd` writes when it is handed a CD-based CHD - a
            // mistake worth reading anyway. 0/16/24 covers where a tool can have
            // put the user data inside one.
            layouts = {{2048u, 0u},  {2064u, 16u}, {2352u, 0u},  {2352u, 16u}, {2352u, 24u},
                       {2448u, 0u},  {2448u, 16u}, {2448u, 24u}};
        }

        uint8_t pvd[2048];
        bool found = false;
        for (const Layout &layout : layouts)
        {
            m_sectorSize = layout.stride;
            m_dataOffset = layout.dataOffset;
            if (readSectors(16u, 1u, pvd) && pvd[0] == 1u && std::memcmp(pvd + 1, "CD001", 5) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            // Read the reason out before close(), which clears it - otherwise every
            // failure reports "not an ISO9660 disc image", including the ones with
            // a much more useful answer.
            const std::string reason = m_readError;
            close();
            error = reason.empty() ? "not an ISO9660 disc image" : ("can't read the disc: " + reason);
            return false;
        }
        m_volumeId.assign(reinterpret_cast<const char *>(pvd + 40), 32);
        m_volumeId.erase(m_volumeId.find_last_not_of(' ') + 1);
        const uint8_t *root = pvd + 156;
        if (!readDirectory("", le32(root + 2), le32(root + 10), 0))
        {
            close();
            error = "can't read the disc's directories";
            return false;
        }
        return true;
    }

    // A .cue names the image file beside it. The data track is the one that
    // matters - a disc can carry audio tracks too - so this takes the first FILE
    // that is followed by a non-audio TRACK, and the first FILE of all when the
    // sheet does not say.
    bool IsoImage::readCueTarget(const std::string &imagePath, std::string &target, std::string &error)
    {
        std::FILE *f = std::fopen(imagePath.c_str(), "rb");
        if (!f)
        {
            error = "can't open " + imagePath + " (" + std::strerror(errno) + ")";
            return false;
        }
        std::string text;
        char buffer[4096];
        size_t n = 0;
        while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0)
            text.append(buffer, n);
        std::fclose(f);

        std::string first, data;
        std::string pending;
        bool havePending = false;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            line = trim(line);
            if (line.rfind("REM", 0) == 0 || line.rfind("//", 0) == 0)
                continue;
            // FILE is the standard cue keyword. DATAFILE is what the Windows tools
            // that produce a "cue" from an already-extracted bin write instead, and
            // it means the same thing - the owner of this project hit exactly that.
            const size_t keyword = line.rfind("DATAFILE", 0) == 0 ? 8 : line.rfind("FILE", 0) == 0 ? 4 : 0;
            if (keyword != 0)
            {
                std::string rest = trim(line.substr(keyword));
                std::string name;
                if (!rest.empty() && rest.front() == '"')
                {
                    const size_t close = rest.find('"', 1);
                    name = rest.substr(1, close == std::string::npos ? std::string::npos : close - 1);
                }
                else
                {
                    const size_t space = rest.find_first_of(" \t");
                    name = rest.substr(0, space);
                }
                if (name.empty())
                    continue;
                pending = name;
                havePending = true;
                if (first.empty())
                    first = name;
                continue;
            }
            if (havePending && line.rfind("TRACK", 0) == 0)
            {
                if (line.find("AUDIO") == std::string::npos && data.empty())
                    data = pending;
                havePending = false;
            }
        }
        const std::string chosen = data.empty() ? first : data;
        if (chosen.empty())
        {
            error = imagePath + " is a .cue sheet, but it does not name an image file";
            return false;
        }

        // The name in a cue is relative to the sheet, and may use Windows
        // separators whatever is reading it.
        std::string name = chosen;
        std::replace(name.begin(), name.end(), '\\', '/');
        std::filesystem::path path(name);
        target = path.is_absolute() ? name : (std::filesystem::path(imagePath).parent_path() / path).string();
        if (!std::filesystem::exists(target))
        {
            error = imagePath + " is a .cue sheet for \"" + chosen + "\", which is not next to it - " +
                    "keep the two files together, or pick the image file itself";
            return false;
        }
        return true;
    }

    bool IsoImage::openImage(const std::string &imagePath, std::string &error)
    {
        m_file = std::fopen(imagePath.c_str(), "rb");
        if (!m_file)
        {
            // errno is the difference between "no such file" and "not allowed".
            error = "can't open " + imagePath + " (" + std::strerror(errno) + ")";
            return false;
        }
        // The descriptor sits at sector 16, so anything under 17 sectors is not
        // worth three read attempts to find that out - and "not an ISO9660 disc
        // image" is a better answer than a read error at sector 16 for a file the
        // player picked by mistake.
        if (seek64(m_file, 0) && fseek(m_file, 0, SEEK_END) == 0)
        {
            const long size = ftell(m_file);
            if (size >= 0 && static_cast<uint64_t>(size) < 17u * 2048u)
            {
                close();
                error = "not an ISO9660 disc image: " + imagePath + " is only " + std::to_string(size) + " bytes";
                return false;
            }
            seek64(m_file, 0);
        }
        return mount(false, error);
    }

    bool IsoImage::openChd(void *handle, std::string &error)
    {
#if defined(PS2X_ISO_CHD)
        m_chd = handle;
        const chd_header *head = chd_get_header(static_cast<chd_file *>(handle));
        // A CD or DVD CHD holds 2448-byte CD frames or flat 2048-byte sectors. A
        // hard-disc one holds 512, and is not something a PlayStation 2 game came
        // on, so saying so is better than reading it as nonsense.
        if (head->unitbytes < 2048u)
        {
            error = "this CHD holds a hard-disc image, not a CD or DVD";
            close();
            return false;
        }
        m_chdFrameBytes = head->unitbytes;
        return mount(true, error);
#else
        (void)handle;
        error = "this is a CHD, a compressed disc image, and this build of ps2x_iso was made without "
                "CHD support - rebuild it with -DPS2X_ISO_CHD=ON, or unpack the CHD to .bin/.cue with chdman first";
        return false;
#endif
    }

    bool IsoImage::openOne(const std::string &imagePath, std::string &error, int depth)
    {
        if (depth > 4)
        {
            error = "too many .cue sheets pointing at each other";
            return false;
        }
        std::FILE *probe = std::fopen(imagePath.c_str(), "rb");
        if (!probe)
        {
            error = "can't open " + imagePath + " (" + std::strerror(errno) + ")";
            return false;
        }
        uint8_t head[1024];
        const size_t size = std::fread(head, 1, sizeof head, probe);
        std::fclose(probe);

        if (size >= 8 && looksLikeChd(head))
        {
#if defined(PS2X_ISO_CHD)
            chd_file *chd = nullptr;
            const chd_error err = chd_open(imagePath.c_str(), CHD_OPEN_READ, nullptr, &chd);
            if (err != CHDERR_NONE)
            {
                error = "can't open " + imagePath + " as a CHD: " + std::string(chd_error_string(err));
                return false;
            }
            return openChd(chd, error);
#else
            return openChd(nullptr, error);
#endif
        }
        if (looksLikeCue(head, size))
        {
            std::string target;
            if (!readCueTarget(imagePath, target, error))
                return false;
            return openOne(target, error, depth + 1);
        }
        return openImage(imagePath, error);
    }

    bool IsoImage::open(const std::string &imagePath, std::string &error)
    {
        close();
#if defined(_WIN32)
        // "D:" (or "D:\\") names a drive: open the volume itself.
        if ((imagePath.size() == 2 || imagePath.size() == 3) && imagePath[1] == ':')
        {
            const std::wstring device = L"\\\\.\\" + std::wstring(1, static_cast<wchar_t>(imagePath[0])) + L":";
            HANDLE handle = CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                // The Windows error is the useful part: it separates "no disc"
                // from "not allowed", which otherwise look identical.
                const DWORD code = GetLastError();
                error = "can't open drive " + imagePath + " (Windows error " + std::to_string(code);
                if (code == 5)
                    error += ": access denied - try running this as administrator";
                else if (code == 2)
                    error += ": no such drive";
                else if (code == 21 || code == 1155)
                    error += ": the drive is not ready - no disc, or still spinning up";
                else
                    error += ": is a disc inserted?";
                error += ")";
                return false;
            }
            m_device = handle;
            return mount(false, error);
        }
#endif
        return openOne(imagePath, error, 0);
    }

    bool IsoImage::readDirectory(const std::string &prefix, uint32_t lba, uint32_t size, int depth)
    {
        if (depth > 32 || size > 64u * 1024u * 1024u)
            return false;
        const uint32_t sectors = (size + 2047u) / 2048u;
        std::vector<uint8_t> data(size_t(sectors) * 2048u);
        if (!readSectors(lba, sectors, data.data()))
            return false;
        uint32_t pos = 0;
        while (pos < size)
        {
            const uint8_t length = data[pos];
            if (length == 0u)
            {
                pos = (pos / 2048u + 1u) * 2048u; // records don't cross sectors
                continue;
            }
            if (pos + length > data.size() || length < 34u)
                return false;
            const uint8_t *record = data.data() + pos;
            const uint8_t nameLength = record[32];
            const bool directory = (record[25] & 0x02u) != 0u;
            std::string name(reinterpret_cast<const char *>(record + 33), nameLength);
            pos += length;
            if (nameLength == 1u && (name[0] == '\0' || name[0] == '\1'))
                continue; // "." and ".."
            if (const auto semicolon = name.find(';'); semicolon != std::string::npos)
                name.resize(semicolon);
            if (!name.empty() && name.back() == '.')
                name.pop_back();
            IsoEntry entry;
            entry.path = prefix + "/" + name;
            entry.lba = le32(record + 2);
            entry.size = le32(record + 10);
            entry.directory = directory;
            m_entries.push_back(entry);
            if (directory && !readDirectory(entry.path, entry.lba, static_cast<uint32_t>(entry.size), depth + 1))
                return false;
        }
        return true;
    }

    const IsoEntry *IsoImage::find(const std::string &path) const
    {
        std::string wanted = upper(path);
        if (wanted.empty() || wanted[0] != '/')
            wanted = "/" + wanted;
        for (const IsoEntry &entry : m_entries)
            if (upper(entry.path) == wanted)
                return &entry;
        return nullptr;
    }

    bool IsoImage::readFile(const IsoEntry &entry, std::vector<uint8_t> &out, std::string &error)
    {
        out.assign(size_t((entry.size + 2047u) / 2048u) * 2048u, 0u);
        if (!readSectors(entry.lba, static_cast<uint32_t>(out.size() / 2048u), out.data()))
        {
            error = "read error in " + entry.path;
            return false;
        }
        out.resize(static_cast<size_t>(entry.size));
        return true;
    }

    bool IsoImage::extractFile(const IsoEntry &entry, const std::string &destPath, std::string &error,
                               const std::function<void(uint64_t)> &progress)
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(destPath).parent_path(), ec);
        std::FILE *out = std::fopen(destPath.c_str(), "wb");
        if (!out)
        {
            error = "can't write " + destPath;
            return false;
        }
        constexpr uint32_t kChunkSectors = 2048u; // 4 MB
        std::vector<uint8_t> buffer(size_t(kChunkSectors) * 2048u);
        uint64_t done = 0;
        bool ok = true;
        while (done < entry.size && ok)
        {
            const uint64_t left = entry.size - done;
            const uint32_t sectors = static_cast<uint32_t>(std::min<uint64_t>(kChunkSectors, (left + 2047u) / 2048u));
            ok = readSectors(entry.lba + static_cast<uint32_t>(done / 2048u), sectors, buffer.data());
            const size_t bytes = static_cast<size_t>(std::min<uint64_t>(left, uint64_t(sectors) * 2048u));
            ok = ok && std::fwrite(buffer.data(), 1, bytes, out) == bytes;
            done += bytes;
            if (progress)
                progress(done);
        }
        ok = std::fclose(out) == 0 && ok;
        if (!ok)
            error = "error extracting " + entry.path;
        return ok;
    }

    uint64_t IsoImage::totalFileBytes() const
    {
        uint64_t total = 0;
        for (const IsoEntry &entry : m_entries)
            if (!entry.directory)
                total += entry.size;
        return total;
    }

    std::vector<std::string> opticalDrives()
    {
        std::vector<std::string> drives;
#if defined(_WIN32)
        const DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i)
        {
            if (!(mask & (1u << i)))
                continue;
            const wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
            if (GetDriveTypeW(root) == DRIVE_CDROM)
                drives.push_back(std::string(1, static_cast<char>('A' + i)) + ":");
        }
#else
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator("/dev", ec))
        {
            const std::string name = entry.path().filename().string();
            if (name.size() > 2 && name.compare(0, 2, "sr") == 0 && std::isdigit(static_cast<unsigned char>(name[2])))
                drives.push_back(entry.path().string());
        }
        std::sort(drives.begin(), drives.end());
#endif
        return drives;
    }

    std::string isoBootFile(IsoImage &image)
    {
        const IsoEntry *cnf = image.find("/SYSTEM.CNF");
        std::vector<uint8_t> data;
        std::string error;
        if (!cnf || !image.readFile(*cnf, data, error))
            return {};
        const std::string text(data.begin(), data.end());
        const auto boot = text.find("BOOT2");
        if (boot == std::string::npos)
            return {};
        const auto colon = text.find(':', boot);
        if (colon == std::string::npos)
            return {};
        size_t start = colon + 1;
        while (start < text.size() && (text[start] == '\\' || text[start] == '/'))
            ++start;
        size_t end = start;
        while (end < text.size() && text[end] != ';' && text[end] != '\r' && text[end] != '\n')
            ++end;
        return text.substr(start, end - start);
    }
}
