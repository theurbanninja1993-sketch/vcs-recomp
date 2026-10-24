#include "ps2_runtime.h"
#include "runtime/gs/gs_gpu_backend.h" // g_gsGpuPresent*: present readback cap
#include "runtime/gs/gpu/gl_context.h"   // publishHostGlContext
#include "runtime/ps2_mods.h"
#include "runtime/ps2_music_station.h"
#include "runtime/ps2_host_settings.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_platform.h"
#include "runtime/ps2_music_station.h"
#include "runtime/ps2_mods.h"
#include "runtime/ps2_achievements.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_gpu_backend.h" // g_gsGpuPresent*: present readback cap
#include "ps2_narrow.h"
extern std::atomic<uint64_t> g_gsComposedFrames;
#include "runtime/ps2_camera.h"
#include "runtime/ee_scheduler.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"
#include "ps2_iop_host.h"

// Defined in ps2_iop_worker.cpp. The PC sampler runs process-wide (ITIMER_PROF),
// so it uses this to skip samples taken on the IOP worker's own thread.
extern thread_local bool t_onIopWorker;
// Diagnostics: which entry point posts the IOP work. iopPost counts them all;
// these say which one is hot (the memory-access ones are the suspects).
std::atomic<uint64_t> g_iopSiteRead{0}, g_iopSiteWrite{0}, g_iopSiteZero{0}, g_iopSiteRange{0};
std::atomic<uint64_t> g_iopSiteHandleRpc{0}, g_iopSiteSif{0}, g_iopSiteAbi{0}, g_iopSiteBind{0};
// Bumped whenever the IOP's service set can change (module load/stop, reset).
// selectIopRpcAbi caches its answer against this.
std::atomic<uint64_t> g_rpcAbiEpoch{1};
// Which SIF RPC the game is hammering. The IOP thread costs a round trip per
// RPC (~75k/s in the intro), so the sid says whether that is normal traffic or
// the game retrying one call. Diagnostics only; racy by design.
namespace
{
    std::atomic<uint64_t> g_rpcSidCount[64];
    std::atomic<uint32_t> g_rpcSidValue[64];
}
void ps2IopRpcHistogram(char *out, size_t size)
{
    uint32_t topSid[4] = {};
    uint64_t topCount[4] = {};
    for (size_t i = 0; i < 64; ++i)
    {
        const uint64_t count = g_rpcSidCount[i].load(std::memory_order_relaxed);
        for (size_t slot = 0; slot < 4; ++slot)
            if (count > topCount[slot])
            {
                for (size_t move = 3; move > slot; --move)
                {
                    topCount[move] = topCount[move - 1];
                    topSid[move] = topSid[move - 1];
                }
                topCount[slot] = count;
                topSid[slot] = g_rpcSidValue[i].load(std::memory_order_relaxed);
                break;
            }
    }
    std::snprintf(out, size, "%08x:%llu,%08x:%llu,%08x:%llu,%08x:%llu", topSid[0],
                  static_cast<unsigned long long>(topCount[0]), topSid[1],
                  static_cast<unsigned long long>(topCount[1]), topSid[2],
                  static_cast<unsigned long long>(topCount[2]), topSid[3],
                  static_cast<unsigned long long>(topCount[3]));
}
#include "ps2_iop_worker.h"
#include "ps2x/iop/iop_subsystem.h"

#include <iostream>

extern "C" unsigned char *rlReadScreenPixels(int width, int height);
extern "C" void rlDrawRenderBatchActive(void);

// raylib's rlgl texture readback. rlgl.h is not on this file's include path, and
// the symbol is exported with C linkage, so declare it here.
#include <stdexcept>
#include <exception>
#include <fstream>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <csignal>
#if defined(__linux__)
#include <ucontext.h>
#endif
#include <ctime>
#include <filesystem>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <sstream>

static void ps2Vu1TraceMscal(const uint8_t *vu1Data, uint32_t startPC, uint32_t top, uint32_t itop);
extern std::atomic<int32_t> g_ps2Vu1TraceRemaining;

namespace ps2_stubs
{
    void resetSifState();
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

extern std::atomic<uint64_t> g_ps2PresentedFrameCounter;
std::atomic<bool> g_ps2Crosshair{false};
bool (*g_ps2AudioFollowsGameClock)(const uint8_t *rdram) = nullptr;
// Present only in -fprofile-generate builds.
extern "C" void __gcov_dump(void) __attribute__((weak));
// And clang's equivalent, which both of its profiling modes provide. The port is
// built with clang on Windows, and until this was here an instrumented Windows run
// created an empty .profraw and stopped there: the exit below asked GCC's runtime
// to dump and nobody asked clang's.
extern "C" void __llvm_profile_write_file(void) __attribute__((weak));
extern std::atomic<int> g_ps2GsWorkerTid; // gs_threaded_backend.cpp
// Async VIF1: FBRST snapshot taken on the EE thread, and VU1 stop bits the
// worker defers to the EE thread (bit 31 = pending).
std::atomic<uint32_t> g_asyncVif1Fbrst{0};
std::atomic<uint32_t> g_asyncVif1StopBits{0};

namespace
{
    // PS2X_VU_STATS: per-unit microprogram call counts and host time, printed
    // every 1200 presented frames.
    struct VuStats
    {
        bool enabled = std::getenv("PS2X_VU_STATS") != nullptr;
        uint64_t calls[2]{};
        uint64_t cycles[2]{};
        double seconds[2]{};
        uint64_t lastReportFrame = 0;
    };
    VuStats &vuStats()
    {
        static VuStats stats;
        return stats;
    }
    struct VuStatsScope
    {
        int unit;
        const uint64_t &cycleCounter;
        uint64_t startCycles;
        std::chrono::steady_clock::time_point start;
        bool enabled;
        VuStatsScope(int u, const uint64_t &counter)
            : unit(u), cycleCounter(counter), startCycles(counter), enabled(vuStats().enabled)
        {
            if (enabled)
                start = std::chrono::steady_clock::now();
        }
        ~VuStatsScope()
        {
            if (!enabled)
                return;
            VuStats &stats = vuStats();
            ++stats.calls[unit];
            stats.cycles[unit] += cycleCounter - startCycles;
            stats.seconds[unit] += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const uint64_t frame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
            if (frame >= stats.lastReportFrame + 1200u)
            {
                stats.lastReportFrame = frame;
                std::fprintf(stderr, "[vu:stats] frame=%llu vu0 calls=%llu cycles=%llu time=%.2fs | vu1 calls=%llu cycles=%llu time=%.2fs\n",
                             static_cast<unsigned long long>(frame),
                             static_cast<unsigned long long>(stats.calls[0]), static_cast<unsigned long long>(stats.cycles[0]), stats.seconds[0],
                             static_cast<unsigned long long>(stats.calls[1]), static_cast<unsigned long long>(stats.cycles[1]), stats.seconds[1]);
                stats = VuStats{};
                stats.lastReportFrame = frame;
            }
        }
    };
}


namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;
    // Interrupt/alarm/callback invocation stacks live in the BIOS kernel region, like
    // the real EE kernel's own stacks. Games own all user RAM from 0x100000 up (their
    // heaps and thread stacks reach the top of RAM), so carving handler stacks there
    // lets handlers overwrite live game frames.
    constexpr uint32_t kAsyncCallbackStackRegionFloor = 0x00010000u;
    constexpr uint32_t kAsyncCallbackStackRegionTop = 0x00080000u;
    constexpr uint32_t kHleArenaBase = 0x00080000u;
    constexpr uint32_t kHleArenaEnd = 0x00100000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

// Learning VU programs during play. VU microprograms come from the game disc,
// so their compiled versions are generated on the player's machine
// (tools/vu1_recomp via build.sh) from recordings of the programs running; a
// program the build never saw runs interpreted, about ten times slower. So the
// runtime records, by default, every program with no compiled version (up to 4
// runs each - enough for vu1_recomp to trace its jump targets) to
// <config>/vu_learn/vu<unit>_learn_<time>.bin, and drops a PENDING marker there;
// `build.sh --optimize` (the launcher's Optimize) compiles them into the
// vu1_programs plugin next to the game, which it loads at startup.
// A file is only created once something is recorded, so a complete build
// writes nothing. PS2X_VU_LEARN=0 turns it off; PS2X_VU1_LEARN=<dir> and
// PS2X_VU0_CAPTURE=<file> name the destination explicitly (build passes).
static FILE *ps2VuLearnFile(int unit)
{
    static std::mutex s_mutex;
    static FILE *s_files[2] = {};
    static bool s_failed[2] = {};
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_files[unit] || s_failed[unit])
        return s_files[unit];
    static const bool s_off = []()
    {
        const char *env = std::getenv("PS2X_VU_LEARN");
        return env != nullptr && std::strcmp(env, "0") == 0;
    }();
    const char *vu1Dir = std::getenv("PS2X_VU1_LEARN");
    const char *vu0File = std::getenv("PS2X_VU0_CAPTURE");
    std::error_code ec;
    std::filesystem::path path;
    std::filesystem::path marker;
    if (unit == 1 && vu1Dir && *vu1Dir)
    {
        std::filesystem::create_directories(vu1Dir, ec);
        path = std::filesystem::path(vu1Dir) / ("vu1_learn_" + std::to_string(static_cast<long long>(std::time(nullptr))) + ".bin");
    }
    else if (unit == 0 && vu0File && *vu0File)
    {
        path = vu0File;
    }
    else if (!s_off)
    {
        const std::filesystem::path dir = ps2ConfigDirectory() / "vu_learn";
        std::filesystem::create_directories(dir, ec);
        path = dir / ("vu" + std::to_string(unit) + "_learn_" + std::to_string(static_cast<long long>(std::time(nullptr))) + ".bin");
        marker = dir / "PENDING";
    }
    if (path.empty())
    {
        s_failed[unit] = true;
        return nullptr;
    }
    s_files[unit] = std::fopen(path.string().c_str(), "wb");
    if (!s_files[unit])
    {
        s_failed[unit] = true;
        return nullptr;
    }
    std::cerr << "[vu" << unit << ":learn] recording a program with no compiled version to " << path.string()
              << (marker.empty() ? "" : " (run Optimize to compile it)") << std::endl;
    if (!marker.empty())
        if (FILE *m = std::fopen(marker.string().c_str(), "w"))
            std::fclose(m);
    return s_files[unit];
}

// `outDraw` is the texture the caller should draw: `tex` (the host's own, for
// the CPU frames) or the renderer's shared display texture when it composes on
// the GPU in the window's context - then there is nothing to upload at all.
static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight, Texture2D &outDraw)
{
    // The texture the caller draws persists between frames: a repeat present
    // (or a frame the renderer composed on the GPU) must keep drawing what was
    // last handed over, whether that is raylib's own texture or the renderer's.
    static Texture2D s_lastDraw{};
    if (s_lastDraw.id == 0)
        s_lastDraw = tex;
    outDraw = s_lastDraw;
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    // The EE scheduler latches the presentation frame at each VBlankStart; only latch
    // here before the first vblank so boot still shows something.
    const bool needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick;
    if (needsLatch)
    {
        if (currentTick == 0u)
            rt->gs().latchHostPresentationFrame();
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

    // The renderer composed this frame into a texture it shares with us: draw
    // it directly (no readback, no upload).
    {
        uint32_t glWidth = 0u, glHeight = 0u;
        const uint32_t glTexture = rt->gs().latchedHostPresentationGlTexture(glWidth, glHeight);
        if (glTexture != 0u && glWidth != 0u && glHeight != 0u)
        {
            static Texture2D s_shared{};
            s_shared.id = glTexture;
            s_shared.width = static_cast<int>(glWidth);
            s_shared.height = static_cast<int>(glHeight);
            s_shared.mipmaps = 1;
            s_shared.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            // The renderer keeps the shared texture's filtering in step with the
            // "smooth filtering" setting (see updateSharedPresentFilter).
            s_lastDraw = s_shared;
            outDraw = s_lastDraw;
            outWidth = glWidth;
            outHeight = glHeight;
            s_lastWidth = glWidth;
            s_lastHeight = glHeight;
            s_hasUploadedFrame = true;
            return;
        }
    }
    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    uint64_t frameId = 0u;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource,
                                                   &frameId))
    {
        // No new picture since the last one (the latch is taken, or the GS got
        // nothing): keep drawing the texture already uploaded rather than
        // clearing it to black. Only the very first presents have nothing yet.
        if (s_hasUploadedFrame)
        {
            outWidth = s_lastWidth != 0u ? s_lastWidth : FB_WIDTH;
            outHeight = s_lastHeight != 0u ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
            return;
        }
        s_lastDraw = tex; // a CPU frame: draw the host's own texture again
        outDraw = s_lastDraw;
        Image blank = GenImageColor(tex.width, tex.height, BLACK); // the texture may have grown (upscaling)
        UpdateTexture(tex, blank.data);
        UnloadImage(blank);
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }
    // The same picture again (a repeat present): the texture is already
    // current, so skip the upload.
    static uint64_t s_lastUploadedFrameId = 0u;
    if (frameId != 0u && frameId == s_lastUploadedFrameId)
    {
        s_lastDraw = tex;
        outDraw = s_lastDraw;
        outWidth = width;
        outHeight = height;
        s_lastWidth = width;
        s_lastHeight = height;
        s_hasUploadedFrame = true;
        return;
    }
    s_lastUploadedFrameId = frameId;

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

    // Upscaled frames (GPU internal resolution) need a bigger texture.
    if (width > static_cast<uint32_t>(tex.width) || height > static_cast<uint32_t>(tex.height))
    {
        Image blank = GenImageColor(std::max<int>(tex.width, static_cast<int>(width)),
                                    std::max<int>(tex.height, static_cast<int>(height)), BLANK);
        UnloadTexture(tex);
        tex = LoadTextureFromImage(blank);
        UnloadImage(blank);
    }
    // The latched frame is already packed at `width`. When it exactly fills
    // the texture, upload it straight: the zero-fill and the row repack below
    // are only needed for padding (a texture that grew, or a narrower host
    // frame layout), and at internal_scale > 1 this buffer is several MB.
    if (width == static_cast<uint32_t>(tex.width) && height == static_cast<uint32_t>(tex.height) &&
        s_scratch.size() == static_cast<size_t>(width) * height * 4u)
    {
        UpdateTexture(tex, s_scratch.data());
        s_lastDraw = tex;
        outDraw = s_lastDraw;
        outWidth = width;
        outHeight = height;
        s_hasUploadedFrame = true;
        return;
    }
    s_uploadBuffer.resize(static_cast<size_t>(tex.width) * static_cast<size_t>(tex.height) * 4u);
    std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, static_cast<uint32_t>(tex.width));
        const uint32_t copyHeight = std::min<uint32_t>(height, static_cast<uint32_t>(tex.height));
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(tex.width) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }

    UpdateTexture(tex, s_uploadBuffer.data());
    s_lastDraw = tex;
    outDraw = s_lastDraw;
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
}

PS2Runtime::PS2Runtime()
{
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);
    m_iopWorker = std::make_unique<IopWorker>(*m_iopSubsystem);
    m_iopWorker->start();

    m_eeScheduler = std::make_unique<EeScheduler>(*this);

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = kAsyncCallbackStackRegionFloor;
    m_asyncCallbackStackTop = kAsyncCallbackStackRegionTop;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        m_iopWorker.reset(); // joins the thread before the IOP it owns goes away
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
            CloseWindow();
        }

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

// Every one of these runs on the IOP thread (when it is enabled), so the IOP's
// state stays owned by that one thread. The call scope travels with them: it
// records the guest context the IOP callbacks run under (m_activeRdram), which
// must be set on the thread that is running the IOP.
ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
    g_rpcAbiEpoch.fetch_add(1u, std::memory_order_release); // the ABI answer may change
    return m_iopWorker->runValue(
        [&]()
        {
            auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
            return m_iopSubsystem->loadModule(path, arguments, argumentSize);
        });
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    g_rpcAbiEpoch.fetch_add(1u, std::memory_order_release); // the ABI answer may change
    return m_iopWorker->runValue(
        [&]()
        {
            auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
            return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
        });
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    g_rpcAbiEpoch.fetch_add(1u, std::memory_order_release); // the ABI answer may change
    return m_iopWorker->runValue(
        [&]()
        {
            auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
            return m_iopSubsystem->stopModule(moduleId, result);
        });
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    // The IOP owns its service table, so this lookup belongs on its thread - but
    // the game calls it once per SIF RPC (measured ~57k times a second in the
    // intro, each a lock, a futex wake and a wait for the worker) and the answer
    // only changes when the set of loaded IOP modules does. Cache it and re-ask
    // after a module load/stop/reset. No service reads the request today -
    // RpcAbiRequest is unused across ps2xIOP, so selectRpcAbi is a constant - so
    // the request is not part of the key; if a service ever starts using it,
    // this must become a per-request cache.
    static std::atomic<uint64_t> s_cachedEpoch{0};
    static std::atomic<uint32_t> s_cachedAbi{static_cast<uint32_t>(ps2x::iop::RpcAbi::RuntimeDefault)};
    const uint64_t epoch = g_rpcAbiEpoch.load(std::memory_order_acquire);
    if (s_cachedEpoch.load(std::memory_order_relaxed) == epoch)
        return static_cast<ps2x::iop::RpcAbi>(s_cachedAbi.load(std::memory_order_relaxed));
    ++g_iopSiteAbi; // a real round trip (the cached path above returns first)
    const ps2x::iop::RpcAbi abi = m_iopWorker->runValue([&]() { return m_iopSubsystem->selectRpcAbi(request); });
    s_cachedAbi.store(static_cast<uint32_t>(abi), std::memory_order_relaxed);
    s_cachedEpoch.store(epoch, std::memory_order_relaxed);
    return abi;
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
    ++g_iopSiteBind;
    return m_iopWorker->runValue([&]() { return m_iopSubsystem->canBindRpc(sid); });
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    ++g_iopSiteHandleRpc;
    {
        const size_t bucket = request.sid & 63u;
        if (g_rpcSidValue[bucket].load(std::memory_order_relaxed) != request.sid)
        {
            g_rpcSidValue[bucket].store(request.sid, std::memory_order_relaxed);
            g_rpcSidCount[bucket].store(0u, std::memory_order_relaxed);
        }
        g_rpcSidCount[bucket].fetch_add(1u, std::memory_order_relaxed);
    }
    return m_iopWorker->runValue(
        [&]()
        {
            auto scope = m_iopHost->enterCall(ctx, rdram);
            request.callToken = scope.token();
            return m_iopSubsystem->handleRpc(request);
        });
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    ++g_iopSiteSif;
    m_iopWorker->run(
        [&]()
        {
            auto scope = m_iopHost->enterCall(nullptr, rdram);
            m_iopSubsystem->onSifTransfer(transfer);
        });
}

namespace
{
    // The EE clock, and the cycles in one NTSC VBlank (294.912 MHz / 59.94 Hz).
    constexpr uint64_t kEeClockHz = 294'912'000u;
    constexpr uint64_t kEeCyclesPerVBlank = 4'920'120u;
    const bool g_iopFreeRun = std::getenv("PS2X_IOP_FREE_RUN") != nullptr;
}

// The IOP (and the SPU2 it clocks) runs at most one VBlank of time per VBlank:
// with the EE cycle count as the only clock the sound chip ran ~20% fast in
// busy stretches (the intro movie), underrunning streamed audio. Before the
// first VBlank it runs freely (boot). PS2X_IOP_FREE_RUN=1: old behaviour.
void PS2Runtime::advanceIopEeCycles(uint64_t eeCycles) noexcept
{
    if (m_iopWorker && m_iopWorker->enabled())
    {
        // The worker runs the IOP. Before the first VBlank it follows the EE's
        // cycles (boot); afterwards it is clocked by real time, as the hardware
        // is - see advanceIopVBlank.
        if (!m_iopClockFromRealTime)
            m_iopWorker->addAllowedEeCycles(eeCycles);
        return;
    }
    if (g_iopFreeRun || m_iopEeAllowed == 0u)
    {
        m_iopSubsystem->runEeCycles(eeCycles);
        return;
    }
    // The IOP clock also advances in synchronous RPC handlers and nested
    // waits, so cap against its own cycle count (EE cycles = 8 IOP cycles).
    const uint64_t iopEe = m_iopSubsystem->cycles() * 8u;
    const uint64_t allowed = m_iopEeAllowed > iopEe ? m_iopEeAllowed - iopEe : 0u;
    const uint64_t give = std::min(eeCycles, allowed);
    if (give != 0u)
        m_iopSubsystem->runEeCycles(give);
}

void PS2Runtime::advanceIopVBlank() noexcept
{
    if (g_iopFreeRun)
        return;
    if (m_iopWorker && m_iopWorker->enabled())
    {
        const auto now = std::chrono::steady_clock::now();
        if (!m_iopClockFromRealTime)
        {
            // First VBlank: from here the IOP follows real time, not the EE.
            m_iopClockFromRealTime = true;
            m_iopLastReal = now;
        }
        const double elapsed = std::chrono::duration<double>(now - m_iopLastReal).count();
        m_iopLastReal = now;
        // In a cutscene: one VBlank of IOP time per VBlank, so the dialogue
        // and the animation advance together. On real time (below), a slow
        // stretch let the voices run ahead of the pictures, and the 2-VBlank
        // cap then also lost audio for good after a late VBlank - the cutscene
        // desync on the owner's laptop. PS2X_AUDIO_REALTIME=1 keeps real time.
        static const bool s_realtime = std::getenv("PS2X_AUDIO_REALTIME") != nullptr;
        if (!s_realtime && g_ps2AudioFollowsGameClock != nullptr && g_ps2AudioFollowsGameClock(m_memory.getRDRAM()))
        {
            m_iopWorker->addAllowedEeCycles(kEeCyclesPerVBlank);
            return;
        }
        m_iopWorker->addAllowedEeCycles(std::min<uint64_t>(
            static_cast<uint64_t>(elapsed * static_cast<double>(kEeClockHz)), 2u * kEeCyclesPerVBlank));
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const uint64_t iopEe = m_iopSubsystem->cycles() * 8u;
    if (m_iopEeAllowed == 0u)
    {
        // First VBlank: start from where boot left the IOP.
        m_iopEeAllowed = iopEe;
        m_iopLastReal = now;
    }
    // Top the allowance up by *real* elapsed time, not by one VBlank per VBlank.
    // A late VBlank is a VBlank the emulation never got, and the SPU2 is clocked
    // by the IOP, so the audio it did not produce is lost for good - the video
    // catches up by rendering extra frames, audio cannot. Measured on the
    // owner's laptop: the emulation runs 20-35% behind and the audio is short by
    // the same 20-35%, while the production rate per *emulated* time is exact.
    // Capped at two VBlanks so a long stall cannot produce a burst.
    const double elapsed = std::chrono::duration<double>(now - m_iopLastReal).count();
    m_iopLastReal = now;
    const uint64_t realEeCycles = static_cast<uint64_t>(elapsed * static_cast<double>(kEeClockHz));
    static const bool s_realtime = std::getenv("PS2X_AUDIO_REALTIME") != nullptr;
    if (!s_realtime && g_ps2AudioFollowsGameClock != nullptr && g_ps2AudioFollowsGameClock(m_memory.getRDRAM()))
        m_iopEeAllowed += kEeCyclesPerVBlank; // cutscene: game time (see the worker branch)
    else
        m_iopEeAllowed += std::min<uint64_t>(realEeCycles, 2u * kEeCyclesPerVBlank);
    // Keep within two VBlanks of real time when the EE reports too few cycles.
    if (m_iopEeAllowed > iopEe + 2u * kEeCyclesPerVBlank)
        m_iopSubsystem->runEeCycles(m_iopEeAllowed - iopEe - kEeCyclesPerVBlank);
}

void PS2Runtime::resetIop()
{
    g_rpcAbiEpoch.fetch_add(1u, std::memory_order_release); // the ABI answer may change
    m_iopWorker->run([&]() { m_iopSubsystem->reset(); });
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopWorker->runValue([&]() { return m_iopSubsystem->debugSnapshot(); });
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    return m_iopSubsystem ? m_iopWorker->runValue([&]() { return m_iopSubsystem->allocateMemory(size, alignment); }) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    return m_iopSubsystem && m_iopWorker->runValue([&]() { return m_iopSubsystem->freeMemory(address); });
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    ++g_iopSiteRead;
    return m_iopSubsystem &&
           m_iopWorker->runValue([&]() { return m_iopSubsystem->readMemory(address, destination, size); });
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    ++g_iopSiteWrite;
    return m_iopSubsystem && m_iopWorker->runValue([&]() { return m_iopSubsystem->writeMemory(address, source, size); });
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    ++g_iopSiteZero;
    return m_iopSubsystem && m_iopWorker->runValue([&]() { return m_iopSubsystem->zeroMemory(address, size); });
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    ++g_iopSiteRange;
    return m_iopSubsystem && m_iopWorker->runValue([&]() { return m_iopSubsystem->isMemoryRange(address, size); });
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    // Each presented picture carries the camera that drew it, read on the game
    // thread at the VBlank latch (frame interpolation pairs them).
    {
        static const uint8_t *s_rdram = nullptr;
        s_rdram = m_memory.getRDRAM();
        g_gsCameraSampler = [](Ps2CameraState &out) { return s_rdram && ps2_game_overrides::readCamera(s_rdram, out); };
    }
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    { m_gs.processGIFPacket(data, size); });
    m_memory.setGifArbiter(&m_gifArbiter);
    auto asyncVif1EeContext = [this]() -> R5900Context *
    {
        if (PS2Memory::onAsyncVif1Worker())
            return nullptr;
        R5900Context *context = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
        return context ? context : &m_cpuContext;
    };
    auto setVu1StopBits = [this](uint32_t bits, R5900Context *context)
    {
        if (context)
            context->vu0_vpu_stat = (context->vu0_vpu_stat & ~0x0600u) | bits;
        else
            g_asyncVif1StopBits.store(bits | 0x80000000u, std::memory_order_release);
    };
    // Async VIF1 completions are applied on the EE thread: publish the VU1
    // D/T stop bits recorded by the worker.
    m_memory.setAsyncVif1CompletionHook([this]()
                                        {
                                            const uint32_t bits = g_asyncVif1StopBits.exchange(0u, std::memory_order_acq_rel);
                                            if ((bits & 0x80000000u) == 0u)
                                                return;
                                            R5900Context *context = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                            if (!context)
                                                context = &m_cpuContext;
                                            context->vu0_vpu_stat = (context->vu0_vpu_stat & ~0x0600u) | (bits & 0x0600u); });
    m_memory.setVu1MscalCallback([this, asyncVif1EeContext, setVu1StopBits](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     ps2Vu1TraceMscal(m_memory.getVU1Data(), startPC, top, itop);
                                     if (const char *dumpDir = std::getenv("PS2X_VU1_DUMP_DIR"))
                                     {
                                         // Microcode changes via MPG, so dump it with every traced MSCAL.
                                         const int32_t traceIndex = g_ps2Vu1TraceRemaining.load();
                                         if (traceIndex > 0)
                                         {
                                             if (FILE *f = std::fopen((std::string(dumpDir) + "/vu1code_" + std::to_string(traceIndex) + ".bin").c_str(), "wb"))
                                             {
                                                 std::fwrite(m_memory.getVU1Code(), 1, PS2_VU1_CODE_SIZE, f);
                                                 std::fclose(f);
                                             }
                                         }
                                     }
                                     // On the async VIF1 worker the EE context belongs to the
                                     // EE thread: use the FBRST snapshot and defer VPU_STAT.
                                     R5900Context *cpuContext = asyncVif1EeContext();
                                     const uint32_t fbrst = cpuContext ? cpuContext->vu0_fbrst : g_asyncVif1Fbrst.load(std::memory_order_relaxed);
                                     m_vu1.state().dBitEnabled = (fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled = (fbrst & (1u << 11)) != 0u;
                                     // PS2X_VU1_CAPTURE=<file>,<startFrame>,<count>: record MSCALs
                                     // (state + code + data before, state + data after) for the
                                     // offline VU1 replay benchmark (tools/vu1_replay).
                                     static FILE *s_capture = nullptr;
                                     static uint64_t s_captureStart = 0;
                                     static uint32_t s_captureLeft = 0;
                                     static const bool s_captureInit = [&]()
                                     {
                                         if (const char *env = std::getenv("PS2X_VU1_CAPTURE"))
                                         {
                                             std::string spec(env);
                                             const auto c1 = spec.find(',');
                                             const auto c2 = c1 == std::string::npos ? std::string::npos : spec.find(',', c1 + 1);
                                             s_capture = std::fopen(spec.substr(0, c1).c_str(), "wb");
                                             s_captureStart = c1 == std::string::npos ? 0u : std::strtoull(spec.c_str() + c1 + 1, nullptr, 10);
                                             s_captureLeft = c2 == std::string::npos ? 2000u : static_cast<uint32_t>(std::strtoul(spec.c_str() + c2 + 1, nullptr, 10));
                                         }
                                         return true;
                                     }();
                                     (void)s_captureInit;
                                     bool capture = s_capture && s_captureLeft > 0u &&
                                                    g_ps2PresentedFrameCounter.load(std::memory_order_relaxed) >= s_captureStart;
                                     // Programs with no compiled version are recorded for the
                                     // next Optimize (see ps2VuLearnFile), in the
                                     // PS2X_VU1_CAPTURE format.
                                     static std::unordered_map<uint64_t, uint32_t> s_learned;
                                     FILE *recordTo = capture ? s_capture : nullptr;
                                     if (!recordTo &&
                                         !m_vu1.lookupCompiled(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE, &m_memory, startPC))
                                     {
                                         uint32_t &runs = s_learned[ps2Vu1ProgramKey(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE, startPC & 0x3FF8u)];
                                         if (runs < 4u)
                                         {
                                             ++runs;
                                             recordTo = ps2VuLearnFile(1);
                                         }
                                     }
                                     if (recordTo)
                                     {
                                         const uint32_t header[4] = {0x31555643u /* 'CVU1' */, startPC, top, itop};
                                         std::fwrite(header, sizeof(header), 1, recordTo);
                                         std::fwrite(&m_vu1.state(), sizeof(VU1State), 1, recordTo);
                                         std::fwrite(m_memory.getVU1Code(), 1, PS2_VU1_CODE_SIZE, recordTo);
                                         std::fwrite(m_memory.getVU1Data(), 1, PS2_VU1_DATA_SIZE, recordTo);
                                     }
                                     {
                                         VuStatsScope statsScope(1, m_vu1.state().cycles);
                                         m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                       m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                       m_gs, &m_memory, startPC, top, itop, 65536);
                                     }
                                     if (recordTo)
                                     {
                                         std::fwrite(&m_vu1.state(), sizeof(VU1State), 1, recordTo);
                                         std::fwrite(m_memory.getVU1Data(), 1, PS2_VU1_DATA_SIZE, recordTo);
                                         // Flushed per record: a run can end through _Exit
                                         // (PS2X_EXIT_AFTER_FRAME) with the buffer still in libc,
                                         // and the file would come out empty.
                                         std::fflush(recordTo);
                                     }
                                     if (capture)
                                     {
                                         if (--s_captureLeft == 0u)
                                         {
                                             std::fclose(s_capture);
                                             s_capture = nullptr;
                                             std::cerr << "[vu1:capture] done" << std::endl;
                                         }
                                     }
                                     setVu1StopBits((m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                                    (m_vu1.state().stoppedByT ? 0x0400u : 0u), cpuContext); });
    m_memory.setVu1MscntCallback([this, asyncVif1EeContext, setVu1StopBits](uint32_t top, uint32_t itop)
                                 {
                                     // On the async VIF1 worker the EE context belongs to the
                                     // EE thread: use the FBRST snapshot and defer VPU_STAT.
                                     R5900Context *cpuContext = asyncVif1EeContext();
                                     const uint32_t fbrst = cpuContext ? cpuContext->vu0_fbrst : g_asyncVif1Fbrst.load(std::memory_order_relaxed);
                                     m_vu1.state().dBitEnabled = (fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled = (fbrst & (1u << 11)) != 0u;
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                  m_gs, &m_memory, top, itop, 65536);
                                     setVu1StopBits((m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                                    (m_vu1.state().stoppedByT ? 0x0400u : 0u), cpuContext); });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

void ps2GsRequestCapture(uint32_t frames);

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (std::getenv("PS2X_HEADLESS"))
        {
            if (!syncCoreSubsystems())
            {
                std::cerr << "Failed to bind runtime core subsystems" << std::endl;
                return false;
            }
            m_audioBackend.setAudioReady(false);
            RUNTIME_LOG("[headless] Core initialized without a window or audio device");
            return true;
        }
#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
        gsgpu::publishHostGlContext(); // the renderer joins the window's GL share group
#else
        // PS2X_HIDDEN_WINDOW keeps headless test runs off the desktop.
        SetConfigFlags(FLAG_WINDOW_RESIZABLE | (std::getenv("PS2X_HIDDEN_WINDOW") ? FLAG_WINDOW_HIDDEN : 0));
        ps2HostSettingsLoad(); // window mode/size applied by the first presented frame
        ps2SetPreferredGamepad(ps2HostSettings().controllerName);
        ps2ModsRefresh();
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
        ps2ApplyGamepadMappings(); // GLFW is up now: install the extra pad mappings
        gsgpu::publishHostGlContext(); // the renderer joins the window's GL share group
        SetWindowMinSize(320, 240);
        // PS2X_NO_AUDIO=1: run without an audio device (silent test runs).
        if (std::getenv("PS2X_NO_AUDIO") == nullptr)
            InitAudioDevice();
        m_audioBackend.setAudioReady(std::getenv("PS2X_NO_AUDIO") == nullptr && IsAudioDeviceReady());
#endif
        // The GS renderer creates its own GL context, and it can only join the
        // window's share group (so the host draws the composed frame directly,
        // without a readback) once the window exists and its context has been
        // published - so the GS comes up after the window.
        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
        // The host loop is the *present* loop, and the game's rate comes from the
        // VBlank clock, not from here - so this can run faster than the game
        // without changing it. That is the frame structure interpolation needs
        // (the repeats are where a synthesized frame goes); on its own it shows
        // each picture twice and looks exactly like 60. With VSync on a 60 Hz
        // display the swap blocks and nothing changes at all.
        SetTargetFPS(120);
        SetExitKey(KEY_NULL); // Esc is the pause button, not "close window"
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
#if !defined(PLATFORM_VITA)
            // First launch (or "show at every launch"): settings before the
            // game starts. Automated runs skip it.
            const bool automated = std::getenv("PS2X_NO_LAUNCH_MENU") || std::getenv("PS2X_EXIT_AFTER_FRAME") ||
                                   std::getenv("PS2X_EXIT_AFTER_VSYNC") ||
                                   std::getenv("PS2X_HIDDEN_WINDOW");
            if (!automated && (ps2HostSettingsFirstLaunch() || ps2HostSettings().showAtLaunch))
            {
                g_ps2LaunchMenuPending.store(true, std::memory_order_relaxed);
                g_ps2SettingsMenuOpen.store(true, std::memory_order_relaxed);
            }
#endif
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        m_asyncCallbackStackFloor = kAsyncCallbackStackRegionFloor;
        m_asyncCallbackStackTop = kAsyncCallbackStackRegionTop;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    m_iopSubsystem->reset();

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

namespace
{
    // Where this build's memory card lives. See the call site for why it is not simply
    // the game folder.
    std::filesystem::path pickMemoryCardRoot(const std::filesystem::path &gameDir)
    {
        const std::filesystem::path beside = gameDir / "mc0";
        std::error_code ec;
        bool besideHasSaves = false;
        for (std::filesystem::directory_iterator it(beside, ec), last; !ec && it != last; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (name != "." && name != "..")
            {
                besideHasSaves = true;
                break;
            }
        }
        if (besideHasSaves)
            return beside;

        const std::filesystem::path parent = gameDir.parent_path();
        if (!parent.empty() && std::filesystem::exists(parent / "portable.txt", ec) && !ec)
            return parent / "mc0";

        return beside;
    }
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        // The disc root has to be the ELF's folder, but the memory card must not be.
        // Putting it there buries the player's saves inside the game data, so updating
        // the game - or using a read-only copy of it - takes them with it.
        //
        // A portable build ships `portable.txt` one level above the game folder, beside
        // the mc0/mc1 its packaging creates, and that is where the card belongs. A card
        // that already sits beside the ELF wins while it has anything in it, so existing
        // saves keep working.
        paths.mcRoot = pickMemoryCardRoot(paths.elfDirectory);
    }

    // Host launchers can keep writable saves outside an extracted disc tree.
    // Port 1 is resolved beside this mc0 root by the memory-card service.
    if (const char *mcRoot = std::getenv("PS2X_MC_ROOT"); mcRoot && *mcRoot)
    {
        paths.mcRoot = std::filesystem::path(mcRoot);
    }

    setIoPaths(paths);
    RUNTIME_LOG("[io:mounts] cd=\"" << getIoPaths().cdRoot.string()
                << "\" mc=\"" << getIoPaths().mcRoot.string() << "\"" << std::endl);
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}


std::atomic<bool> g_ps2DumpRamRequest{false}; // F8 RAM snapshot (ps2_pad.cpp)
extern std::atomic<float> g_gsInterpLastPhase;  // gs_gpu_renderer.cpp, for PS2X_INTERP_LOG
extern std::atomic<float> g_gsInterpShownYaw;
bool g_ps2TraceBoolWrites = std::getenv("PS2X_WATCH_BOOL_WRITES") != nullptr;

#if defined(__linux__)
// PS2X_PC_SAMPLE=<file>[,<startFrame>]: SIGPROF-based sampling of the guest PC on
// the game thread (every 100 us of game-thread CPU time, from startFrame on),
// written as raw little-endian u32 PCs. Unlike PS2X_COUNT_CALLS this also sees
// functions reached through direct recompiled calls.
extern std::atomic<int> g_ps2GsGpuWorkerTid, g_ps2GsGpuWorkerClock;
namespace
{
    constexpr size_t kPcSampleCapacity = 1u << 22;
    uint32_t g_pcSamples[kPcSampleCapacity]; // host RIP - &pcSampleHandler (wraps mod 2^32)
    // [rsp] at the sample (the caller when RIP is in a frameless leaf such as
    // libc memcpy), same encoding; written to <file>.callers.
    uint32_t g_pcSampleCallers[kPcSampleCapacity];
    std::atomic<uint32_t> g_pcSampleCount{0};
    uint64_t g_pcSampleStartFrame = 0;
    PS2Runtime *g_pcSampleRuntime = nullptr;
    const std::atomic<uint64_t> *g_pcSampleFrameClock = &g_ps2PresentedFrameCounter;

    // Records the host instruction pointer (offset from this handler's address,
    // so the PIE load base cancels out); map with nm on ps2EntryRunner.
    void pcSampleHandler(int, siginfo_t *, void *contextPtr)
    {
        if (!g_pcSampleRuntime || g_pcSampleFrameClock->load(std::memory_order_relaxed) < g_pcSampleStartFrame)
            return;
        static const bool s_vif1Only = std::getenv("PS2X_PC_SAMPLE_VIF1") != nullptr &&
                                      std::getenv("PS2X_PC_SAMPLE_GPU") == nullptr;
        if (s_vif1Only && !PS2Memory::onAsyncVif1Worker())
            return;
        // ITIMER_PROF is process-wide, so samples land on whichever thread is
        // running. Without this the IOP worker's own lock/futex traffic is
        // attributed to the game thread. PS2X_PC_SAMPLE_ALL=1 keeps the old
        // whole-process behaviour.
        static const bool s_allThreads = std::getenv("PS2X_PC_SAMPLE_ALL") != nullptr;
        if (!s_allThreads && t_onIopWorker)
            return;
        const auto *context = static_cast<const ucontext_t *>(contextPtr);
        const uint64_t rip = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_RIP]);
        const uint32_t index = g_pcSampleCount.load(std::memory_order_relaxed);
        if (index < kPcSampleCapacity)
        {
            g_pcSamples[index] = static_cast<uint32_t>(rip - reinterpret_cast<uint64_t>(&pcSampleHandler));
            const uint64_t rsp = static_cast<uint64_t>(context->uc_mcontext.gregs[REG_RSP]);
            g_pcSampleCallers[index] = static_cast<uint32_t>(*reinterpret_cast<const uint64_t *>(rsp) -
                                                             reinterpret_cast<uint64_t>(&pcSampleHandler));
            g_pcSampleCount.store(index + 1u, std::memory_order_release);
        }
    }

    void startPcSampling(PS2Runtime *runtime)
    {
        const char *env = std::getenv("PS2X_PC_SAMPLE");
        if (!env)
            return;
        if (const char *comma = std::strchr(env, ','))
            g_pcSampleStartFrame = std::strtoull(comma + 1, nullptr, 10);
        // Select before installing the handler; its only clock operation is an
        // atomic load. Existing startFrame semantics remain the default.
        g_pcSampleFrameClock = std::getenv("PS2X_PC_SAMPLE_VSYNC")
                                  ? &runtime->memory().gs().vsyncTick : &g_ps2PresentedFrameCounter;
        g_pcSampleRuntime = runtime;
        std::cerr << "[profile] start=" << g_pcSampleStartFrame
                  << " clock=" << (g_pcSampleFrameClock == &g_ps2PresentedFrameCounter ? "frame" : "vsync")
                  << std::endl;
        struct sigaction action{};
        action.sa_sigaction = pcSampleHandler;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &action, nullptr);
        sigevent event{};
        event.sigev_notify = SIGEV_THREAD_ID;
        event.sigev_signo = SIGPROF;
        event._sigev_un._tid = static_cast<pid_t>(syscall(SYS_gettid));
        clockid_t sampleClock = CLOCK_THREAD_CPUTIME_ID;
        const bool gpu = std::getenv("PS2X_PC_SAMPLE_GPU") != nullptr;
        if (gpu)
        {
            event._sigev_un._tid = static_cast<pid_t>(g_ps2GsGpuWorkerTid.load());
            sampleClock = static_cast<clockid_t>(g_ps2GsGpuWorkerClock.load());
            if (event._sigev_un._tid == 0)
            {
                std::cerr << "[profile] GPU worker unavailable; no samples armed" << std::endl;
                return;
            }
        }
        // PS2X_PC_SAMPLE_GS=1: sample GS rasterizer worker 0 instead.
        if (!gpu && std::getenv("PS2X_PC_SAMPLE_GS") && g_ps2GsWorkerTid.load() != 0)
            event._sigev_un._tid = static_cast<pid_t>(g_ps2GsWorkerTid.load());
        // PS2X_PC_SAMPLE_VIF1=1: the async VIF1/VU1 worker arms its own timer
        // (ps2PcSampleCurrentThread) instead.
        if (!gpu && std::getenv("PS2X_PC_SAMPLE_VIF1"))
            return;
        timer_t timer{};
        if (timer_create(sampleClock, &event, &timer) != 0)
        {
            std::perror("CPU profile timer");
            return;
        }
        itimerspec spec{};
        spec.it_interval.tv_nsec = 100000;
        spec.it_value.tv_nsec = 100000;
        timer_settime(timer, 0, &spec, nullptr);
        std::cerr << "[profile] thread=" << (gpu ? "gpu" : "ee") << " tid=" << event._sigev_un._tid
                  << " clock=thread-cpu startFrame=" << g_pcSampleStartFrame << std::endl;
    }

    void writePcSamples();
}

// Samples the calling thread's CPU time with the PS2X_PC_SAMPLE handler.
void ps2PcSampleCurrentThread()
{
    if (!std::getenv("PS2X_PC_SAMPLE") || !std::getenv("PS2X_PC_SAMPLE_VIF1"))
        return;
    sigevent event{};
    event.sigev_notify = SIGEV_THREAD_ID;
    event.sigev_signo = SIGPROF;
    event._sigev_un._tid = static_cast<pid_t>(syscall(SYS_gettid));
    timer_t timer{};
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &event, &timer) != 0)
        return;
    itimerspec spec{};
    spec.it_interval.tv_nsec = 100000;
    spec.it_value.tv_nsec = 100000;
    timer_settime(timer, 0, &spec, nullptr);
}

namespace
{
    void writePcSamples()
    {
        const char *env = std::getenv("PS2X_PC_SAMPLE");
        if (!env)
            return;
        std::string path(env);
        path = path.substr(0, path.find(','));
        const uint32_t count = g_pcSampleCount.load(std::memory_order_acquire);
        if (FILE *f = std::fopen(ps2x::narrow(path).c_str(), "wb"))
        {
            std::fwrite(g_pcSamples, sizeof(uint32_t), count, f);
            std::fclose(f);
        }
        if (FILE *f = std::fopen((path + ".callers").c_str(), "wb"))
        {
            std::fwrite(g_pcSampleCallers, sizeof(uint32_t), count, f);
            std::fclose(f);
        }
        // <file>.maps: the handler's absolute address followed by
        // /proc/self/maps, so samples outside the executable (libc, libm, ...)
        // can be attributed.
        if (FILE *f = std::fopen((path + ".maps").c_str(), "w"))
        {
            std::fprintf(f, "handler %llx\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&pcSampleHandler)));
            if (FILE *maps = std::fopen("/proc/self/maps", "r"))
            {
                char line[512];
                while (std::fgets(line, sizeof(line), maps))
                    std::fputs(line, f);
                std::fclose(maps);
            }
            std::fclose(f);
        }
    }
}
#else
// The SIGPROF sampler is Linux-only (PS2X_PC_SAMPLE does nothing elsewhere).
namespace
{
    void startPcSampling(PS2Runtime *) {}
    void writePcSamples() {}
}
void ps2PcSampleCurrentThread() {}
#endif

// PS2X_VU1_TRACE=<frame>[,<count>]: from that presented frame on, log <count> (default 8)
// VU1 microprogram starts with the input buffer at TOP, plus the GIF packets they XGKICK.
std::atomic<int32_t> g_ps2Vu1TraceRemaining{0};
std::atomic<int32_t> g_ps2Vu1TraceKicksRemaining{0};

static void ps2Vu1TraceMscal(const uint8_t *vu1Data, uint32_t startPC, uint32_t top, uint32_t itop)
{
    static const char *s_env = std::getenv("PS2X_VU1_TRACE");
    if (!s_env)
    {
        return;
    }
    static const uint64_t s_frame = std::strtoull(s_env, nullptr, 10);
    static const int32_t s_count = []() -> int32_t
    {
        const char *comma = std::strchr(std::getenv("PS2X_VU1_TRACE"), ',');
        return comma ? static_cast<int32_t>(std::strtol(comma + 1, nullptr, 10)) : 8;
    }();
    static bool s_armed = false;
    if (!s_armed)
    {
        if (g_ps2PresentedFrameCounter.load(std::memory_order_relaxed) < s_frame)
        {
            return;
        }
        s_armed = true;
        g_ps2Vu1TraceRemaining.store(s_count, std::memory_order_relaxed);
        g_ps2Vu1TraceKicksRemaining.store(s_count * 3, std::memory_order_relaxed);
    }
    if (g_ps2Vu1TraceRemaining.load(std::memory_order_relaxed) <= 0)
    {
        return;
    }
    const int32_t traceIndex = g_ps2Vu1TraceRemaining.fetch_sub(1, std::memory_order_relaxed);
    if (const char *dumpDir = std::getenv("PS2X_VU1_DUMP_DIR"))
    {
        // Dump VU1 data memory at each traced MSCAL (before the program runs).
        const std::string path = std::string(dumpDir) + "/vu1data_" + std::to_string(traceIndex) +
                                 "_pc" + std::to_string(startPC) + ".bin";
        if (FILE *f = std::fopen(ps2x::narrow(path).c_str(), "wb"))
        {
            std::fwrite(vu1Data, 1, PS2_VU1_DATA_SIZE, f);
            std::fclose(f);
        }
    }
    std::ostringstream out;
    out << "[vu1:mscal] pc=0x" << std::hex << startPC << " top=0x" << top << " itop=0x" << itop << " in@top:";
    for (uint32_t q = 0; q < 6u; ++q)
    {
        const uint32_t off = ((top + q) & 0x3FFu) * 16u;
        uint32_t w[4];
        std::memcpy(w, vu1Data + off, 16u);
        out << " [" << w[0] << " " << w[1] << " " << w[2] << " " << w[3] << "]";
    }
    std::cerr << out.str() << std::dec << std::endl;
}



bool g_ps2WriteWatchActive = std::getenv("PS2X_WATCH_WRITE") != nullptr;

void ps2CheckWatchedWrite(uint32_t addr, uint32_t size, uint64_t value, const R5900Context *ctx, const char *op)
{
    bool hit = g_ps2WriteWatchAddr != 0u && (g_ps2WriteWatchAddr - (addr & PS2_RAM_MASK)) < size;
    if (!hit && !g_ps2WriteWatchSet.empty())
        for (uint32_t i = 0; i < size && !hit; ++i)
            hit = g_ps2WriteWatchSet.count((addr & PS2_RAM_MASK) + i) != 0u;
    if (hit)
        ps2ReportWatchedWrite(addr, size, value, ctx, op);
}

uint32_t g_ps2WriteWatchAddr = []() -> uint32_t
{
    const char *env = std::getenv("PS2X_WATCH_WRITE");
    return env ? static_cast<uint32_t>(std::strtoul(env, nullptr, 0)) & PS2_RAM_MASK : 0u;
}();

std::unordered_set<uint32_t> g_ps2WriteWatchSet = []()
{
    // The env also accepts a comma-separated list; each byte inside a wider
    // store's range counts as hit, and reports include the presented frame.
    std::unordered_set<uint32_t> set;
    const char *env = std::getenv("PS2X_WATCH_WRITE");
    if (!env)
        return set;
    for (const char *p = env; *p;)
    {
        char *end = nullptr;
        const uint32_t a = static_cast<uint32_t>(std::strtoul(p, &end, 0)) & PS2_RAM_MASK;
        if (a != 0u)
            set.insert(a);
        p = (*end == ',') ? end + 1 : end;
        if (end == p)
            break;
    }
    return set;
}();
namespace
{
    // The executor publishes its RAM for optional write diagnostics. Device
    // threads without an executor context leave this unset; the shadow watch
    // can still capture their writes on the next guest dispatch.
    thread_local const uint8_t *s_writeWatchRam = nullptr;
}

void ps2ReportWatchedWrite(uint32_t addr, uint32_t size, uint64_t value, const R5900Context *ctx, const char *op)
{
    const uint32_t watchedWord = g_ps2WriteWatchAddr & ~3u;
    uint32_t oldWord = 0u, observedWord = 0u;
    bool predicted = false;
    if (s_writeWatchRam && watchedWord <= PS2_RAM_SIZE - 4u)
    {
        std::memcpy(&oldWord, s_writeWatchRam + watchedWord, 4u);
        observedWord = oldWord;
        // Store watches run before the store. Host range watches may run
        // after a copy; their value argument is a placeholder, not copied data.
        const bool scalar = op && (std::strcmp(op, "SB") == 0 || std::strcmp(op, "SH") == 0 ||
                                  std::strcmp(op, "SW") == 0 || std::strcmp(op, "SD") == 0 ||
                                  std::strcmp(op, "SQ") == 0);
        if (scalar)
        {
            const uint32_t physical = addr & PS2_RAM_MASK;
            for (uint32_t byte = 0u; byte < 4u; ++byte)
            {
                const uint32_t offset = watchedWord + byte - physical;
                if (offset < size && offset < sizeof(value))
                {
                    const uint32_t shift = byte * 8u;
                    observedWord = (observedWord & ~(0xFFu << shift)) |
                                   (static_cast<uint32_t>((value >> (offset * 8u)) & 0xFFu) << shift);
                    predicted = true;
                }
            }
        }
        static const std::string snapshotDir = []
        {
            const char *directory = std::getenv("PS2X_WATCH_SNAPSHOT_DIR");
            return directory ? std::string(directory) : std::string{};
        }();
        static const bool hasWord = std::getenv("PS2X_WATCH_SNAPSHOT_WORD") != nullptr;
        static const uint32_t snapshotWord = []
        {
            const char *word = std::getenv("PS2X_WATCH_SNAPSHOT_WORD");
            return word ? static_cast<uint32_t>(std::strtoul(word, nullptr, 0)) : 0u;
        }();
        static const uint64_t startFrame = []
        {
            const char *frame = std::getenv("PS2X_WATCH_SNAPSHOT_START_FRAME");
            return frame ? std::strtoull(frame, nullptr, 0) : 0ull;
        }();
        static std::atomic<bool> captured{false};
        if (!snapshotDir.empty() && hasWord && observedWord == snapshotWord &&
            g_ps2PresentedFrameCounter.load(std::memory_order_relaxed) >= startFrame && !captured.exchange(true))
        {
            std::error_code error;
            const std::filesystem::path directory(snapshotDir);
            std::filesystem::create_directories(directory, error);
            if (!error)
            {
                std::ofstream ram(directory / "watched-write.ram.bin", std::ios::binary);
                ram.write(reinterpret_cast<const char *>(s_writeWatchRam), PS2_RAM_SIZE);
                std::ofstream registers(directory / "watched-write.registers.txt");
                registers << "op=" << (op ? op : "unknown") << " predicted=" << predicted
                          << " frame=" << g_ps2PresentedFrameCounter.load(std::memory_order_relaxed)
                          << " addr=0x" << std::hex << addr << " wordAddr=0x" << watchedWord
                          << " oldWord=0x" << oldWord << " observedWord=0x" << observedWord << '\n';
                if (ctx)
                {
                    // Keep the full 128-bit registers, HI/LO, FPU and VU0
                    // state too. The binary layout is native to this build.
                    std::ofstream context(directory / "watched-write.context.bin", std::ios::binary);
                    context.write(reinterpret_cast<const char *>(ctx), sizeof(*ctx));
                    registers << "pc=0x" << ctx->pc << " branchPc=0x" << ctx->branch_pc
                              << " delaySlot=" << ctx->in_delay_slot << " contextBytes=" << std::dec << sizeof(*ctx)
                              << std::hex << '\n';
                    for (uint32_t reg = 0u; reg < 32u; ++reg)
                        registers << "r" << std::dec << reg << "=0x" << std::hex << getRegU32(ctx, reg) << '\n';
                }
                registers << "dispatch=" << formatDispatchHistory() << '\n';
                std::cerr << "[watch-snapshot] saved=" << (ram.good() && registers.good())
                          << " predicted=" << predicted << " word=0x" << std::hex << observedWord
                          << std::dec << " directory=" << directory << std::endl;
            }
        }
    }
    static const uint32_t s_limit = []() -> uint32_t
    {
        const char *env = std::getenv("PS2X_WATCH_LIMIT");
        return env ? static_cast<uint32_t>(std::strtoul(env, nullptr, 0)) : 200u;
    }();
    static std::atomic<uint32_t> s_reports{0};
    if (s_reports.fetch_add(1, std::memory_order_relaxed) >= s_limit)
    {
        return;
    }
    std::cerr << "[watch-write] " << (op ? op : "?") << " addr=0x" << std::hex << addr
              << " size=" << std::dec << size << " value=0x" << std::hex << value;
    if (s_writeWatchRam)
        std::cerr << " wordAddr=0x" << watchedWord << " oldWord=0x" << oldWord
                  << " observedWord=0x" << observedWord << " predicted=" << predicted;
    if (ctx)
    {
        extern std::atomic<uint64_t> g_ps2PresentedFrameCounter;
        std::cerr << " frame=" << std::dec << g_ps2PresentedFrameCounter.load(std::memory_order_relaxed)
                  << " pc=0x" << std::hex << ctx->pc
                  << " ra=0x" << static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0))
                  << " sp=0x" << static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
        static const bool s_regs = std::getenv("PS2X_WATCH_REGS") != nullptr;
        if (s_regs)
        {
            for (int r = 1; r < 32; ++r)
                std::cerr << " r" << std::dec << r << "=0x" << std::hex << static_cast<uint32_t>(_mm_extract_epi32(ctx->r[r], 0));
            std::cerr << "\n  dispatch: " << formatDispatchHistory();
        }
    }
    std::cerr << std::dec << std::endl;
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    if (g_ps2WriteWatchAddr != 0u)
        s_writeWatchRam = m_memory.getRDRAM();
    // PS2X_SHADOW_WATCH=<addr>: report word changes (from any source, including
    // host-side writes that bypass the store watchpoint) at dispatch granularity.
    {
        static const uint32_t s_shadowAddr = []() -> uint32_t
        {
            const char *env = std::getenv("PS2X_SHADOW_WATCH");
            return env ? (static_cast<uint32_t>(std::strtoul(env, nullptr, 0)) & PS2_RAM_MASK & ~3u) : 0u;
        }();
        if (s_shadowAddr != 0u)
        {
            static uint32_t s_last = 0u;
            static uint32_t s_reports = 0u;
            uint32_t now = 0u;
            std::memcpy(&now, m_memory.getRDRAM() + s_shadowAddr, 4u);
            if (now != s_last && s_reports < 64u)
            {
                ++s_reports;
                std::cerr << std::hex << "[shadow-watch] addr=0x" << s_shadowAddr << " 0x" << s_last << " -> 0x" << now
                          << " entering=0x" << address << "\n  dispatch: " << formatDispatchHistory() << std::dec << std::endl;
                if (g_ps2WriteWatchAddr != 0u && (g_ps2WriteWatchAddr & ~3u) == s_shadowAddr)
                    ps2ReportWatchedWrite(s_shadowAddr, 4u, now, nullptr, "SHADOW");
            }
            s_last = now;
        }
    }
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }

        // Fallback: resolve to the nearest lower registered function. JAL+8
        // return sites and checkpoint-resume points inside a function do not
        // always have their own table slot; the containing function's entry
        // switch routes the PC to the correct internal label. The strict policy
        // (tests/CI) wants exact registration only.
        uint32_t scan = missingFunctionPolicy() == MissingFunctionPolicy::Stop ? 0u : slot;
        while (scan > 0u)
        {
            --scan;
            fn = g_ps2RecompiledFunctionTable[scan];
            if (fn != nullptr)
            {
                static std::unordered_set<uint32_t> s_fallbackLogged;
                if (s_fallbackLogged.insert(address).second && s_fallbackLogged.size() <= 64u)
                {
                    std::cerr << "[dispatch:fallback] pc=0x" << std::hex << address
                              << " -> table+0x" << (g_ps2RecompiledFunctionTableBase + (scan << 2))
                              << " trace=" << formatDispatchHistory() << std::dec << std::endl;
                }
                return fn;
            }
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
        if (const char *dumpDir = std::getenv("PS2X_MISSING_TARGET_DUMP_DIR"); dumpDir && *dumpDir)
        {
            // Capture the failing EE call on its execution thread. Other device
            // workers are not stopped, so this is a diagnostic memory snapshot.
            std::error_code error;
            const std::filesystem::path directory(dumpDir);
            std::filesystem::create_directories(directory, error);
            if (!error && m_memory.getRDRAM())
            {
                std::ostringstream name;
                name << "missing_" << std::hex << sourcePc << "_" << targetPc;
                const auto path = directory / (name.str() + ".ram.bin");
                std::ofstream ram(path, std::ios::binary);
                ram.write(reinterpret_cast<const char *>(m_memory.getRDRAM()), PS2_RAM_SIZE);
                std::ofstream registers(directory / (name.str() + ".registers.txt"));
                registers << oss.str() << '\n';
                if (ctx)
                    for (uint32_t reg = 0u; reg < 32u; ++reg)
                        registers << "r" << std::dec << reg << "=0x" << std::hex << getRegU32(ctx, reg) << '\n';
                std::cerr << "[guest-branch:dump] saved=" << (ram.good() && registers.good())
                          << " path=" << path << std::endl;
            }
            else
                std::cerr << "[guest-branch:dump] failed: "
                          << (error ? error.message() : "EE RAM is not initialized") << std::endl;
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (std::getenv("PS2X_FATAL_MISSING_TARGET"))
    {
        std::ostringstream failure;
        failure << "Missing guest branch target 0x" << std::hex << targetPc
                << " from 0x" << sourcePc;
        throw std::runtime_error(failure.str());
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    // PS2X_STALL_TRACE=1: report host gaps > 5 ms between guest calls on the
    // game thread (what holds a VBlank up).
    static const bool s_stallTrace = std::getenv("PS2X_STALL_TRACE") != nullptr;
    if (s_stallTrace)
    {
        static thread_local auto s_last = std::chrono::steady_clock::now();
        static thread_local uint32_t s_lastTarget = 0, s_lastSource = 0;
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - s_last).count();
        if (ms > 5.0)
            std::fprintf(stderr, "[stall] %.1f ms between call 0x%x (from 0x%x) and call 0x%x (from 0x%x) f=%llu\n", ms, s_lastTarget,
                         s_lastSource, targetPc, sourcePc, (unsigned long long)g_ps2PresentedFrameCounter.load());
        s_last = now;
        s_lastTarget = targetPc;
        s_lastSource = sourcePc;
    }
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        return false;
    }

    if (!isCall)
    {
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        return false;
    }

    if (!hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            // if you need the app to keep open to open debug pannel change this to false
            return false;
        }

        return false;
    }

    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    const uint32_t entrySp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    targetFn(rdram, ctx, this);

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }
    // The callee switched threads: unwind to the dispatcher (ctx->pc resumes).
    if (m_eeScheduler && m_eeScheduler->unwindByReturnPending())
    {
        return false;
    }

    // Handlers bound straight into the function table may return without touching
    // pc; treat that as a completed call. Only when $sp is unchanged, though: in
    // recursion a deeper call to this same target can stop at a scheduler safe point
    // with pc == target, leaving its callers' frames live on the guest stack. Taking
    // that for a return resumes the caller one or more frames too deep.
    if (ctx->pc == entryPc &&
        static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) == entrySp)
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}


void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    m_vu0.reset(); // zeroes the cycle counter, so measure from here
    VuStatsScope statsScope(0, m_vu0.state().cycles);
    copyVu0ContextToState(ctx, m_vu0.state());
    // VU0 programs with no compiled version are recorded for the next Optimize
    // (see ps2VuLearnFile) in the VU1 capture format, tagged 'CVU0' so the
    // tooling can tell VU0's 4 KB code/data from VU1's 16 KB. VU0 microcode is
    // loaded by the game in gameplay, not at boot.
    static std::unordered_map<uint64_t, uint32_t> s_vu0Captured;
    FILE *recordTo = nullptr;
    if (m_vu0.lookupCompiled(vu0Code, PS2_VU0_CODE_SIZE, &m_memory, startPC) == nullptr)
    {
        uint32_t &runs = s_vu0Captured[ps2Vu1ProgramKey(vu0Code, PS2_VU0_CODE_SIZE, startPC & 0xFF8u)];
        if (runs < 4u)
        {
            ++runs;
            recordTo = ps2VuLearnFile(0);
        }
    }
    if (recordTo)
    {
        const uint32_t header[4] = {0x30555643u /* 'CVU0' */, startPC, 0u, ctx->vu0_itop};
        std::fwrite(header, sizeof(header), 1, recordTo);
        std::fwrite(&m_vu0.state(), sizeof(VU1State), 1, recordTo);
        std::fwrite(vu0Code, 1, PS2_VU0_CODE_SIZE, recordTo);
        std::fwrite(vu0Data, 1, PS2_VU0_DATA_SIZE, recordTo);
    }
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, 4096);
    if (recordTo)
    {
        std::fwrite(&m_vu0.state(), sizeof(VU1State), 1, recordTo);
        std::fwrite(vu0Data, 1, PS2_VU0_DATA_SIZE, recordTo);
        std::fflush(recordTo);
    }
    copyVu0StateToContext(m_vu0.state(), ctx);
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::pollAsyncDma()
{
    if (!m_memory.asyncVif1Enabled())
        return;
    if (R5900Context *context = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr)
        g_asyncVif1Fbrst.store(context->vu0_fbrst, std::memory_order_relaxed);
    if (m_memory.pollAsyncVif1())
        drainCompletedDmacHandlers(m_memory.getRDRAM());
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

uint32_t PS2Runtime::hleMalloc(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }
    const uint32_t align = std::max<uint32_t>(16u, normalizeGuestHeapAlignment(alignment));
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        uint32_t cursor = kHleArenaBase;
        for (auto it = m_hleArenaBlocks.begin();; ++it)
        {
            const uint32_t candidate = (cursor + align - 1u) & ~(align - 1u);
            const uint32_t gapEnd = (it == m_hleArenaBlocks.end()) ? kHleArenaEnd : it->first;
            if (candidate <= gapEnd && gapEnd - candidate >= allocSize)
            {
                m_hleArenaBlocks.emplace(candidate, allocSize);
                return candidate;
            }
            if (it == m_hleArenaBlocks.end())
            {
                break;
            }
            cursor = it->first + it->second;
        }
    }
    static bool s_warned = false;
    if (!s_warned)
    {
        s_warned = true;
        std::cerr << "[hle-arena] exhausted; falling back to the guest heap (size=0x" << std::hex << size << std::dec << ")" << std::endl;
    }
    return guestMalloc(size, alignment);
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    const uint32_t phys = guestAddr & PS2_RAM_MASK;
    if (phys >= kHleArenaBase && phys < kHleArenaEnd)
    {
        m_hleArenaBlocks.erase(phys);
        return;
    }
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    return m_eeScheduler->checkpointDue(cycles);
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
    ps2StartHostProfiler(); // PS2X_HOST_PROFILE (Windows)
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    const bool headless = std::getenv("PS2X_HEADLESS") != nullptr;
    Texture2D frameTex{};
    if (!headless)
    {
        Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
        frameTex = LoadTextureFromImage(blank);
        UnloadImage(blank);
    }

    std::atomic<bool> gameThreadFinished{false};
    std::exception_ptr gameThreadError;

    std::thread gameThread([&]()
                           {
        ps2RaiseCurrentThreadPriority(); // feeds the audio ring too: keep it scheduled evenly
        ThreadNaming::SetCurrentThreadName("GameThread");
        try
        {
            // The launch menu (settings before the game starts) holds the game.
            while (g_ps2LaunchMenuPending.load(std::memory_order_relaxed) && !isStopRequested())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!isStopRequested())
            {
                m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
                startPcSampling(this);
                m_eeScheduler->run();
            }
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
            gameThreadError = std::current_exception();
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
            gameThreadError = std::current_exception();
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    uint64_t tick = 0;
    auto nextHeadlessReport = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    const uint64_t exitFrame = []() -> uint64_t
    {
        const char *env = std::getenv("PS2X_EXIT_AFTER_FRAME");
        return env ? std::strtoull(env, nullptr, 10) : 0u;
    }();
    const uint64_t exitVsync = []() -> uint64_t
    {
        const char *env = std::getenv("PS2X_EXIT_AFTER_VSYNC");
        return env ? std::strtoull(env, nullptr, 10) : 0u;
    }();
    const uint64_t shotVsyncEvery = []() -> uint64_t
    {
        const char *env = std::getenv("PS2X_SCREENSHOT_VSYNC_EVERY");
        return env ? std::strtoull(env, nullptr, 10) : 0u;
    }();
    uint64_t lastShotVsyncBucket = 0;
    uint64_t hostFrames = 0;
    const bool speedStats = std::getenv("PS2X_SPEED_STATS") != nullptr;
    auto speedStart = std::chrono::steady_clock::now();
    uint64_t speedTick = 0u;
    uint64_t speedCompiled = 0u, speedInterpreted = 0u;
    uint64_t speedVu0Compiled = 0u, speedVu0Interpreted = 0u;
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        const uint64_t virtualTick = m_memory.gs().vsyncTick.load(std::memory_order_acquire);
        const auto speedNow = std::chrono::steady_clock::now();
        const double speedSeconds = std::chrono::duration<double>(speedNow - speedStart).count();
        if (speedStats && speedSeconds >= 5.0)
        {
            // Display-field progress, independent of host window refresh/FPS.
            // The scheduler currently uses a 16667 us NTSC field period.
            std::ostringstream report;
            const uint64_t compiled = g_ps2Vu1CompiledCalls.load(std::memory_order_relaxed);
            const uint64_t interpreted = g_ps2Vu1InterpretedCalls.load(std::memory_order_relaxed);
            const uint64_t vu0Compiled = g_ps2Vu0CompiledCalls.load(std::memory_order_relaxed);
            const uint64_t vu0Interpreted = g_ps2Vu0InterpretedCalls.load(std::memory_order_relaxed);
            report << "[speed] vsync=" << virtualTick << " fields=" << (virtualTick - speedTick)
                   << " hostFrames=" << hostFrames
                   << " wallSeconds=" << speedSeconds
                   << " fieldsPerSecond=" << (virtualTick - speedTick) / speedSeconds
                   << " percent=" << (virtualTick - speedTick) * 0.016667 * 100.0 / speedSeconds
                   << " compiledVu1Calls=" << (compiled - speedCompiled)
                   << " interpretedVu1Calls=" << (interpreted - speedInterpreted)
                   << " compiledVu0Calls=" << (vu0Compiled - speedVu0Compiled)
                   << " interpretedVu0Calls=" << (vu0Interpreted - speedVu0Interpreted);
            std::cerr << report.str() << std::endl;
            speedStart = speedNow;
            speedTick = virtualTick;
            speedCompiled = compiled;
            speedInterpreted = interpreted;
            speedVu0Compiled = vu0Compiled;
            speedVu0Interpreted = vu0Interpreted;
        }
        // A separate guest-progress limit works in both windowed and headless
        // runs without changing the existing frame clock used by pad scripts,
        // watchpoints and legacy captures. The host polls, so a small overshoot
        // is possible; report the actual counters and use normal shutdown.
        if (exitVsync != 0u && virtualTick >= exitVsync)
        {
            std::cerr << "[run] PS2X_EXIT_AFTER_VSYNC reached vsync=" << virtualTick
                      << " hostFrames=" << hostFrames << " target=" << exitVsync << std::endl;
            requestStop();
            break;
        }
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);
                if ((tick % 600) == 0)
                {
                    for (const auto &t : eeSnapshot.threads)
                    {
                        RUNTIME_LOG("[run:thread] id=" << t.id
                                                       << " status=" << static_cast<int>(t.status)
                                                       << " wait=" << static_cast<int>(t.waitReason)
                                                       << " waitId=" << t.waitId
                                                       << " prio=" << t.currentPriority
                                                       << std::hex << " pc=0x" << t.pc
                                                       << " ra=0x" << t.ra
                                                       << " sp=0x" << t.sp
                                                       << " entry=0x" << t.entry
                                                       << std::dec << " inv=" << t.invocationDepth
                                                       << std::endl);
                    }
                }

            }
        });
        if (headless)
        {
            // Headless input scripts and diagnostics use virtual display ticks
            // as their frame clock; the windowed presentation loop is absent.
            g_ps2PresentedFrameCounter.store(virtualTick, std::memory_order_relaxed);
            if (exitFrame != 0u && virtualTick >= exitFrame)
            {
                std::cerr << "[run] PS2X_EXIT_AFTER_FRAME reached at virtual field " << virtualTick << std::endl;
                requestStop();
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextHeadlessReport)
            {
                const auto kernel = m_eeScheduler->snapshot();
                const auto gsSnapshot = m_gs.getDebugSnapshot();
                std::cerr << "[headless] pc=0x" << std::hex << m_debugPc.load(std::memory_order_relaxed)
                          << " ra=0x" << m_debugRa.load(std::memory_order_relaxed) << std::dec
                          << " running=" << kernel.runningThreadId
                          << " vsync=" << m_eeScheduler->currentVSyncTick()
                          << " dma=" << m_memory.dmaStartCount()
                          << " gif=" << m_memory.gifCopyCount()
                          << " gs=" << m_memory.gsWriteCount()
                          << " scanout=" << gsSnapshot.hostPresentationWidth << "x" << gsSnapshot.hostPresentationHeight
                          << " present=" << gsSnapshot.hasHostPresentationFrame << std::endl;
                for (const auto &thread : kernel.threads)
                {
                    std::cerr << "[headless:thread] id=" << thread.id
                              << " status=" << static_cast<unsigned>(thread.status)
                              << " wait=" << static_cast<unsigned>(thread.waitReason)
                              << " waitId=" << thread.waitId
                              << " invocations=" << thread.invocationDepth
                              << " pc=0x" << std::hex << thread.pc << " entry=0x" << thread.entry
                              << " ra=0x" << thread.ra << " sp=0x" << thread.sp
                              << std::dec << std::endl;
                    if (std::getenv("PS2X_HEADLESS_STACK") && thread.waitReason != EeWaitReason::None)
                    {
                        const uint32_t stack = thread.sp & PS2_RAM_MASK;
                        constexpr uint32_t wordCount = 24u;
                        if (stack <= PS2_RAM_SIZE - wordCount * sizeof(uint32_t))
                        {
                            std::cerr << "[headless:stack] id=" << thread.id << " addr=0x" << std::hex << stack << " words=";
                            for (uint32_t i = 0; i < wordCount; ++i)
                            {
                                uint32_t word = 0u;
                                std::memcpy(&word, m_memory.getRDRAM() + stack + i * sizeof(word), sizeof(word));
                                std::cerr << " " << word;
                            }
                            std::cerr << std::dec << std::endl;
                        }
                    }
                }
                // Capture the same latched RGBA buffer used by UploadFrame,
                // without creating a texture or calling any OpenGL function.
                if (const char *captureDir = std::getenv("PS2X_HEADLESS_CAPTURE_DIR"); captureDir && *captureDir)
                {
                    std::vector<uint8_t> pixels;
                    uint32_t width = 0u, height = 0u;
                    if (m_gs.copyLatchedHostPresentationFrame(pixels, width, height) &&
                        width != 0u && height != 0u &&
                        pixels.size() == static_cast<size_t>(width) * height * 4u)
                    {
                        size_t rgbNonzero = 0u;
                        for (size_t offset = 0u; offset < pixels.size(); offset += 4u)
                            rgbNonzero += (pixels[offset] | pixels[offset + 1u] | pixels[offset + 2u]) != 0u;
                        std::error_code ec;
                        const std::filesystem::path directory(captureDir);
                        std::filesystem::create_directories(directory, ec);
                        const auto path = directory / ("headless_" + std::to_string(m_eeScheduler->currentVSyncTick()) + ".png");
                        Image shot{pixels.data(), static_cast<int>(width), static_cast<int>(height), 1,
                                   PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                        const bool saved = !ec && ExportImage(shot, path.string().c_str());
                        std::cerr << "[headless:capture] saved=" << saved
                                  << " size=" << width << "x" << height << " rgbNonzero=" << rgbNonzero
                                  << " path=\"" << path.string() << "\"" << std::endl;
                    }
                }
                nextHeadlessReport = now + std::chrono::seconds(1);
            }
            // EE/VBlank timing is owned by the scheduler. No window polling,
            // audio device or OpenGL calls are needed for a startup observation.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        // The GPU renderer caps its presentation readback to the window (see
        // upscalePresentation); publish it before the frame is latched.
        {
            const uint32_t windowWidth = static_cast<uint32_t>(std::max(GetScreenWidth(), 0));
            const uint32_t windowHeight = static_cast<uint32_t>(std::max(GetScreenHeight(), 0));
            if (g_gsGpuPresentWidth.load(std::memory_order_relaxed) != windowWidth)
                g_gsGpuPresentWidth.store(windowWidth, std::memory_order_relaxed);
            if (g_gsGpuPresentHeight.load(std::memory_order_relaxed) != windowHeight)
                g_gsGpuPresentHeight.store(windowHeight, std::memory_order_relaxed);
        }
        Texture2D presentDraw = frameTex;
        UploadFrame(frameTex, this, presentWidth, presentHeight, presentDraw);

        // The reprojection pass needs a camera sample per presented picture. The
        // view-window hook only runs when the game sets the window, so reading the
        // camera there leaves it stale in between; the CCam is updated every
        // frame, so take the sample here, where a frame is actually being shown.
        // Once per *picture*, not per host frame: the reprojection deltas are
        // between pictures, and publishing twice per picture would halve them.
        // The same test tells the host when the game produced nothing, which is
        // where a synthesized frame goes.
        static uint64_t s_lastCameraTick = ~0ull;
        const uint64_t cameraTick = m_eeScheduler ? m_eeScheduler->currentVSyncTick() : 0u;
        const bool newPicture = cameraTick != s_lastCameraTick;
        if (newPicture)
        {
            s_lastCameraTick = cameraTick;
            ps2_game_overrides::publishCameraFrame(m_memory.getRDRAM());
            // Achievements are evaluated against the game's own frames, the way an
            // emulator's client evaluates them: a host frame that shows the same
            // game frame twice must not count twice, or a "hold this for a second"
            // condition would be met at half speed. Init is idempotent and does the
            // set's file read on the first call.
            ps2AchievementsInit(m_memory.getRDRAM());
            ps2AchievementsFrame(m_memory.getRDRAM());
            // g_ps2PresentedFrameCounter counts *pictures*, not host frames. It
            // used to advance with the host loop, which coincided at 60 Hz and
            // broke everything that gates on it once the host ran faster - most
            // visibly the vehicle mouse look, whose offset is applied only for a
            // frame or two after the camera update, so it stopped being applied
            // and the game's own recentring pulled the view behind the car.
            static uint64_t s_pictures = 0;
            g_ps2PresentedFrameCounter.store(++s_pictures, std::memory_order_relaxed);
        }

        // The display rate drives the host cap. Above 60 the host presents faster
        // than the game produces pictures, and the frames in between are the ones
        // the interpolation fills in.
        {
            static int s_appliedRate = -1;
            const uint32_t rate = std::max<uint32_t>(60u, g_gsFrameRate.load(std::memory_order_relaxed));
            // With VSync on and interpolation running, VSync alone sets the pace:
            // one interpolated frame per display refresh. Capping as well (120 on
            // a 180 Hz display) makes the swap land on alternate 1- and 2-refresh
            // intervals - frames shown for 5.6 / 11.1 ms in turn - which reads as
            // a bouncing frame rate and juddering motion. VSync off: the setting
            // caps the rate as before.
            const int wanted = (rate > 60u && ps2HostSettings().vsync) ? 0 : static_cast<int>(rate);
            if (wanted != s_appliedRate)
            {
                s_appliedRate = wanted;
                SetTargetFPS(wanted);
            }
        }
        // Frame interpolation (PS2X_FRAME_INTERP=1): on a frame the game produced
        // no new picture, draw one synthesized from the last composed frame,
        // warped forward by half the last camera motion, instead of repeating it.
        // Above 60 the setting itself is the request: the host presents faster
        // than the game produces pictures, and the frames in between have to be
        // synthesized. PS2X_FRAME_INTERP=1 forces it on regardless, for testing.
        static const bool s_forceInterp = std::getenv("PS2X_FRAME_INTERP") != nullptr;
        static int s_hostCaptureLeft = 0;
        // PS2X_FRAME_INTERP_OFF=1 keeps the host rate but drops the synthesis, so
        // "the host runs fast" and "the synthesis runs" can be told apart.
        static const bool s_interpOff = std::getenv("PS2X_FRAME_INTERP_OFF") != nullptr;
        // Cutscenes are left alone: their pictures reach the GS only a couple of
        // times a second, so the interpolation's pair of composed frames goes
        // stale between them and the screen freezes on the last composable
        // frame. Gameplay is where the interpolation belongs anyway.
        static const bool s_interpInCutscene = std::getenv("PS2X_INTERP_IN_CUTSCENE") != nullptr;
        const bool inCutscene = ps2_game_overrides::isInCutscene(m_memory.getRDRAM()) && !s_interpInCutscene;
        const bool interp = !s_interpOff && !inCutscene &&
                            (s_forceInterp || g_gsFrameRate.load(std::memory_order_relaxed) > 60u);
        // Tell the renderer, so it builds the overlay mask: a synthesized frame
        // must leave the HUD where it is.
        g_gsFrameInterp.store(interp, std::memory_order_relaxed);
        bool drewSynth = false;
        static double s_synthMs = 0.0; // the previous host frame's synthesis round trip
        // PS2X_HOST_TIMING=1: every 180 host frames, the spread of host frame
        // times and of the synthesis round trip (frame pacing).
        {
            static const bool s_timing = std::getenv("PS2X_HOST_TIMING") != nullptr;
            static std::vector<double> s_frames, s_synths;
            if (s_timing)
            {
                s_frames.push_back(GetFrameTime() * 1000.0);
                s_synths.push_back(s_synthMs);
                if (s_frames.size() >= 180u)
                {
                    auto report = [](std::vector<double> v, const char *what)
                    {
                        std::sort(v.begin(), v.end());
                        double sum = 0.0;
                        for (double x : v)
                            sum += x;
                        std::fprintf(stderr, " %s avg=%.2f p50=%.2f p99=%.2f max=%.2f", what, sum / v.size(), v[v.size() / 2],
                                     v[v.size() * 99 / 100], v.back());
                    };
                    std::fprintf(stderr, "[hosttiming]");
                    report(s_frames, "frameMs");
                    report(s_synths, "synthMs");
                    std::fprintf(stderr, "\n");
                    s_frames.clear();
                    s_synths.clear();
                }
            }
            s_synthMs = 0.0;
        }
                // The synthesized frame as pixels, when this platform cannot be handed a
        // texture - filled below, used when the draw happens.
        static std::vector<uint8_t> s_synthPixels;
        s_synthPixels.clear();
float synthPhase = -1.0f;
        // Every frame is synthesized when interpolating: the display runs one
        // picture behind the game, and each host frame shows the position
        // between the previous picture and the current one. The phase sweeps
        // 0 -> 1 across a picture interval, and at the picture itself it is
        // near 1, which is exactly the newest picture - so the two agree.
        if (interp)
        {
            // The phase is the renderer's (-1): it knows when the game frame it
            // interpolates from began and how long game frames take, on its own
            // clock. The host's VBlank clock does not - the pair changes when a
            // picture is composed, not when the host notices the VBlank - and the
            // game draws a new view only every 2-4 VBlanks anyway.
            // PS2X_INTERP_PHASE_FIXED=<t> pins the phase instead (a test aid).
            static const char *s_fixedPhase = std::getenv("PS2X_INTERP_PHASE_FIXED");
            const float phase = s_fixedPhase != nullptr ? static_cast<float>(std::atof(s_fixedPhase)) : -1.0f;
            synthPhase = phase;
            uint32_t synthWidth = 0, synthHeight = 0;
            const auto synthStart = std::chrono::steady_clock::now();
            uint32_t synthTexture = 0;
            if (s_fixedPhase != nullptr)
            {
                // Pinned phase (a test aid): the blocking form.
                synthTexture = m_gs.synthesizeFrame(phase, synthWidth, synthHeight);
            }
            else
            {
                // Non-blocking: ask for the frame this refresh will show and draw
                // the newest finished one. Waiting on the render thread here made
                // the host frame time follow its load (spikes to 30 ms in busy
                // scenes at 4x) - a bouncing frame rate on screen.
                const int refresh = GetMonitorRefreshRate(GetCurrentMonitor());
                const double period = (ps2HostSettings().vsync && refresh > 0)
                                          ? 1.0 / refresh
                                          : 1.0 / std::max<uint32_t>(60u, g_gsFrameRate.load(std::memory_order_relaxed));
                const double now =
                    std::chrono::duration<double>(synthStart.time_since_epoch()).count();
                m_gs.requestSynthesis(now + period);
                uint64_t seq = 0;
                if (!m_gs.latestSynthesis(synthTexture, synthWidth, synthHeight, seq))
                    synthTexture = 0;
                // Where the renderer cannot hand a texture over - its context cannot be
                // shared on Windows - the frame arrives as pixels instead and is uploaded
                // below. Both calls describe the same latest frame; pixels win when present.
                                if (!m_gs.latestSynthesisPixels(s_synthPixels))
                    s_synthPixels.clear();
            }
            s_synthMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - synthStart).count();
                        if (!s_synthPixels.empty() && synthWidth != 0u && synthHeight != 0u)
            {
                // Pixels, not a texture: upload into a host texture sized to match exactly,
                // which is what the composed frames do on this platform too.
                static Texture2D s_synthTex{};
                if (s_synthTex.id != 0 && (s_synthTex.width != static_cast<int>(synthWidth) ||
                                           s_synthTex.height != static_cast<int>(synthHeight)))
                {
                    UnloadTexture(s_synthTex);
                    s_synthTex.id = 0;
                }
                if (s_synthTex.id == 0)
                {
                    Image blank = GenImageColor(static_cast<int>(synthWidth), static_cast<int>(synthHeight), BLANK);
                    s_synthTex = LoadTextureFromImage(blank);
                    UnloadImage(blank);
                }
                if (s_synthPixels.size() == static_cast<size_t>(synthWidth) * synthHeight * 4u)
                {
                                        {
                        static bool s_reported = false;
                        if (!s_reported)
                        {
                            s_reported = true;
                            std::cerr << "[gs] frame interp: synthesized frames arrive as pixels (this platform "
                                         "cannot share the GL context), uploaded at " << synthWidth << "x"
                                      << synthHeight << std::endl;
                        }
                    }
UpdateTexture(s_synthTex, s_synthPixels.data());
                    presentDraw = s_synthTex;
                    drewSynth = true;
                    presentWidth = synthWidth;
                    presentHeight = synthHeight;
                }
            }
            else if (synthTexture != 0u && synthWidth != 0u && synthHeight != 0u)
{
                {
                    static bool s_reported = false;
                    if (!s_reported)
                    {
                        s_reported = true;
                        std::cerr << "[gs] frame interp: first synthesized frame " << synthWidth << "x" << synthHeight
                                  << " at phase " << phase << std::endl;
                    }
                }
                // A plain GL texture, drawn directly: the renderer shares the
                // window's context, which is also why the shared present path
                // works. The id stays valid until the renderer reuses it.
                static Texture2D s_synth{};
                s_synth.id = synthTexture;
                s_synth.width = static_cast<int>(synthWidth);
                s_synth.height = static_cast<int>(synthHeight);
                s_synth.mipmaps = 1;
                s_synth.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                presentDraw = s_synth;
                drewSynth = true;
                presentWidth = synthWidth;
                presentHeight = synthHeight;
            }
        }

        m_padBackend.updateHostInput();
        if (IsKeyPressed(KEY_F10))
            ps2MusicStationNextTrack();
        ps2MusicStationUpdate(m_memory.getRDRAM());
        ps2ModsRefreshButtonPromptsIfNeeded(); // Auto prompt set follows the device
        ps2ModsApplyPatches(m_memory.getRDRAM());
        if (IsKeyPressed(KEY_F9))
        {
            ps2GsRequestCapture(180u); // ~3 s of GS input for tools/gs_replay
            // Also snapshot EE RAM here rather than relying on the pad layer's
            // key handling, which some sessions never reach.
            g_ps2DumpRamRequest.store(true, std::memory_order_relaxed);
        }
        // F8: capture the next 24 frames the *host* draws. A synthesized frame only
        // exists there, so a flicker that is not in the composed frame will show
        // here and nowhere else. PS2X_HOST_CAPTURE sets the prefix.
        if (IsKeyPressed(KEY_F8))
            s_hostCaptureLeft = 24;
        // PS2X_HOST_CAPTURE_AT=<hostFrame> starts the same capture without a key
        // press, so the path can be checked on its own.
        {
            static const char *s_captureAt = std::getenv("PS2X_HOST_CAPTURE_AT");
            static uint64_t s_hostFrames = 0;
            ++s_hostFrames;
            if (s_captureAt != nullptr && s_hostFrames == static_cast<uint64_t>(std::strtoull(s_captureAt, nullptr, 10)))
                s_hostCaptureLeft = 24;
        }
#if !defined(PLATFORM_VITA)
        ps2HostSettingsApplyWindow();
#endif
        ps2ModsApplyPatches(m_memory.getRDRAM()); // enabled mods' .pnach writes, once per frame
        const PS2HostSettings &hostSettings = ps2HostSettings();
        {
            static int s_filter = -1;
            static unsigned int s_filterTexture = 0;
            const int filter = hostSettings.smoothFiltering ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT;
            if (filter != s_filter || frameTex.id != s_filterTexture)
            {
                SetTextureFilter(frameTex, filter);
                s_filter = filter;
                s_filterTexture = frameTex.id;
            }
        }
        // PS2X_INTERP_LOG=1: one line per host frame - whether the game produced
        // a picture, whether this frame is synthesized, and the phase it used.
        // The phase is what moves the picture, so a phase that does not advance
        // smoothly is a picture that jitters. Printing only, so unlike a readback
        // this does not change the timing it is measuring.
        {
            static const bool s_interpLog = std::getenv("PS2X_INTERP_LOG") != nullptr;
            if (s_interpLog)
            {
                static uint64_t s_hostFrame = 0;
                if (++s_hostFrame < 240000u)
                    std::cerr << "[interp] f=" << s_hostFrame << " picture=" << (newPicture ? 1 : 0)
                              << " synth=" << (drewSynth ? 1 : 0) << " phase=" << synthPhase
                              << " t=" << g_gsInterpLastPhase.load(std::memory_order_relaxed)
                              << " shownYaw=" << g_gsInterpShownYaw.load(std::memory_order_relaxed)
                              << " draw=" << presentDraw.id << " size=" << presentWidth << "x" << presentHeight
                              << " screen=" << GetScreenWidth() << "x" << GetScreenHeight()
                              << " composed=" << g_gsComposedFrames.load(std::memory_order_relaxed)<< " camValid=" << g_ps2Camera.valid << " camPos=" << g_ps2Camera.pos[0] << ","
                              << g_ps2Camera.pos[1] << "," << g_ps2Camera.pos[2] << " camFront=" << g_ps2Camera.front[0]
                              << "," << g_ps2Camera.front[1] << "," << g_ps2Camera.front[2] << std::endl;
            }
        }
        BeginDrawing();
        ClearBackground(BLACK);
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        // A PS2 picture is shown at the TV's aspect, whatever its pixel dimensions
        // (field-mode games present 640x224/240 per field, frame-mode ones
        // 640x448/480): 4:3, or 16:9 for a game's anamorphic widescreen mode.
        const float displayAspect = hostSettings.aspect == PS2HostSettings::Aspect::Widescreen ? 16.0f / 9.0f
                                    : hostSettings.aspect == PS2HostSettings::Aspect::Stretch
                                        ? screenWidth / std::max(1.0f, screenHeight)
                                        : 4.0f / 3.0f;
        const float dstWidth = std::min(screenWidth, screenHeight * displayAspect);
        const float dstHeight = dstWidth / displayAspect;
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        const Rectangle dstRect{
            (screenWidth - dstWidth) * 0.5f,
            (screenHeight - dstHeight) * 0.5f,
            dstWidth,
            dstHeight};
        DrawTexturePro(presentDraw, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        if (g_ps2Crosshair.load(std::memory_order_relaxed))
        {
            // Free aim: a single dot, white over a dark outline, sized to the
            // picture.
            const float cx = dstRect.x + dstRect.width * 0.5f, cy = dstRect.y + dstRect.height * 0.5f;
            const float unit = std::max(1.0f, dstRect.height / 480.0f);
            DrawCircleV(Vector2{cx, cy}, 2.5f * unit, Color{0, 0, 0, 170});
            DrawCircleV(Vector2{cx, cy}, 1.5f * unit, Color{255, 255, 255, 235});
        }
        if (hostSettings.showFps)
        {
            // Two different rates, and the one that matters is the game's. The
            // host loop presents at the target frame rate whether or not the game
            // has produced a new picture, so GetFPS() alone reads the host rate.
            // The game's rate is the VBlank clock, which is what its logic runs
            // on; counting presented frames instead reads double once the host
            // loop is faster than the game.
            static uint64_t s_lastTicks = 0;
            static double s_lastSample = 0.0;
            static int s_gameFps = 0;
            const uint64_t ticks = m_eeScheduler ? m_eeScheduler->currentVSyncTick() : 0u;
            const double now = GetTime();
            const double elapsed = now - s_lastSample;
            if (elapsed >= 0.5)
            {
                s_gameFps = static_cast<int>((ticks - s_lastTicks) / elapsed + 0.5);
                s_lastTicks = ticks;
                s_lastSample = now;
            }
            DrawText(TextFormat("game %d fps", s_gameFps), 10, 10, 20, LIME);
            DrawText(TextFormat("host %d fps  %.1f ms", GetFPS(), GetFrameTime() * 1000.0f), 10, 34, 20, RAYWHITE);
        }
        // The unlock banner, over the game and under nothing: it is drawn whether
        // or not the fps overlay is on, because it is the game talking to the
        // player rather than a debug readout.
        ps2AchievementsDrawOverlay();
        // The host rate in the perf log too, independent of the overlay: the game
        // rate is already there and the two together are how a pacing change is
        // checked without a visible window.
        if (const char *perfPath = std::getenv("PS2X_PERF_LOG"))
        {
            static double s_lastHostLog = 0.0;
            const double hostNow = GetTime();
            if (hostNow - s_lastHostLog >= 5.0)
            {
                s_lastHostLog = hostNow;
                if (FILE *f = std::fopen(perfPath, "a"))
                {
                    // The game rate is the VBlank clock, the same source the
                    // overlay uses - logging it here makes the two checkable
                    // together without a visible window.
                    static uint64_t s_lastTickLog = 0;
                    static double s_lastTickTime = 0.0;
                    const uint64_t tickNow = m_eeScheduler ? m_eeScheduler->currentVSyncTick() : 0u;
                    const int gameFps =
                        s_lastTickTime > 0.0
                            ? static_cast<int>((tickNow - s_lastTickLog) / (hostNow - s_lastTickTime) + 0.5)
                            : 0;
                    s_lastTickLog = tickNow;
                    s_lastTickTime = hostNow;
                    std::fprintf(f, "[host] fps=%d frameMs=%.2f gameFps=%d\n", GetFPS(), GetFrameTime() * 1000.0f,
                                 gameFps);
                    std::fclose(f);
                }
            }
        }
        // PS2X_HOST_FRAMECHECK=1: read back the frame the host just drew, the way
        // the display will, and report how bright it is, averaged separately for
        // picture and synthesized frames. A synthesized frame being darker than a
        // picture frame is the whole question, and the two are interleaved, so a
        // single average hides it. This is what the eye actually sees; no readback
        // from the renderer's side can answer it.
        {
            static const bool s_frameCheck = std::getenv("PS2X_HOST_FRAMECHECK") != nullptr;
            if (s_frameCheck)
            {
                const int fw = GetScreenWidth(), fh = GetScreenHeight();
                if (fw > 0 && fh > 0)
                {
                    // raylib batches its draws and only submits them at
                    // EndDrawing, so without this the readback sees the buffer as
                    // ClearBackground left it, not what was drawn.
                    rlDrawRenderBatchActive();
                    unsigned char *px = rlReadScreenPixels(fw, fh);
                    if (px != nullptr)
                    {
                        const size_t n = static_cast<size_t>(fw) * static_cast<size_t>(fh);
                        uint64_t sum = 0;
                        for (size_t i = 0; i < n; ++i)
                            sum += px[i * 4u] + px[i * 4u + 1u] + px[i * 4u + 2u];
                        static uint64_t s_synthN = 0, s_picN = 0;
                        static double s_synthSum = 0.0, s_picSum = 0.0;
                        const double mean =
                            n != 0u ? static_cast<double>(sum) / static_cast<double>(n * 3u) : 0.0;
                        if (drewSynth)
                        {
                            ++s_synthN;
                            s_synthSum += mean;
                        }
                        else
                        {
                            ++s_picN;
                            s_picSum += mean;
                        }
                        if (((s_synthN + s_picN) % 240u) == 0u)
                            std::cerr << "[host] frame check: synth mean "
                                      << (s_synthN != 0u ? s_synthSum / static_cast<double>(s_synthN) : 0.0) << " over "
                                      << s_synthN << ", picture mean "
                                      << (s_picN != 0u ? s_picSum / static_cast<double>(s_picN) : 0.0) << " over "
                                      << s_picN << std::endl;
                        std::free(px);
                    }
                }
            }
        }
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        EndDrawing();

        // F8 / PS2X_HOST_CAPTURE: the frame the host drew. TakeScreenshot reads
        // after the swap, so a minority of these come back black - that is the
        // capture, not the renderer. Treat an isolated black frame here as
        // suspect; a run of them is real.
        if (const char *capturePrefix = std::getenv("PS2X_HOST_CAPTURE");
            capturePrefix != nullptr && s_hostCaptureLeft > 0)
        {
            char path[1024];
            std::snprintf(path, sizeof(path), "%s_%04d.png", capturePrefix, 24 - s_hostCaptureLeft);
            --s_hostCaptureLeft;
            TakeScreenshot(path);
            std::cerr << "[host] captured " << path << std::endl;
        }


        // Opt-in frame capture for headless bring-up: PS2X_SCREENSHOT_EVERY=<frames>
        // writes the presented guest frame (no debug UI) to frame_<n>.png in the cwd.
        // Windowed screenshots use host frames by default. The separate
        // PS2X_SCREENSHOT_VSYNC_EVERY option uses guest progress, with a bucket
        // crossing check so polling past an exact multiple does not miss a shot.
        {
            static const uint64_t s_shotEvery = []() -> uint64_t
            {
                const char *env = std::getenv("PS2X_SCREENSHOT_EVERY");
                return env ? std::strtoull(env, nullptr, 10) : 0u;
            }();
            static uint64_t s_presentedFrames = 0;
            ++s_presentedFrames;
            ++hostFrames; // the exit gates (PS2X_EXIT_AFTER_VSYNC) report it
            // g_ps2PresentedFrameCounter is *not* this: it counts pictures and is
            // updated above, where a new one is detected. This one is the host
            // frame count, which is what the dumps and the exit gates want.
            PS2_IF_AGRESSIVE_LOGS({
                static const bool s_traceOn = std::getenv("PS2X_GS_TRACE_FRAME") != nullptr;
                if (s_traceOn)
                    std::cout << "[gs:frame] " << s_presentedFrames << " vsync=" << m_eeScheduler->currentVSyncTick() << std::endl;
            });
            if ((s_presentedFrames % 1200u) == 0u)
                writePcSamples();
            // PS2X_VU0_DUMP=<prefix>: one-shot dump of VU0 code+data memory at
            // frame 300. Temporary probe: is the game's VU0 microcode resident
            // even when nothing calls it (the intro)?
            {
                static const char *s_vu0Dump = std::getenv("PS2X_VU0_DUMP");
                static bool s_vu0Dumped = false;
                if (s_vu0Dump != nullptr && !s_vu0Dumped && s_presentedFrames >= 300u)
                {
                    s_vu0Dumped = true;
                    if (FILE *f = std::fopen((std::string(s_vu0Dump) + "_code.bin").c_str(), "wb"))
                    {
                        std::fwrite(m_memory.getVU0Code(), 1, PS2_VU0_CODE_SIZE, f);
                        std::fclose(f);
                    }
                    if (FILE *f = std::fopen((std::string(s_vu0Dump) + "_data.bin").c_str(), "wb"))
                    {
                        std::fwrite(m_memory.getVU0Data(), 1, PS2_VU0_DATA_SIZE, f);
                        std::fclose(f);
                    }
                    std::cerr << "[vu0] dumped code+data to " << s_vu0Dump << "_*.bin" << std::endl;
                }
            }
            // PS2X_EXIT_AFTER_FRAME=<n>: exit cleanly at frame n (PGO training
            // runs need the profile written, which a signal would skip).
            {
                static const uint64_t s_exitFrame = []() -> uint64_t
                {
                    const char *env = std::getenv("PS2X_EXIT_AFTER_FRAME");
                    return env ? std::strtoull(env, nullptr, 10) : 0u;
                }();
                if (s_exitFrame != 0u && s_presentedFrames >= s_exitFrame)
                {
                    std::cerr << "[run] PS2X_EXIT_AFTER_FRAME reached at frame " << s_presentedFrames << std::endl;
                    // This path skips the normal shutdown by design, but the PC
                    // profile has to be written: ending a run this way is how a
                    // profile is captured, and _Exit would drop it.
                    writePcSamples();
                    if (__gcov_dump)
                        __gcov_dump();
                    if (__llvm_profile_write_file)
                        __llvm_profile_write_file();
                    // libgcov flushes its counters from its atexit handler, which
                    // _Exit skips - and an explicit __gcov_dump() does not replace
                    // it (measured: nothing is written). PS2X_PGO_EXIT=1 trades
                    // the hard exit for a clean one so a profile-generating run
                    // actually writes build/pgo. The window may linger while the
                    // static destructors run with worker threads alive; the
                    // profile is already written by then, so closing it is safe.
                    if (std::getenv("PS2X_PGO_EXIT") != nullptr)
                        std::exit(0);
                    std::_Exit(0);
                }
            }
            // PS2X_HASH_CHECK=<addr>: validate an STL hashtable (buckets vector at +4/+8,
            // nodes [next, value]) each frame and report the first bad link.
            {
                static const uint32_t s_hashAddr = []() -> uint32_t
                {
                    const char *env = std::getenv("PS2X_HASH_CHECK");
                    return env ? static_cast<uint32_t>(std::strtoul(env, nullptr, 0)) : 0u;
                }();
                static bool s_hashReported = false;
                if (s_hashAddr != 0u && !s_hashReported)
                {
                    const uint8_t *ram = m_memory.getRDRAM();
                    auto rd = [&](uint32_t a) -> uint32_t
                    {
                        uint32_t v = 0;
                        std::memcpy(&v, ram + (a & PS2_RAM_MASK & ~3u), 4u);
                        return v;
                    };
                    auto bad = [](uint32_t v)
                    { return v != 0u && ((v & 3u) != 0u || v < 0x100000u || v >= PS2_RAM_SIZE); };
                    const uint32_t begin = rd(s_hashAddr + 4u), end = rd(s_hashAddr + 8u);
                    if (begin != 0u && end > begin && (end - begin) < 0x100000u)
                    {
                        for (uint32_t b = begin; b < end && !s_hashReported; b += 4u)
                        {
                            uint32_t holder = b, node = rd(b);
                            for (uint32_t steps = 0; node != 0u && steps < 4096u; ++steps)
                            {
                                if (bad(node) || steps == 4095u)
                                {
                                    std::cerr << std::hex << "[hash-check] frame=" << std::dec << s_presentedFrames
                                              << std::hex << " bucket=" << ((b - begin) / 4u) << " holder=0x" << holder
                                              << " value=0x" << node << " begin=0x" << begin << " end=0x" << end
                                              << std::dec << std::endl;
                                    s_hashReported = true;
                                    break;
                                }
                                holder = node;
                                node = rd(node);
                            }
                        }
                    }
                }
            }
            // F8 (ps2_pad.cpp): EE RAM snapshot on request.
            if (g_ps2DumpRamRequest.exchange(false, std::memory_order_relaxed))
            {
                const std::string name = "ram_f8_" + std::to_string(s_presentedFrames) + ".bin";
                const std::filesystem::path path = std::filesystem::absolute(name);
                if (FILE *f = std::fopen(ps2x::narrow(path).c_str(), "wb"))
                {
                    std::fwrite(m_memory.getRDRAM(), 1, PS2_RAM_SIZE, f);
                    std::fclose(f);
                    std::cerr << "[dump] wrote " << path << std::endl;
                }
                else
                {
                    std::cerr << "[dump] FAILED to write " << path << std::endl;
                }
            }
            // PS2X_DUMP_RAM_FRAME=<frame>: write EE RAM + scratchpad to ram_<frame>.bin / spr_<frame>.bin.
            {
                // (a comma-separated list dumps at each frame)
                static const std::vector<uint64_t> s_dumpFrames = []()
                {
                    std::vector<uint64_t> frames;
                    if (const char *env = std::getenv("PS2X_DUMP_RAM_FRAME"))
                        for (const char *p = env; *p;)
                        {
                            char *end = nullptr;
                            frames.push_back(std::strtoull(p, &end, 10));
                            p = (*end == ',') ? end + 1 : end;
                            if (end == p && *p != 0)
                                break;
                        }
                    return frames;
                }();
                if (std::find(s_dumpFrames.begin(), s_dumpFrames.end(), static_cast<uint64_t>(s_presentedFrames)) != s_dumpFrames.end())
                {
                    const std::string ramPath = "ram_" + std::to_string(s_presentedFrames) + ".bin";
                    if (FILE *f = std::fopen(ps2x::narrow(ramPath).c_str(), "wb"))
                    {
                        std::fwrite(m_memory.getRDRAM(), 1, PS2_RAM_SIZE, f);
                        std::fclose(f);
                    }
                    const std::string sprPath = "spr_" + std::to_string(s_presentedFrames) + ".bin";
                    if (FILE *f = std::fopen(ps2x::narrow(sprPath).c_str(), "wb"))
                    {
                        std::fwrite(m_memory.getScratchpad(), 1, PS2_SCRATCHPAD_SIZE, f);
                        std::fclose(f);
                    }
                    std::cerr << "[ram:dump] frame=" << s_presentedFrames << std::endl;
                }
            }
            const uint64_t shotVsync = m_memory.gs().vsyncTick.load(std::memory_order_acquire);
            const uint64_t shotBucket = shotVsyncEvery != 0u ? shotVsync / shotVsyncEvery : 0u;
            const bool guestShot = shotVsyncEvery != 0u && shotBucket > lastShotVsyncBucket;
            if (guestShot || (shotVsyncEvery == 0u && s_shotEvery != 0u && (s_presentedFrames % s_shotEvery) == 0u))
            {
                lastShotVsyncBucket = shotBucket;
                Image shot = LoadImageFromTexture(presentDraw);
                ImageCrop(&shot, Rectangle{0.0f, 0.0f, srcWidth, srcHeight});
                ImageResize(&shot, 640, 480); // 4:3 display aspect, like the window
                char shotPath[64];
                std::snprintf(shotPath, sizeof(shotPath), guestShot ? "frame_vsync_%06llu.png" : "frame_%06llu.png",
                              static_cast<unsigned long long>(guestShot ? shotVsync : s_presentedFrames));
                const bool saved = ExportImage(shot, shotPath);
                if (guestShot)
                    std::cerr << "[run:screenshot] clock=vsync vsync=" << shotVsync
                              << " hostFrames=" << hostFrames << " saved=" << saved
                              << " path=" << shotPath << std::endl;
                UnloadImage(shot);
            }
        }

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();
#if !defined(PLATFORM_VITA)
    if (!headless)
        ps2HostSettingsSave(); // keeps a window size set by hand
#endif
    if (gameThread.joinable())
    {
        gameThread.join();
    }

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    writePcSamples();
    if (!headless)
    {
        UnloadTexture(frameTex);
        CloseWindow();
    }

    RUNTIME_LOG("[run] exiting loop");
    // join() synchronizes the worker's exception with this thread. Let main()
    // report failure instead of returning exit code zero after a guest crash.
    if (gameThreadError)
        std::rethrow_exception(gameThreadError);
}
