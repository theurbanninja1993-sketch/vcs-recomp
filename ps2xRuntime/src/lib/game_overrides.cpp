#include "runtime/ps2_music_station.h"
#include "runtime/ps2_camera.h"
void ps2CameraStreamPush(const Ps2CameraState &camera); // gs_frontend.cpp
void ps2InterpEvent(char event); // gs_frontend.cpp
#include "runtime/ps2_audio.h"
#include "runtime/ps2_mods.h" // ps2CurrentPromptSet (button-prompt text)
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_runtime_calls.h"
#include "ps2_stubs.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_syscalls.h"
#include "ps2_log.h"
#include "runtime/ps2_camera.h"
#include "runtime/ps2_platform.h"
#include "runtime/ps2_host_settings.h"
#include "runtime/ps2_pad.h"
#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <sstream>
#include <utility>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <unordered_set>
#include <optional>
#include <vector>

extern std::atomic<uint64_t> g_ps2PresentedFrameCounter;

// Defined in EeScheduler.cpp. The button-text scan gate counts frames, not
// calls (this is reached many times per frame).
extern std::atomic<uint64_t> g_ps2PresentedFrameCounter;
// Full 32 MB button-text scans, reported in the perf log.
std::atomic<uint64_t> g_vcsButtonScans{0};

namespace
{
    std::mutex &registryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    std::vector<ps2_game_overrides::Descriptor> &descriptorRegistry()
    {
        static std::vector<ps2_game_overrides::Descriptor> registry;
        return registry;
    }

    bool equalsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        for (size_t i = 0; i < lhs.size(); ++i)
        {
            const auto l = static_cast<unsigned char>(lhs[i]);
            const auto r = static_cast<unsigned char>(rhs[i]);
            if (std::tolower(l) != std::tolower(r))
            {
                return false;
            }
        }

        return true;
    }

    std::string basenameFromPath(const std::string &path)
    {
        std::error_code ec;
        const std::filesystem::path fsPath(path);
        const std::filesystem::path leaf = fsPath.filename();
        if (leaf.empty())
        {
            return path;
        }
        return leaf.string();
    }

    std::optional<PS2Runtime::RecompiledFunction> resolveHandlerByName(std::string_view handlerName)
    {
        const std::string_view resolvedSyscall = ps2_runtime_calls::resolveSyscallName(handlerName);
        if (!resolvedSyscall.empty())
        {
#define PS2_RESOLVE_SYSCALL(name)                   \
    if (resolvedSyscall == std::string_view{#name}) \
    {                                               \
        return &ps2_syscalls::name;                 \
    }
            PS2_SYSCALL_LIST(PS2_RESOLVE_SYSCALL)
#undef PS2_RESOLVE_SYSCALL
        }

        const std::string_view resolvedStub = ps2_runtime_calls::resolveStubName(handlerName);
        if (!resolvedStub.empty())
        {
#define PS2_RESOLVE_STUB(name)                   \
    if (resolvedStub == std::string_view{#name}) \
    {                                            \
        return &ps2_stubs::name;                 \
    }
            PS2_STUB_LIST(PS2_RESOLVE_STUB)
#undef PS2_RESOLVE_STUB
        }

        return std::nullopt;
    }
}
namespace ps2_game_overrides
{
    AutoRegister::AutoRegister(const Descriptor &descriptor)
    {
        registerDescriptor(descriptor);
    }

    void registerDescriptor(const Descriptor &descriptor)
    {
        if (!descriptor.apply)
        {
            std::cerr << "[game_overrides] ignoring descriptor with null apply callback." << std::endl;
            return;
        }

        std::lock_guard<std::mutex> lock(registryMutex());
        descriptorRegistry().push_back(descriptor);
    }

    bool bindAddressHandler(PS2Runtime &runtime, uint32_t address, std::string_view handlerName)
    {
        const auto resolved = resolveHandlerByName(handlerName);
        if (!resolved.has_value())
        {
            std::cerr << "[game_overrides] unresolved handler '" << handlerName
                      << "' for address 0x" << std::hex << address << std::dec << std::endl;
            return false;
        }

        return runtime.replaceFunction(address, resolved.value());
    }

    void applyMatching(PS2Runtime &runtime,
                       const std::string &elfPath,
                       uint32_t entry,
                       uint32_t fileCrc32,
                       bool fileCrcValid)
    {

        std::vector<Descriptor> descriptors;
        {
            std::lock_guard<std::mutex> lock(registryMutex());
            descriptors = descriptorRegistry();
        }

        if (descriptors.empty())
        {
            return;
        }

        const std::string elfName = basenameFromPath(elfPath);
        size_t appliedCount = 0;
        for (const Descriptor &descriptor : descriptors)
        {
            if (!descriptor.apply)
            {
                continue;
            }

            if (descriptor.elfName && descriptor.elfName[0] != '\0')
            {
                if (!equalsIgnoreCaseAscii(descriptor.elfName, elfName))
                {
                    continue;
                }
            }

            if (descriptor.entry != 0u && descriptor.entry != entry)
            {
                continue;
            }

            if (descriptor.crc32 != 0u)
            {
                if (!fileCrcValid || fileCrc32 != descriptor.crc32)
                {
                    continue;
                }
            }

            const char *name = (descriptor.name && descriptor.name[0] != '\0')
                                   ? descriptor.name
                                   : "unnamed";
            RUNTIME_LOG("[game_overrides] applying '" << name << "'");
            descriptor.apply(runtime);
            ++appliedCount;
        }

        if (appliedCount > 0)
        {
            RUNTIME_LOG("[game_overrides] applied " << appliedCount << " matching override(s).");
        }
    }
}

// ---------------------------------------------------------------------------
// Grand Theft Auto: Vice City Stories (NTSC-U, SLUS_215.90)
//
// Lives in this TU (rather than its own file) so the static registration is
// never dropped when ps2_runtime is linked as a static archive.
// ---------------------------------------------------------------------------
namespace
{
    uint32_t vcsRead32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t v = 0u;
        std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), sizeof(v));
        return v;
    }

    uint64_t vcsRead64(const uint8_t *rdram, uint32_t addr)
    {
        uint64_t v = 0u;
        std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), sizeof(v));
        return v;
    }

    void vcsWrite32(uint8_t *rdram, uint32_t addr, uint32_t v)
    {
        std::memcpy(rdram + (addr & PS2_RAM_MASK), &v, sizeof(v));
    }

    void vcsWrite64(uint8_t *rdram, uint32_t addr, uint64_t v)
    {
        std::memcpy(rdram + (addr & PS2_RAM_MASK), &v, sizeof(v));
    }

    // Exact libpkt semantics, translated from the game's own code. The generic
    // runtime stubs also rewrite the open DMA tag's QWC on every pointer move,
    // but this game closes DMA tags natively (0x448AB8: qwc += (cur-tag)/16 - 1),
    // so the generic stubs double-count and corrupt every GIF chain (e.g. the
    // movie's per-macroblock texture uploads).
    //
    // void sceGifPkAddGsAD(sceGifPacket *pkt, u_int addr, u_long data)
    void vcsGifPkAddGsAD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t pkt = getRegU32(ctx, 4);
        const uint32_t cur = vcsRead32(rdram, pkt);
        vcsWrite64(rdram, cur, GPR_U64(ctx, 6));
        vcsWrite64(rdram, cur + 8u, static_cast<uint64_t>(getRegU32(ctx, 5)));
        vcsWrite32(rdram, pkt, cur + 16u);
        ctx->pc = getRegU32(ctx, 31);
    }

    // void sceGifPkCloseGifTag(sceGifPacket *pkt)
    void vcsGifPkCloseGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t pkt = getRegU32(ctx, 4);
        const uint32_t tagAddr = vcsRead32(rdram, pkt + 12u);
        uint32_t cur = vcsRead32(rdram, pkt);
        if (tagAddr != 0u)
        {
            uint64_t tag = vcsRead64(rdram, tagAddr);
            uint32_t count = static_cast<uint32_t>(static_cast<int32_t>(cur - tagAddr) >> 3) - 2u;
            const uint32_t flg = static_cast<uint32_t>(tag >> 58) & 3u;
            if (flg != 1u)
            {
                count >>= 1u; // PACKED/IMAGE count qwords, REGLIST counts dwords
            }
            if (flg != 2u)
            {
                uint32_t nreg = static_cast<uint32_t>(tag >> 60);
                if (nreg == 0u)
                {
                    nreg = 16u;
                }
                count = (count + nreg - 1u) / nreg;
            }
            vcsWrite32(rdram, pkt + 12u, 0u);
            vcsWrite64(rdram, tagAddr, tag + count);
        }
        while ((cur & 0xCu) != 0u)
        {
            vcsWrite32(rdram, cur, 0u);
            cur += 4u;
        }
        vcsWrite32(rdram, pkt, cur);
        ctx->pc = getRegU32(ctx, 31);
    }

    // Movie player end-of-stream (0x31A5D0). The game streams exactly the PSS file's
    // byte size, skips its trailing program_end, and instead retries writing
    // 00 00 01 B7 into libmpeg's video ES ring until the IPU has drained room for it.
    // The HLE decoder never reads that ring (so it never drains), and doesn't need the
    // end code there: report it queued and finish the HLE playback instead. Without
    // this the player deadlocks after every movie.
    void vcsMoviePushEndCode(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_stubs::notifyMpegCdStreamEof(runtime);
        setReturnS32(ctx, 1);
        ctx->pc = getRegU32(ctx, 31);
    }

    // Movie player "finished?" (0x31A6A8: PSS ring drained && sceMpegIsEnd). The HLE
    // demuxer accepts data faster than the IPU would, so when the main loop stops
    // presenting, a few decoded fade-out pictures can still be owed to the decoder
    // thread, which then waits forever for a display slot nobody frees. Report done
    // once the HLE stream has ended and been shown (isMpegStreamFinished).
    PS2Runtime::RecompiledFunction g_vcsOrigMovieFinished = nullptr;
    void vcsMovieFinished(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t player = getRegU32(ctx, 4);
        g_vcsOrigMovieFinished(rdram, ctx, runtime);
        ps2_stubs::pumpMpegAudio(rdram, ctx, runtime); // the soundtrack outlasts the demux calls
        if (getRegU32(ctx, 2) == 0u && ps2_stubs::isMpegStreamFinished(player, runtime))
        {
            setReturnS32(ctx, 1);
        }
    }

    // PS2X_COUNT_CALLS=0xA,0xB,...: debug wrapper that counts calls to up to 16 guest
    // functions and prints the counts (and the first calls' args) every 600 frames.
    constexpr int kVcsMaxCounted = 16;
    PS2Runtime::RecompiledFunction g_vcsCountOrig[kVcsMaxCounted]{};
    uint32_t g_vcsCountAddr[kVcsMaxCounted]{};
    std::atomic<uint64_t> g_vcsCountHits[kVcsMaxCounted]{};
    int g_vcsCountUsed = 0;

    void vcsReportCounts()
    {
        static uint64_t s_lastFrame = 0;
        const uint64_t frame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        if (frame < s_lastFrame + 600u)
            return;
        s_lastFrame = frame;
        std::ostringstream out;
        out << "[calls] frame=" << frame;
        for (int i = 0; i < g_vcsCountUsed; ++i)
            out << " " << std::hex << g_vcsCountAddr[i] << std::dec << "=" << g_vcsCountHits[i].load();
        std::cerr << out.str() << std::endl;
    }

    template <int N>
    void vcsCountWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint64_t hit = g_vcsCountHits[N].fetch_add(1u, std::memory_order_relaxed);
        if (hit < 4u)
        {
            std::cerr << "[calls:first] fn=0x" << std::hex << g_vcsCountAddr[N] << " a0=0x" << getRegU32(ctx, 4)
                      << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6) << " a3=0x" << getRegU32(ctx, 7)
                      << " ra=0x" << getRegU32(ctx, 31) << std::dec << std::endl;
        }
        vcsReportCounts();
        g_vcsCountOrig[N](rdram, ctx, runtime);
    }

    template <int... Is>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(Is)> vcsMakeCountWrappers(std::integer_sequence<int, Is...>)
    {
        return {&vcsCountWrapper<Is>...};
    }

    void vcsInstallCallCounters(PS2Runtime &runtime)
    {
        const char *env = std::getenv("PS2X_COUNT_CALLS");
        if (!env)
            return;
        static const auto wrappers = vcsMakeCountWrappers(std::make_integer_sequence<int, kVcsMaxCounted>{});
        const char *cursor = env;
        while (*cursor && g_vcsCountUsed < kVcsMaxCounted)
        {
            char *end = nullptr;
            const uint32_t addr = static_cast<uint32_t>(std::strtoul(cursor, &end, 0));
            if (end == cursor)
                break;
            if (PS2Runtime::RecompiledFunction orig = runtime.lookupFunction(addr))
            {
                const int slot = g_vcsCountUsed++;
                g_vcsCountOrig[slot] = orig;
                g_vcsCountAddr[slot] = addr;
                runtime.replaceFunction(addr, wrappers[slot]);
            }
            cursor = (*end == ',') ? end + 1 : end;
        }
    }

    // Whether the player is driving: the player ped (found through one of a
    // few static pointers, validated by the back-pointer) points to its vehicle
    // at +0x480, and the vehicle points back to its driver at +0x384.
    // The player's vehicle, or 0. Found through a few static ped pointers and
    // validated by the vehicle's back-pointer to its driver.
    uint32_t findPlayerVehicle(const uint8_t *rdram)
    {
        auto read32 = [rdram](uint32_t address) -> uint32_t
        {
            uint32_t value = 0;
            if (address >= 0x00100000u && address + 4u <= PS2_RAM_SIZE)
                std::memcpy(&value, rdram + address, 4);
            return value;
        };
        static constexpr uint32_t kPlayerPedPointers[] = {0x004E4910u, 0x00517834u, 0x006F4820u, 0x00744AC8u};
        for (const uint32_t pointer : kPlayerPedPointers)
        {
            const uint32_t ped = read32(pointer);
            if (ped == 0u)
                continue;
            const uint32_t vehicle = read32(ped + 0x480u);
            if (vehicle != 0u && read32(vehicle + 0x384u) == ped)
                return vehicle;
        }
        return 0u;
    }

    bool vcsPlayerInVehicle(const uint8_t *rdram)
    {
        const uint32_t vehicle = findPlayerVehicle(rdram);
        if (vehicle == 0u)
            return false;
        return true;
    }

    // Whether the game's pause menu / map is open. 0x004CD2AC is the pause
    // menu flag the game's own pause handlers set on START and clear when the
    // menu closes (the Custom Soundtracks query reads it the same way).
    bool vcsPauseMenuOpen(const uint8_t *rdram)
    {
        return rdram != nullptr && rdram[0x004CD2ACu] != 0u;
    }

    // Whether a cutscene is playing. Found by RAM-diffing F8 dumps taken
    // during cutscenes (B/C) and in gameplay (A/D): the flag is 1 in the
    // cutscene dumps and 0 in the gameplay ones. The opening logos and
    // credits are FMV movies rather than in-engine cutscenes, so a recently
    // served movie picture counts too. PS2X_CUTSCENE_FLAG=<hex> overrides
    // the address for testing (read as a 32-bit word).
    bool vcsInCutscene(const uint8_t *rdram)
    {
        if (ps2_stubs::isMpegMovieActive(g_ps2PresentedFrameCounter.load(std::memory_order_relaxed)))
            return true;
        static const uint32_t s_flag = []()
        {
            if (const char *env = std::getenv("PS2X_CUTSCENE_FLAG"))
                return static_cast<uint32_t>(std::strtoul(env, nullptr, 16));
            return 0x006F4D14u;
        }();
        if (rdram == nullptr || static_cast<size_t>(s_flag) + 4u > PS2_RAM_SIZE)
            return false;
        uint32_t value = 0;
        std::memcpy(&value, rdram + s_flag, 4);
        return value != 0u;
    }

    // The player's ped: CWorld::Players[CWorld::PlayerInFocus].m_pPed
    // (FindPlayerPed 0x24A430: 0x4E4910 + index byte 0x4CD128 * 0x190).
    uint32_t vcsPlayerPed(const uint8_t *rdram)
    {
        const uint32_t index = rdram[0x004CD128u];
        uint32_t ped = 0;
        std::memcpy(&ped, rdram + 0x004E4910u + index * 0x190u, 4);
        return (ped >= 0x00100000u && ped + 0x900u <= PS2_RAM_SIZE) ? ped : 0u;
    }

    // Free aim applies on foot with a gun. The current weapon slot is the
    // signed byte at ped+0x789 (weapons at ped+0x574, 0x1C each, type at +4).
    // Slots 3-6 and 8: handgun, shotgun, SMG, assault rifle, heavy. The sniper
    // rifle (7) keeps its scope, and melee / thrown keep the lock-on.
    bool vcsFreeAimWeapon(const uint8_t *rdram)
    {
        if (rdram == nullptr || vcsPlayerInVehicle(rdram) || vcsInCutscene(rdram))
            return false;
        const uint32_t ped = vcsPlayerPed(rdram);
        if (ped == 0u)
            return false;
        const int slot = static_cast<int8_t>(rdram[ped + 0x789u]);
        if (slot < 3 || slot > 8 || slot == 7)
            return false;
        uint32_t type = 0;
        std::memcpy(&type, rdram + ped + 0x574u + static_cast<uint32_t>(slot) * 0x1Cu + 4u, 4);
        return type != 0u;
    }

    // The current in-vehicle radio station: 0..8 = the nine real stations,
    // 9 = the game's silent "Radio Off" slot. The station field is at +0x2BA
    // of the audio/vehicle entity (not byte 0x4E4755, which is overloaded with
    // radio *modes* 9..30). FUN_0018aad8 (every frame, a1 = that entity) keeps
    // the field; FUN_0018b8d8 (on each L1/R1 change) applies and wraps it.
    // 0xFF = not set yet.
    std::atomic<uint32_t> g_vcsRadioStation{0xFFu};
    // True when the player is on the injected "Custom Soundtracks" stop. The
    // game has no spare station value (its field is clamped to 0..8 + 9), so
    // the custom stop and Radio Off share value 9 and this flag says which one
    // is selected. The music folder plays while it is set (in a vehicle).
    std::atomic<bool> g_vcsCustomStation{false};
    // True while the plain "Radio Off" stop is selected (the custom stop and off
    // share the silent slot, so they need separate flags for the pop-up names).
    std::atomic<bool> g_vcsOffStation{false};
    PS2Runtime::RecompiledFunction g_vcsOrigRadioUpdate = nullptr;     // FUN_0018aad8
    PS2Runtime::RecompiledFunction g_vcsOrigRadioStationGet = nullptr; // FUN_0018b8d8
    PS2Runtime::RecompiledFunction g_vcsFnRadioEntity = nullptr;       // FUN_1B5768(0x514A90)

    // The station pop-up draws the GXT name of the current station slot. The
    // custom stop and Radio Off share slot 9, so both would read "Radio Off";
    // rename that string in guest RAM while the custom stop is active and put it
    // back for Radio Off. The GXT is loaded lazily, so rescan until it is found.
    uint32_t vcsRadioOffNameAddr(uint8_t *rdram)
    {
        static uint32_t s_addr = 0;
        if (s_addr != 0u)
            return s_addr;
        const char needle[] = "Radio Off";
        uint8_t pat[sizeof(needle) * 2] = {};
        for (size_t i = 0; i + 1 < sizeof(needle); ++i)
            pat[i * 2] = static_cast<uint8_t>(needle[i]);
        for (uint32_t a = 0x00100000u; a + sizeof(pat) < PS2_RAM_SIZE; ++a)
        {
            if (std::memcmp(rdram + a, pat, sizeof(pat)) == 0)
            {
                s_addr = a;
                break;
            }
        }
        return s_addr;
    }

    // While one of our stops is selected (the custom stop or Radio Off - they
    // share the silent slot), blank every *other* station's GXT name and give the
    // silent slot our own name. The pop-up reads the name live, so while the
    // engine has not retuned yet it draws nothing instead of the previous
    // station's name. Everything is restored for a normal station.
    struct VcsStationNames
    {
        uint32_t addr[10]{};      // 0..8 by FEA_FM0..8, 9 = the "Radio Off" string
        uint16_t firstChar[10]{}; // saved first UTF-16 char of each name
        uint16_t offName[20]{};   // slot 9's original name ("Radio Off")
        bool found = false;
        int applied = -1;         // 0 = normal, 1 = custom, 2 = Radio Off
    };
    VcsStationNames g_vcsStationNames;

    void vcsFindStationNames(uint8_t *rdram)
    {
        if (g_vcsStationNames.found)
            return;
        // The loaded GXT keeps its key table as {char[8] key, uint32 text offset}.
        for (uint32_t a = 0x00100000u; a + 12u < PS2_RAM_SIZE; ++a)
        {
            if (std::memcmp(rdram + a, "FEA_FM", 6u) != 0)
                continue;
            const int idx = static_cast<int>(rdram[a + 6u]) - '0';
            if (idx < 0 || idx > 8 || rdram[a + 7u] != 0u)
                continue;
            uint32_t p = 0;
            std::memcpy(&p, rdram + a + 8u, sizeof(p));
            if (p >= 0x00100000u && p + 4u < PS2_RAM_SIZE)
                g_vcsStationNames.addr[idx] = p;
        }
        const uint32_t off = vcsRadioOffNameAddr(rdram);
        g_vcsStationNames.addr[9] = off;
        bool ok = off != 0u;
        for (int i = 0; i < 9; ++i)
            ok = ok && g_vcsStationNames.addr[i] != 0u;
        if (!ok)
        {
            if (std::getenv("PS2X_RADIO_TRACE") != nullptr)
                std::cerr << "[radio] station names: not found" << std::endl;
            return;
        }
        if (std::getenv("PS2X_RADIO_TRACE") != nullptr)
            std::cerr << "[radio] station names found" << std::endl;
        for (int i = 0; i < 10; ++i)
            std::memcpy(&g_vcsStationNames.firstChar[i], rdram + g_vcsStationNames.addr[i], sizeof(uint16_t));
        std::memcpy(g_vcsStationNames.offName, rdram + off, sizeof(g_vcsStationNames.offName));
        g_vcsStationNames.found = true;
    }

    // The silent slot IS our Custom Soundtracks station, so its GXT name is
    // "Custom" - except while the runtime's radio is switched off, when it is the
    // original "Radio Off" (the re-tune to the silent slot then shows the proper
    // name instead of the custom one). Writing the same text every frame means
    // the game's station pop-up can never pick up a stale name.
    void vcsKeepCustomName(uint8_t *rdram)
    {
        // The station names (and the "Radio Off" string) come from the GXT,
        // which loads late: looking for them is a full RAM pass, so do that at
        // most once every 60 frames until they turn up instead of every frame.
        if (!g_vcsStationNames.found)
        {
            static int s_searchCooldown = 0;
            if (s_searchCooldown-- > 0)
                return;
            s_searchCooldown = 60;
            vcsFindStationNames(rdram);
            if (!g_vcsStationNames.found)
                return;
        }
        static const uint16_t kCustom[10] = {'C', 'u', 's', 't', 'o', 'm', 0, 0, 0, 0};
        static const uint16_t kOff[10] = {'R', 'a', 'd', 'i', 'o', ' ', 'O', 'f', 'f', 0};
        const uint16_t *want = ps2MusicStationRadioOff() ? kOff : kCustom;
        uint16_t cur[10];
        std::memcpy(cur, rdram + g_vcsStationNames.addr[9], sizeof(cur));
        if (std::memcmp(cur, want, sizeof(cur)) != 0)
        {
            std::memcpy(rdram + g_vcsStationNames.addr[9], want, sizeof(cur));
            if (std::getenv("PS2X_RADIO_TRACE") != nullptr)
                std::cerr << "[radio] silent slot named " << (want == kOff ? "Radio Off" : "Custom") << std::endl;
        }
    }

    // ------------------------------------------------------ button-name text
    // The loaded GXT also carries button names as text: menu legends like
    // "L1 - Warp Player To Site", "CROSS - Complete Config" and "(L1+R1)".
    // The per-device prompt set rewrites those words (the HUD prompts use the
    // texture glyphs; L1/R1/L2/R2 only exist as text). Every rewrite is
    // same-length-or-shorter and rebuilt from a cached original string, so
    // switching sets - or back to Original - is idempotent.
    enum VcsButtonToken
    {
        VcsBtnL1, VcsBtnR1, VcsBtnL2, VcsBtnR2, VcsBtnL3, VcsBtnR3,
        VcsBtnCross, VcsBtnCircle, VcsBtnSquare, VcsBtnTriangle, VcsBtnSelect, VcsBtnStart,
        VcsBtnTokenCount
    };
    const char *const kButtonTokens[VcsBtnTokenCount] = {"L1", "R1", "L2", "R2", "L3", "R3",
                                                         "CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "SELECT", "START"};

    // Host-button names per prompt set (host enum order: A, B, X, Y, LB, RB,
    // LT, RT, LS, RS, View, Menu, D-pad...). The gamepad sets rename the face
    // buttons positionally; the PlayStation sets keep the game's own PS2
    // vocabulary (classic mappings leave their text unchanged).
    const char *const kHostLabelsXbox[ps2controls::kHostButtonCount] = {
        "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "VIEW", "MENU",
        "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right"};
    const char *const kHostLabelsSwitch[ps2controls::kHostButtonCount] = {
        "B", "A", "Y", "X", "L", "R", "ZL", "ZR", "LS", "RS", "MINUS", "PLUS",
        "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right"};
    const char *const kHostLabelsPs[ps2controls::kHostButtonCount] = {
        "CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "L1", "R1", "L2", "R2", "L3", "R3", "SELECT", "START",
        "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right"};

    // VcsButtonToken -> PS2 pad button index (ps2controls order).
    const int kVcsTokenPs2[VcsBtnTokenCount] = {10, 11, 8, 9, 1, 2, 14, 13, 15, 12, 0, 3};

    // Keyboard: the action that best represents `ps2Button` for prompt text.
    // The button-name strings live in menus, so menu actions come first; the
    // rest follows the current pad context (on foot / in a vehicle).
    int vcsKeyboardAction(int ps2Button)
    {
        namespace pc = ps2controls;
        // In the game's menus the keyboard uses the fixed menu actions: accept
        // is Enter, back is Backspace (the pad sends Cross / Triangle), and the
        // D-pad tokens are the arrow keys.
        if (pc::currentContext() == pc::Menus)
        {
            switch (ps2Button)
            {
            case 14: return pc::MenuAccept; // Cross
            case 13:
            case 12: return pc::MenuBack; // Circle / Triangle
            case 15: return pc::MapTarget; // Square: Target On/Off on the map
            case 4: return pc::MenuUp;
            case 6: return pc::MenuDown;
            case 7: return pc::MenuLeft;
            case 5: return pc::MenuRight;
            case 10:
            case 11: return pc::kNone; // the wheel zooms the map; no key label
            default: break;
            }
        }
        const bool driving = pc::currentContext() == pc::InVehicle;
        switch (ps2Button)
        {
        case 14: return pc::MenuAccept; // Cross
        case 13: return pc::Fire; // Circle
        case 15: return driving ? pc::MoveBackward : pc::Jump; // Square
        case 12: return pc::MenuBack; // Triangle
        case 10: return driving ? pc::Horn : pc::Interact; // L1
        case 11: return driving ? pc::Handbrake : pc::Aim; // R1
        case 8:
        case 9: return pc::NextWeapon; // L2 / R2
        case 2: return pc::LookBehind; // R3
        case 0: return pc::CameraMode; // Select
        default: return pc::kNone; // L3, Start
        }
    }

    // The text a token site should show for the current prompt set and
    // bindings. Empty = leave the game's own text alone.
    std::string vcsButtonLabel(int token)
    {
        namespace pc = ps2controls;
        const std::string set = ps2CurrentPromptSet();
        if (set.empty())
            return {};
        const int ps2 = kVcsTokenPs2[token];
        if (set == "keyboard")
        {
            const int action = vcsKeyboardAction(ps2);
            if (action >= 0)
            {
                const auto &keys = ps2HostSettings().keys[action];
                if (std::string s = pc::promptBindingName(keys[0]); !s.empty())
                    return s;
                if (std::string s = pc::promptBindingName(keys[1]); !s.empty())
                    return s;
            }
            return {};
        }
        const int host = pc::hostButtonForPs2(ps2, pc::currentContext());
        if (host == pc::kNone)
            return {};
        if (set == "xbox" || set == "steam")
            return kHostLabelsXbox[host];
        if (set == "switch")
            return kHostLabelsSwitch[host];
        return kHostLabelsPs[host]; // dualsense / dualshock
    }

    // Changes to the keyboard bindings, gamepad map or pad context invalidate
    // the applied text (the owner asked for prompts that always match).
    uint64_t vcsMappingSignature()
    {
        const PS2HostSettings &s = ps2HostSettings();
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](int v)
        {
            h ^= static_cast<uint64_t>(static_cast<uint32_t>(v));
            h *= 1099511628211ull;
        };
        for (int a = 0; a < ps2controls::kActionCount; ++a)
        {
            mix(s.keys[a][0]);
            mix(s.keys[a][1]);
        }
        for (int c = 0; c < ps2controls::kContextCount; ++c)
            for (int b = 0; b < ps2controls::kHostButtonCount; ++b)
                mix(s.gamepadMap[c][b]);
        mix(static_cast<int>(s.gamepadLayout));
        mix(ps2controls::currentContext());
        return h;
    }

    struct VcsButtonTextSite
    {
        uint32_t addr = 0;         // UTF-16 string start in guest RAM
        int token = 0;             // VcsButtonToken of the leading word
        int tokenLen = 0;          // original token length in UTF-16 chars
        uint16_t original[96]{};   // the original string (bounded)
        int originalLen = 0;       // chars before the original NUL
    };
    std::vector<VcsButtonTextSite> g_vcsButtonSites;
    std::unordered_set<uint32_t> g_vcsButtonAddrs;
    // Strings that embed the PS2 face-button symbols as text tokens ("Hold
    // ~X~ to display the map options."). The tokens render as font glyphs, so
    // console sets need no rewrite; the keyboard set gets the bound key names
    // because one keycap cannot stand for the menu meaning (ENTER, not W).
    struct VcsMenuPromptSite
    {
        uint32_t addr = 0;
        std::string original;
        int kind = 0; // 0 = a "~X~" symbol string, 1 = the map zoom pair
    };
    std::vector<VcsMenuPromptSite> g_vcsMenuPromptSites;
    std::unordered_set<uint32_t> g_vcsMenuPromptAddrs;
    std::string g_vcsButtonAppliedSet = "<init>";
    size_t g_vcsButtonAppliedCount = 0;
    size_t g_vcsMenuPromptAppliedCount = 0;
    uint64_t g_vcsMappingAppliedSig = 0;
    bool g_vcsButtonLogged = false;

    inline uint16_t vcsUtf16At(const uint8_t *rdram, uint32_t a)
    {
        return static_cast<uint16_t>(rdram[a] | (rdram[a + 1] << 8));
    }

    // A '~X~' / '~O~' token at `tokenAddr`: cache the whole containing string
    // once (the scanner visits every offset, so embedded tokens are seen too).
    void vcsTryMenuPromptSite(const uint8_t *rdram, uint32_t tokenAddr)
    {
        if (tokenAddr < 0x00100002u)
            return;
        const uint16_t sym = vcsUtf16At(rdram, tokenAddr + 2u);
        if (vcsUtf16At(rdram, tokenAddr + 4u) != '~' || (sym != 'X' && sym != 'O'))
            return;
        const uint16_t before = vcsUtf16At(rdram, tokenAddr - 2u);
        const uint16_t after = vcsUtf16At(rdram, tokenAddr + 6u);
        const bool okBefore = before == 0u || before == ' ' || before == '(';
        const bool okAfter = after == 0u || after == ' ' || after == '.' || after == ')' || after == ',' ||
                             after == '!' || after == '+' || after == '?';
        if (!okBefore || !okAfter)
            return;
        uint32_t start = tokenAddr;
        for (int back = 0; back < 160 && vcsUtf16At(rdram, start - 2u) != 0u; ++back)
            start -= 2u;
        if (start < 0x00100000u)
            return;
        if (!g_vcsMenuPromptAddrs.insert(start).second || g_vcsMenuPromptSites.size() >= 192u)
            return;
        VcsMenuPromptSite site;
        site.addr = start;
        for (int i = 0; i < 160; ++i)
        {
            const uint16_t c = vcsUtf16At(rdram, start + static_cast<uint32_t>(i) * 2u);
            if (c == 0u)
                break;
            site.original.push_back(static_cast<char>(c < 256u ? c : '?'));
        }
        if (site.original.size() >= 160u || (site.original.find("~X~") == std::string::npos &&
                                             site.original.find("~O~") == std::string::npos))
            return;
        g_vcsMenuPromptSites.push_back(std::move(site));
    }

    void vcsFindButtonText(const uint8_t *rdram)
    {
        // The strings load lazily, with the menus that use them (cheat menu,
        // business lists, pause pages), so this keeps looking for the whole
        // session - a full pass every 60 frames is cheap (first-char filter).
        //
        // The gate counts *frames*, not calls. This is reached from vcsFrameUpdate,
        // which hooks a game function the game calls many times per frame, so a
        // per-call countdown of 60 ran the scan every couple of frames: a full
        // 32 MB pass costs ~7.5 ms, which made this 26% of the game thread in a
        // gameplay profile - by far its largest single cost.
        static uint64_t s_nextScanFrame = 0;
        const uint64_t scanFrame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        if (scanFrame < s_nextScanFrame)
            return;
        s_nextScanFrame = scanFrame + 60;
        g_vcsButtonScans.fetch_add(1u, std::memory_order_relaxed);
        // One pass over RAM. Cheap first-char filter keeps it fast. '~' finds
        // the embedded menu tokens (~X~ / ~O~), which sit anywhere in their
        // string, so that branch walks back to the string start itself.
        // One uint16 load per step rather than two byte loads plus a shift: this
        // loop runs 16.7M times a pass, so it is the whole cost of the scan.
        const uint16_t *words = reinterpret_cast<const uint16_t *>(rdram);
        for (uint32_t a = 0x00100000u; a + 128u < PS2_RAM_SIZE; a += 2u)
        {
            const uint16_t first = words[a >> 1];
            if (first == '~')
            {
                vcsTryMenuPromptSite(rdram, a);
                continue;
            }
            if (first != 'L' && first != 'R' && first != 'C' && first != 'S' && first != 'T')
                continue;
            int token = -1, tokenLen = 0;
            for (int t = 0; t < VcsBtnTokenCount && token < 0; ++t)
            {
                if (t == VcsBtnStart)
                    continue; // "START" is only ever a verb / menu title in the GXT
                const size_t n = std::strlen(kButtonTokens[t]);
                bool ok = true;
                for (size_t i = 0; i < n && ok; ++i)
                    ok = vcsUtf16At(rdram, a + static_cast<uint32_t>(i) * 2u) == static_cast<uint8_t>(kButtonTokens[t][i]);
                if (!ok)
                    continue;
                // A button word sits at a string start or after one of the
                // separators the game uses; it never glues to another word on
                // the right.
                const uint16_t before = vcsUtf16At(rdram, a - 2u);
                const uint16_t after = vcsUtf16At(rdram, a + static_cast<uint32_t>(n) * 2u);
                auto alnum = [](uint16_t c)
                { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
                const bool okBefore = before == 0u || before == ' ' || before == '(' || before == '+' || before == '~';
                bool okAfter = !alnum(after);
                // "SELECT" is a button only in the settings lists' literal
                // "SELECT button" ("SELECT GAME TO JOIN" is a verb).
                if (t == VcsBtnSelect)
                    okAfter = okAfter && after == ' ' && vcsUtf16At(rdram, a + static_cast<uint32_t>(n + 1u) * 2u) == 'b';
                if (okBefore && okAfter)
                {
                    token = t;
                    tokenLen = static_cast<int>(n);
                }
            }
            if (token < 0)
                continue;
            VcsButtonTextSite site;
            site.addr = a;
            site.token = token;
            site.tokenLen = tokenLen;
            int n = 0;
            while (n < 95 && vcsUtf16At(rdram, a + static_cast<uint32_t>(n) * 2u) != 0u)
            {
                site.original[n] = vcsUtf16At(rdram, a + static_cast<uint32_t>(n) * 2u);
                ++n;
            }
            if (n >= 95) // very long string: leave it alone rather than risk a tail
                continue;
            site.original[n] = 0;
            site.originalLen = n;
            if (n <= tokenLen)
                continue;
            if (!g_vcsButtonAddrs.insert(a).second)
                continue; // already collected in an earlier pass
            g_vcsButtonSites.push_back(site);
            // The map's zoom hint ("L1 and R1 buttons..."): with the keyboard
            // set the wheel is the zoom, so the whole pair collapses to one
            // prompt. Registered as a whole-string site, applied last.
            if (token == VcsBtnL1 && site.originalLen > 0)
            {
                std::string orig;
                for (int i = 0; i < site.originalLen; ++i)
                    orig.push_back(static_cast<char>(site.original[i] < 256u ? site.original[i] : '?'));
                if (orig.rfind("L1 and R1 buttons", 0) == 0 && g_vcsMenuPromptAddrs.insert(a).second &&
                    g_vcsMenuPromptSites.size() < 192u)
                {
                    VcsMenuPromptSite pair;
                    pair.addr = a;
                    pair.original = std::move(orig);
                    pair.kind = 1;
                    g_vcsMenuPromptSites.push_back(std::move(pair));
                }
            }
            if (g_vcsButtonSites.size() >= 256u)
                break;
            // Do not skip the rest of the string: a later token ("R1" in
            // "(L1+R1)") must become a site too.
        }
        if (!g_vcsButtonLogged && !g_vcsButtonSites.empty())
        {
            g_vcsButtonLogged = true;
            if (std::getenv("PS2X_PROMPT_TRACE") != nullptr)
                std::cerr << "[prompts] button-name strings: " << g_vcsButtonSites.size() << std::endl;
        }
    }

    void vcsApplyButtonText(uint8_t *rdram)
    {
        if (!g_vcsStationNames.found) // waits for the loaded GXT
            return;
        vcsFindButtonText(rdram);
        if (g_vcsButtonSites.empty() && g_vcsMenuPromptSites.empty())
            return;
        const std::string set = ps2CurrentPromptSet();
        const uint64_t sig = vcsMappingSignature();
        if (set == g_vcsButtonAppliedSet && g_vcsButtonSites.size() == g_vcsButtonAppliedCount &&
            g_vcsMenuPromptSites.size() == g_vcsMenuPromptAppliedCount && sig == g_vcsMappingAppliedSig)
            return; // nothing new; sites can appear later with their menus
        for (VcsButtonTextSite &site : g_vcsButtonSites)
        {
            const std::string label = vcsButtonLabel(site.token);
            uint16_t buf[128];
            int n = 0;
            if (!label.empty())
            {
                for (char ch : label)
                    if (n < 120)
                        buf[n++] = static_cast<uint16_t>(static_cast<uint8_t>(ch));
                for (int i = site.tokenLen; i < site.originalLen && n < 120; ++i)
                    buf[n++] = site.original[i];
                if (n > site.originalLen && static_cast<int>(label.size()) <= site.originalLen)
                {
                    // A longer label ("BKSP" for "L2"): keep just the label
                    // rather than truncating the sentence.
                    n = 0;
                    for (char ch : label)
                        if (n < 120)
                            buf[n++] = static_cast<uint16_t>(static_cast<uint8_t>(ch));
                }
                if (n > site.originalLen)
                    n = -1; // does not fit: leave the game's own text
            }
            if (n < 0 || label.empty())
            {
                n = 0;
                for (int i = 0; i < site.originalLen && n < 120; ++i)
                    buf[n++] = site.original[i];
            }
            buf[n] = 0;
            for (int i = 0; i <= site.originalLen; ++i)
            {
                const uint16_t v = i <= n ? buf[i] : 0u;
                rdram[site.addr + static_cast<uint32_t>(i) * 2u] = static_cast<uint8_t>(v & 0xFFu);
                rdram[site.addr + static_cast<uint32_t>(i) * 2u + 1u] = static_cast<uint8_t>(v >> 8);
            }
        }
        // Menu strings that embed the symbol tokens: the keyboard's keycap
        // glyph stands for the gameplay meaning, so rewrite the text to the
        // key that works on that screen (ENTER on the map options, ...).
        for (VcsMenuPromptSite &site : g_vcsMenuPromptSites)
        {
            std::string text = site.original;
            if (site.kind == 1)
            {
                // The map zoom pair: one wheel prompt for the keyboard set.
                // The console sets keep the per-word labels ("LB and RB
                // buttons"), so only the keyboard set writes this string.
                if (set == "keyboard")
                    text = "Mouse Wheel";
                else if (!set.empty())
                    continue;
            }
            else if (set == "keyboard")
            {
                auto replaceAll = [](std::string &s, const std::string &from, const std::string &to)
                {
                    bool any = false;
                    for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size()))
                    {
                        s.replace(pos, from.size(), to);
                        any = true;
                    }
                    return any;
                };
                const std::string cross = vcsButtonLabel(VcsBtnCross);
                const std::string circle = vcsButtonLabel(VcsBtnCircle);
                bool changed = false;
                if (!cross.empty())
                    changed |= replaceAll(text, "~X~", cross);
                if (!circle.empty())
                    changed |= replaceAll(text, "~O~", circle);
                if (changed && text.size() > site.original.size())
                {
                    // The rewrite must fit the original allocation; try
                    // shorter phrasings ("...to display map options.").
                    std::string alt = text;
                    replaceAll(alt, " the ", " ");
                    if (alt.size() > site.original.size())
                    {
                        std::string trimmed = alt;
                        while (!trimmed.empty() && trimmed.back() == '.')
                            trimmed.pop_back();
                        alt = std::move(trimmed);
                    }
                    text = alt.size() <= site.original.size() ? alt : site.original;
                }
                else if (!changed)
                {
                    text = site.original;
                }
            }
            for (size_t i = 0; i <= site.original.size(); ++i)
            {
                const uint16_t v = i < text.size() ? static_cast<uint16_t>(static_cast<uint8_t>(text[i])) : 0u;
                rdram[site.addr + static_cast<uint32_t>(i) * 2u] = static_cast<uint8_t>(v & 0xFFu);
                rdram[site.addr + static_cast<uint32_t>(i) * 2u + 1u] = static_cast<uint8_t>(v >> 8);
            }
        }
        g_vcsButtonAppliedSet = set;
        g_vcsButtonAppliedCount = g_vcsButtonSites.size();
        g_vcsMenuPromptAppliedCount = g_vcsMenuPromptSites.size();
        g_vcsMappingAppliedSig = sig;
        if (std::getenv("PS2X_PROMPT_TRACE") != nullptr)
            std::cerr << "[prompts] button-name text -> " << (set.empty() ? "original" : set) << " ("
                      << g_vcsButtonSites.size() << " tokens, " << g_vcsMenuPromptSites.size() << " menu strings)" << std::endl;
    }

    uint32_t vcsRadioEntity(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_vcsFnRadioEntity)
            return 0u;
        R5900Context tmp = *ctx;
        tmp.r[4] = _mm_set_epi64x(0, 0x00514A90); // a0 = the radio/vehicle global
        g_vcsFnRadioEntity(rdram, &tmp, runtime);
        return getRegU32(&tmp, 2);
    }

    void vcsRadioUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t entity = getRegU32(ctx, 5);
        g_vcsOrigRadioUpdate(rdram, ctx, runtime);
        if (entity >= 0x00100000u && entity + 0x2BAu < PS2_RAM_SIZE)
        {
            const uint32_t station = rdram[entity + 0x2BAu];
            if (station != g_vcsRadioStation.load(std::memory_order_relaxed))
            {
                // A station change to a real slot while the runtime radio is off
                // means the player retuned (the push to the silent slot holds at
                // 9, so any departure from 9 is a real change): switch the
                // runtime radio back on.
                if (ps2MusicStationRadioOff() && station != 9u)
                {
                    ps2MusicStationSetRadioOff(false);
                    if (std::getenv("PS2X_RADIO_TRACE") != nullptr)
                        std::cerr << "[radio] station change -> radio on" << std::endl;
                }
                static const bool s_trace = std::getenv("PS2X_RADIO_TRACE") != nullptr;
                if (s_trace)
                {
                    std::ostringstream out;
                    out << "[radio] station=" << station;
                    std::cerr << out.str() << std::endl;
                }
                g_vcsRadioStation.store(station, std::memory_order_relaxed);
            }
        }
    }

    // Runs when the game applies a station change (L1 = next, R1-like = prev).
    // The custom stop is inserted after the last real station:
    //   ... 6 7 8 [Custom Soundtracks] [Radio Off] 0 ...  and the reverse.
    void vcsRadioStationGet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t delta = 0u;
        std::memcpy(&delta, rdram + 0x0048613Cu, sizeof(delta));
        const uint32_t previous = g_vcsRadioStation.load(std::memory_order_relaxed);
        const bool custom = g_vcsCustomStation.load(std::memory_order_relaxed);
        bool wantCustom = false;
        bool override = false;
        if (delta == 1u) // next
        {
            if (previous == 8u && !custom)
            {
                wantCustom = true; // 8 -> Custom
                override = true;
            }
            else if (previous >= 9u && custom)
            {
                override = true; // Custom -> off (the game went 9 -> 0)
            }
        }
        else if (delta == 9u) // previous
        {
            if (previous == 0u)
                override = true; // 0 -> off
            else if (previous >= 9u && !custom)
            {
                wantCustom = true; // off -> Custom
                override = true;
            }
        }
        g_vcsOrigRadioStationGet(rdram, ctx, runtime);
        uint32_t result = getRegU32(ctx, 2) & 0xFFu;
        if (override)
        {
            result = 9u; // the shared silent slot (Radio Off / Custom)
            const uint32_t entity = vcsRadioEntity(rdram, ctx, runtime);
            if (entity >= 0x00100000u && entity + 0x2BAu < PS2_RAM_SIZE)
                rdram[entity + 0x2BAu] = 9u;
        }
        setReturnU32(ctx, result);
        g_vcsRadioStation.store(result, std::memory_order_relaxed);
        if (override)
        {
            g_vcsCustomStation.store(wantCustom, std::memory_order_relaxed);
            g_vcsOffStation.store(!wantCustom, std::memory_order_relaxed);
        }
        else
        {
            g_vcsCustomStation.store(false, std::memory_order_relaxed);
            g_vcsOffStation.store(false, std::memory_order_relaxed);
        }
        static const bool s_trace = std::getenv("PS2X_RADIO_TRACE") != nullptr;
        if (s_trace)
        {
            std::ostringstream out;
            out << "[radio] change prev=" << previous << " delta=" << delta << " -> " << result
                << (wantCustom ? " (custom)" : "") << " [575=" << static_cast<int>(rdram[0x4E4755u])
                << " 576=" << static_cast<int>(rdram[0x4E4756u]) << " 57B=" << static_cast<int>(rdram[0x4E475Bu]) << "]";
            std::cerr << out.str() << std::endl;
        }
    }

    // (FUN_002a69e0 / HUD message 0xB2 was checked as the station pop-up; it is
    // not - suppressing it did not remove the logo. The pop-up is avoided by
    // keeping the station value unchanged on the custom stop instead; see
    // vcsRadioStationGet.)

    // Direct mouse look on foot (follow camera 0x29DAB0, a0 = CCam). The
    // hook converts exactly the mouse motion since the last camera update
    // into the camera's own controls (see below), so there is no stick
    // ramp-up, dead zone, lag or drift.
    PS2Runtime::RecompiledFunction g_vcsOrigFollowPedCam = nullptr;
    PS2Runtime::RecompiledFunction g_vcsOrigFollowCarCam = nullptr;

    void vcsRunCameraWithMouse(PS2Runtime::RecompiledFunction original, uint8_t *rdram, R5900Context *ctx,
                               PS2Runtime *runtime)
    {
        float yaw = 0.0f, pitch = 0.0f;
        const bool captured = runtime->padBackend().consumeMouseLook(yaw, pitch);
        const uint32_t cam = getRegU32(ctx, 4);
        const bool valid = cam >= 0x00100000u && cam + 0x240u <= PS2_RAM_SIZE;
        const bool apply = captured && (yaw != 0.0f || pitch != 0.0f) && valid;
        auto readF = [&](uint32_t off)
        {
            float v;
            std::memcpy(&v, rdram + cam + off, 4);
            return v;
        };
        auto writeF = [&](uint32_t off, float v) { std::memcpy(rdram + cam + off, &v, 4); };
        if (apply)
        {
            // Horizontal: one update turns by ~1.15x the speed (+0x140,
            // positive = left).
            constexpr float kAnglePerSpeed = 1.15f;
            writeF(0x140u, -yaw / kAnglePerSpeed);
            // Vertical: the stick moves a target camera height (+0x228/+0x238)
            // that the camera position (+0x28, copy +0x98) eases towards,
            // which lags. Moving both by the same amount makes it immediate;
            // looking up by a small angle lowers the camera by about its
            // horizontal distance to the target times the angle.
            uint32_t target = 0;
            std::memcpy(&target, rdram + cam + 0x70u, 4);
            if (pitch != 0.0f && target >= 0x00100000u && target + 0x40u <= PS2_RAM_SIZE)
            {
                float centre[3];
                std::memcpy(centre, rdram + target + 0x30u, 12);
                const float dx = readF(0x20u) - centre[0], dy = readF(0x24u) - centre[1];
                const float distance = std::sqrt(dx * dx + dy * dy);
                const float dz = -pitch * distance;
                if (std::isfinite(dz))
                    for (uint32_t off : {0x28u, 0x98u, 0x228u, 0x238u})
                        writeF(off, readF(off) + dz);
            }
        }
        float before[2] = {}; // alpha, beta (+0x78, +0x7C)
        static const bool s_debug = std::getenv("PS2X_PAD_DEBUG") != nullptr;
        if (valid)
            std::memcpy(before, rdram + cam + 0x78u, 8);
        original(rdram, ctx, runtime);
        if (!valid)
            return;
        if (apply)
        {
            writeF(0x140u, 0.0f);
            // Some updates ignore the turn speed altogether (about one in
            // 15-20, more while running); carrying the motion to the next one
            // made the view stop, then jump. Turn the result instead: the
            // camera position (+0x20, copy +0x90), its eased targets
            // (+0x220, +0x230) and direction (+0x10) about the player, and
            // the angle beta (+0x7C, increasing = counter-clockwise).
            const float want = -yaw, got = std::remainder(readF(0x7Cu) - before[1], 6.2831853f);
            uint32_t target = 0;
            std::memcpy(&target, rdram + cam + 0x70u, 4);
            if (want != 0.0f && std::fabs(got) < 0.25f * std::fabs(want) && target >= 0x00100000u &&
                target + 0x40u <= PS2_RAM_SIZE)
            {
                const float missing = want - got;
                const float c = std::cos(missing), sn = std::sin(missing);
                float centre[2];
                std::memcpy(centre, rdram + target + 0x30u, 8);
                for (uint32_t off : {0x20u, 0x90u, 0x220u, 0x230u})
                {
                    const float x = readF(off) - centre[0], y = readF(off + 4u) - centre[1];
                    writeF(off, centre[0] + x * c - y * sn);
                    writeF(off + 4u, centre[1] + x * sn + y * c);
                }
                const float fx = readF(0x10u), fy = readF(0x14u);
                writeF(0x10u, fx * c - fy * sn);
                writeF(0x14u, fx * sn + fy * c);
                writeF(0x7Cu, std::remainder(readF(0x7Cu) + missing, 6.2831853f));
            }
        }
        if (apply && ps2HostSettings().mouseTurnsPlayer && runtime->padBackend().movingInput())
        {
            // Movement is camera-relative: a fast mouse turn swings the
            // wanted heading away from the player's and the game answers a
            // big difference with a skid stop. Turning the player
            // (m_fRotationCur / m_fRotationDest, ped + 0x8E0 / 0x8E4) by the
            // camera's turn keeps the difference; a "back" press still
            // flips the wanted heading and skids. Heading = beta + 90 deg.
            float after[2];
            std::memcpy(after, rdram + cam + 0x78u, 8);
            const float turn = std::remainder(after[1] - before[1], 6.2831853f);
            uint32_t ped = 0;
            std::memcpy(&ped, rdram + cam + 0x70u, 4);
            if (std::isfinite(turn) && turn != 0.0f && ped >= 0x00100000u && ped + 0x8E8u <= PS2_RAM_SIZE)
                for (uint32_t off : {0x8E0u, 0x8E4u})
                {
                    float heading;
                    std::memcpy(&heading, rdram + ped + off, 4);
                    if (std::isfinite(heading))
                    {
                        heading = std::remainder(heading + turn, 6.2831853f);
                        std::memcpy(rdram + ped + off, &heading, 4);
                    }
                }
        }
        if (s_debug)
        {
            float after[2];
            std::memcpy(after, rdram + cam + 0x78u, 8);
            uint32_t ped = 0;
            std::memcpy(&ped, rdram + cam + 0x70u, 4);
            float pos[3] = {}, heading = 0.0f, headingDest = 0.0f;
            uint32_t moveState = 0;
            if (ped >= 0x00100000u && ped + 0x8E8u <= PS2_RAM_SIZE)
            {
                std::memcpy(pos, rdram + ped + 0x30u, 12);
                std::memcpy(&heading, rdram + ped + 0x8E0u, 4);
                std::memcpy(&headingDest, rdram + ped + 0x8E4u, 4);
                std::memcpy(&moveState, rdram + ped + 0x8D4u, 4); // CPed move state: 1 still, 4 run, 5 sprint
            }
            std::fprintf(stderr, "[mouselook] f=%llu want yaw %.4f pitch %.4f got beta %.4f alpha %.4f (alpha %.4f) ped %.3f %.3f heading %.3f dest %.3f state %u moving %d\n",
                         (unsigned long long)g_ps2PresentedFrameCounter.load(), yaw, pitch, std::remainder(after[1] - before[1], 6.2831853f),
                         after[0] - before[0], after[0], pos[0], pos[1], heading, headingDest, moveState, runtime->padBackend().movingInput() ? 1 : 0);
        }
    }

    // Free aim (modern controls; the pad withholds the lock-on button while
    // the aim is held, see PSPadBackend::setFreeAimProbe). While aiming:
    //  - the player turns to face where the camera looks (vcsFreeAimPedCam);
    //  - the camera slides over the right shoulder, so the screen centre is
    //    not the player (vcsFreeAimOffsetCam, undone before the next update
    //    like the vehicle look offset);
    //  - the crosshair ray (camera through screen centre) is cast once per
    //    camera update after the game's own processing (vcsFreeAimMeasure);
    //  - the player's instant-hit shots are turned from the ped's facing to
    //    that point (vcsLineOfSightHook): spread (the shotgun's pellets) is
    //    kept, and the shot can go up or down, which the PS2 game never did
    //    without a target.
    struct VcsFreeAim
    {
        bool held = false;        // aiming this camera update
        float blend = 0.0f;       // shoulder offset, eased in and out (0..1)
        uint32_t pedCam = 0;      // the on-foot CCam of the last update
        bool aimValid = false;
        float aimPoint[3] = {};   // where the crosshair ray meets the world (or its far end)
        uint64_t aimFrame = 0;
        // The shoulder offset's undo: the camera position (+0x20, copy +0x50)
        // before and after the offset. The on-foot camera eases its position
        // from the last one, so an offset left in place would pile up every
        // update; each field goes back if it still holds what we wrote.
        uint32_t offsetCam = 0;
        float original[4][3] = {};
        float written[4][3] = {};
    };
    VcsFreeAim g_vcsFreeAim;

    void vcsFreeAimUndoOffset(uint8_t *rdram)
    {
        VcsFreeAim &aim = g_vcsFreeAim;
        if (aim.offsetCam == 0u)
            return;
        const uint32_t fields[4] = {0x10u, 0x20u, 0x50u, 0x60u}; // Front, Source, its copy, Up
        for (int f = 0; f < 4; ++f)
            if (std::memcmp(rdram + aim.offsetCam + fields[f], aim.written[f], 12) == 0)
                std::memcpy(rdram + aim.offsetCam + fields[f], aim.original[f], 12);
        aim.offsetCam = 0u;
    }

    void vcsFreeAimPedCam(uint8_t *rdram, PS2Runtime *runtime, uint32_t cam)
    {
        VcsFreeAim &aim = g_vcsFreeAim;
        aim.held = ps2HostSettings().freeAim && runtime->padBackend().freeAimHeld() && vcsFreeAimWeapon(rdram);
        aim.blend = std::clamp(aim.blend + (aim.held ? 0.25f : -0.25f), 0.0f, 1.0f);
        aim.pedCam = cam;
        g_ps2Crosshair.store(aim.held, std::memory_order_relaxed);
        static const bool s_log = std::getenv("PS2X_FREE_AIM_LOG") != nullptr;
        if (s_log)
        {
            static bool s_lastHeld = false;
            if (aim.held != s_lastHeld)
                std::fprintf(stderr, "[freeaim] f=%llu held=%d (pad %d, weapon %d)\n",
                             (unsigned long long)g_ps2PresentedFrameCounter.load(std::memory_order_relaxed), aim.held ? 1 : 0,
                             runtime->padBackend().freeAimHeld() ? 1 : 0, vcsFreeAimWeapon(rdram) ? 1 : 0);
            s_lastHeld = aim.held;
        }
        if (!aim.held || cam < 0x00100000u || cam + 0x80u > PS2_RAM_SIZE)
            return;
        // Standing: face the camera. Moving: the game turns the ped towards
        // the stick as usual (it has no strafe outside its lock-on, and
        // overriding the heading there just fought the turn); shots still
        // follow the crosshair within 90 degrees of where the ped faces.
        if (runtime->padBackend().movingInput())
            return;
        uint32_t ped = 0;
        std::memcpy(&ped, rdram + cam + 0x70u, 4);
        if (ped == 0u || ped != vcsPlayerPed(rdram))
            return;
        // Face the camera's direction: GTA headings point along
        // (-sin h, cos h), current and wanted (+0x8E0, +0x8E4).
        float front[3];
        std::memcpy(front, rdram + cam + 0x10u, 12);
        if (!(std::fabs(front[0]) + std::fabs(front[1]) > 1e-4f))
            return;
        const float heading = std::atan2(-front[0], front[1]);
        if (std::isfinite(heading))
            for (uint32_t off : {0x8E0u, 0x8E4u})
                std::memcpy(rdram + ped + off, &heading, 4);
    }

    // PS2X_VCS_PED_DUMP=<every>: dump the player ped (0x1000 bytes) to
    // ped_<frame>_m<camera mode>.bin every that many frames (reverse
    // engineering aid; runs from the per-frame hook, any camera mode).
    void vcsPedDump(uint8_t *rdram)
    {
        {
            static const uint64_t s_every = []()
            {
                const char *env = std::getenv("PS2X_VCS_PED_DUMP");
                return env ? std::strtoull(env, nullptr, 10) : 0ull;
            }();
            static uint64_t s_next = 0;
            const uint64_t frame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
            if (s_every != 0u && frame >= s_next)
            {
                s_next = frame + s_every;
                if (const uint32_t ped = vcsPlayerPed(rdram); ped != 0u && ped + 0x1000u <= PS2_RAM_SIZE)
                {
                    uint16_t mode = 0;
                    std::memcpy(&mode, rdram + 0x006F4540u, 2);
                    char path[64];
                    std::snprintf(path, sizeof(path), "ped_%llu_m%u.bin", (unsigned long long)frame, mode);
                    if (FILE *f = std::fopen(path, "wb"))
                    {
                        std::fwrite(rdram + ped, 1, 0x1000, f);
                        std::fclose(f);
                    }
                }
            }
        }
    }

    void vcsFollowPedCam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Last update's shoulder offset comes off first: the camera logic
        // must build on its own values.
        vcsFreeAimUndoOffset(rdram);
        const uint32_t cam = getRegU32(ctx, 4);
        vcsRunCameraWithMouse(g_vcsOrigFollowPedCam, rdram, ctx, runtime);
        vcsFreeAimPedCam(rdram, runtime, cam);
    }

    // Vehicle cameras have no stick look on the PS2, so the mouse adds a view
    // offset instead (like GTA IV/V): only the RenderWare camera is rotated
    // about the vehicle (see vcsFrameUpdate), so game logic never sees it.
    // (Wrapping CCamera::Process is unreliable: thread switches inside it
    // resume past a wrapper.) The offset eases back behind the vehicle once
    // the mouse rests.
    struct VcsCarLook
    {
        float yaw = 0.0f, pitch = 0.0f; // radians; yaw positive = look right
        uint32_t idleUpdates = 0;
        uint32_t vehicleCam = 0;        // CCam of the last vehicle-camera update
        uint64_t vehicleFrame = 0;      // presented frame of that update
    };
    VcsCarLook g_vcsCarLook;

    // CCam vectors (Front +0x10, Source +0x20 / +0x50, Up +0x60) the look
    // offset rotated for this frame's rendering; put back before the next
    // camera update so the game's camera logic never builds on the offset.
    // The camera vectors (Front +0x10, Source +0x20, its copy +0x50, Up
    // +0x60) our rotation replaced, plus what we left there: the game's own
    // camera update must see its own values again, but only if it has not
    // rewritten them itself in the meantime (comparing the written bytes is
    // exact, unlike a frame-count guess which depends on how fast the host
    // presents relative to the game's camera updates).
    struct VcsCamBackup
    {
        uint32_t cam = 0;
        uint8_t original[0x60] = {};
        uint8_t written[0x60] = {};
    };
    VcsCamBackup g_vcsCamBackup[3];

    // The wall result the rotation applies one frame later: the pose the
    // camera should be pulled to (and, when there is no room beside the
    // vehicle, the overhead pose with its Front/Up). Measured on entry to the
    // next frame's rotation, i.e. after the game's own camera processing, so
    // a yield in the raycast costs nothing but the next frame's clamp.
    struct VcsCarWall
    {
        uint32_t cam = 0;
        uint64_t frame = 0;
        bool clamp = false;
        float position[3] = {};
        bool raised = false;
        float front[3] = {};
        float up[3] = {};
        // The position the rotation *would* use without the clamp, refreshed
        // every rotation. The wall ray must aim at this, not at the clamped
        // position: measuring to the clamped one made the ray miss (it is in
        // free space in front of the wall), so the clamp released and jumped
        // back on alternate frames.
        uint32_t desiredCam = 0;
        float desired[3] = {};
    };
    VcsCarWall g_vcsCarWall;

    // pIgnoreEntity (0x4CD184) has to be set around the raycast. A yield
    // inside the guest call skips our restore (EeDispatcherTransfer unwinds
    // this frame), so remember what to put back and re-assert it at the start
    // of the next camera hook - the game's own raycasts would otherwise keep
    // ignoring the player's vehicle.
    uint32_t g_vcsIgnoreSaved = 0u;
    bool g_vcsIgnoreDirty = false;

    void vcsCarLookRecoverIgnore(uint8_t *rdram)
    {
        if (g_vcsIgnoreDirty)
        {
            std::memcpy(rdram + 0x004CD184u, &g_vcsIgnoreSaved, 4);
            g_vcsIgnoreDirty = false;
        }
    }

    void vcsRotateVehicleCam(uint8_t *rdram, uint32_t cam, R5900Context *ctx, PS2Runtime *runtime);
    void vcsMeasureCarWall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t cam);

    void vcsRestoreCamBackups(uint8_t *rdram)
    {
        static FILE *s_log = []() -> FILE *
        {
            const char *path = std::getenv("PS2X_CARLOOK_DEBUG");
            return path ? std::fopen(path, "a") : nullptr;
        }();
        for (VcsCamBackup &backup : g_vcsCamBackup)
        {
            if (backup.cam == 0u)
                continue;
            const bool untouched =
                std::memcmp(rdram + backup.cam + 0x10u, backup.written, sizeof(backup.written)) == 0;
            if (s_log)
                std::fprintf(s_log, "f=%llu restore cam=0x%x %s\n",
                             (unsigned long long)g_ps2PresentedFrameCounter.load(std::memory_order_relaxed), backup.cam,
                             untouched ? "applied" : "skipped (the game rewrote them)");
            if (untouched)
                std::memcpy(rdram + backup.cam + 0x10u, backup.original, sizeof(backup.original));
            backup.cam = 0u;
        }
    }

    void vcsRotate(float v[3], const float axis[3], float angle)
    {
        // Rodrigues rotation about a unit axis.
        const float c = std::cos(angle), sn = std::sin(angle);
        const float dot = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
        const float cross[3] = {axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0]};
        for (int i = 0; i < 3; ++i)
            v[i] = v[i] * c + cross[i] * sn + axis[i] * dot * (1.0f - c);
    }

    void vcsFollowCarCam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        vcsCarLookRecoverIgnore(rdram);
        VcsCarLook &look = g_vcsCarLook;
        static FILE *s_log = []() -> FILE *
        {
            const char *path = std::getenv("PS2X_CARLOOK_DEBUG");
            return path ? std::fopen(path, "a") : nullptr;
        }();
        vcsRestoreCamBackups(rdram);
        const uint64_t now = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        if (now > look.vehicleFrame + 4u)
            look.yaw = look.pitch = 0.0f; // a new vehicle-camera stretch starts straight
        look.vehicleCam = getRegU32(ctx, 4);
        look.vehicleFrame = now;
        float yaw = 0.0f, pitch = 0.0f;
        const bool captured = runtime->padBackend().consumeMouseLook(yaw, pitch);
        if (captured && (yaw != 0.0f || pitch != 0.0f))
        {
            look.yaw = std::remainder(look.yaw + yaw, 6.2831853f);
            look.pitch = std::clamp(look.pitch + pitch, -0.6f, 0.9f);
            look.idleUpdates = 0;
        }
        else if (++look.idleUpdates > 45u) // ~1.5 s at the game's 30 Hz
        {
            look.yaw *= 0.9f;
            look.pitch *= 0.9f;
        }
        if (std::fabs(look.yaw) < 1.0e-3f && std::fabs(look.pitch) < 1.0e-3f)
            look.yaw = look.pitch = 0.0f;
        if (s_log)
            std::fprintf(s_log, "f=%llu followCarCam cam=0x%x yaw=%.3f pitch=%.3f\n",
                         (unsigned long long)now, getRegU32(ctx, 4), look.yaw, look.pitch);
        g_vcsOrigFollowCarCam(rdram, ctx, runtime);
    }

    // CCamera::Process calls CCam::Process (0x294540) on the active camera,
    // which runs the vehicle camera and then rewrites Front / Source, and
    // then 0x2A2ED0 on the same camera; on entry to that, the vehicle
    // camera's result is final and TheCamera is not built from it yet.
    PS2Runtime::RecompiledFunction g_vcsOrigAfterCamProcess = nullptr;
    void vcsPushFrameCamera(const uint8_t *rdram); // after the camera reader below

    void vcsFreeAimOffsetCam(uint8_t *rdram, uint32_t cam);
    void vcsFreeAimMeasure(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t cam);

    void vcsAfterCamProcess(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        vcsCarLookRecoverIgnore(rdram);
        const uint32_t cam = getRegU32(ctx, 4);
        static FILE *s_log = []() -> FILE *
        {
            const char *path = std::getenv("PS2X_CARLOOK_DEBUG");
            return path ? std::fopen(path, "a") : nullptr;
        }();
        if (s_log)
            std::fprintf(s_log, "f=%llu afterCamProcess cam=0x%x vehicleCam=0x%x vehicleFrame=%llu\n",
                         (unsigned long long)g_ps2PresentedFrameCounter.load(std::memory_order_relaxed), cam,
                         g_vcsCarLook.vehicleCam, (unsigned long long)g_vcsCarLook.vehicleFrame);
        const bool rotated = cam == g_vcsCarLook.vehicleCam &&
                             g_ps2PresentedFrameCounter.load(std::memory_order_relaxed) <= g_vcsCarLook.vehicleFrame + 1u;
        if (rotated)
            vcsRotateVehicleCam(rdram, cam, ctx, runtime);
        const bool aiming = !rotated && cam != 0u && cam == g_vcsFreeAim.pedCam && g_vcsFreeAim.blend > 0.0f;
        if (aiming)
            vcsFreeAimOffsetCam(rdram, cam);
        // The game's own processing builds TheCamera (and the RenderWare
        // camera) from the rotated vectors; only after it may the wall
        // raycast run (a guest call that can yield and unwind this frame).
        g_vcsOrigAfterCamProcess(rdram, ctx, runtime);
        ps2InterpEvent('P');
        // The camera this frame will be drawn with, stamped into the game's
        // VIF1 stream order so the display copy can find it (interpolation).
        vcsPushFrameCamera(rdram);
        if (rotated)
            vcsMeasureCarWall(rdram, ctx, runtime, cam);
        if (aiming && g_vcsFreeAim.held)
            vcsFreeAimMeasure(rdram, ctx, runtime, cam);
        else if (!g_vcsFreeAim.held)
            g_vcsFreeAim.aimValid = false;
    }

    // The main RenderWare camera and the FOV state (see vcsSetViewWindow).
    struct VcsFov
    {
        uint32_t camera = 0;
        float gameX = 0.0f, gameY = 0.0f; // the game's last request
        float appliedFov = 70.0f;
    };
    VcsFov g_vcsFov;

    // The last view window the game asked for, kept for the per-frame camera
    // publish: the window changes only when the FOV does, so the camera is read
    // every frame but the window only when it is re-issued.
    float g_vcsViewWindowX = 0.0f;
    float g_vcsViewWindowY = 0.0f;

    // Applies the look offset to the vehicle camera's result (CCam Front
    // +0x10, Source +0x20 and copy +0x50, Up +0x60), as the game's own look
    // left / right does: TheCamera, the RenderWare camera and everything
    // derived from them (culling, sky, water) follow consistently. (Rotating
    // only the RenderWare frame left the sky projecting its horizon with
    // the unrotated camera: grey sky, missing world.) The unrotated result is
    // put back before the next update, so the camera logic never builds on
    // the offset.
    // CWorld::ProcessLineOfSight(p1, p2, colPoint&, entity*&, buildings,
    // vehicles, peds, objects, dummies, ignoreSeeThrough, ignoreSomeObjects,
    // ...) (0x1908C0, as the vehicle camera calls it; ignores the entity at
    // CWorld::pIgnoreEntity, 0x4CD184). Called from a hook on the game
    // thread with a scratch stack frame; true and the hit point when blocked.
    PS2Runtime::RecompiledFunction g_vcsOrigLineOfSight = nullptr; // the game's, under vcsLineOfSightHook

    bool vcsLineOfSightBlocked(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const float from[3], const float to[3],
                               uint32_t ignoreEntity, float hit[3], bool peds = false)
    {
        // The original, not our hook (installed over 0x1908C0 at startup).
        PS2Runtime::RecompiledFunction s_lineOfSight =
            g_vcsOrigLineOfSight ? g_vcsOrigLineOfSight : runtime->lookupFunction(0x001908C0u);
        if (!s_lineOfSight)
            return false;
        const R5900Context saved = *ctx;
        const uint32_t sp = (getRegU32(ctx, 29) - 0x100u) & ~0xFu;
        uint32_t oldIgnore = 0;
        std::memcpy(&oldIgnore, rdram + 0x004CD184u, 4);
        std::memcpy(rdram + 0x004CD184u, &ignoreEntity, 4);
        g_vcsIgnoreSaved = oldIgnore;
        g_vcsIgnoreDirty = true;
        std::memset(rdram + sp, 0, 0xC0u);
        std::memcpy(rdram + sp + 0x40u, from, 12);
        std::memcpy(rdram + sp + 0x50u, to, 12);
        const uint32_t one = 1u;
        std::memcpy(rdram + sp + 0x08u, &one, 4); // ignoreSeeThrough
        std::memcpy(rdram + sp + 0x10u, &one, 4); // ignoreSomeObjects
        SET_GPR_U32(ctx, 29, sp);
        SET_GPR_U32(ctx, 4, sp + 0x40u);
        SET_GPR_U32(ctx, 5, sp + 0x50u);
        SET_GPR_U32(ctx, 6, sp + 0x60u); // CColPoint
        SET_GPR_U32(ctx, 7, sp + 0xA0u); // CEntity *
        SET_GPR_U32(ctx, 8, 1u);         // buildings
        SET_GPR_U32(ctx, 9, 1u);         // vehicles
        SET_GPR_U32(ctx, 10, peds ? 1u : 0u); // peds
        SET_GPR_U32(ctx, 11, 1u);        // objects
        s_lineOfSight(rdram, ctx, runtime);
        const bool blocked = (getRegU32(ctx, 2) & 0xFFu) != 0u;
        if (blocked)
            std::memcpy(hit, rdram + sp + 0x60u, 12);
        std::memcpy(rdram + 0x004CD184u, &oldIgnore, 4);
        g_vcsIgnoreDirty = false;
        *ctx = saved;
        return blocked && std::isfinite(hit[0] + hit[1] + hit[2]);
    }

    void vcsRotateVehicleCam(uint8_t *rdram, uint32_t cam, R5900Context *ctx, PS2Runtime *runtime)
    {
        VcsCarLook &look = g_vcsCarLook;
        static FILE *s_log = []() -> FILE *
        {
            const char *path = std::getenv("PS2X_CARLOOK_DEBUG");
            return path ? std::fopen(path, "w") : nullptr;
        }();
        const uint64_t frame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        if (look.yaw == 0.0f && look.pitch == 0.0f)
        {
            if (s_log)
                std::fprintf(s_log, "f=%llu rotate skipped (no offset)\n", (unsigned long long)frame);
            return;
        }
        uint32_t target = 0;
        if (cam < 0x00100000u || cam + 0x80u > PS2_RAM_SIZE)
            return;
        std::memcpy(&target, rdram + cam + 0x70u, 4);
        if (target < 0x00100000u || target + 0x40u > PS2_RAM_SIZE)
            return;
        float *c = reinterpret_cast<float *>(rdram + cam);
        float pivot[3];
        std::memcpy(pivot, rdram + target + 0x30u, 12);
        const float *front = c + 4, *source = c + 8;
        const float offset0[3] = {source[0] - pivot[0], source[1] - pivot[1], source[2] - pivot[2]};
        float pitchAxis[3] = {front[1], -front[0], 0.0f}; // horizontal, to the right of Front
        const float axisLength = std::sqrt(pitchAxis[0] * pitchAxis[0] + pitchAxis[1] * pitchAxis[1]);
        if (!std::isfinite(axisLength) || axisLength < 1.0e-4f || !std::isfinite(offset0[0] + offset0[1] + offset0[2]))
            return;
        pitchAxis[0] /= axisLength;
        pitchAxis[1] /= axisLength;
        const float zAxis[3] = {0.0f, 0.0f, 1.0f};
        // Pitch about the horizontal right axis, then yaw about Z (Z up:
        // looking right is clockwise seen from above).
        auto rotate = [&](float v[3], float pitch)
        {
            vcsRotate(v, pitchAxis, pitch);
            vcsRotate(v, zAxis, -look.yaw);
        };

        // Looking up swings the camera down around the vehicle: stop it at
        // the ground (taken as a little below the vehicle's centre).
        // The entity's origin sits on the road for a vehicle, so the floor
        // must be *above* it: 0.3 below put the camera under the road (the
        // green plane seen from underneath when looking up near the ground).
        constexpr float kGroundAbovePivot = 0.25f;
        auto cameraZ = [&](float pitch)
        {
            float o[3] = {offset0[0], offset0[1], offset0[2]};
            rotate(o, pitch);
            return pivot[2] + o[2];
        };
        const float floorZ = pivot[2] + kGroundAbovePivot;
        if (look.pitch > 0.0f && cameraZ(look.pitch) < floorZ)
        {
            float lo = 0.0f, hi = look.pitch;
            if (cameraZ(0.0f) < floorZ)
                hi = 0.0f;
            for (int iteration = 0; iteration < 16 && hi > 0.0f; ++iteration)
            {
                const float mid = 0.5f * (lo + hi);
                (cameraZ(mid) < floorZ ? hi : lo) = mid;
            }
            look.pitch = lo; // further "up" movement stops here instead of piling up
        }

        VcsCamBackup &backup = g_vcsCamBackup[0];
        backup.cam = cam;
        std::memcpy(backup.original, rdram + cam + 0x10u, sizeof(backup.original));
        for (uint32_t off : {0x10u, 0x60u}) // Front, Up
        {
            float *v = c + off / 4u;
            float r[3] = {v[0], v[1], v[2]};
            rotate(r, look.pitch);
            std::memcpy(v, r, sizeof(r));
        }
        for (uint32_t off : {0x20u, 0x50u}) // Source and its copy: about the pivot
        {
            float *v = c + off / 4u;
            float o[3] = {v[0] - pivot[0], v[1] - pivot[1], v[2] - pivot[2]};
            rotate(o, look.pitch);
            for (int i = 0; i < 3; ++i)
                v[i] = pivot[i] + o[i];
        }

        std::memcpy(g_vcsCarWall.desired, c + 8, sizeof(g_vcsCarWall.desired));
        g_vcsCarWall.desiredCam = cam;
        // The game's own wall check ran before the offset: swinging the
        // camera around could put it inside a wall. The correction for that
        // is measured *after* the game's camera processing - see
        // vcsMeasureCarWall - and applied here one frame later. It must not
        // be measured here: the raycast is a guest call, and one that yields
        // (the scheduler's EeDispatcherTransfer) unwinds this host frame,
        // which skipped the game's own 0x2A2ED0 body below it and left the
        // camera state culled (the vehicle "world flickering" bug).
        // PS2X_CARLOOK_WALLS=0 turns the whole wall stop off.
        static const bool s_wallCheck = []()
        {
            const char *env = std::getenv("PS2X_CARLOOK_WALLS");
            return env == nullptr || std::atoi(env) != 0;
        }();
        if (s_wallCheck && g_vcsCarWall.clamp && g_vcsCarWall.cam == cam)
        {
            for (uint32_t off : {0x20u, 0x50u})
                std::memcpy(c + off / 4u, g_vcsCarWall.position, sizeof(g_vcsCarWall.position));
            if (g_vcsCarWall.raised)
            {
                std::memcpy(c + 4, g_vcsCarWall.front, sizeof(g_vcsCarWall.front));
                std::memcpy(c + 0x60 / 4u, g_vcsCarWall.up, sizeof(g_vcsCarWall.up));
            }
        }
        // What we leave behind, so the next camera update can tell whether the
        // game has rewritten these fields itself.
        std::memcpy(backup.written, rdram + cam + 0x10u, sizeof(backup.written));
        if (s_log)
            std::fprintf(s_log, "f=%llu rotate cam=0x%x yaw=%.3f pitch=%.3f clamp=%d pos=(%.2f,%.2f,%.2f)\n",
                         (unsigned long long)frame, cam, look.yaw, look.pitch,
                         (s_wallCheck && g_vcsCarWall.clamp && g_vcsCarWall.cam == cam) ? 1 : 0, c[8], c[9], c[10]);
    }

    void vcsMeasureCarWall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t cam)
    {
        VcsCarLook &look = g_vcsCarLook;
        VcsCarWall &wall = g_vcsCarWall;
        // The previous clamp stands until this measurement completes: the
        // raycast below is a guest call that can yield (EeDispatcherTransfer
        // unwinds this frame), and clearing first made the clamp alternate
        // on and off frame by frame - the camera popped in and out of the
        // wall while looking sideways (the vehicle flicker).
        wall.cam = cam;
        wall.frame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        static const bool s_wallCheck = []()
        {
            const char *env = std::getenv("PS2X_CARLOOK_WALLS");
            return env == nullptr || std::atoi(env) != 0;
        }();
        static FILE *s_log = []() -> FILE *
        {
            const char *path = std::getenv("PS2X_CARLOOK_DEBUG");
            return path ? std::fopen(path, "a") : nullptr;
        }();
        if (!s_wallCheck || (look.yaw == 0.0f && look.pitch == 0.0f))
        {
            wall.clamp = false;
            wall.raised = false;
            return;
        }
        if (cam < 0x00100000u || cam + 0x80u > PS2_RAM_SIZE)
            return;
        uint32_t target = 0;
        std::memcpy(&target, rdram + cam + 0x70u, 4);
        if (target < 0x00100000u || target + 0x40u > PS2_RAM_SIZE)
            return;
        float *c = reinterpret_cast<float *>(rdram + cam);
        float pivot[3];
        std::memcpy(pivot, rdram + target + 0x30u, 12);
        float hit[3];
        const float *camera = (wall.desiredCam == cam) ? wall.desired : c + 8;
        if (s_log)
            std::fprintf(s_log, "f=%llu measure desired=(%.2f,%.2f,%.2f) rendered=(%.2f,%.2f,%.2f) pivot=(%.2f,%.2f,%.2f)\n",
                         (unsigned long long)wall.frame, camera[0], camera[1], camera[2], c[8], c[9], c[10], pivot[0],
                         pivot[1], pivot[2]);
        if (!vcsLineOfSightBlocked(rdram, ctx, runtime, pivot, camera, target, hit))
        {
            wall.clamp = false; // measured clear
            wall.raised = false;
            return;
        }
        if (s_log)
            std::fprintf(s_log, "f=%llu LOS blocked hit=(%.2f,%.2f,%.2f)\n",
                         (unsigned long long)wall.frame, hit[0], hit[1], hit[2]);
        // A hit behind the vehicle, or far past the camera, is not a wall the
        // camera should be pulled in front of: ignore it rather than move the
        // camera somewhere arbitrary.
        const float toCam[3] = {camera[0] - pivot[0], camera[1] - pivot[1], camera[2] - pivot[2]};
        const float toHit[3] = {hit[0] - pivot[0], hit[1] - pivot[1], hit[2] - pivot[2]};
        const float camLen2 = toCam[0] * toCam[0] + toCam[1] * toCam[1] + toCam[2] * toCam[2];
        const float along = toCam[0] * toHit[0] + toCam[1] * toHit[1] + toCam[2] * toHit[2];
        if (camLen2 <= 1.0e-6f || along < 0.0f || along > camLen2 * 1.25f)
        {
            wall.clamp = false;
            wall.raised = false;
            return;
        }
        float toPivot[3] = {pivot[0] - hit[0], pivot[1] - hit[1], pivot[2] - hit[2]};
        const float length = std::sqrt(toPivot[0] * toPivot[0] + toPivot[1] * toPivot[1] + toPivot[2] * toPivot[2]);
        constexpr float kWallGap = 0.8f;
        const float pull = length > kWallGap ? kWallGap / length : 0.0f;
        float position[3];
        for (int i = 0; i < 3; ++i)
            position[i] = hit[i] + toPivot[i] * pull;
        // Publish the pull now: the ceiling raycast below can yield, and the
        // clamp must survive that.
        std::memcpy(wall.position, position, sizeof(position));
        wall.raised = false;
        wall.clamp = true;
        // Too little room (a wall right beside the vehicle): rise above the
        // vehicle and look down at it instead of ending up inside it - unless
        // that is blocked too (a low ceiling).
        constexpr float kMinDistance = 1.6f;
        const float dx = position[0] - pivot[0], dy = position[1] - pivot[1], dz = position[2] - pivot[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance < kMinDistance)
        {
            float raised[3] = {position[0], position[1], position[2] + (kMinDistance - distance) * 1.5f};
            float ceiling[3];
            if (!vcsLineOfSightBlocked(rdram, ctx, runtime, pivot, raised, target, ceiling))
            {
                std::memcpy(position, raised, sizeof(position));
                const float lookAt[3] = {pivot[0], pivot[1], pivot[2] + 0.3f};
                float f[3] = {lookAt[0] - position[0], lookAt[1] - position[1], lookAt[2] - position[2]};
                const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
                if (fl > 1.0e-3f)
                {
                    for (float &x : f)
                        x /= fl;
                    // Up: world Z made perpendicular to Front.
                    float u[3] = {-f[2] * f[0], -f[2] * f[1], 1.0f - f[2] * f[2]};
                    const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                    if (ul > 1.0e-3f)
                    {
                        std::memcpy(wall.front, f, sizeof(f));
                        for (int i = 0; i < 3; ++i)
                            wall.up[i] = u[i] / ul;
                        std::memcpy(wall.position, position, sizeof(position));
                        wall.raised = true;
                    }
                }
            }
        }
    }

    // Field of view: RwCameraSetViewWindow(camera, &viewWindow) (0x18F2D8;
    // view window = tan(FOV / 2) wide, at camera + 0x60) gets the game's
    // request scaled by settings FOV / 70, so zooms keep their relative
    // changes (cutscenes are left as they are). The game only sets it when its FOV changes:
    // the last request is kept to re-apply a new setting.
    PS2Runtime::RecompiledFunction g_vcsOrigSetViewWindow = nullptr;

    // Cutscenes keep the original framing: TheCamera.Cams[0].Mode
    // (0x6F4540) is MODE_FLYBY (17) while one plays (4 = on foot).
    // The game's own widescreen option follows the port's aspect setting
    // (16:9 turns it on, 4:3 / Stretch turn it off), so the owner does not
    // have to set both.
    //
    // The option itself lives at *(0x48F3D0) + 0x30 (32-bit: 0 = 4:3,
    // 1 = 16:9). Writing it is exactly what the in-game Display Options
    // toggle does (menu setter 0x2AF660); everything else follows from it
    // live, every frame:
    //  - the 2D/HUD aspect 0x487488 (1.33333 / 1.77778, forced to 1.66667
    //    while a cutscene plays) by 0x2653F0,
    //  - the 3D camera half-FOV pair at CCam + 0x60 / + 0x64 (tan(FOV/2)
    //    and tan(FOV/2) / aspect) and the renderer aspect 0x742AC4,
    //  - the HUD layout scale (0.85 for 4:3, 0.68 for 16:9),
    //  - the frontend's "Widescreen On/Off" text.
    // The pointer is allocated at boot, so re-read it each frame and skip
    // until it is valid. The display-config bit 0x20 of 0x6F2889 /
    // 0x6F288D / 0x6F2A49 / 0x6F2A4D is only written by the game at
    // toggle time, so the port keeps replicating it.
    // PS2X_WIDESCREEN_FLAG=<hex[,hex]> overrides those addresses for
    // testing; 0 writes nothing.
    void vcsApplyWidescreen(uint8_t *rdram)
    {
        static const std::vector<uint32_t> s_bits = []()
        {
            std::vector<uint32_t> flags;
            if (const char *env = std::getenv("PS2X_WIDESCREEN_FLAG"))
            {
                const std::string text(env);
                size_t start = 0;
                while (start < text.size())
                {
                    const size_t comma = text.find(',', start);
                    const std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                    if (!part.empty())
                        flags.push_back(static_cast<uint32_t>(std::strtoul(part.c_str(), nullptr, 16)));
                    if (comma == std::string::npos)
                        break;
                    start = comma + 1;
                }
                return flags;
            }
            return std::vector<uint32_t>{0x006F2889u, 0x006F288Du, 0x006F2A49u, 0x006F2A4Du};
        }();
        const bool widescreen = ps2HostSettings().aspect == PS2HostSettings::Aspect::Widescreen;

        // The game's own option state. This is the single source everything
        // else is derived from, so writing it is the whole job; the derived
        // values (0x487488, 0x742AC4, the camera FOV pair, the HUD scale)
        // must not be written - the game recomputes them every frame and
        // would fight us (and cutscenes use their own 1.66667).
        uint32_t option = 0;
        std::memcpy(&option, rdram + 0x0048F3D0u, sizeof(option));
        if (option >= 0x00100000u && static_cast<size_t>(option) + 0x34u <= PS2_RAM_SIZE)
        {
            const uint32_t value = widescreen ? 1u : 0u;
            uint32_t current = 0;
            std::memcpy(&current, rdram + option + 0x30u, sizeof(current));
            if (current != value)
            {
                std::memcpy(rdram + option + 0x30u, &value, sizeof(value));
                if (std::getenv("PS2X_WIDESCREEN_LOG") != nullptr)
                    std::cerr << "[ws] game option at 0x" << std::hex << (option + 0x30u) << std::dec
                              << " = " << value << std::endl;
            }
        }
        for (const uint32_t addr : s_bits)
        {
            if (addr < 0x00100000u || static_cast<size_t>(addr) + 1u > PS2_RAM_SIZE)
                continue;
            const uint8_t value =
                widescreen ? static_cast<uint8_t>(rdram[addr] & ~0x20u) : static_cast<uint8_t>(rdram[addr] | 0x20u);
            if (rdram[addr] != value)
                rdram[addr] = value;
        }
        // Nudge the game's own "video settings changed" apply once per
        // setting change (the frontend toggle sets these two flags), so the
        // display state is re-read even if some path only looks on change.
        static int s_lastWidescreen = -1;
        const int wanted = widescreen ? 1 : 0;
        if (wanted != s_lastWidescreen)
        {
            s_lastWidescreen = wanted;
            const uint32_t one = 1u;
            if (static_cast<size_t>(0x00487480u) + 4u <= PS2_RAM_SIZE)
                std::memcpy(rdram + 0x00487480u, &one, 4);
            if (static_cast<size_t>(0x0048A1DCu) + 4u <= PS2_RAM_SIZE)
                std::memcpy(rdram + 0x0048A1DCu, &one, 4);
        }
    }

    bool vcsCutsceneCamera(const uint8_t *rdram)
    {
        uint16_t mode = 0;
        std::memcpy(&mode, rdram + 0x006F4540u, sizeof(mode));
        return mode == 17u;
    }

    float vcsFovSetting(const uint8_t *rdram)
    {
        // Always the game's own value now: the port used to scale the view
        // window to a host FOV slider, but the game has its own swappable FOVs
        // and overriding them only fought them. Kept as the identity so the
        // hook below stays inert rather than being ripped out.
        (void)rdram;
        return 70.0f;
    }

    // The camera, as the reprojection pass needs it: orientation (+0x78/+0x7C),
    // position (+0x20) and the view window (tan(halfFov) wide). Published here
    // because this hook runs every frame with the camera in hand, after the
    // game has updated it.
    Ps2CameraState vcsReadCamera(const uint8_t *rdram, float viewX, float viewY);
    void vcsLogCamera(const Ps2CameraState &state);

    void vcsPublishCamera(const uint8_t *rdram, float viewX, float viewY)
    {
        const Ps2CameraState state = vcsReadCamera(rdram, viewX, viewY);
        g_ps2Camera = state;
        g_ps2CameraFrame.store(g_ps2PresentedFrameCounter.load(std::memory_order_relaxed), std::memory_order_relaxed);
        vcsLogCamera(state);
    }

    void vcsPushFrameCamera(const uint8_t *rdram)
    {
        const Ps2CameraState state = vcsReadCamera(rdram, g_vcsViewWindowX, g_vcsViewWindowY);
        if (state.valid)
            ps2CameraStreamPush(state);
    }

    Ps2CameraState vcsReadCamera(const uint8_t *rdram, float viewX, float viewY)
    {
        // TheCamera.Cams[0]: Front +0x10, Source +0x20, Up +0x60. Fixed address,
        // the same camera the vehicle look rotates, so these are offsets the port
        // already relies on. (The first attempt read +0x78/+0x7C off the
        // RenderWare camera - a change detector reading 2000, not an angle - and
        // a +0x20 that was not a world position; the camera log caught both.)
        constexpr uint32_t kActiveCam = 0x006F4540u;
        Ps2CameraState state;
        std::memcpy(state.front, rdram + kActiveCam + 0x10u, 12);
        std::memcpy(state.pos, rdram + kActiveCam + 0x20u, 12);
        std::memcpy(state.up, rdram + kActiveCam + 0x60u, 12);
        state.viewX = viewX;
        state.viewY = viewY;
        const auto len3 = [](const float v[3])
        { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
        const auto finite3 = [](const float v[3])
        { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); };
        // Before the game builds a camera the CCam is all zeros: a unit -Z front
        // with a zero position. A real camera has a unit basis, is not at the
        // world origin, and has been given a view window (the intro is a cutscene
        // with no main camera at all).
        const auto unit = [&](const float v[3]) { return std::fabs(len3(v) - 1.0f) < 0.05f; };
        state.valid = finite3(state.front) && finite3(state.pos) && finite3(state.up) && unit(state.front) &&
                      unit(state.up) && len3(state.pos) > 0.01f && std::isfinite(viewX) && viewX > 0.0f;
        return state;
    }

    void vcsLogCamera(const Ps2CameraState &state)
    {
        const auto len3 = [](const float v[3])
        { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
        // PS2X_CAMERA_LOG=1: write the camera each frame to cameralog.txt next to
        // the executable. A file rather than stderr because the Windows build
        // has no console, and this is the data the reprojection pass is built
        // on - worth eyeballing (alpha should swing when the view turns, pos
        // when the camera moves) before trusting the warp.
        static const bool s_log = std::getenv("PS2X_CAMERA_LOG") != nullptr;
        if (s_log)
        {
            static FILE *s_file = []()
            {
                const std::filesystem::path path = ps2ExecutableDirectory() / "cameralog.txt";
                return std::fopen(path.string().c_str(), "w");
            }();
            static int s_n = 0;
            if (s_file && (s_n < 30 || (s_n % 30) == 0))
            {
                std::fprintf(s_file,
                             "n=%d frame=%llu front=(%.4f,%.4f,%.4f)|%.3f up=(%.4f,%.4f,%.4f)|%.3f "
                             "pos=(%.1f,%.1f,%.1f)|%.0f view=(%.4f,%.4f) valid=%d\n",
                             s_n, static_cast<unsigned long long>(g_ps2PresentedFrameCounter.load()), state.front[0],
                             state.front[1], state.front[2], len3(state.front), state.up[0], state.up[1], state.up[2],
                             len3(state.up), state.pos[0], state.pos[1], state.pos[2], len3(state.pos), state.viewX, state.viewY,
                             state.valid ? 1 : 0);
            }
            ++s_n;
            if (s_file && (s_n % 30) == 0)
                std::fflush(s_file);
        }
    }

    void vcsSetViewWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t camera = getRegU32(ctx, 4), viewWindow = getRegU32(ctx, 5);
        if (viewWindow >= 0x00100000u && viewWindow + 8u <= PS2_RAM_SIZE)
        {
            float vw[2];
            std::memcpy(vw, rdram + viewWindow, sizeof(vw));
            g_vcsFov.camera = camera;
            g_vcsFov.gameX = vw[0];
            g_vcsFov.gameY = vw[1];
            // Remember the window; the camera itself is published once per
            // presented frame from the present path (publishCameraFrame), so
            // publishing here too would just duplicate samples and race the
            // present thread.
            g_vcsViewWindowX = vw[0];
            g_vcsViewWindowY = vw[1];
            const float setting = vcsFovSetting(rdram);
            g_vcsFov.appliedFov = setting;
            if (setting != 70.0f && vw[0] > 0.0f && std::isfinite(vw[0]) && std::isfinite(vw[1]))
            {
                constexpr float kMaxHalfFov = 1.48f; // ~170 degrees
                const float half = std::min(std::atan(vw[0]) * (setting / 70.0f), kMaxHalfFov);
                const float x = std::tan(half);
                vw[1] *= x / vw[0];
                vw[0] = x;
                std::memcpy(rdram + viewWindow, vw, sizeof(vw));
            }
        }
        g_vcsOrigSetViewWindow(rdram, ctx, runtime);
    }

    // Draw distance (far part): RwCameraSetFarClipPlane(camera, far)
    // (0x18F400, far at camera + 0x7C) on the main camera gets the game's
    // value times the setting; kept for re-applying, like the FOV.
    PS2Runtime::RecompiledFunction g_vcsOrigSetFarClip = nullptr;
    float g_vcsGameFarClip = 0.0f, g_vcsAppliedDrawDistance = 1.0f;

    float vcsDrawDistanceSetting()
    {
        return std::clamp(ps2HostSettings().drawDistance, 0.5f, 4.0f);
    }

    void vcsSetFarClip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t camera = getRegU32(ctx, 4);
        if (camera != 0u && camera == g_vcsFov.camera && std::isfinite(ctx->f[12]) && ctx->f[12] > 0.0f)
        {
            g_vcsGameFarClip = ctx->f[12];
            g_vcsAppliedDrawDistance = vcsDrawDistanceSetting();
            ctx->f[12] = g_vcsGameFarClip * g_vcsAppliedDrawDistance;
        }
        g_vcsOrigSetFarClip(rdram, ctx, runtime);
    }

    // Draw distance (detail part): CCam::Process (0x294540) ends with the
    // LOD distance multiplier, tan(35 deg) / tan(FOV / 2), at CCam + 0x138;
    // for TheCamera.Cams[0] (0x6F4540, the one in use) that is 0x6F4678,
    // read by the renderer after CCamera::Process. Only a freshly computed
    // value is scaled (an unchanged one was ours: it must not compound).
    float g_vcsLodWritten = 0.0f;

    void vcsScaleLodDistance(uint8_t *rdram)
    {
        constexpr uint32_t kLodMultiplier = 0x006F4678u;
        float value = 0.0f;
        std::memcpy(&value, rdram + kLodMultiplier, sizeof(value));
        if (value == g_vcsLodWritten || !std::isfinite(value) || value <= 0.0f || value > 100.0f)
            return;
        value *= vcsDrawDistanceSetting();
        std::memcpy(rdram + kLodMultiplier, &value, sizeof(value));
        g_vcsLodWritten = value;
    }

    // RwFrameUpdateObjects; CCamera::Process calls it (returning to
    // 0x37AE5C) right after copying TheCamera's matrix into the RenderWare
    // camera's frame, so the look offset reaches rendering only.
    PS2Runtime::RecompiledFunction g_vcsOrigFrameUpdate = nullptr;

    void vcsFrameUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        vcsPedDump(rdram);
        // The game's silent slot is our Custom Soundtracks station: keep its GXT
        // name "Custom" (or "Radio Off" while the radio is switched off).
        vcsKeepCustomName(rdram);
        // Button-name text ("L1", "CROSS", ...) follows the prompt set too.
        vcsApplyButtonText(rdram);
        // The game's widescreen option follows the port's aspect setting.
        vcsApplyWidescreen(rdram);
        // Radio Off re-tune: push the game to its silent slot exactly once, on
        // the first frame after the radio switches off, then leave the station
        // alone - the game's own update then holds the silent slot (no station
        // audio, the engine and atmosphere keep playing), and a later station
        // press is free to move away and switch the radio back on.
        static bool s_pushedWhileOff = false;
        if (!ps2MusicStationRadioOff())
            s_pushedWhileOff = false;
        else if (!s_pushedWhileOff)
        {
            s_pushedWhileOff = true;
            rdram[0x004E4756u] = 9u;
            rdram[0x004E4755u] = 9u;
            rdram[0x004E475Bu] = 9u;
            const uint32_t entity = vcsRadioEntity(rdram, ctx, runtime);
            if (entity >= 0x00100000u && entity + 0x2BAu < PS2_RAM_SIZE)
                rdram[entity + 0x2BAu] = 9u;
            if (std::getenv("PS2X_RADIO_TRACE") != nullptr)
                std::cerr << "[radio] pushed to silent slot once" << std::endl;
        }
        if (getRegU32(ctx, 31) == 0x0037AE5Cu)
        {
            vcsScaleLodDistance(rdram);
            // A new FOV setting: re-issue the game's last view window.
            const uint32_t camera = g_vcsFov.camera;
            if (g_vcsOrigSetViewWindow && camera >= 0x00100000u && camera + 0x70u <= PS2_RAM_SIZE &&
                g_vcsFov.appliedFov != vcsFovSetting(rdram))
            {
                const R5900Context saved = *ctx;
                const float vw[2] = {g_vcsFov.gameX, g_vcsFov.gameY};
                std::memcpy(rdram + camera + 0x60u, vw, sizeof(vw));
                SET_GPR_U32(ctx, 4, camera);
                SET_GPR_U32(ctx, 5, camera + 0x60u);
                vcsSetViewWindow(rdram, ctx, runtime);
                *ctx = saved;
            }
            if (g_vcsOrigSetFarClip && camera >= 0x00100000u && g_vcsGameFarClip > 0.0f &&
                g_vcsAppliedDrawDistance != vcsDrawDistanceSetting())
            {
                const R5900Context saved = *ctx;
                SET_GPR_U32(ctx, 4, camera);
                ctx->f[12] = g_vcsGameFarClip;
                vcsSetFarClip(rdram, ctx, runtime);
                *ctx = saved;
            }
        }
        g_vcsOrigFrameUpdate(rdram, ctx, runtime);
    }

    // ---------------------------------------------------------------------
    // Weapon-work test aid.
    // PS2X_VCS_LOS_TRACE=1: who calls CWorld::ProcessLineOfSight (0x1908C0), by
    // return address, with a sample of the segment; reported every ~10 s.
    // Over the right shoulder: Source (+0x20, copy +0x50) moved right, up a
    // little and in, Front unchanged - the crosshair at the screen centre
    // then looks past the player. Undone by vcsRestoreCamBackups before the
    // next camera update.
    void vcsFreeAimOffsetCam(uint8_t *rdram, uint32_t cam)
    {
        if (cam < 0x00100000u || cam + 0x80u > PS2_RAM_SIZE)
            return;
        VcsFreeAim &aim = g_vcsFreeAim;
        vcsFreeAimUndoOffset(rdram); // never stack two
        float *c = reinterpret_cast<float *>(rdram + cam);
        float *front = c + 0x10 / 4, *source = c + 0x20 / 4, *copy = c + 0x50 / 4, *up = c + 0x60 / 4;
        float right[3] = {front[1] * up[2] - front[2] * up[1], front[2] * up[0] - front[0] * up[2],
                          front[0] * up[1] - front[1] * up[0]};
        const float length = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
        if (!(length > 1e-4f) || !std::isfinite(length))
            return;
        for (float &v : right)
            v /= length;
        float *fields[4] = {front, source, copy, up};
        for (int f = 0; f < 4; ++f)
            std::memcpy(aim.original[f], fields[f], 12);
        const float b = aim.blend;
        // Level the view: the follow camera looks down at the player, which
        // put the screen centre on the ground a few metres ahead. Front and
        // Up turn up about the camera's right axis.
        constexpr float kTiltUp = 0.12f; // ~7 degrees
        // About +right a positive angle takes Front towards Up (right x
        // front = up), i.e. tilts the view up.
        vcsRotate(front, right, b * kTiltUp);
        vcsRotate(up, right, b * kTiltUp);
        // Over the right shoulder, a little lower (nearer head height: with
        // the view levelled the player otherwise sinks to the bottom) and in.
        constexpr float kRight = 0.45f, kUp = -0.25f, kIn = 0.35f;
        float offset[3];
        for (int i = 0; i < 3; ++i)
            offset[i] = b * (kRight * right[i] + kUp * up[i] + kIn * front[i]);
        for (float *v : {source, copy})
            for (int i = 0; i < 3; ++i)
                v[i] += offset[i];
        for (int f = 0; f < 4; ++f)
            std::memcpy(aim.written[f], fields[f], 12);
        aim.offsetCam = cam;
    }

    // The crosshair ray, from the (offset) camera through the screen centre,
    // against buildings, vehicles, peds and objects (not the player).
    void vcsFreeAimMeasure(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t cam)
    {
        VcsFreeAim &aim = g_vcsFreeAim;
        float source[3], front[3];
        std::memcpy(source, rdram + cam + 0x20u, 12);
        std::memcpy(front, rdram + cam + 0x10u, 12);
        const float length = std::sqrt(front[0] * front[0] + front[1] * front[1] + front[2] * front[2]);
        if (!(length > 1e-4f) || !std::isfinite(length + source[0] + source[1] + source[2]))
            return;
        constexpr float kReach = 150.0f;
        float far[3], hit[3];
        for (int i = 0; i < 3; ++i)
            far[i] = source[i] + front[i] / length * kReach;
        const bool blocked = vcsLineOfSightBlocked(rdram, ctx, runtime, source, far, vcsPlayerPed(rdram), hit, true);
        std::memcpy(aim.aimPoint, blocked ? hit : far, sizeof(aim.aimPoint));
        aim.aimValid = true;
        aim.aimFrame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
    }

    // Turns a player instant-hit shot (CWeapon::FireInstantHit 0x2C3E48 calls
    // ProcessLineOfSight through the wrapper 0x2D0870, which returns to
    // 0x2D08B8 and keeps its caller's return address at sp+0x30; the shooter
    // stays in s4) by the rotation that takes the ped's facing to the
    // crosshair point. Writes the end point (a1) in place: the impact and the
    // tracer are both built from it.
    void vcsFreeAimShot(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const VcsFreeAim &aim = g_vcsFreeAim;
        static const bool s_log = std::getenv("PS2X_FREE_AIM_LOG") != nullptr;
        auto skip = [&](const char *why)
        {
            if (s_log)
                std::fprintf(stderr, "[freeaim] f=%llu shot left alone: %s\n",
                             (unsigned long long)g_ps2PresentedFrameCounter.load(std::memory_order_relaxed), why);
        };
        if (!aim.held || !aim.aimValid || !runtime->padBackend().freeAimHeld())
            return skip(!aim.held ? "not aiming" : !aim.aimValid ? "no aim point" : "aim released");
        const uint64_t frame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        if (frame > aim.aimFrame + 8u)
            return skip("aim point stale");
        const uint32_t sp = getRegU32(ctx, 29);
        if (sp + 0x34u > PS2_RAM_SIZE)
            return;
        uint32_t outer = 0;
        std::memcpy(&outer, rdram + sp + 0x30u, 4);
        if (outer < 0x002C3E48u || outer >= 0x002C5320u)
            return; // not FireInstantHit
        const uint32_t ped = vcsPlayerPed(rdram);
        if (ped == 0u || getRegU32(ctx, 20) != ped)
            return; // someone else's shot
        const uint32_t p1 = getRegU32(ctx, 4), p2 = getRegU32(ctx, 5);
        if (p1 < 0x1000u || p1 + 12u > PS2_RAM_SIZE || p2 < 0x1000u || p2 + 12u > PS2_RAM_SIZE)
            return;
        float from[3], to[3], facing[3];
        std::memcpy(from, rdram + p1, 12);
        std::memcpy(to, rdram + p2, 12);
        std::memcpy(facing, rdram + ped + 0x10u, 12); // the ped matrix's forward
        auto normalize = [](float v[3])
        {
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (!(l > 1e-5f) || !std::isfinite(l))
                return false;
            for (int i = 0; i < 3; ++i)
                v[i] /= l;
            return true;
        };
        float want[3] = {aim.aimPoint[0] - from[0], aim.aimPoint[1] - from[1], aim.aimPoint[2] - from[2]};
        float shot[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
        if (!normalize(want) || !normalize(facing))
            return;
        const float c = facing[0] * want[0] + facing[1] * want[1] + facing[2] * want[2];
        if (!(c > 0.0f)) // more than 90 degrees off: the ped has not turned yet
            return skip("more than 90 degrees from the ped's facing");
        // Rodrigues with k = facing x want: v' = v c + k x v + k (k.v) / (1 + c).
        const float k[3] = {facing[1] * want[2] - facing[2] * want[1], facing[2] * want[0] - facing[0] * want[2],
                            facing[0] * want[1] - facing[1] * want[0]};
        const float kv = k[0] * shot[0] + k[1] * shot[1] + k[2] * shot[2];
        const float kxv[3] = {k[1] * shot[2] - k[2] * shot[1], k[2] * shot[0] - k[0] * shot[2], k[0] * shot[1] - k[1] * shot[0]};
        float out[3];
        for (int i = 0; i < 3; ++i)
            out[i] = from[i] + shot[i] * c + kxv[i] + k[i] * kv / (1.0f + c);
        if (std::isfinite(out[0] + out[1] + out[2]))
            std::memcpy(rdram + p2, out, 12);
        if (s_log)
            std::fprintf(stderr, "[freeaim] f=%llu shot from=(%.2f,%.2f,%.2f) to=(%.2f,%.2f,%.2f) -> (%.2f,%.2f,%.2f) aim=(%.2f,%.2f,%.2f)\n",
                         (unsigned long long)frame, from[0], from[1], from[2], to[0], to[1], to[2], out[0], out[1], out[2],
                         aim.aimPoint[0], aim.aimPoint[1], aim.aimPoint[2]);
    }

    void vcsLineOfSightTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // CWorld::ProcessLineOfSight (0x1908C0): free aim shots, and the
    // PS2X_VCS_LOS_TRACE caller report.
    void vcsLineOfSightHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 31) == 0x002D08B8u)
            vcsFreeAimShot(rdram, ctx, runtime);
        static const bool s_trace = std::getenv("PS2X_VCS_LOS_TRACE") != nullptr;
        if (s_trace)
            vcsLineOfSightTrace(rdram, ctx, runtime);
        else
            g_vcsOrigLineOfSight(rdram, ctx, runtime);
    }

    void vcsLineOfSightTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        struct Site
        {
            uint64_t calls = 0;
            float from[3] = {}, to[3] = {};
        };
        static std::map<uint32_t, Site> s_sites;
        static uint64_t s_lastReport = 0;
        const uint32_t ra = getRegU32(ctx, 31);
        Site &site = s_sites[ra];
        ++site.calls;
        const uint32_t p1 = getRegU32(ctx, 4), p2 = getRegU32(ctx, 5);
        if (p1 >= 0x1000u && p1 + 12u <= PS2_RAM_SIZE && p2 >= 0x1000u && p2 + 12u <= PS2_RAM_SIZE)
        {
            std::memcpy(site.from, rdram + p1, 12);
            std::memcpy(site.to, rdram + p2, 12);
        }
        const uint64_t frame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        if (frame >= s_lastReport + 600u)
        {
            s_lastReport = frame;
            for (const auto &[addr, entry] : s_sites)
                std::fprintf(stderr, "[los] f=%llu ra=0x%x calls=%llu from=(%.1f,%.1f,%.1f) to=(%.1f,%.1f,%.1f)\n",
                             (unsigned long long)frame, addr, (unsigned long long)entry.calls, entry.from[0], entry.from[1],
                             entry.from[2], entry.to[0], entry.to[1], entry.to[2]);
            s_sites.clear();
        }
        g_vcsOrigLineOfSight(rdram, ctx, runtime);
    }

    // PS2X_CAM_TRACE=1: which CCam CCam::Process (0x294540) runs, and its
    // mode, whenever either changes (reverse engineering aid).
    PS2Runtime::RecompiledFunction g_vcsOrigCamProcess = nullptr;
    void vcsCamProcessTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cam = getRegU32(ctx, 4);
        static uint32_t s_lastCam = 0;
        static uint16_t s_lastMode = 0xFFFFu;
        uint16_t mode = 0;
        if (cam >= 0x00100000u && cam + 2u <= PS2_RAM_SIZE)
            std::memcpy(&mode, rdram + cam, 2);
        if (cam != s_lastCam || mode != s_lastMode)
        {
            s_lastCam = cam;
            s_lastMode = mode;
            std::fprintf(stderr, "[camtrace] f=%llu cam=0x%x mode=%u\n",
                         (unsigned long long)g_ps2PresentedFrameCounter.load(std::memory_order_relaxed), cam, mode);
        }
        g_vcsOrigCamProcess(rdram, ctx, runtime);
    }

    void applyGtaVcsUs(PS2Runtime &runtime)
    {
        if (std::getenv("PS2X_CAM_TRACE") && (g_vcsOrigCamProcess = runtime.lookupFunction(0x00294540u)))
            runtime.replaceFunction(0x00294540u, &vcsCamProcessTrace);
        if ((g_vcsOrigFollowPedCam = runtime.lookupFunction(0x0029DAB0u)))
            runtime.replaceFunction(0x0029DAB0u, &vcsFollowPedCam);
        if ((g_vcsOrigFollowCarCam = runtime.lookupFunction(0x0029FAF0u)))
            runtime.replaceFunction(0x0029FAF0u, &vcsFollowCarCam);
        if ((g_vcsOrigAfterCamProcess = runtime.lookupFunction(0x002A2ED0u)))
            runtime.replaceFunction(0x002A2ED0u, &vcsAfterCamProcess);
        if ((g_vcsOrigFrameUpdate = runtime.lookupFunction(0x001B4688u)))
            runtime.replaceFunction(0x001B4688u, &vcsFrameUpdate);
        if ((g_vcsOrigSetViewWindow = runtime.lookupFunction(0x0018F2D8u)))
            runtime.replaceFunction(0x0018F2D8u, &vcsSetViewWindow);
        if ((g_vcsOrigSetFarClip = runtime.lookupFunction(0x0018F400u)))
            runtime.replaceFunction(0x0018F400u, &vcsSetFarClip);
        if ((g_vcsOrigLineOfSight = runtime.lookupFunction(0x001908C0u)))
            runtime.replaceFunction(0x001908C0u, &vcsLineOfSightHook);
        runtime.padBackend().setFreeAimProbe(&vcsFreeAimWeapon, runtime.memory().getRDRAM());

        runtime.padBackend().setVehicleProbe(&vcsPlayerInVehicle, runtime.memory().getRDRAM());
        runtime.padBackend().setMenuProbe(&vcsPauseMenuOpen, runtime.memory().getRDRAM());
        runtime.padBackend().setCutsceneProbe(&vcsInCutscene, runtime.memory().getRDRAM());
        // Cutscene dialogue stays with the animation (see g_ps2AudioFollowsGameClock).
        g_ps2AudioFollowsGameClock = [](const uint8_t *rdram)
        {
            // In-engine cutscenes only: the FMV movies are paced by their own
            // audio already (vcsInCutscene also counts a recent movie picture).
            uint32_t value = 0;
            std::memcpy(&value, rdram + 0x006F4D14u, 4);
            return value != 0u;
        };
        g_vcsFnRadioEntity = runtime.lookupFunction(0x001B5768u);
        if ((g_vcsOrigRadioUpdate = runtime.lookupFunction(0x0018AAD8u)))
            runtime.replaceFunction(0x0018AAD8u, &vcsRadioUpdate);
        // NOTE: the game's own cycle already runs ... 8 -> the silent slot -> 0,
        // and that silent slot is now our Custom Soundtracks stop (its GXT name is
        // renamed once, see vcsKeepCustomName). Nothing is inserted into the
        // cycle, so every transition is one the game expects.
        // Custom Soundtracks: the music folder plays on the silent slot while in a
        // vehicle, the runtime's radio is not switched off, and the game's own
        // pause menu is closed (0x4CD2AC is set to 1 by the pause open/close
        // handlers at 0x2F4D64/0x2F4D70 on the START press). The music then
        // pauses and resumes with the game, on the same track.
        ps2MusicStationSetQuery([](const uint8_t *rdram)
                                { return g_vcsRadioStation.load(std::memory_order_relaxed) == 9u &&
                                         vcsPlayerInVehicle(rdram) && rdram[0x004CD2ACu] == 0u; });

        g_vcsOrigMovieFinished = runtime.lookupFunction(0x0031A6A8u);
        runtime.replaceFunction(0x0031A6A8u, &vcsMovieFinished);

        runtime.replaceFunction(0x0031A5D0u, &vcsMoviePushEndCode);

        runtime.replaceFunction(0x00448D70u, &vcsGifPkAddGsAD);
        runtime.replaceFunction(0x00448CC8u, &vcsGifPkCloseGifTag);

        // libcdvd sceCdLayerSearchFile/sceCdSearchFile (one function in this
        // build). Natively it binds the CDVDFSV search RPC (sid 0x80000597),
        // which no IOP module provides, so the game spins in sceSifBindRpc.
        ps2_game_overrides::bindAddressHandler(runtime, 0x0044E180u, "sceCdSearchFile");

        // libmpeg sceMpegIsEnd (`return mpeg->sys->active == 0`): the HLE decoder never
        // updates that internal field, so route it to the HLE end-of-stream check.
        ps2_game_overrides::bindAddressHandler(runtime, 0x00452F68u, "sceMpegIsEnd");
        // The movie player stops taking audio once the file has been read, so
        // demux must not run ahead of the soundtrack (logo / intro audio tails).
        ps2_stubs::setMpegAudioThrottle(16u);

        vcsInstallCallCounters(runtime);
    }
}

namespace ps2_game_overrides
{
    // Called from the present path, once per presented frame. The anonymous
    // namespace above is at file scope, so this needs its own namespace block to
    // match the declaration the runtime sees.
    void publishCameraFrame(const uint8_t *rdram)
    {
        vcsPublishCamera(rdram, g_vcsViewWindowX, g_vcsViewWindowY);
    }

    bool readCamera(const uint8_t *rdram, Ps2CameraState &out)
    {
        out = vcsReadCamera(rdram, g_vcsViewWindowX, g_vcsViewWindowY);
        return out.valid;
    }

    // The cutscene flag, reachable from the runtime: the definition above is in
    // this file's anonymous namespace.
    bool isInCutscene(const uint8_t *rdram)
    {
        return vcsInCutscene(rdram);
    }
}

PS2_REGISTER_GAME_OVERRIDE("GTA Vice City Stories (SLUS_215.90)", "SLUS_215.90", 0x001DD6A0u, 0u, &applyGtaVcsUs)
