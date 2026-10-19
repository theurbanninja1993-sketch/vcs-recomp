#include "game_overrides.h"
#include "runtime/ps2_host_settings.h"
#include "runtime/ps2_music_station.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_runtime_calls.h"
#include "ps2_stubs.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_syscalls.h"
#include "ps2_log.h"
#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <map>
#include <optional>
#include <vector>

extern std::atomic<uint64_t> g_ps2PresentedFrameCounter;
extern std::atomic<bool> g_ps2Crosshair;

namespace ps2_game_overrides::detail
{
    bool lcsPlayerInVehicle(const uint8_t *rdram)
    {
        if (!rdram)
            return false;
        // SLUS_214.23: 0x00107368 returns Players[PlayerInFocus].ped;
        // 0x00107278 returns ped->vehicle only while ped+0x538 is nonzero.
        // The original helpers use a 0x190-byte player-info stride. Addresses
        // and offsets stay in this identity-scoped module, not the pad backend.
        constexpr uint32_t playerInFocus = 0x003D9BC8u;
        constexpr uint32_t players = 0x00408E40u;
        const uint32_t index = rdram[playerInFocus];
        if (index >= 2u)
            return false;
        auto read32 = [rdram](uint32_t address)
        {
            uint32_t value = 0u;
            std::memcpy(&value, rdram + address, sizeof(value));
            return value;
        };
        const uint32_t ped = read32(players + index * 0x190u);
        if (ped < 0x00100000u || ped > PS2_RAM_SIZE - 0x53Cu || (ped & 3u) != 0u)
            return false;
        if (read32(ped + 0x538u) == 0u)
            return false;
        const uint32_t vehicle = read32(ped + 0x534u);
        return vehicle >= 0x00100000u && vehicle <= PS2_RAM_SIZE - 4u && (vehicle & 3u) == 0u;
    }
}

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
    // once the HLE stream itself has ended; the undelivered tail frames are dropped.
    PS2Runtime::RecompiledFunction g_vcsOrigMovieFinished = nullptr;
    void vcsMovieFinished(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t player = getRegU32(ctx, 4);
        g_vcsOrigMovieFinished(rdram, ctx, runtime);
        if (getRegU32(ctx, 2) == 0u && ps2_stubs::isMpegStreamFinished(player))
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
    bool vcsPlayerInVehicle(const uint8_t *rdram)
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
                return true;
        }
        return false;
    }

    // Direct mouse look on foot (follow camera 0x29DAB0, a0 = CCam). The
    // hook converts exactly the mouse motion since the last camera update
    // into the camera's own controls (see below), so there is no stick
    // ramp-up, dead zone, lag or drift.
    PS2Runtime::RecompiledFunction g_vcsOrigFollowPedCam = nullptr;
    PS2Runtime::RecompiledFunction g_vcsOrigFollowCarCam = nullptr;

    void vcsRunCameraWithMouse(PS2Runtime::RecompiledFunction original, uint8_t *rdram, R5900Context *ctx,
                               PS2Runtime *runtime)
    {
        // Mouse motion an update did not use (some camera calls change nothing).
        static float s_carryYaw = 0.0f, s_carryPitch = 0.0f;
        float yaw = 0.0f, pitch = 0.0f;
        const bool captured = runtime->padBackend().consumeMouseLook(yaw, pitch);
        yaw += s_carryYaw;
        pitch += s_carryPitch;
        s_carryYaw = s_carryPitch = 0.0f;
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
        float frontBefore[3] = {};
        if (valid)
            std::memcpy(frontBefore, rdram + cam + 0x10u, 12);
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
        float before[2] = {};
        static const bool s_debug = std::getenv("PS2X_PAD_DEBUG") != nullptr;
        if (s_debug && valid)
            std::memcpy(before, rdram + cam + 0x78u, 8);
        original(rdram, ctx, runtime);
        if (!valid)
            return;
        if (s_debug)
        {
            float after[2];
            std::memcpy(after, rdram + cam + 0x78u, 8);
            std::fprintf(stderr, "[mouselook] f=%llu want yaw %.4f pitch %.4f got beta %.4f alpha %.4f (alpha %.4f)\n",
                         (unsigned long long)g_ps2PresentedFrameCounter.load(), yaw, pitch, std::remainder(after[1] - before[1], 6.2831853f),
                         after[0] - before[0], after[0]);
        }
        if (apply)
        {
            writeF(0x140u, 0.0f);
            // A call that left the camera untouched did not turn it either.
            if (std::memcmp(frontBefore, rdram + cam + 0x10u, 12) == 0)
            {
                s_carryYaw = yaw;
                s_carryPitch = pitch;
            }
        }
    }

    void vcsFollowPedCam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        vcsRunCameraWithMouse(g_vcsOrigFollowPedCam, rdram, ctx, runtime);
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
        VcsCarLook &look = g_vcsCarLook;
        look.vehicleCam = getRegU32(ctx, 4);
        look.vehicleFrame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
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
        g_vcsOrigFollowCarCam(rdram, ctx, runtime);
    }

    // Rotates the RenderWare camera frame's modelling matrix (right +0x10,
    // up +0x20, at +0x30, pos +0x40) by the vehicle look offset.
    void vcsApplyCarLook(uint8_t *rdram, uint32_t frame)
    {
        VcsCarLook &look = g_vcsCarLook;
        const uint64_t now = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        if (now > look.vehicleFrame + 4u)
            look.yaw = look.pitch = 0.0f; // left the vehicle
        if (look.yaw == 0.0f && look.pitch == 0.0f)
            return;
        const uint32_t cam = look.vehicleCam;
        uint32_t target = 0;
        if (cam < 0x00100000u || cam + 0x80u > PS2_RAM_SIZE || frame < 0x00100000u || frame + 0x50u > PS2_RAM_SIZE)
            return;
        std::memcpy(&target, rdram + cam + 0x70u, 4);
        if (target < 0x00100000u || target + 0x40u > PS2_RAM_SIZE)
            return;
        float *m = reinterpret_cast<float *>(rdram + frame + 0x10u);
        float pivot[3];
        std::memcpy(pivot, rdram + target + 0x30u, 12);
        float right[3] = {m[0], m[1], m[2]};
        float up[3] = {m[4], m[5], m[6]};
        float at[3] = {m[8], m[9], m[10]};
        float offset[3] = {m[12] - pivot[0], m[13] - pivot[1], m[14] - pivot[2]};
        float pitchAxis[3] = {at[1], -at[0], 0.0f}; // horizontal, to the right
        const float axisLength = std::sqrt(pitchAxis[0] * pitchAxis[0] + pitchAxis[1] * pitchAxis[1]);
        if (!std::isfinite(axisLength) || axisLength < 1.0e-4f || !std::isfinite(offset[0] + offset[1] + offset[2]))
            return;
        pitchAxis[0] /= axisLength;
        pitchAxis[1] /= axisLength;
        const float zAxis[3] = {0.0f, 0.0f, 1.0f};
        // Pitch about the horizontal right axis, then yaw about Z (Z up:
        // looking right is clockwise seen from above).
        for (float *v : {right, up, at, offset})
        {
            vcsRotate(v, pitchAxis, look.pitch);
            vcsRotate(v, zAxis, -look.yaw);
        }
        for (int i = 0; i < 3; ++i)
        {
            m[i] = right[i];
            m[4 + i] = up[i];
            m[8 + i] = at[i];
            m[12 + i] = pivot[i] + offset[i];
        }
    }

    // RwFrameUpdateObjects; CCamera::Process calls it (returning to
    // 0x37AE5C) right after copying TheCamera's matrix into the RenderWare
    // camera's frame, so the look offset reaches rendering only.
    PS2Runtime::RecompiledFunction g_vcsOrigFrameUpdate = nullptr;

    void vcsFrameUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 31) == 0x0037AE5Cu)
            vcsApplyCarLook(rdram, getRegU32(ctx, 4));
        g_vcsOrigFrameUpdate(rdram, ctx, runtime);
    }

    void applyGtaVcsUs(PS2Runtime &runtime)
    {
        if ((g_vcsOrigFollowPedCam = runtime.lookupFunction(0x0029DAB0u)))
            runtime.replaceFunction(0x0029DAB0u, &vcsFollowPedCam);
        if ((g_vcsOrigFollowCarCam = runtime.lookupFunction(0x0029FAF0u)))
            runtime.replaceFunction(0x0029FAF0u, &vcsFollowCarCam);
        if ((g_vcsOrigFrameUpdate = runtime.lookupFunction(0x001B4688u)))
            runtime.replaceFunction(0x001B4688u, &vcsFrameUpdate);

        runtime.padBackend().setVehicleProbe(&vcsPlayerInVehicle, runtime.memory().getRDRAM());

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

        vcsInstallCallCounters(runtime);
    }
}

PS2_REGISTER_GAME_OVERRIDE("GTA Vice City Stories (SLUS_215.90)", "SLUS_215.90", 0x001DD6A0u, 0u, &applyGtaVcsUs)

namespace
{
    PS2Runtime::RecompiledFunction g_lcsOrigMovieFinished = nullptr;
    int g_lcsLastMovieFinishedState = -1;

    // PS2X_TRACE_CALLERS=0xADDR[,0xADDR...]: count and report the return
    // addresses of calls into those guest functions, per 600 guest fields. Used
    // to find the once-per-frame caller of an engine function: RwFrameUpdateObjects
    // (0x00207810) is called from CCamera::Process right after it copies
    // TheCamera's matrix into the RenderWare camera's frame, and the RenderWare
    // camera setters are called once per frame by the game's camera code.
    constexpr int kLcsTraceMax = 4;
    PS2Runtime::RecompiledFunction g_lcsTraceOrig[kLcsTraceMax]{};
    uint32_t g_lcsTraceAddr[kLcsTraceMax]{};
    std::map<uint32_t, uint64_t> g_lcsTraceCounts[kLcsTraceMax];
    int g_lcsTraceUsed = 0;
    uint64_t g_lcsTraceLastTick = 0;

    void lcsReportTraceCounts(PS2Runtime *runtime)
    {
        const uint64_t tick = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
        if (tick < g_lcsTraceLastTick + 600u)
            return;
        g_lcsTraceLastTick = tick;
        for (int i = 0; i < g_lcsTraceUsed; ++i)
        {
            std::ostringstream out;
            out << "[trace:callers] tick=" << tick << " fn=0x" << std::hex << g_lcsTraceAddr[i] << std::dec;
            for (const auto &[ra, count] : g_lcsTraceCounts[i])
                out << " 0x" << std::hex << ra << std::dec << "=" << count;
            std::cerr << out.str() << std::endl;
            g_lcsTraceCounts[i].clear();
        }
    }

    template <int N>
    void lcsTraceWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        ++g_lcsTraceCounts[N][ra];
        // PS2X_TRACE_ARGS=1: log the first calls' arguments too (the camera
        // functions take the CCam in a0 and the look/other inputs in a1..a3
        // and f12..f15).
        static const bool s_args = std::getenv("PS2X_TRACE_ARGS") != nullptr;
        static const uint64_t s_argsFrom = []()
        {
            const char *env = std::getenv("PS2X_TRACE_ARGS_FROM");
            return env ? std::strtoull(env, nullptr, 0) : 0ull;
        }();
        static int s_logged[kLcsTraceMax] = {};
        if (s_args && s_logged[N] < 24 &&
            runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) >= s_argsFrom)
        {
            ++s_logged[N];
            std::ostringstream out;
            out << "[trace:args] fn=0x" << std::hex << g_lcsTraceAddr[N] << " ra=0x" << ra << std::dec
                << " a0=0x" << std::hex << getRegU32(ctx, 4) << " a1=0x" << getRegU32(ctx, 5)
                << " a2=0x" << getRegU32(ctx, 6) << " a3=0x" << getRegU32(ctx, 7)
                << std::dec << " f12=" << ctx->f[12] << " f13=" << ctx->f[13] << " f14=" << ctx->f[14];
            std::cerr << out.str() << std::endl;
        }
        lcsReportTraceCounts(runtime);
        g_lcsTraceOrig[N](rdram, ctx, runtime);
    }

    void lcsInstallCallerTrace(PS2Runtime &runtime)
    {
        const char *env = std::getenv("PS2X_TRACE_CALLERS");
        if (!env)
            return;
        static const auto wrappers = []()
        {
            return std::array<PS2Runtime::RecompiledFunction, kLcsTraceMax>{
                &lcsTraceWrapper<0>, &lcsTraceWrapper<1>, &lcsTraceWrapper<2>, &lcsTraceWrapper<3>};
        }();
        const char *cursor = env;
        while (*cursor && g_lcsTraceUsed < kLcsTraceMax)
        {
            char *end = nullptr;
            const uint32_t addr = static_cast<uint32_t>(std::strtoul(cursor, &end, 0));
            if (end == cursor)
                break;
            if (PS2Runtime::RecompiledFunction orig = runtime.lookupFunction(addr))
            {
                const int slot = g_lcsTraceUsed++;
                g_lcsTraceOrig[slot] = orig;
                g_lcsTraceAddr[slot] = addr;
                runtime.replaceFunction(addr, wrappers[slot]);
            }
            cursor = (*end == ',') ? end + 1 : end;
        }
    }

    void lcsMoviePushEndCode(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The LCS routine queues 00 00 01 B7 from .sdata at 0x003D7320.
        // Its caller retries until accepted. The HLE decoder consumes PSS
        // directly, so notify it instead of waiting on the unused EE ES ring.
        ps2_stubs::notifyMpegCdStreamEof(runtime);
        std::cerr << "[lcs:movie-end-code] notified host decoder" << std::endl;
        setReturnS32(ctx, 1);
        ctx->pc = getRegU32(ctx, 31);
    }

    void lcsMovieFinished(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t player = getRegU32(ctx, 4);
        const uint32_t returnPc = getRegU32(ctx, 31);
        g_lcsOrigMovieFinished(rdram, ctx, runtime);
        // Do not interpret an unfinished invocation's scratch registers.
        if (ctx->pc != returnPc)
            return;
        const bool guestDone = getRegU32(ctx, 2) != 0u;
        const bool hostEnded = ps2_stubs::isMpegStreamFinished(player);
        // The original also requires the unused legacy ES ring to drain. Use
        // the HLE query as a fallback; it requires producer EOF, an empty
        // decoded-frame queue, and completion of the presentation clock.
        R5900Context query = *ctx;
        SET_GPR_U32(&query, 4, player);
        ps2_stubs::sceMpegIsEnd(rdram, &query, runtime);
        const bool hostDone = getRegU32(&query, 2) != 0u;
        const int state = static_cast<int>(guestDone) * 4 + static_cast<int>(hostEnded) * 2 + static_cast<int>(hostDone);
        if (state != g_lcsLastMovieFinishedState)
        {
            std::cerr << "[lcs:movie-finished] player=0x" << std::hex << player << std::dec
                      << " guestDone=" << guestDone << " hostEnded=" << hostEnded
                      << " hostDone=" << hostDone << std::endl;
            g_lcsLastMovieFinishedState = state;
        }
        if (!guestDone && hostDone)
            setReturnS32(ctx, 1);
    }

    // ---------------------------------------------------------------------
    // Mouse look on foot: the follow-ped camera mode (CCam::Process_FollowPed,
    // 0x281E90, a0 = CCam; called from the camera update 0x27A380 for mode 4).
    //
    // That mode keeps the camera's real angles and their speeds:
    //   +0xF0 yaw (beta, [0, 2*pi); looking right decreases it), +0xF4 its speed
    //   +0xE0 pitch (alpha; front.z = sin(pitch)),             +0xE4 its speed
    // Every update it eases each speed towards "auto-turn + look stick" (the
    // auto-turn swings the camera behind the player's facing, read from the
    // ped matrix), adds the speed to the angle and builds the front vector
    // (+0x1C0) and position from the two angles. The camera update then
    // recomputes +0xD4/+0xD8 from that position, so those two are outputs.
    //
    // The mouse motion since the last update is added to the angles *before*
    // the mode runs, so the view turns that same frame, and the speeds are
    // cleared so no eased auto-turn pulls against it. While the player moves,
    // the ped's heading turns with the camera (as the VCS port does), so the
    // auto-turn target stays behind the view instead of springing it back.
    constexpr uint32_t kLcsCamsBase = 0x0043BE10u; // TheCamera.Cams[0]; 3 cameras, 0x2B0 apart
    constexpr uint32_t kLcsCamStride = 0x2B0u;
    constexpr uint32_t kLcsCamPitch = 0xE0u;
    constexpr uint32_t kLcsCamPitchSpeed = 0xE4u;
    constexpr uint32_t kLcsCamYaw = 0xF0u;
    constexpr uint32_t kLcsCamYawSpeed = 0xF4u;
    constexpr uint32_t kLcsCamTarget = 0x274u; // the followed entity
    // CPed heading (current, wanted), radians in [-pi, pi]; the ped matrix the
    // camera's auto-turn reads is rebuilt from it every frame. (Found as the
    // only ped fields equal to atan2(-forward.x, forward.y) across turns.)
    constexpr uint32_t kLcsPedHeading = 0x4E0u;
    constexpr uint32_t kLcsPedHeadingWanted = 0x4E4u;
    // Follow-ped settings (two variants, 0x3C apart, chosen by a global
    // flag). +0x18 caps the per-update automatic pitch correction, which eases
    // the pitch back to the mode's preferred height every update: with the
    // stick that is a spring-back, but it undoes any vertical mouse look.
    constexpr uint32_t kLcsFollowPedSettings = 0x003936D0u;
    constexpr uint32_t kLcsFollowPedSettingsStride = 0x3Cu;
    constexpr uint32_t kLcsAutoPitchRate = 0x18u;
    PS2Runtime::RecompiledFunction g_lcsOrigFollowPed = nullptr;

    float lcsReadF(const uint8_t *rdram, uint32_t address)
    {
        float v;
        std::memcpy(&v, rdram + address, 4);
        return v;
    }

    void lcsWriteF(uint8_t *rdram, uint32_t address, float v) { std::memcpy(rdram + address, &v, 4); }

    void lcsFreeAimUndoOffset(uint8_t *rdram);
    void lcsFreeAimPedCam(uint8_t *rdram, PS2Runtime *runtime, uint32_t cam);

    void lcsFollowPed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Last update's free-aim shoulder offset comes off first: the camera
        // logic must build on its own values.
        lcsFreeAimUndoOffset(rdram);
        const uint32_t cam = getRegU32(ctx, 4);
        const bool camOk = cam >= kLcsCamsBase && cam <= kLcsCamsBase + 2u * kLcsCamStride &&
                           ((cam - kLcsCamsBase) % kLcsCamStride) == 0u;
        float yaw = 0.0f, pitch = 0.0f;
        const bool captured = runtime->padBackend().consumeMouseLook(yaw, pitch);
        const bool apply = camOk && captured && (yaw != 0.0f || pitch != 0.0f) && std::isfinite(yaw + pitch);
        const uint32_t ped = camOk ? vcsRead32(rdram, cam + kLcsCamTarget) : 0u;
        const bool pedOk = ped >= 0x00100000u && ped + 0x1400u <= PS2_RAM_SIZE && (ped & 3u) == 0u;
        if (apply)
        {
            constexpr float kTwoPi = 6.2831853f;
            float yawAngle = std::fmod(lcsReadF(rdram, cam + kLcsCamYaw) - yaw, kTwoPi);
            if (yawAngle < 0.0f)
                yawAngle += kTwoPi;
            lcsWriteF(rdram, cam + kLcsCamYaw, yawAngle);
            lcsWriteF(rdram, cam + kLcsCamYawSpeed, 0.0f);
            // The mode clamps the pitch to its own limits; the view can also stop
            // short of them (looking up lowers the camera until it meets the
            // ground). Do not wind the angle up past what the view showed last
            // update (front.z = sin(pitch) when nothing is in the way), or
            // moving back the other way would do nothing until it unwound.
            constexpr float kPitchSlack = 0.05f;
            const float shown = std::asin(std::clamp(lcsReadF(rdram, cam + 0x1C8u), -1.0f, 1.0f));
            float pitchAngle = lcsReadF(rdram, cam + kLcsCamPitch) + pitch;
            if (pitch > 0.0f)
                pitchAngle = std::min(pitchAngle, std::max(shown + kPitchSlack, lcsReadF(rdram, cam + kLcsCamPitch)));
            else if (pitch < 0.0f)
                pitchAngle = std::max(pitchAngle, std::min(shown - kPitchSlack, lcsReadF(rdram, cam + kLcsCamPitch)));
            lcsWriteF(rdram, cam + kLcsCamPitch, pitchAngle);
            lcsWriteF(rdram, cam + kLcsCamPitchSpeed, 0.0f);
        }
        const float yawBefore = camOk ? lcsReadF(rdram, cam + kLcsCamYaw) : 0.0f;
        const float pitchBefore = camOk ? lcsReadF(rdram, cam + kLcsCamPitch) : 0.0f;

        // While the mouse has the camera, the pitch stays where it is put: the
        // automatic correction is off until the mouse is released. (Set for
        // as long as that lasts, not around this call: the runtime can resume
        // the mode part-way through after this hook has returned.)
        static float s_autoPitchRate[2] = {};
        static bool s_pitchHeld = false;
        if (camOk && captured != s_pitchHeld)
        {
            for (uint32_t i = 0; i < 2u; ++i)
            {
                const uint32_t address = kLcsFollowPedSettings + i * kLcsFollowPedSettingsStride + kLcsAutoPitchRate;
                if (captured)
                {
                    s_autoPitchRate[i] = lcsReadF(rdram, address);
                    lcsWriteF(rdram, address, 0.0f);
                }
                else
                {
                    lcsWriteF(rdram, address, s_autoPitchRate[i]);
                }
            }
            s_pitchHeld = captured;
        }
        g_lcsOrigFollowPed(rdram, ctx, runtime);
        if (camOk)
            lcsFreeAimPedCam(rdram, runtime, cam);

        if (!camOk || !pedOk)
            return;
        // Walking: on-foot movement is camera-relative, so turn the player with
        // the view. Turning only the camera leaves the auto-turn aiming behind
        // the old facing, and a fast turn swings the wanted heading far enough
        // for the game to answer with a skid stop.
        const float vx = lcsReadF(rdram, ped + 0x70u), vy = lcsReadF(rdram, ped + 0x74u);
        if (apply && yaw != 0.0f && vx * vx + vy * vy > 0.0001f && ps2HostSettings().mouseTurnsPlayer)
            for (uint32_t off : {kLcsPedHeading, kLcsPedHeadingWanted})
            {
                const float heading = lcsReadF(rdram, ped + off);
                if (std::isfinite(heading))
                    lcsWriteF(rdram, ped + off, std::remainder(heading - yaw, 6.2831853f));
            }
        static const bool s_debug = std::getenv("PS2X_MOUSELOOK_DEBUG") != nullptr;
        if (s_debug)
        {
            static uint64_t s_n = 0;
            if ((s_n++ % 15u) == 0u || apply)
            {
                std::fprintf(stderr,
                             "[mouselook] f=%llu in=%.4f,%.4f yaw %.4f->%.4f spd %.4f pitch %.4f->%.4f spd %.4f frontZ %.3f "
                             "pedHeading %.4f vel %.3f\n",
                             (unsigned long long)g_ps2PresentedFrameCounter.load(std::memory_order_relaxed), yaw, pitch,
                             yawBefore, lcsReadF(rdram, cam + kLcsCamYaw), lcsReadF(rdram, cam + kLcsCamYawSpeed),
                             pitchBefore, lcsReadF(rdram, cam + kLcsCamPitch), lcsReadF(rdram, cam + kLcsCamPitchSpeed),
                             lcsReadF(rdram, cam + 0x1C8u),
                             std::atan2(-lcsReadF(rdram, ped + 0x10u), lcsReadF(rdram, ped + 0x14u)),
                             std::sqrt(vx * vx + vy * vy));
            }
        }
    }

    // ---------------------------------------------------------------------
    // Mouse look in vehicles (the PS2 game has none), as in the VCS port: a
    // look offset applied to the vehicle camera's *result* - the active CCam's
    // Front (+0x1C0), Source (+0x1D0) and its copy (+0x1E0), Up (+0x1F0) - on
    // entry to 0x287020, which CCamera::Process calls on the active camera right
    // after the camera update (0x27A380), before it builds TheCamera and the
    // RenderWare camera from it. Everything derived from the camera (culling,
    // sky, water) then follows; rotating TheCamera or the RenderWare frame
    // instead left the sky and world drawn for the unrotated view in VCS (grey
    // sky, missing world). The game's own vectors are put back before the next
    // vehicle-camera update (0x283EB8, modes 18/22) so its logic never builds on
    // the offset. The offset eases back after ~1.5 s without mouse motion.
    constexpr uint32_t kLcsCamVectors = 0x1C0u; // Front, Source, Source copy, Up
    constexpr uint32_t kLcsCamVectorsSize = 0x40u;
    constexpr uint32_t kLcsLineOfSight = 0x00174538u;  // CWorld::ProcessLineOfSight
    constexpr uint32_t kLcsIgnoreEntity = 0x003D9CA4u; // CWorld::pIgnoreEntity
    PS2Runtime::RecompiledFunction g_lcsOrigFollowCar = nullptr;
    PS2Runtime::RecompiledFunction g_lcsOrigAfterCamProcess = nullptr;

    struct LcsCarLook
    {
        float yaw = 0.0f, pitch = 0.0f; // radians; yaw positive = look right, pitch positive = look up
        uint32_t idleUpdates = 0;
        uint32_t cam = 0;   // CCam of the last vehicle-camera update
        uint64_t frame = 0; // presented frame of that update
    };
    LcsCarLook g_lcsCarLook;

    // The vectors the rotation replaced and what it left there: put back only
    // if the game has not rewritten them itself in the meantime.
    struct LcsCamBackup
    {
        uint32_t cam = 0;
        uint8_t original[kLcsCamVectorsSize] = {};
        uint8_t written[kLcsCamVectorsSize] = {};
    };
    LcsCamBackup g_lcsCamBackup;

    // Wall stop, measured after the game's camera processing and applied by
    // the next frame's rotation (the raycast is a guest call that can yield and
    // unwind the hook, so it must not run before the game's own processing).
    struct LcsCarWall
    {
        uint32_t cam = 0;
        bool clamp = false, raised = false;
        float position[3] = {}, front[3] = {}, up[3] = {};
        uint32_t desiredCam = 0;
        float desired[3] = {}; // where the rotation put the camera, unclamped
    };
    LcsCarWall g_lcsCarWall;

    // pIgnoreEntity is set around the raycast; a yield inside it skips the
    // restore, so it is re-asserted at the start of the next camera hook.
    uint32_t g_lcsIgnoreSaved = 0u;
    bool g_lcsIgnoreDirty = false;

    void lcsRecoverIgnore(uint8_t *rdram)
    {
        if (g_lcsIgnoreDirty)
        {
            std::memcpy(rdram + kLcsIgnoreEntity, &g_lcsIgnoreSaved, 4);
            g_lcsIgnoreDirty = false;
        }
    }

    void lcsRestoreCamBackup(uint8_t *rdram)
    {
        LcsCamBackup &backup = g_lcsCamBackup;
        if (backup.cam == 0u)
            return;
        if (std::memcmp(rdram + backup.cam + kLcsCamVectors, backup.written, kLcsCamVectorsSize) == 0)
            std::memcpy(rdram + backup.cam + kLcsCamVectors, backup.original, kLcsCamVectorsSize);
        backup.cam = 0u;
    }

    void lcsRotateAxis(float v[3], const float axis[3], float angle)
    {
        // Rodrigues rotation about a unit axis.
        const float c = std::cos(angle), sn = std::sin(angle);
        const float dot = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
        const float cross[3] = {axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2],
                                axis[0] * v[1] - axis[1] * v[0]};
        for (int i = 0; i < 3; ++i)
            v[i] = v[i] * c + cross[i] * sn + axis[i] * dot * (1.0f - c);
    }

    // CWorld::ProcessLineOfSight(p1, p2, colPoint&, entity*&, buildings,
    // vehicles, peds, objects, dummies, ignoreSeeThrough, ignoreSomeObjects, ...)
    // with the arguments the vehicle camera passes (0x2859A4); ignores the
    // entity at pIgnoreEntity. Runs on the game thread from a hook, with a
    // scratch stack frame; true and the hit point when blocked.
    PS2Runtime::RecompiledFunction g_lcsOrigLineOfSight = nullptr; // the game's, under lcsLineOfSightHook

    bool lcsLineOfSightBlocked(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const float from[3],
                               const float to[3], uint32_t ignoreEntity, float hit[3], bool peds = false)
    {
        const PS2Runtime::RecompiledFunction s_lineOfSight =
            g_lcsOrigLineOfSight ? g_lcsOrigLineOfSight : runtime->lookupFunction(kLcsLineOfSight);
        if (!s_lineOfSight)
            return false;
        const R5900Context saved = *ctx;
        const uint32_t sp = (getRegU32(ctx, 29) - 0x100u) & ~0xFu;
        uint32_t oldIgnore = 0;
        std::memcpy(&oldIgnore, rdram + kLcsIgnoreEntity, 4);
        std::memcpy(rdram + kLcsIgnoreEntity, &ignoreEntity, 4);
        g_lcsIgnoreSaved = oldIgnore;
        g_lcsIgnoreDirty = true;
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
        std::memcpy(rdram + kLcsIgnoreEntity, &oldIgnore, 4);
        g_lcsIgnoreDirty = false;
        *ctx = saved;
        return blocked && std::isfinite(hit[0] + hit[1] + hit[2]);
    }

    bool lcsCarLookWalls()
    {
        static const bool s_on = []()
        {
            const char *env = std::getenv("PS2X_CARLOOK_WALLS");
            return env == nullptr || std::atoi(env) != 0;
        }();
        return s_on;
    }

    FILE *lcsCarLookLog()
    {
        static FILE *s_log = []() -> FILE *
        {
            const char *path = std::getenv("PS2X_CARLOOK_DEBUG");
            return path ? std::fopen(path, "w") : nullptr;
        }();
        return s_log;
    }

    void lcsRotateVehicleCam(uint8_t *rdram, uint32_t cam)
    {
        LcsCarLook &look = g_lcsCarLook;
        if (look.yaw == 0.0f && look.pitch == 0.0f)
            return;
        if (cam + kLcsCamTarget + 4u > PS2_RAM_SIZE)
            return;
        const uint32_t target = vcsRead32(rdram, cam + kLcsCamTarget);
        if (target < 0x00100000u || target + 0x40u > PS2_RAM_SIZE)
            return;
        float *c = reinterpret_cast<float *>(rdram + cam + kLcsCamVectors);
        float *front = c, *source = c + 4, *sourceCopy = c + 8, *up = c + 12;
        float pivot[3];
        std::memcpy(pivot, rdram + target + 0x30u, 12);
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
            lcsRotateAxis(v, pitchAxis, pitch);
            lcsRotateAxis(v, zAxis, -look.yaw);
        };

        // Looking up swings the camera down around the vehicle: stop it just
        // above the vehicle's origin, which sits on the road (below it the
        // camera would look at the road from underneath).
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

        LcsCamBackup &backup = g_lcsCamBackup;
        backup.cam = cam;
        std::memcpy(backup.original, c, kLcsCamVectorsSize);
        for (float *v : {front, up})
        {
            float r[3] = {v[0], v[1], v[2]};
            rotate(r, look.pitch);
            std::memcpy(v, r, sizeof(r));
        }
        for (float *v : {source, sourceCopy}) // positions: about the pivot
        {
            float o[3] = {v[0] - pivot[0], v[1] - pivot[1], v[2] - pivot[2]};
            rotate(o, look.pitch);
            for (int i = 0; i < 3; ++i)
                v[i] = pivot[i] + o[i];
        }
        LcsCarWall &wall = g_lcsCarWall;
        std::memcpy(wall.desired, source, sizeof(wall.desired));
        wall.desiredCam = cam;
        const bool clamp = lcsCarLookWalls() && wall.clamp && wall.cam == cam;
        if (clamp)
        {
            for (float *v : {source, sourceCopy})
                std::memcpy(v, wall.position, sizeof(wall.position));
            if (wall.raised)
            {
                std::memcpy(front, wall.front, sizeof(wall.front));
                std::memcpy(up, wall.up, sizeof(wall.up));
            }
        }
        std::memcpy(backup.written, c, kLcsCamVectorsSize);
        if (FILE *log = lcsCarLookLog())
            std::fprintf(log, "f=%llu rotate cam=0x%x yaw=%.3f pitch=%.3f clamp=%d raised=%d src=(%.2f,%.2f,%.2f)\n",
                         (unsigned long long)g_ps2PresentedFrameCounter.load(std::memory_order_relaxed), cam, look.yaw,
                         look.pitch, clamp ? 1 : 0, (clamp && wall.raised) ? 1 : 0, source[0], source[1], source[2]);
    }

    void lcsMeasureCarWall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t cam)
    {
        LcsCarLook &look = g_lcsCarLook;
        LcsCarWall &wall = g_lcsCarWall;
        // The previous clamp stands until this measurement completes (the
        // raycast can yield; clearing first made the clamp alternate).
        wall.cam = cam;
        if (!lcsCarLookWalls() || (look.yaw == 0.0f && look.pitch == 0.0f))
        {
            wall.clamp = wall.raised = false;
            return;
        }
        const uint32_t target = vcsRead32(rdram, cam + kLcsCamTarget);
        if (target < 0x00100000u || target + 0x40u > PS2_RAM_SIZE)
            return;
        float pivot[3];
        std::memcpy(pivot, rdram + target + 0x30u, 12);
        const float *rendered = reinterpret_cast<const float *>(rdram + cam + kLcsCamVectors + 0x10u);
        // Aim at the unclamped pose: the clamped one is in free space in front
        // of the wall, and measuring to it would release the clamp every other
        // frame.
        const float *camera = wall.desiredCam == cam ? wall.desired : rendered;
        float hit[3];
        if (!lcsLineOfSightBlocked(rdram, ctx, runtime, pivot, camera, target, hit))
        {
            wall.clamp = wall.raised = false;
            return;
        }
        // A hit behind the vehicle, or well past the camera, is not a wall to
        // pull the camera in front of.
        const float toCam[3] = {camera[0] - pivot[0], camera[1] - pivot[1], camera[2] - pivot[2]};
        const float toHit[3] = {hit[0] - pivot[0], hit[1] - pivot[1], hit[2] - pivot[2]};
        const float camLen2 = toCam[0] * toCam[0] + toCam[1] * toCam[1] + toCam[2] * toCam[2];
        const float along = toCam[0] * toHit[0] + toCam[1] * toHit[1] + toCam[2] * toHit[2];
        if (camLen2 <= 1.0e-6f || along < 0.0f || along > camLen2 * 1.25f)
        {
            wall.clamp = wall.raised = false;
            return;
        }
        float toPivot[3] = {pivot[0] - hit[0], pivot[1] - hit[1], pivot[2] - hit[2]};
        const float length = std::sqrt(toPivot[0] * toPivot[0] + toPivot[1] * toPivot[1] + toPivot[2] * toPivot[2]);
        constexpr float kWallGap = 0.8f;
        const float pull = length > kWallGap ? kWallGap / length : 0.0f;
        float position[3];
        for (int i = 0; i < 3; ++i)
            position[i] = hit[i] + toPivot[i] * pull;
        // Publish now: the ceiling raycast below can yield.
        std::memcpy(wall.position, position, sizeof(position));
        wall.raised = false;
        wall.clamp = true;
        // No room beside the vehicle: rise above it and look down at it,
        // unless that is blocked too (a low ceiling).
        constexpr float kMinDistance = 1.6f;
        const float dx = position[0] - pivot[0], dy = position[1] - pivot[1], dz = position[2] - pivot[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance >= kMinDistance)
            return;
        float raised[3] = {position[0], position[1], position[2] + (kMinDistance - distance) * 1.5f};
        float ceiling[3];
        if (lcsLineOfSightBlocked(rdram, ctx, runtime, pivot, raised, target, ceiling))
            return;
        const float lookAt[3] = {pivot[0], pivot[1], pivot[2] + 0.3f};
        float f[3] = {lookAt[0] - raised[0], lookAt[1] - raised[1], lookAt[2] - raised[2]};
        const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (fl <= 1.0e-3f)
            return;
        for (float &x : f)
            x /= fl;
        float u[3] = {-f[2] * f[0], -f[2] * f[1], 1.0f - f[2] * f[2]}; // world Z made perpendicular to Front
        const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (ul <= 1.0e-3f)
            return;
        std::memcpy(wall.front, f, sizeof(f));
        for (int i = 0; i < 3; ++i)
            wall.up[i] = u[i] / ul;
        std::memcpy(wall.position, raised, sizeof(raised));
        wall.raised = true;
    }

    // The vehicle camera mode: entry only (the runtime can resume a hooked
    // function part-way through, so nothing here depends on running after it).
    void lcsFollowCar(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        lcsRecoverIgnore(rdram);
        lcsRestoreCamBackup(rdram);
        LcsCarLook &look = g_lcsCarLook;
        const uint64_t now = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        if (now > look.frame + 4u)
            look.yaw = look.pitch = 0.0f; // a new stretch in a vehicle starts straight
        look.cam = getRegU32(ctx, 4);
        look.frame = now;
        float yaw = 0.0f, pitch = 0.0f;
        const bool captured = runtime->padBackend().consumeMouseLook(yaw, pitch);
        if (captured && (yaw != 0.0f || pitch != 0.0f) && std::isfinite(yaw + pitch))
        {
            look.yaw = std::remainder(look.yaw + yaw, 6.2831853f);
            look.pitch = std::clamp(look.pitch + pitch, -0.6f, 0.9f);
            look.idleUpdates = 0;
        }
        else if (++look.idleUpdates > 45u) // ~1.5 s at 30 camera updates a second
        {
            look.yaw *= 0.9f;
            look.pitch *= 0.9f;
        }
        if (std::fabs(look.yaw) < 1.0e-3f && std::fabs(look.pitch) < 1.0e-3f)
            look.yaw = look.pitch = 0.0f;
        g_lcsOrigFollowCar(rdram, ctx, runtime);
    }

    // 0x287020(active CCam): the vehicle camera's result is final and TheCamera
    // is not built from it yet.
    // ------------------------------------------------------------- free aim
    // Ported from VCS (modern controls; the pad withholds the lock-on button
    // while the aim is held, see PSPadBackend::setFreeAimProbe). While aiming:
    //  - standing, the player turns to face where the camera looks;
    //  - the camera slides over the right shoulder (undone before the next
    //    camera update, like the vehicle look offset);
    //  - the crosshair ray (camera through screen centre) is cast once per
    //    camera update after the game's own processing;
    //  - the player's instant-hit shots are turned from the ped's facing to
    //    that point at CWorld::ProcessLineOfSight (0x174538) when it is called
    //    by the shot wrapper 0x2B0BD0 (matched to VCS 0x2D0870; returns to
    //    0x2B0C18, its caller's return address at sp+0x30) from
    //    CWeapon::FireInstantHit (0x2A4BF8-0x2A5D30, shooter in s3).
    // LCS ped: current weapon slot (s8) +0x6B8, weapons +0x594 (0x1C each,
    // type +0); heading +0x4E0 / +0x4E4; matrix forward +0x10.
    struct LcsFreeAim
    {
        bool held = false;        // aiming this camera update
        float blend = 0.0f;       // shoulder offset, eased in and out (0..1)
        uint32_t pedCam = 0;      // the on-foot CCam of the last update
        bool aimValid = false;
        float aimPoint[3] = {};   // where the crosshair ray meets the world (or its far end)
        uint64_t aimFrame = 0;
        uint32_t offsetCam = 0;   // the shoulder offset's undo (Front, Source, copy, Up)
        float original[4][3] = {};
        float written[4][3] = {};
    };
    LcsFreeAim g_lcsFreeAim;

    uint32_t lcsPlayerPed(const uint8_t *rdram)
    {
        const uint32_t index = rdram[0x003D9BC8u];
        if (index >= 2u)
            return 0u;
        const uint32_t ped = vcsRead32(rdram, 0x00408E40u + index * 0x190u);
        return ped >= 0x00100000u && ped + 0x1000u <= PS2_RAM_SIZE && (ped & 3u) == 0u ? ped : 0u;
    }

    bool lcsFreeAimWeapon(const uint8_t *rdram)
    {
        // Guns only: handgun (3), shotgun (4), SMG (5), rifle (6) and heavy (7)
        // slots; not melee / thrown, and not the sniper slot (8 in LCS - the
        // reverse of VCS's 7/8), which keeps its own scope. The weapon type is
        // the first word of each 0x1C-byte weapon (weapon cheat: 17, 19, 22, 27,
        // 31 in slots 3..7, 28 = sniper in slot 8).
        const uint32_t ped = lcsPlayerPed(rdram);
        if (ped == 0u || ps2_game_overrides::detail::lcsPlayerInVehicle(rdram))
            return false;
        const int slot = static_cast<int8_t>(rdram[ped + 0x6B8u]);
        if (slot < 3 || slot > 7)
            return false;
        return vcsRead32(rdram, ped + 0x594u + static_cast<uint32_t>(slot) * 0x1Cu) != 0u;
    }

    void lcsFreeAimUndoOffset(uint8_t *rdram)
    {
        LcsFreeAim &aim = g_lcsFreeAim;
        if (aim.offsetCam == 0u)
            return;
        for (uint32_t f = 0; f < 4u; ++f)
        {
            const uint32_t at = aim.offsetCam + kLcsCamVectors + f * 0x10u;
            if (std::memcmp(rdram + at, aim.written[f], 12) == 0)
                std::memcpy(rdram + at, aim.original[f], 12);
        }
        aim.offsetCam = 0u;
    }

    void lcsFreeAimPedCam(uint8_t *rdram, PS2Runtime *runtime, uint32_t cam)
    {
        LcsFreeAim &aim = g_lcsFreeAim;
        aim.held = ps2HostSettings().freeAim && runtime->padBackend().freeAimHeld() && lcsFreeAimWeapon(rdram);
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
                             runtime->padBackend().freeAimHeld() ? 1 : 0, lcsFreeAimWeapon(rdram) ? 1 : 0);
            s_lastHeld = aim.held;
        }
        // Standing: face the camera. Moving: the game turns the ped towards the
        // stick as usual; shots still follow the crosshair within 90 degrees.
        if (!aim.held || runtime->padBackend().movingInput())
            return;
        const uint32_t ped = lcsPlayerPed(rdram);
        if (ped == 0u)
            return;
        const float fx = lcsReadF(rdram, cam + kLcsCamVectors), fy = lcsReadF(rdram, cam + kLcsCamVectors + 4u);
        if (!(std::fabs(fx) + std::fabs(fy) > 1e-4f))
            return;
        const float heading = std::atan2(-fx, fy); // headings point along (-sin h, cos h)
        if (std::isfinite(heading))
            for (uint32_t off : {kLcsPedHeading, kLcsPedHeadingWanted})
                lcsWriteF(rdram, ped + off, heading);
    }

    // Over the right shoulder: Source (and its copy) moved right, a little
    // down and in, the view levelled up a little.
    void lcsFreeAimOffsetCam(uint8_t *rdram, uint32_t cam)
    {
        LcsFreeAim &aim = g_lcsFreeAim;
        lcsFreeAimUndoOffset(rdram); // never stack two
        float *c = reinterpret_cast<float *>(rdram + cam + kLcsCamVectors);
        float *front = c, *source = c + 4, *copy = c + 8, *up = c + 12;
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
        constexpr float kTiltUp = 0.12f; // ~7 degrees
        lcsRotateAxis(front, right, b * kTiltUp);
        lcsRotateAxis(up, right, b * kTiltUp);
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
    void lcsFreeAimMeasure(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t cam)
    {
        LcsFreeAim &aim = g_lcsFreeAim;
        float source[3], front[3];
        std::memcpy(source, rdram + cam + kLcsCamVectors + 0x10u, 12);
        std::memcpy(front, rdram + cam + kLcsCamVectors, 12);
        const float length = std::sqrt(front[0] * front[0] + front[1] * front[1] + front[2] * front[2]);
        if (!(length > 1e-4f) || !std::isfinite(length + source[0] + source[1] + source[2]))
            return;
        constexpr float kReach = 150.0f;
        float far[3], hit[3];
        for (int i = 0; i < 3; ++i)
            far[i] = source[i] + front[i] / length * kReach;
        const bool blocked = lcsLineOfSightBlocked(rdram, ctx, runtime, source, far, lcsPlayerPed(rdram), hit, true);
        std::memcpy(aim.aimPoint, blocked ? hit : far, sizeof(aim.aimPoint));
        aim.aimValid = true;
        aim.aimFrame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
    }

    // Turns a player instant-hit shot by the rotation that takes the ped's
    // facing to the crosshair point; writes the end point (a1) in place, which
    // both the impact and the tracer are built from.
    void lcsFreeAimShot(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const LcsFreeAim &aim = g_lcsFreeAim;
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
        const uint32_t outer = vcsRead32(rdram, sp + 0x30u);
        if (outer < 0x002A4BF8u || outer >= 0x002A5D30u)
            return; // not FireInstantHit
        const uint32_t ped = lcsPlayerPed(rdram);
        if (ped == 0u || getRegU32(ctx, 19) != ped)
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

    void lcsLineOfSightHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 31) == 0x002B0C18u)
            lcsFreeAimShot(rdram, ctx, runtime);
        g_lcsOrigLineOfSight(rdram, ctx, runtime);
    }

    void lcsAfterCamProcess(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        lcsRecoverIgnore(rdram);
        const uint32_t cam = getRegU32(ctx, 4);
        const bool rotated = cam == g_lcsCarLook.cam &&
                             g_ps2PresentedFrameCounter.load(std::memory_order_relaxed) <= g_lcsCarLook.frame + 1u;
        if (rotated)
            lcsRotateVehicleCam(rdram, cam);
        const bool aiming = !rotated && cam != 0u && cam == g_lcsFreeAim.pedCam && g_lcsFreeAim.blend > 0.0f;
        if (aiming)
            lcsFreeAimOffsetCam(rdram, cam);
        g_lcsOrigAfterCamProcess(rdram, ctx, runtime);
        if (aiming && g_lcsFreeAim.held)
            lcsFreeAimMeasure(rdram, ctx, runtime, cam);
        else if (!g_lcsFreeAim.held)
            g_lcsFreeAim.aimValid = false;
        // After the game's own processing: the raycast is a guest call that
        // can yield.
        if (rotated)
            lcsMeasureCarWall(rdram, ctx, runtime, cam);
    }

    // Draw distance (F2 > Display > View), as in VCS. Two parts:
    //  - view distance: the scene camera's far clip, set every frame by
    //    RwCameraSetFarClipPlane (0x1006A0, matched to VCS 0x18F400 byte for
    //    byte) from the call at 0x1F5DD8 (Scene.camera, the time cycle's far
    //    clip). Only that caller is scaled; the others are fixed 1000/2000
    //    values for special cameras.
    //  - detail distance: CCamera::Process writes LODDistMultiplier
    //    (70 / FOV * 4/3) at TheCamera + 0x170 (0x291AAC/0x291AC0) every frame;
    //    it is scaled at the scene far-clip call, which comes after it.
    // ------------------------------------------------- radio: Custom Soundtracks
    // The player's vehicle keeps its station at +0x2A0: 0..9 are the ten
    // stations, 10 the silent slot (the pending L1 step, gp-0x6760 =
    // 0x3D0790, is applied at 0x16A22C as (station + step) mod 11). As in VCS,
    // the silent slot is our Custom Soundtracks station: the music folder plays
    // there, and its "Radio Off" GXT name reads "Custom" - except while the
    // runtime's own radio switch (hold D-pad Down / M) is off, when the game is
    // re-tuned to that slot once and keeps its real name.
    constexpr uint32_t kLcsRadioField = 0x2A0u;
    constexpr uint8_t kLcsSilentSlot = 10u;
    constexpr uint32_t kLcsPauseMenuOpen = 0x003D9CB4u; // 1 while the pause menu is up
    constexpr uint32_t kLcsCutsceneRunning = 0x003D70CCu; // nonzero during in-engine cutscenes
    std::atomic<uint32_t> g_lcsRadioStation{0xFFu};

    // The radio's per-frame update (0x1684D0, a0 = the music manager). An L1
    // press adds 1 to the pending step (0x3D0790) and sets manager+4 = 1,
    // which makes the game apply it (and show the station pop-up).
    // "Next station just pressed" (0x171D28, the L1 check). The radio update
    // asks it at 0x1685D0 (enter the change block) and 0x16863C (+1 = next,
    // else +10 = previous). Radio Off answers "pressed" to both once, with the
    // pending step pre-set, so the game re-tunes - audio, pop-up and all - by
    // its own L1 path in a single jump.
    PS2Runtime::RecompiledFunction g_lcsOrigNextStationPressed = nullptr;
    std::atomic<bool> g_lcsForceNextStation{false};

    void lcsNextStationPressed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        g_lcsOrigNextStationPressed(rdram, ctx, runtime);
        if (g_lcsForceNextStation.load(std::memory_order_relaxed) && (ra == 0x001685D8u || ra == 0x00168644u))
        {
            setReturnU32(ctx, 1u);
            if (ra == 0x00168644u)
                g_lcsForceNextStation.store(false, std::memory_order_relaxed);
        }
    }

    uint32_t lcsPlayerVehicle(const uint8_t *rdram)
    {
        if (!ps2_game_overrides::detail::lcsPlayerInVehicle(rdram))
            return 0u;
        auto read32 = [rdram](uint32_t address)
        {
            uint32_t value = 0u;
            std::memcpy(&value, rdram + address, sizeof(value));
            return value;
        };
        const uint32_t ped = read32(0x00408E40u + rdram[0x003D9BC8u] * 0x190u);
        return read32(ped + 0x534u);
    }

    uint32_t lcsRadioOffNameAddr(uint8_t *rdram)
    {
        // The GXT loads late, so look at most every 60 frames until found, and
        // re-check the cached spot (a reload can move it).
        static uint32_t s_addr = 0;
        static int s_cooldown = 0;
        const char needle[] = "Radio Off";
        uint8_t pat[(sizeof(needle) - 1) * 2] = {};
        for (size_t i = 0; i + 1 < sizeof(needle); ++i)
            pat[i * 2] = static_cast<uint8_t>(needle[i]);
        static const uint8_t kCustomPat[] = {'C', 0, 'u', 0, 's', 0, 't', 0, 'o', 0, 'm', 0, 0, 0};
        if (s_addr != 0u && (std::memcmp(rdram + s_addr, pat, sizeof(pat)) == 0 ||
                             std::memcmp(rdram + s_addr, kCustomPat, sizeof(kCustomPat)) == 0))
            return s_addr;
        s_addr = 0u;
        if (s_cooldown-- > 0)
            return 0u;
        s_cooldown = 60;
        for (uint32_t a = 0x00100000u; a + sizeof(pat) + 2u < PS2_RAM_SIZE; a += 2u)
            if (std::memcmp(rdram + a, pat, sizeof(pat)) == 0 && rdram[a + sizeof(pat)] == 0u)
            {
                s_addr = a;
                if (std::getenv("PS2X_RADIO_TRACE") != nullptr)
                    std::cerr << "[radio] \"Radio Off\" text at 0x" << std::hex << a << std::dec << std::endl;
                break;
            }
        return s_addr;
    }

    void lcsRadioFrame(uint8_t *rdram)
    {
        static const bool s_trace = std::getenv("PS2X_RADIO_TRACE") != nullptr;
        {
            // PS2X_RADIO_OFF_FRAME=<presented frame>: switch the runtime radio off
            // then, as the D-pad Down hold would (scripted tests cannot hold it).
            static const uint64_t s_offFrame = std::getenv("PS2X_RADIO_OFF_FRAME")
                                                   ? std::strtoull(std::getenv("PS2X_RADIO_OFF_FRAME"), nullptr, 10)
                                                   : 0u;
            static bool s_done = false;
            if (s_offFrame != 0u && !s_done && g_ps2PresentedFrameCounter.load(std::memory_order_relaxed) >= s_offFrame)
            {
                s_done = true;
                ps2MusicStationSetRadioOff(true);
            }
        }
        const uint32_t vehicle = lcsPlayerVehicle(rdram);
        const bool valid = vehicle >= 0x00100000u && vehicle + kLcsRadioField < PS2_RAM_SIZE;
        const uint32_t station = valid ? rdram[vehicle + kLcsRadioField] : 0xFFu;
        const uint32_t previous = g_lcsRadioStation.exchange(station, std::memory_order_relaxed);
        if (station != previous && s_trace)
            std::cerr << "[radio] station=" << station << std::endl;
        // Radio Off: re-tune the car to the silent slot once per switch-off
        // (and again in a different car). From then on the state, not a
        // station *change*, decides: leaving the silent slot is a retune by
        // the player, so the radio is back on. Deciding on changes missed a
        // retune when the vehicle lookup blinked for a frame, left the switch
        // off, and the next "off" hold turned it on - playing Custom.
        static bool s_pushed = false;
        static uint32_t s_pushedVehicle = 0u;
        if (!ps2MusicStationRadioOff() || (valid && vehicle != s_pushedVehicle))
            s_pushed = false;
        // The re-tune goes through the game's own L1 path (see
        // lcsNextStationPressed): the pending step word at 0x3D0790 plus one
        // faked L1 press. Writing the vehicle's station byte
        // directly changes the number but not what plays - the station kept
        // playing (it only looked right from Custom, which is silent anyway).
        // The handler adds its own one step for the press, so the step is
        // corrected by a learned extra and re-tried (closed loop) until the car
        // sits on the silent slot.
        constexpr uint32_t kLcsPendingStationStep = 0x003D0790u;
        static int s_pushWait = 0, s_pushTries = 0;
        static uint32_t s_extra = 1u, s_stationBefore = 0xFFu;
        if (!ps2MusicStationRadioOff())
            s_pushWait = s_pushTries = 0;
        if (ps2MusicStationRadioOff() && valid)
        {
            if (!s_pushed)
            {
                bool done = station == kLcsSilentSlot;
                const bool canStep = g_lcsOrigNextStationPressed != nullptr && station <= kLcsSilentSlot;
                if (!done && s_pushWait > 0 && station != s_stationBefore)
                {
                    // Applied, but off target: learn the handler's extra step.
                    s_extra = (s_extra + station + 11u - kLcsSilentSlot) % 11u;
                    s_pushWait = 0;
                }
                if (!done && s_pushWait == 0 && canStep && s_pushTries < 3)
                {
                    const uint32_t step = (kLcsSilentSlot + 22u - station - s_extra) % 11u;
                    std::memcpy(rdram + kLcsPendingStationStep, &step, sizeof(step));
                    g_lcsForceNextStation.store(true, std::memory_order_relaxed); // + the game's own 1
                    s_stationBefore = station;
                    s_pushWait = 90; // frames for the game to apply it
                    ++s_pushTries;
                    if (s_trace)
                        std::cerr << "[radio] off: stepping " << station << " +" << step << " (+" << s_extra
                                  << " by the game) to the silent slot" << std::endl;
                }
                else if (!done && (s_pushWait == 0 || --s_pushWait == 0))
                {
                    rdram[vehicle + kLcsRadioField] = kLcsSilentSlot; // fallback
                    done = true;
                    if (s_trace)
                        std::cerr << "[radio] off: step not applied, wrote the slot directly" << std::endl;
                }
                if (done)
                {
                    g_lcsRadioStation.store(kLcsSilentSlot, std::memory_order_relaxed);
                    s_pushed = true;
                    s_pushWait = s_pushTries = 0;
                    s_pushedVehicle = vehicle;
                    if (s_trace)
                        std::cerr << "[radio] off: on the silent slot" << std::endl;
                }
            }
            else if (station != kLcsSilentSlot)
            {
                ps2MusicStationSetRadioOff(false);
                s_pushed = false;
                if (s_trace)
                    std::cerr << "[radio] retuned to " << station << " -> radio on" << std::endl;
            }
        }
        if (const uint32_t name = lcsRadioOffNameAddr(rdram))
        {
            static const uint16_t kCustom[10] = {'C', 'u', 's', 't', 'o', 'm', 0, 0, 0, 0};
            static const uint16_t kOff[10] = {'R', 'a', 'd', 'i', 'o', ' ', 'O', 'f', 'f', 0};
            const uint16_t *want = (ps2MusicStationRadioOff() || !ps2HostSettings().musicStation) ? kOff : kCustom;
            if (std::memcmp(rdram + name, want, sizeof(kCustom)) != 0)
                std::memcpy(rdram + name, want, sizeof(kCustom));
        }
    }

    PS2Runtime::RecompiledFunction g_lcsOrigSetFarClip = nullptr;

    float lcsDrawDistanceSetting()
    {
        return std::clamp(ps2HostSettings().drawDistance, 0.5f, 4.0f);
    }

    void lcsSetFarClip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 31) == 0x001F5DE0u) // the scene camera, once per frame
        {
            const float scale = lcsDrawDistanceSetting();
            if (scale != 1.0f && std::isfinite(ctx->f[12]) && ctx->f[12] > 0.0f)
                ctx->f[12] *= scale;
            // Detail distance: CCamera::Process rewrote LODDistMultiplier
            // (TheCamera 0x43BBF0 + 0x170) earlier this frame; the scene is
            // built after this point, so scaling the fresh value here never
            // compounds.
            constexpr uint32_t kLodMultiplier = 0x0043BBF0u + 0x170u;
            float lod = 0.0f;
            std::memcpy(&lod, rdram + kLodMultiplier, sizeof(lod));
            static float s_written = 0.0f;
            if (scale != 1.0f && lod != s_written && std::isfinite(lod) && lod > 0.0f && lod < 100.0f)
            {
                lod *= scale;
                std::memcpy(rdram + kLodMultiplier, &lod, sizeof(lod));
                s_written = lod;
            }
            lcsRadioFrame(rdram);
        }
        g_lcsOrigSetFarClip(rdram, ctx, runtime);
    }

    void applyGtaLcsUs(PS2Runtime &runtime)
    {
        runtime.padBackend().setVehicleProbe(&ps2_game_overrides::detail::lcsPlayerInVehicle,
                                            runtime.memory().getRDRAM());
        // "In a menu": the pause menu (which is also LCS's start menu), or no
        // player yet (title / loading) - the menu keys, not the gameplay keys.
        runtime.padBackend().setMenuProbe([](const uint8_t *rdram)
                                          { return rdram[kLcsPauseMenuOpen] != 0u || vcsRead32(rdram, 0x00408E40u) == 0u; });
        runtime.padBackend().setCutsceneProbe([](const uint8_t *rdram) { return vcsRead32(rdram, kLcsCutsceneRunning) != 0u; });
        // Mouse look: the follow-ped camera mode.
        if ((g_lcsOrigFollowPed = runtime.lookupFunction(0x00281E90u)))
            runtime.replaceFunction(0x00281E90u, &lcsFollowPed);
        // Mouse look in vehicles: the vehicle camera mode and the step after the
        // camera update.
        if ((g_lcsOrigFollowCar = runtime.lookupFunction(0x00283EB8u)))
            runtime.replaceFunction(0x00283EB8u, &lcsFollowCar);
        if ((g_lcsOrigAfterCamProcess = runtime.lookupFunction(0x00287020u)))
            runtime.replaceFunction(0x00287020u, &lcsAfterCamProcess);
        // Custom Soundtracks: the music folder plays on the silent slot while in
        // a vehicle, the runtime radio is on, the pause menu is closed and no
        // cutscene runs (see lcsRadioFrame).
        ps2MusicStationSetQuery([](const uint8_t *rdram)
                                {
                                    uint32_t cutscene = 0;
                                    std::memcpy(&cutscene, rdram + kLcsCutsceneRunning, 4);
                                    return g_lcsRadioStation.load(std::memory_order_relaxed) == kLcsSilentSlot &&
                                           lcsPlayerVehicle(rdram) != 0u && rdram[kLcsPauseMenuOpen] == 0u &&
                                           cutscene == 0u;
                                });
        // Free aim: shots at ProcessLineOfSight; "aimable gun on foot" for the pad.
        if ((g_lcsOrigLineOfSight = runtime.lookupFunction(kLcsLineOfSight)))
            runtime.replaceFunction(kLcsLineOfSight, &lcsLineOfSightHook);
        runtime.padBackend().setFreeAimProbe(&lcsFreeAimWeapon);
        if ((g_lcsOrigNextStationPressed = runtime.lookupFunction(0x00171D28u)))
            runtime.replaceFunction(0x00171D28u, &lcsNextStationPressed);
        // Draw distance and the per-frame radio work (see lcsSetFarClip).
        if ((g_lcsOrigSetFarClip = runtime.lookupFunction(0x001006A0u)))
            runtime.replaceFunction(0x001006A0u, &lcsSetFarClip);
        // Mouse-look port probe: identify the callers of candidate engine hooks.
        lcsInstallCallerTrace(runtime);
        // Alloc/InitIopHeap already use HLE bindings in the active LCS config.
        // The guest free wrapper at 0x00382C78 calls 0x00382C00, which exits
        // while .data[0x0039D694] retains its initial -1. Complete that API
        // pairing so movie teardown frees the same physical IOP allocations.
        if (!ps2_game_overrides::bindAddressHandler(runtime, 0x00382C78u, "sceSifFreeIopHeap"))
            throw std::runtime_error("LCS sceSifFreeIopHeap binding failed");
        // Verified LCS code: lw v1,0x40(a0); lw v0,4(v1); jr ra;
        // sltiu v0,v0,1. HLE decoding does not update that legacy active field.
        if (!ps2_game_overrides::bindAddressHandler(runtime, 0x0036A240u, "sceMpegIsEnd"))
            throw std::runtime_error("LCS sceMpegIsEnd binding failed");
        if (!runtime.replaceFunction(0x002B6B20u, &lcsMoviePushEndCode))
            throw std::runtime_error("LCS movie end-code binding failed");
        const auto original = runtime.lookupFunction(0x002B6BF8u);
        if (original != &lcsMovieFinished)
            g_lcsOrigMovieFinished = original;
        g_lcsLastMovieFinishedState = -1;
        if (g_lcsOrigMovieFinished)
            runtime.replaceFunction(0x002B6BF8u, &lcsMovieFinished);
    }
}

PS2_REGISTER_GAME_OVERRIDE("GTA Liberty City Stories (SLUS_214.23)", "SLUS_214.23", 0x001C6A60u, 0xDC486B9Du, &applyGtaLcsUs)
