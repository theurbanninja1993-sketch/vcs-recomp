// Offline GS rasterizer benchmark. Replays a stream recorded with
// PS2X_GS_CAPTURE=<file>,<startFrame>,<frames> into a fresh GS (threaded CPU
// rasterizer, PS2X_GS_THREADS / PS2X_GS_SYNC as at runtime), times it, and
// prints a hash of the final local memory so rasterizer changes can be checked
// for identical output.
//
// usage: gs_replay <capture.bin> [passes]
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_mods.h" // PS2X_MODS_DIR texture packs (PS2X_TEXREPL_DUMP)
#include "runtime/ps2_host_settings.h" // renderer settings (motion blur and friends)
#include "ps2_runtime.h"

#include <csignal>
#include <sys/time.h>
#include <ucontext.h>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

// The runtime library references the generated function table; no guest code runs here.
const uint32_t g_ps2RecompiledFunctionTableBase = 0;
const uint32_t g_ps2RecompiledFunctionTableEnd = 0;
const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

namespace
{
    // GS_REPLAY_PROFILE=<file>: process-wide SIGPROF samples of the host RIP
    // (all rasterizer workers), as "offset from profileHandler" u32s.
    std::vector<uint32_t> g_samples;
    std::atomic<uint32_t> g_sampleCount{0};
    void profileHandler(int, siginfo_t *, void *contextPtr)
    {
        const auto *context = static_cast<const ucontext_t *>(contextPtr);
        const uint32_t index = g_sampleCount.fetch_add(1u, std::memory_order_relaxed);
        if (index < g_samples.size())
            g_samples[index] = static_cast<uint32_t>(context->uc_mcontext.gregs[REG_RIP] -
                                                     reinterpret_cast<uint64_t>(&profileHandler));
    }

    struct Record
    {
        uint32_t type = 0;
        std::vector<uint8_t> data;
    };

    uint64_t fnv1a(const uint8_t *data, size_t size)
    {
        uint64_t hash = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ data[i]) * 1099511628211ull;
        return hash;
    }
}

extern std::atomic<uint64_t> g_ps2PresentedFrameCounter;

// GS_REPLAY_FRAME_VOLUME=1: per presented frame, the GIF packet count and
// bytes fed to the GS (a proxy for how much geometry the guest sent).
static bool g_frameVolume = false;
static uint64_t g_framePackets = 0;
static uint64_t g_framePacketBytes = 0;

int main(int argc, char **argv)
{
    // Frame dumps must line up with the presents that produced them.
    setenv("PS2X_GS_GPU_SYNC_PRESENT", "1", 0);
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <capture.bin> [passes]\n", argv[0]);
        return 2;
    }
    const int passes = argc > 2 ? std::atoi(argv[2]) : 1;
    g_frameVolume = std::getenv("GS_REPLAY_FRAME_VOLUME") != nullptr;

    // Load texture packs (PS2X_MODS_DIR or the default mods folder) so the
    // replacement path - and PS2X_TEXREPL_DUMP's normal-texture hook - runs
    // during the replay, exactly like in game.
    ps2ModsRefresh();

    // Apply the renderer settings once, as the live loop does when the settings
    // are first read. Without this a replay ignores the motion blur (and anything
    // else the settings drive), which silently tests a different renderer.
    ps2HostSettingsApplyRenderer();

    // Optional stress of binding invalidation when targets are recreated.
    // No effect on normal replay. Each present selects the next scale.
    std::vector<uint32_t> scaleSequence;
    if (const char *value = std::getenv("GS_REPLAY_SCALE_SEQUENCE"))
    {
        const char *cursor = value;
        for (;;)
        {
            char *end = nullptr;
            const unsigned long scale = std::strtoul(cursor, &end, 10);
            if (end == cursor || scale < 1u || scale > 8u || (*end != ',' && *end != '\0'))
            {
                std::fprintf(stderr, "GS_REPLAY_SCALE_SEQUENCE requires comma-separated scales 1..8\n");
                return 2;
            }
            scaleSequence.push_back(static_cast<uint32_t>(scale));
            if (*end == '\0')
                break;
            cursor = end + 1;
        }
    }
    const char *resetAt = std::getenv("GS_REPLAY_RESET_AT_PRESENT");
    const uint64_t resetPresent = resetAt ? std::strtoull(resetAt, nullptr, 10) : ~0ull;

    std::vector<Record> records;
    std::vector<uint8_t> initialVram;
    if (FILE *f = std::fopen(argv[1], "rb"))
    {
        for (;;)
        {
            uint32_t header[2];
            if (std::fread(header, sizeof(header), 1, f) != 1)
                break;
            Record r;
            r.type = header[0];
            r.data.resize(header[1]);
            if (header[1] && std::fread(r.data.data(), 1, header[1], f) != header[1])
                break;
            if (r.type == 4u)
                initialVram = std::move(r.data);
            else
                records.push_back(std::move(r));
        }
        std::fclose(f);
    }
    size_t packets = 0, presents = 0;
    for (const Record &r : records)
    {
        packets += r.type == 1u;
        presents += r.type == 3u;
    }
    std::printf("%zu records (%zu GIF packets, %zu presents), initial VRAM %zu bytes\n", records.size(), packets,
                presents, initialVram.size());

    const char *profilePath = std::getenv("GS_REPLAY_PROFILE");
    if (profilePath)
    {
        g_samples.assign(1u << 22, 0u);
        struct sigaction action{};
        action.sa_sigaction = profileHandler;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &action, nullptr);
        itimerval timer{{0, 200}, {0, 200}};
        setitimer(ITIMER_PROF, &timer, nullptr);
    }

    double total = 0.0;
    uint64_t hash = 0;
    for (int pass = 0; pass < passes; ++pass)
    {
        std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
        if (!initialVram.empty())
            std::memcpy(vram.data(), initialVram.data(), std::min(vram.size(), initialVram.size()));
        GSRegisters regs{};
        auto gs = std::make_unique<GS>();
        gs->init(vram.data(), static_cast<uint32_t>(vram.size()), &regs);

        const auto t0 = std::chrono::steady_clock::now();
        // GS_REPLAY_LIMIT=<n>: replay only the first n records (divergence bisection).
        static const size_t s_limit = std::getenv("GS_REPLAY_LIMIT") ? std::strtoull(std::getenv("GS_REPLAY_LIMIT"), nullptr, 10) : records.size();
        size_t replayed = 0;
        uint64_t presentIndex = 0;
        for (const Record &r : records)
        {
            if (replayed++ >= s_limit)
                break;
            static const bool s_mark = std::getenv("GS_REPLAY_MARK") != nullptr;
            if (s_mark)
                std::fprintf(stderr, "[rec %zu] type=%u size=%zu\n", replayed, (unsigned)r.type, r.data.size());
            switch (r.type)
            {
            case 1u:
                gs->processGIFPacket(r.data.data(), static_cast<uint32_t>(r.data.size()));
                g_framePacketBytes += r.data.size();
                ++g_framePackets;
                break;
            case 2u:
                if (r.data.size() == 9u)
                {
                    uint64_t value = 0;
                    std::memcpy(&value, r.data.data() + 1, 8);
                    gs->writeRegister(r.data[0], value);
                }
                break;
            case 3u:
                if (g_frameVolume)
                    std::fprintf(stderr, "[vol] present packets=%llu bytes=%llu\n",
                                 (unsigned long long)g_framePackets, (unsigned long long)g_framePacketBytes);
                g_framePackets = 0;
                g_framePacketBytes = 0;
                if (!scaleSequence.empty())
                    g_gsGpuScaleRequest.store(scaleSequence[presentIndex % scaleSequence.size()], std::memory_order_relaxed);
                if (presentIndex == resetPresent)
                    gs->reset();
                ++presentIndex;
                if (r.data.size() == sizeof(GSRegisters))
                    std::memcpy(static_cast<void *>(&regs), r.data.data(), sizeof(GSRegisters));
                gs->latchHostPresentationFrame();
                g_ps2PresentedFrameCounter.fetch_add(1, std::memory_order_relaxed);
                // GS_REPLAY_FRAMES=<dir>: save every presented frame as raw RGBA
                // (<dir>/frame_<n>_<w>x<h>.rgba) for backend comparisons.
                if (const char *dir = std::getenv("GS_REPLAY_FRAMES"))
                {
                    static uint32_t s_frame = 0;
                    std::vector<uint8_t> pixels;
                    uint32_t w = 0, h = 0;
                    if (gs->copyLatchedHostPresentationFrame(pixels, w, h) && !pixels.empty())
                    {
                        char path[512];
                        std::snprintf(path, sizeof(path), "%s/frame_%04u_%ux%u.rgba", dir, s_frame, w, h);
                        if (FILE *out = std::fopen(path, "wb"))
                        {
                            std::fwrite(pixels.data(), 1, pixels.size(), out);
                            std::fclose(out);
                        }
                    }
                    ++s_frame;
                }
                break;
            default:
                break;
            }
        }
        gs.reset(); // workers finish every queued command before exiting
        total += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        hash = fnv1a(vram.data(), vram.size());
        if (const char *dump = std::getenv("GS_REPLAY_DUMP"))
            if (FILE *out = std::fopen(dump, "wb"))
            {
                std::fwrite(vram.data(), 1, vram.size(), out);
                std::fclose(out);
            }
        std::printf("pass %d: vramHash=%016llx\n", pass, static_cast<unsigned long long>(hash));
        // Draw/resolve/load totals: says whether a capture exercises the paths a
        // renderer change touches, and lets a change be checked against them as
        // well as the hash. The perf log's equivalents are gsDraws/gsRes/gsLoad/
        // gsBar/gsMem.
        {
            extern std::atomic<uint64_t> g_gsGpuDraws, g_gsGpuResolves, g_gsGpuLoads;
            extern std::atomic<uint64_t> g_gsGpuBarriers, g_gsGpuMemBarriers;
            extern std::atomic<uint64_t> g_gsGpuSyncAlias, g_gsGpuSyncTexture, g_gsGpuSyncStale;
            std::printf("  resolves by reason: alias=%llu texture=%llu stale=%llu\n",
                        static_cast<unsigned long long>(g_gsGpuSyncAlias.load()),
                        static_cast<unsigned long long>(g_gsGpuSyncTexture.load()),
                        static_cast<unsigned long long>(g_gsGpuSyncStale.load()));
            extern std::atomic<uint64_t> g_gsGpuResolveRows, g_gsGpuLoadRows, g_gsGpuDrawRows;
            std::printf("  rows: draws=%llu resolves=%llu loads=%llu\n",
                        static_cast<unsigned long long>(g_gsGpuDrawRows.load()),
                        static_cast<unsigned long long>(g_gsGpuResolveRows.load()),
                        static_cast<unsigned long long>(g_gsGpuLoadRows.load()));
            extern std::atomic<uint64_t> g_gsGpuStateSkips;
            extern std::atomic<uint64_t> g_gsGpuUniformSkips;
            std::printf("  stateSkips=%llu uniformSkips=%llu\n",
                        static_cast<unsigned long long>(g_gsGpuStateSkips.load()),
                        static_cast<unsigned long long>(g_gsGpuUniformSkips.load()));
            std::printf("  draws=%llu resolves=%llu loads=%llu texBarriers=%llu memBarriers=%llu\n",
                        static_cast<unsigned long long>(g_gsGpuDraws.load()), static_cast<unsigned long long>(g_gsGpuResolves.load()),
                        static_cast<unsigned long long>(g_gsGpuLoads.load()), static_cast<unsigned long long>(g_gsGpuBarriers.load()),
                        static_cast<unsigned long long>(g_gsGpuMemBarriers.load()));
        }
    }
    if (profilePath)
    {
        itimerval off{};
        setitimer(ITIMER_PROF, &off, nullptr);
        if (FILE *f = std::fopen(profilePath, "wb"))
        {
            std::fwrite(g_samples.data(), sizeof(uint32_t), std::min<size_t>(g_sampleCount.load(), g_samples.size()), f);
            std::fclose(f);
        }
    }
    std::printf("time=%.3fs per pass=%.3fs (%.2f ms/present)\n", total, total / passes,
                presents ? 1000.0 * total / passes / static_cast<double>(presents) : 0.0);
    return 0;
}
