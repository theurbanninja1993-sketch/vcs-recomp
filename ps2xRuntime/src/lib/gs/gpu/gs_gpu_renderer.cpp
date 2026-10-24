#include "runtime/gs/gpu/gs_gpu_renderer.h"

#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/ps2_camera.h"
#include "runtime/ps2_host_settings.h" // smoothFiltering: the shared display texture's filter

#include "ThreadNaming.h"
#include "raylib.h" // PS2X_TEXREPL_DUMP: ExportImage writes the dumped PNGs

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include "../../ps2_narrow.h"
namespace fs = std::filesystem;

std::atomic<uint32_t> g_gsGpuScaleRequest{0};
std::atomic<uint64_t> g_gsComposedFrames{0}; // successful GPU composes, for the interp log
std::atomic<float> g_gsInterpLastPhase{-1.0f}; // the phase the last synthesis used (PS2X_INTERP_LOG)
std::atomic<float> g_gsInterpShownYaw{0.0f};    // the camera yaw the host is shown (PS2X_INTERP_LOG)

    // Anisotropic filtering for replacement textures. Owner-measured at the
    // airport with the HD pack: 16 taps per sample at grazing angles cost about
    // half the frame rate (into the 30s), while 4 shows no slowdown at all - and
    // 4 is where anisotropic quality plateaus for most art. So 4 is the default,
    // and PS2X_TEXREPL_ANISO overrides it for testing (1 disables it).
    // The replay cannot measure this: it is GPU sampling cost, and the GPU is
    // asynchronous.
    static float replacementAniso()
    {
        static const float s_aniso = []()
        {
            const char *env = std::getenv("PS2X_TEXREPL_ANISO");
            return env ? static_cast<float>(std::atof(env)) : 4.0f;
        }();
        return s_aniso;
    }

std::atomic<bool> g_gsMotionBlurOff{false};
// The game's motion-blur buffers (TEX0 bases), set by its override: VCS's two
// accumulation buffers by default; LCS uses 12288 plus the front-buffer trail.
std::atomic<uint32_t> g_gsMotionBlurBase[2]{{13440u}, {14464u}};
std::atomic<bool> g_gsMotionBlurFrontTrail{false};
std::atomic<uint32_t> g_gsFrameRate{60};
std::atomic<bool> g_gsFrameInterp{false};
std::atomic<bool> g_gsModernBlur{false};
std::atomic<float> g_gsModernBlurStrength{0.0f};
// The host window size, published by the main thread each frame (0 = unknown).
// The upscaled display readback is capped to it: the composed frame is only
// ever shown at window size, so transferring more than that is pure waste.
// Draw calls issued to GL. One per state-change flush, so this is the number
// the GPU driver's own threads have to process each frame.
std::atomic<uint64_t> g_gsGpuDraws{0};
// GL state calls the cache skipped. Driver-independent: fewer calls is fewer
// calls, so this is the number to measure locally - the fps effect is only
// visible on the owner's driver.
std::atomic<uint64_t> g_gsGpuStateSkips{0};
std::atomic<uint64_t> g_gsGpuUniformSkips{0};
// Row volume of the two sync passes vs the real draws: says whether they are
// pixel-heavy (narrow them) or call-heavy (batch them).
std::atomic<uint64_t> g_gsGpuResolveRows{0};
std::atomic<uint64_t> g_gsGpuLoadRows{0};
std::atomic<uint64_t> g_gsGpuDrawRows{0};
// What the draw path asks the driver to do beyond the draws themselves. Both
// barriers are pipeline-synchronising operations, so their counts say whether
// the driver thread is paying for state churn or for synchronisation.
std::atomic<uint64_t> g_gsGpuResolves{0};
std::atomic<uint64_t> g_gsGpuLoads{0};
std::atomic<uint64_t> g_gsGpuReadbacks{0};
std::atomic<uint64_t> g_gsGpuBarriers{0};    // glTextureBarrier (feedback draws)
std::atomic<uint64_t> g_gsGpuMemBarriers{0}; // glMemoryBarrier (target resolves)
std::atomic<uint64_t> g_gsGpuSyncAlias{0};   // resolves because targets alias
std::atomic<uint64_t> g_gsGpuSyncTexture{0}; // resolves to feed a texture read
std::atomic<uint64_t> g_gsGpuSyncStale{0};   // resolves from the stale-row load path
std::atomic<uint64_t> g_gsGpuTargets{0};     // live targets (views of local memory)
std::atomic<uint32_t> g_gsGpuPresentWidth{0};
std::atomic<uint32_t> g_gsGpuPresentHeight{0};
// Set once the GL path is really running (see the Renderer constructor).
std::atomic<bool> g_gsGpuActive{false};
char g_gsGpuInfo[160]{};
extern std::atomic<uint64_t> g_ps2PresentedFrameCounter; // EeScheduler.cpp (dump index)

// ------------------------------------------------------------- dump worker
// PS2X_TEXREPL_DUMP writes from a background thread: PNG encoding on the
// render thread caused visible stutter while hunting textures. The queue is
// bounded - under backlog new dumps are dropped (this is a hunting tool).
namespace
{
    struct DumpJob
    {
        std::vector<uint8_t> rgba;
        int width = 0, height = 0;
        uint32_t tbp0 = 0, tbw = 0, psm = 0, tw = 0, th = 0;
        uint64_t frame = 0;
        char name[64] = {};
    };
    std::deque<DumpJob> g_dumpQueue;
    std::mutex g_dumpMutex;
    std::condition_variable g_dumpCv;
    std::atomic<bool> g_dumpStop{false};
    std::atomic<uint32_t> g_dumpQueued{0};

    void dumpWorkerThread()
    {
        const char *dir = std::getenv("PS2X_TEXREPL_DUMP");
        if (!dir || !*dir)
            return;
        FILE *index = std::fopen((std::string(dir) + "/_dump_index.log").c_str(), "a");
        for (;;)
        {
            DumpJob job;
            {
                std::unique_lock<std::mutex> lock(g_dumpMutex);
                g_dumpCv.wait(lock, []
                              { return g_dumpStop.load(std::memory_order_relaxed) || !g_dumpQueue.empty(); });
                if (g_dumpQueue.empty())
                {
                    if (g_dumpStop.load(std::memory_order_relaxed))
                        break;
                    continue;
                }
                job = std::move(g_dumpQueue.front());
                g_dumpQueue.pop_front();
            }
            ::Image image{job.rgba.data(), job.width, job.height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            ExportImage(image, (std::string(dir) + "/" + job.name).c_str());
            if (index)
            {
                std::fprintf(index, "%s tbp0=%u tbw=%u psm=%u tw=%u th=%u frame=%llu\n", job.name, job.tbp0, job.tbw,
                             job.psm, job.tw, job.th, static_cast<unsigned long long>(job.frame));
                std::fflush(index);
            }
            g_dumpQueued.fetch_sub(1, std::memory_order_relaxed);
        }
        if (index)
            std::fclose(index);
    }

    void enqueueDump(DumpJob &&job)
    {
        static std::thread *s_worker = []()
        {
            auto *thread = new std::thread(dumpWorkerThread);
            thread->detach(); // runs until process exit
            return thread;
        }();
        (void)s_worker;
        {
            std::lock_guard<std::mutex> lock(g_dumpMutex);
            if (g_dumpQueued.load(std::memory_order_relaxed) > 256u)
                return; // backlog: drop rather than grow memory
            g_dumpQueue.push_back(std::move(job));
            g_dumpQueued.fetch_add(1, std::memory_order_relaxed);
        }
        g_dumpCv.notify_one();
    }
}

namespace gsgpu
{
    namespace
    {
        // Adds the scope's duration to a stats counter.
        struct ScopeTimer
        {
            double &total;
            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            ~ScopeTimer() { total += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
        };

        // PS2X_TEXREPL_STATS: replacement-related render-thread time (seconds),
        // reported and reset on each stats line. mirror = decoding uploads into
        // the CPU copy that hashing reads; hash = texture/CLUT hashing.
        double g_texreplMirrorSecs = 0.0;
        double g_texreplHashSecs = 0.0;
        // Bytes behind those costs, per GS format (PSM & 0x3F): mirrored =
        // upload stream bytes decoded into the CPU copy; hashed = bytes the
        // texture hash read. Decides which formats an optimization helps.
        uint64_t g_texreplMirrorBytes[64] = {};
        uint64_t g_texreplHashBytes[64] = {};

        constexpr uint32_t kLocalMemoryBytes = 4u * 1024u * 1024u;
        constexpr uint32_t kPageBytes = 8192u;
        constexpr uint32_t kMaxTargetHeight = 1024u;

        // PSM codes (GS TEX0/FRAME/ZBUF encodings; Z formats carry 0x30).
        constexpr uint8_t PSM_CT32 = 0x00, PSM_CT24 = 0x01, PSM_CT16 = 0x02, PSM_CT16S = 0x0A;
        constexpr uint8_t PSM_T8 = 0x13, PSM_T4 = 0x14, PSM_T8H = 0x1B, PSM_T4HL = 0x24, PSM_T4HH = 0x2C;
        constexpr uint8_t PSM_Z32 = 0x30, PSM_Z24 = 0x31, PSM_Z16 = 0x32, PSM_Z16S = 0x3A;

        // Swizzle tables uploaded to the GPU, one per page layout: pixel index
        // within a page for (x, y) at block 0 (base blocks add whole blocks).
        struct SwizzleClass
        {
            uint32_t width, height, offset;
        };
        constexpr SwizzleClass kSwizzleClasses[8] = {
            {64u, 32u, 0u},      // C32 (also C24, T8H, T4HL, T4HH)
            {64u, 32u, 2048u},   // Z32 (also Z24)
            {64u, 64u, 4096u},   // C16
            {64u, 64u, 8192u},   // C16S
            {64u, 64u, 12288u},  // Z16
            {64u, 64u, 16384u},  // Z16S
            {128u, 64u, 20480u}, // T8
            {128u, 128u, 28672u} // T4
        };
        constexpr uint32_t kSwizzleEntries = 28672u + 16384u;

        struct PageShape
        {
            uint32_t width, height;
        };

        PageShape pageShape(uint8_t psm)
        {
            switch (psm)
            {
            case PSM_CT16:
            case PSM_CT16S:
            case PSM_Z16:
            case PSM_Z16S:
                return {64u, 64u};
            case PSM_T8:
                return {128u, 64u};
            case PSM_T4:
                return {128u, 128u};
            default:
                return {64u, 32u};
            }
        }

        bool is16Bit(uint8_t psm)
        {
            return psm == PSM_CT16 || psm == PSM_CT16S || psm == PSM_Z16 || psm == PSM_Z16S;
        }

        bool isPalettized(uint8_t psm)
        {
            return psm == PSM_T8 || psm == PSM_T4 || psm == PSM_T8H || psm == PSM_T4HL || psm == PSM_T4HH;
        }

        uint32_t bitsPerPixel(uint8_t psm)
        {
            return static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(psm)));
        }

        // Byte range of a buffer: whole pages from the base page.
        void bufferRange(uint32_t block, uint32_t bw, uint32_t height, uint8_t psm, uint32_t &begin, uint32_t &end)
        {
            const PageShape shape = pageShape(psm);
            const uint32_t strideWidth = std::max<uint32_t>(bw, 1u) * 64u;
            const uint32_t pagesWide = (strideWidth + shape.width - 1u) / shape.width;
            const uint32_t pagesHigh = (std::max<uint32_t>(height, 1u) + shape.height - 1u) / shape.height;
            const uint32_t baseBytes = block * 256u;
            begin = baseBytes & ~(kPageBytes - 1u);
            const uint32_t misalign = (baseBytes & (kPageBytes - 1u)) != 0u ? kPageBytes : 0u;
            const uint64_t bytes = static_cast<uint64_t>(pagesWide) * pagesHigh * kPageBytes + misalign;
            end = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(begin) + bytes, kLocalMemoryBytes));
        }

        // Builds the swizzle tables by reading a page of memory whose pixel
        // units hold their own index, so they match GSMem exactly.
        std::vector<uint32_t> buildSwizzleTables()
        {
            std::vector<uint32_t> tables(kSwizzleEntries, 0u);
            std::vector<uint8_t> memory(GSMem::MEMORY_SIZE, 0u);
            using Reader = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);
            struct Probe
            {
                Reader read;
                uint32_t unitBits;
            };
            const Probe probes[8] = {{GSMem::ReadCT32, 32u}, {GSMem::ReadZ32, 32u}, {GSMem::ReadCT16, 16u},
                                     {GSMem::ReadCT16S, 16u}, {GSMem::ReadZ16, 16u}, {GSMem::ReadZ16S, 16u},
                                     {GSMem::ReadP8, 8u}, {GSMem::ReadP4, 4u}};
            for (uint32_t cls = 0; cls < 8u; ++cls)
            {
                const SwizzleClass &shape = kSwizzleClasses[cls];
                const uint32_t units = shape.width * shape.height;
                const uint32_t bw = shape.width / 64u;
                // Each pass stores `unitBits` bits of every unit's index.
                const uint32_t bitsNeeded = 32u - static_cast<uint32_t>(__builtin_clz(units - 1u));
                for (uint32_t shift = 0; shift < bitsNeeded; shift += probes[cls].unitBits)
                {
                    for (uint32_t unit = 0; unit < units; ++unit)
                    {
                        const uint32_t part = (unit >> shift) & ((probes[cls].unitBits == 32u) ? 0xFFFFFFFFu : ((1u << probes[cls].unitBits) - 1u));
                        const uint32_t bit = unit * probes[cls].unitBits;
                        if (probes[cls].unitBits == 32u)
                            std::memcpy(&memory[bit / 8u], &part, 4u);
                        else if (probes[cls].unitBits == 16u)
                        {
                            const uint16_t v = static_cast<uint16_t>(part);
                            std::memcpy(&memory[bit / 8u], &v, 2u);
                        }
                        else if (probes[cls].unitBits == 8u)
                            memory[bit / 8u] = static_cast<uint8_t>(part);
                        else
                            memory[bit / 8u] = static_cast<uint8_t>((memory[bit / 8u] & ~(0xFu << (bit % 8u))) |
                                                                    (part << (bit % 8u)));
                    }
                    for (uint32_t y = 0; y < shape.height; ++y)
                        for (uint32_t x = 0; x < shape.width; ++x)
                        {
                            const uint32_t value = probes[cls].read(memory.data(), 0u, bw, x, y);
                            tables[shape.offset + y * shape.width + x] |= value << shift;
                        }
                }
            }
            return tables;
        }

        // ------------------------------------------------------------ shaders

        // VRAM access shared by every shader: swizzled pixel addressing and
        // field reads/writes, mirroring GSMem::PixelStorageTraits.
        const char *kVramGlsl = R"(
layout(std430, binding = 0) coherent buffer VramBuffer { uint vram[]; };
layout(std430, binding = 1) readonly buffer SwizzleBuffer { uint swizzle[]; };

uint psmClass(uint psm)
{
    switch (psm)
    {
    case 0x30u: case 0x31u: return 1u;
    case 0x02u: return 2u;
    case 0x0Au: return 3u;
    case 0x32u: return 4u;
    case 0x3Au: return 5u;
    case 0x13u: return 6u;
    case 0x14u: return 7u;
    default: return 0u;
    }
}

const uint kClassWidth[8] = uint[8](64u, 64u, 64u, 64u, 64u, 64u, 128u, 128u);
const uint kClassHeight[8] = uint[8](32u, 32u, 64u, 64u, 64u, 64u, 64u, 128u);
const uint kClassOffset[8] = uint[8](0u, 2048u, 4096u, 8192u, 12288u, 16384u, 20480u, 28672u);

// Word, bit shift and mask of pixel (x, y) of a buffer.
void vramField(uint psm, uint base, uint bw, uint x, uint y, out uint word, out uint shift, out uint mask)
{
    uint cls = psmClass(psm);
    uint pw = kClassWidth[cls], ph = kClassHeight[cls];
    uint page = base / 32u + (y / ph) * ((bw * 64u) / pw) + x / pw;
    uint addr = page * (pw * ph) + (base % 32u) * ((pw * ph) / 32u) + swizzle[kClassOffset[cls] + (y % ph) * pw + (x % pw)];
    if (cls <= 1u)
    {
        word = ((addr * 4u) & 0x3FFFFCu) >> 2;
        shift = 0u;
        mask = 0xFFFFFFFFu;
        if (psm == 0x01u || psm == 0x31u) mask = 0x00FFFFFFu;
        else if (psm == 0x1Bu) { shift = 24u; mask = 0xFFu; }
        else if (psm == 0x24u) { shift = 24u; mask = 0xFu; }
        else if (psm == 0x2Cu) { shift = 28u; mask = 0xFu; }
    }
    else if (cls <= 5u)
    {
        uint byteAddr = (addr * 2u) & 0x3FFFFEu;
        word = byteAddr >> 2;
        shift = (byteAddr & 2u) * 8u;
        mask = 0xFFFFu;
    }
    else if (cls == 6u)
    {
        uint byteAddr = addr & 0x3FFFFFu;
        word = byteAddr >> 2;
        shift = (byteAddr & 3u) * 8u;
        mask = 0xFFu;
    }
    else
    {
        uint bits = addr * 4u;
        uint byteAddr = (bits >> 3) & 0x3FFFFFu;
        word = byteAddr >> 2;
        shift = (byteAddr & 3u) * 8u + (bits & 7u);
        mask = 0xFu;
    }
}

uint readVram(uint psm, uint base, uint bw, uint x, uint y)
{
    uint word, shift, mask;
    vramField(psm, base, bw, x, y, word, shift, mask);
    return (vram[word] >> shift) & mask;
}

void writeVram(uint psm, uint base, uint bw, uint x, uint y, uint value)
{
    uint word, shift, mask;
    vramField(psm, base, bw, x, y, word, shift, mask);
    if (mask == 0xFFFFFFFFu)
    {
        vram[word] = value;
    }
    else
    {
        // Neighbouring fields of the word may be written concurrently.
        atomicAnd(vram[word], ~(mask << shift));
        atomicOr(vram[word], (value & mask) << shift);
    }
}

uint rgba5551To8888(uint c)
{
    return ((c & 0x1Fu) << 3) | (((c >> 5) & 0x1Fu) << 11) | (((c >> 10) & 0x1Fu) << 19) | (((c >> 15) & 1u) << 31);
}

uint rgba8888To5551(uint c)
{
    return ((c & 0xFFu) >> 3) | ((((c >> 8) & 0xFFu) >> 3) << 5) | ((((c >> 16) & 0xFFu) >> 3) << 10) | ((c >> 31) << 15);
}

bool is16BitPsm(uint psm)
{
    return psm == 0x02u || psm == 0x0Au || psm == 0x32u || psm == 0x3Au;
}
)";

        const char *kVertexShader = R"(#version 450 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in uint a_z;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec3 a_stq;
layout(location = 4) in float a_fog;
uniform vec2 u_targetSize;
out vec4 v_color;
noperspective out vec3 v_stq;
noperspective out float v_fog;
void main()
{
    vec2 ndc = a_pos / u_targetSize * 2.0 - 1.0;
    gl_Position = vec4(ndc, float(a_z) * (1.0 / 4294967296.0), 1.0);
    v_color = a_color;
    v_stq = a_stq;
    v_fog = a_fog;
}
)";

        const char *kFragmentShader = R"(
in vec4 v_color;
noperspective in vec3 v_stq;
noperspective in float v_fog;
layout(location = 0, index = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_factor;
layout(binding = 1) uniform sampler2D u_target;
layout(binding = 3) uniform sampler2D u_rtTex;
layout(binding = 4) uniform sampler2D u_replTex;
uniform int u_replace; // the texture is a replacement image (texels with GS-scale alpha)
uniform int u_rtScale; // 0: texture read from local memory; else sampled from a render target at this scale
layout(std430, binding = 2) readonly buffer ClutBuffer { uint clut[]; };
uniform int u_tme, u_tfx, u_tcc, u_fge;
uniform uvec4 u_tex;      // TBP0, TBW, PSM, CPSM
uniform uvec4 u_tex2;     // CSM, CSA, WMS | WMT << 2, linear
uniform ivec4 u_region;   // MINU, MAXU, MINV, MAXV
uniform ivec2 u_texSize;
uniform uvec3 u_texa;     // TA0, TA1, AEM
uniform int u_ate, u_atst, u_aref, u_afail;
uniform int u_abe, u_csel, u_pabe, u_date, u_datm, u_fba;
uniform vec4 u_blend; // s_k, s_c, d_k, d_c (alpha_s = C*s_k + s_c, alpha_d = C*d_k + d_c)
uniform float u_fix;
uniform vec3 u_fogColor;

uint applyTexa(uint psm, uint texel)
{
    if (psm == 0x00u)
        return texel;
    bool rgbZero = (texel & 0x00FFFFFFu) == 0u;
    uint a = texel >> 24;
    if (psm == 0x01u)
        a = (u_texa.z != 0u && rgbZero) ? 0u : u_texa.x;
    else if (psm == 0x02u || psm == 0x0Au)
        a = ((a & 0x80u) != 0u) ? u_texa.y : ((u_texa.z != 0u && rgbZero) ? 0u : u_texa.x);
    return (texel & 0x00FFFFFFu) | (a << 24);
}

uint lookupClut(uint index)
{
    uint tpsm = u_tex.z, cpsm = u_tex.w, csm = u_tex2.x, csa = u_tex2.y;
    bool sixteenBit = cpsm == 0x02u || cpsm == 0x0Au;
    bool fourBit = tpsm == 0x14u || tpsm == 0x24u || tpsm == 0x2Cu;
    bool eightBit = tpsm == 0x13u || tpsm == 0x1Bu;
    uint clutBase = (csa & (sixteenBit ? 0x1Fu : 0x0Fu)) << 4;
    uint sourceIndex = fourBit ? (index & 0x0Fu) : index;
    uint clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
    if (!sixteenBit && csm == 0u && eightBit)
        clutIndex = min((sourceIndex & 0xF0u) + clutBase, 240u) + (sourceIndex & 0x0Fu);
    if (cpsm == 0x00u)
        return clut[clutIndex] | (clut[clutIndex + 256u] << 16);
    if (cpsm == 0x01u)
        return applyTexa(cpsm, (clut[clutIndex] | (clut[clutIndex + 256u] << 16)) & 0x00FFFFFFu);
    if (sixteenBit)
        return applyTexa(cpsm, rgba5551To8888(clut[clutIndex]));
    return 0xFFFF00FFu;
}

int wrapCoordinate(int coordinate, int size, uint mode, int regionMin, int regionMax)
{
    if (mode == 0u) return int(uint(coordinate) & uint(size - 1));
    if (mode == 1u) return clamp(coordinate, 0, size - 1);
    if (mode == 2u) return min(max(coordinate, regionMin), regionMax);
    return int((uint(coordinate) & uint(regionMin)) | uint(regionMax));
}

uint texel(int u, int v)
{
    u = wrapCoordinate(u, u_texSize.x, u_tex2.z & 3u, u_region.x, u_region.y);
    v = wrapCoordinate(v, u_texSize.y, (u_tex2.z >> 2) & 3u, u_region.z, u_region.w);
    uint psm = u_tex.z;
    uint raw = readVram(psm, u_tex.x, u_tex.y, uint(u), uint(v));
    switch (psm)
    {
    case 0x00u: case 0x30u: case 0x01u: case 0x31u:
        return applyTexa(psm, raw);
    case 0x02u: case 0x0Au: case 0x32u: case 0x3Au:
        return applyTexa(psm, rgba5551To8888(raw));
    case 0x13u: case 0x1Bu: case 0x14u: case 0x24u: case 0x2Cu:
        return lookupClut(raw & 0xFFu);
    }
    return 0xFFFF00FFu;
}

// Two 8-bit channels (bits 0-7 and 16-23) blended with an 8-bit weight.
uint lerp2(uint a, uint b, uint w)
{
    uint lo = ((a & 0xFFu) * (256u - w) + (b & 0xFFu) * w + 128u) >> 8;
    uint hi = (((a >> 16) & 0xFFu) * (256u - w) + ((b >> 16) & 0xFFu) * w + 128u) >> 8;
    return lo | (hi << 16);
}

// Texel of a render target used as the texture, in target pixels: GS wrap
// modes apply to the GS texel the pixel belongs to, and the colour goes
// through the same format conversion as a local-memory read.
uint rtTexel(int uh, int vh)
{
    int u = int(floor(float(uh) / float(u_rtScale)));
    int v = int(floor(float(vh) / float(u_rtScale)));
    int su = uh - u * u_rtScale, sv = vh - v * u_rtScale;
    u = wrapCoordinate(u, u_texSize.x, u_tex2.z & 3u, u_region.x, u_region.y);
    v = wrapCoordinate(v, u_texSize.y, (u_tex2.z >> 2) & 3u, u_region.z, u_region.w);
    uint c = packUnorm4x8(texelFetch(u_rtTex, ivec2(u * u_rtScale + su, v * u_rtScale + sv), 0));
    uint psm = u_tex.z;
    return applyTexa(psm, is16BitPsm(psm) ? rgba5551To8888(rgba8888To5551(c)) : c);
}

uint sampleTexture(vec2 uv)
{
    if (u_rtScale != 0)
    {
        // A pixel drawn at GS position p is texel position p + 0.5, so
        // target pixel k (drawn at k / scale) sits at k / scale + 0.5.
        vec2 s = (uv - 0.5) * float(u_rtScale);
        if (u_tex2.w == 0u)
            return rtTexel(int(floor(s.x + 0.5)), int(floor(s.y + 0.5)));
        int u0 = int(floor(s.x)), v0 = int(floor(s.y));
        uint wx = uint((s.x - float(u0)) * 256.0 + 0.5);
        uint wy = uint((s.y - float(v0)) * 256.0 + 0.5);
        uint c00 = rtTexel(u0, v0), c10 = rtTexel(u0 + 1, v0), c01 = rtTexel(u0, v0 + 1), c11 = rtTexel(u0 + 1, v0 + 1);
        uint even = lerp2(lerp2(c00 & 0x00FF00FFu, c10 & 0x00FF00FFu, wx), lerp2(c01 & 0x00FF00FFu, c11 & 0x00FF00FFu, wx), wy);
        uint odd = lerp2(lerp2((c00 >> 8) & 0x00FF00FFu, (c10 >> 8) & 0x00FF00FFu, wx),
                         lerp2((c01 >> 8) & 0x00FF00FFu, (c11 >> 8) & 0x00FF00FFu, wx), wy);
        return even | (odd << 8);
    }
    if (u_tex2.w == 0u)
        return texel(int(uv.x), int(uv.y));
    vec2 s = uv - 0.5;
    int u0 = int(floor(s.x)), v0 = int(floor(s.y));
    uint wx = uint((s.x - float(u0)) * 256.0 + 0.5);
    uint wy = uint((s.y - float(v0)) * 256.0 + 0.5);
    uint c00 = texel(u0, v0), c10 = texel(u0 + 1, v0), c01 = texel(u0, v0 + 1), c11 = texel(u0 + 1, v0 + 1);
    uint even = lerp2(lerp2(c00 & 0x00FF00FFu, c10 & 0x00FF00FFu, wx), lerp2(c01 & 0x00FF00FFu, c11 & 0x00FF00FFu, wx), wy);
    uint odd = lerp2(lerp2((c00 >> 8) & 0x00FF00FFu, (c10 >> 8) & 0x00FF00FFu, wx),
                     lerp2((c01 >> 8) & 0x00FF00FFu, (c11 >> 8) & 0x00FF00FFu, wx), wy);
    return even | (odd << 8);
}

void main()
{
    vec4 cf = floor(v_color * 255.0 + 0.5);
    vec4 c = cf;
    if (u_tme != 0)
    {
        float q = abs(v_stq.z) > 1.0e-8 ? v_stq.z : 1.0;
        vec2 uv = v_stq.xy * (1.0 / q) * vec2(u_texSize);
        vec4 ct;
        if (u_replace != 0)
        {
            ct = floor(texture(u_replTex, uv / vec2(u_texSize)) * 255.0 + 0.5);
        }
        else
        {
            uint t = sampleTexture(uv);
            ct = vec4(float(t & 0xFFu), float((t >> 8) & 0xFFu), float((t >> 16) & 0xFFu), float(t >> 24));
        }
        if (u_tfx == 0)
        {
            c.rgb = min(floor(ct.rgb * cf.rgb / 128.0), 255.0);
            c.a = (u_tcc != 0) ? min(floor(ct.a * cf.a / 128.0), 255.0) : cf.a;
        }
        else if (u_tfx == 1)
        {
            c.rgb = ct.rgb;
            c.a = (u_tcc != 0) ? ct.a : cf.a;
        }
        else
        {
            c.rgb = min(floor(ct.rgb * cf.rgb / 128.0) + cf.a, 255.0);
            c.a = (u_tcc != 0) ? ((u_tfx == 2) ? min(ct.a + cf.a, 255.0) : ct.a) : cf.a;
        }
    }
    if (u_fge != 0)
    {
        float f = floor(v_fog * 255.0 + 0.5);
        c.rgb = floor(f * c.rgb / 256.0) + floor((255.0 - f) * u_fogColor / 256.0);
    }

    if (u_ate != 0)
    {
        float aref = float(u_aref);
        bool pass = true;
        switch (u_atst)
        {
        case 0: pass = false; break;
        case 1: pass = true; break;
        case 2: pass = c.a < aref; break;
        case 3: pass = c.a <= aref; break;
        case 4: pass = c.a == aref; break;
        case 5: pass = c.a >= aref; break;
        case 6: pass = c.a > aref; break;
        default: pass = c.a != aref; break;
        }
        // AFAIL: 0 KEEP; FB_ONLY / RGB_ONLY still write colour (Z write is
        // not per-fragment controllable here); ZB_ONLY drops colour.
        if (!pass && (u_afail == 0 || u_afail == 2))
            discard;
    }

    if (u_date != 0)
    {
        float ad = texelFetch(u_target, ivec2(gl_FragCoord.xy), 0).a * 255.0;
        if ((ad >= 128.0) != (u_datm != 0))
            discard;
    }

    float storedAlpha = c.a;
    if (u_fba != 0)
        storedAlpha = max(storedAlpha, mod(storedAlpha, 128.0) + 128.0);

    if (u_abe != 0)
    {
        float cAlpha = (u_csel == 0) ? c.a
                     : (u_csel == 1) ? floor(texelFetch(u_target, ivec2(gl_FragCoord.xy), 0).a * 255.0 + 0.5)
                     : u_fix;
        float k = cAlpha / 128.0;
        float alphaS = k * u_blend.x + u_blend.y;
        float alphaD = k * u_blend.z + u_blend.w;
        if (u_pabe != 0 && c.a < 128.0)
        {
            alphaS = 1.0;
            alphaD = 0.0;
        }
        o_color = vec4(min(c.rgb * abs(alphaS), 255.0) / 255.0, storedAlpha / 255.0);
        o_factor = vec4(clamp(abs(alphaD), 0.0, 1.0));
    }
    else
    {
        o_color = vec4(c.rgb / 255.0, storedAlpha / 255.0);
        o_factor = vec4(0.0);
    }
}
)";

        // Upscaled display output: the PCRTC merge of the two read circuits
        // (circuit 2 or the background below, circuit 1 blended over it).
        const char *kComposeShader = R"(#version 450 core
layout(binding = 0) uniform sampler2D u_circuit1;
layout(binding = 1) uniform sampler2D u_circuit2;
layout(binding = 2) uniform sampler2D u_overlay1; // screen-space overlay mask for circuit 1
uniform ivec4 u_rect1; // read origin and size in target pixels; size 0 = circuit off
uniform ivec4 u_rect2;
uniform vec3 u_bg;
uniform vec2 u_mode;   // MMOD ALP factor (or < 0: circuit 1's alpha), SLBG
uniform float u_overlayValid; // 0 = no mask (nothing to exclude)
uniform float u_overlayToAlpha; // 1 = put the mask in alpha for the blur pass
uniform float u_overlayDebug; // 1 = show the mask itself (PS2X_OVERLAY_MASK_DEBUG)
out vec4 o_color;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 dst = u_bg;
    if (u_rect2.z > 0 && u_mode.y == 0.0 && p.x < u_rect2.z && p.y < u_rect2.w)
        dst = texelFetch(u_circuit2, u_rect2.xy + p, 0).rgb;
    if (u_rect1.z > 0 && p.x < u_rect1.z && p.y < u_rect1.w)
    {
        vec4 src = texelFetch(u_circuit1, u_rect1.xy + p, 0);
        float factor = u_mode.x >= 0.0 ? u_mode.x : min(1.0, src.a * 2.0);
        dst = mix(dst, src.rgb, factor);
    }
    // The overlay mask: 1 where a screen-space primitive was drawn (the HUD, the
    // minimap). Those do not move with the camera, so the motion blur and frame
    // interpolation must leave them alone. It rides in alpha for the blur pass
    // (0 = leave alone, 1 = warp), and only when something will consume it - the
    // display texture itself must stay opaque.
    float overlay = 0.0;
    if (u_overlayValid > 0.5 && u_rect1.z > 0 && p.x < u_rect1.z && p.y < u_rect1.w)
        overlay = texelFetch(u_overlay1, u_rect1.xy + p, 0).r;
    if (u_overlayDebug > 0.5)
    {
        o_color = vec4(overlay, 0.0, 0.0, 1.0); // red = left alone by the blur
        return;
    }
    o_color = vec4(dst, u_overlayToAlpha > 0.5 ? 1.0 - overlay : 1.0);
}
)";

        // Screen-space overlay mask: 1 wherever an overlay primitive lands. The
        // vertex shader is the draw one, so the geometry lands in the same place.
        const char *kMaskFragmentShader = R"(#version 450 core
out float o_mask;
void main()
{
    o_mask = 1.0;
}
)";

        // The same, for a pass that copies one target into another: the mask
        // follows the image, sampled the same way the draw shader samples the
        // source - the ST is normalized, so it becomes texels via the source's
        // size, then the render-target scale maps into the target's texels.
        const char *kMaskCopyFragmentShader = R"(#version 450 core
noperspective in vec3 v_stq;
uniform sampler2D u_srcMask;
uniform vec2 u_srcSize;   // the source target's size in GS texels
uniform float u_srcScale; // its render-target scale
out float o_mask;
void main()
{
    vec2 uv = v_stq.xy * (1.0 / v_stq.z) * u_srcSize;
    vec2 s = (uv - 0.5) * u_srcScale;
    ivec2 texel = ivec2(floor(s.x + 0.5), floor(s.y + 0.5));
    texel = clamp(texel, ivec2(0), ivec2(u_srcSize * u_srcScale) - 1);
    o_mask = texelFetch(u_srcMask, texel, 0).r;
}
)";

        // Motion blur: smear the composed frame along the camera's own motion.
        // u_warp maps a current screen coordinate to where that content was in
        // the previous frame (see ps2CameraHomography), so uv - prev is the
        // per-pixel screen motion over one frame and the taps run from uv back
        // along it. That is a shutter: the picture is integrated over the
        // interval the frame represents. Deliberately not a blend against the
        // previous frame - that accumulates, and an accumulated frame reads as a
        // sharp double image (ghosting) rather than a blur.
        //
        // The compose puts the overlay mask in alpha (1 = world, 0 = HUD), so the
        // smear is scaled per pixel: the HUD and the minimap are drawn in screen
        // space and do not move with the camera, so they must stay sharp.
        const char *kBlurShader = R"(#version 450 core
layout(binding = 0) uniform sampler2D u_current;
// Column-major, passed as three columns.
uniform vec3 u_warp0;
uniform vec3 u_warp1;
uniform vec3 u_warp2;
uniform vec2 u_size;      // composed size in pixels
uniform float u_shutter;  // smear length in frames of camera motion (0 = off)
out vec4 o_color;
const int kTaps = 6;
void main()
{
    vec2 uv = gl_FragCoord.xy / u_size;
    vec4 cur = texture(u_current, uv);
    // 1 = world, 0 = screen-space overlay. Take the minimum of the neighbours
    // too: the mask is resampled as it travels through the game's frame copies,
    // so its edge can land a texel either side of the HUD, and a pixel adjacent
    // to the HUD that flickers between blurred and sharp reads as the HUD itself
    // jittering.
    vec2 texel = 1.0 / u_size;
    float mask = min(min(min(cur.a, texture(u_current, uv + vec2(texel.x, 0.0)).a),
                         texture(u_current, uv - vec2(texel.x, 0.0)).a),
                     min(texture(u_current, uv + vec2(0.0, texel.y)).a,
                         texture(u_current, uv - vec2(0.0, texel.y)).a));
    if (u_shutter <= 0.0 || mask <= 0.0)
    {
        o_color = vec4(cur.rgb, 1.0);
        return;
    }
    // v-down homography, v-up gl_FragCoord: see the synthesis shader's note.
    vec2 uvD = vec2(uv.x, 1.0 - uv.y);
    vec3 q = mat3(u_warp0, u_warp1, u_warp2) * vec3(uvD, 1.0);
    vec2 prevD = q.xy / q.z;
    vec2 prev = vec2(prevD.x, 1.0 - prevD.y);
    vec2 motion = (uv - prev) * u_shutter * mask;
    vec4 acc = vec4(0.0);
    for (int i = 0; i < kTaps; ++i)
    {
        float t = float(i) / float(kTaps - 1);
        acc += texture(u_current, clamp(uv - motion * t, vec2(0.0), vec2(1.0)));
    }
    o_color = vec4((acc / float(kTaps)).rgb, 1.0);
}
)";

        // Frame interpolation: warp the last composed frame forward by half the
        // last camera motion, so the frame shown between two game pictures is the
        // midpoint of the motion rather than a repeat. The overlays must not move
        // - the HUD is identical in both pictures - so where the mask says overlay
        // the source is taken as-is.
        const char *kSynthShader = R"(#version 450 core
layout(binding = 0) uniform sampler2D u_source;
layout(binding = 1) uniform sampler2D u_mask;
layout(binding = 2) uniform sampler2D u_next; // the newest picture
uniform vec3 u_warp0; // output -> previous picture, column-major
uniform vec3 u_warp1;
uniform vec3 u_warp2;
uniform vec3 u_back0; // output -> newest picture
uniform vec3 u_back1;
uniform vec3 u_back2;
uniform float u_t;         // how far the sweep has got (0 = previous picture)
uniform float u_nextValid;
uniform vec2 u_size;
uniform vec2 u_maskCoordScale; // mask texels per output texel (1 when full size)
uniform float u_maskValid;
layout(binding = 3) uniform sampler2D u_maskNext; // the newest picture's overlay mask
out vec4 o_color;
// The pictures' own overlay masks (snapshotted at their compose, so always
// complete and aligned), eroded by a texel so an edge cannot flicker.
float maskAt(sampler2D m)
{
    ivec2 mp = ivec2(gl_FragCoord.xy * u_maskCoordScale);
    return min(min(min(texelFetch(m, mp, 0).r, texelFetch(m, mp + ivec2(1, 0), 0).r),
                   texelFetch(m, mp + ivec2(-1, 0), 0).r),
               min(texelFetch(m, mp + ivec2(0, 1), 0).r, texelFetch(m, mp + ivec2(0, -1), 0).r));
}
void main()
{
    vec2 uv = gl_FragCoord.xy / u_size;
    if (u_maskValid > 0.5)
    {
        // An overlay (HUD, minimap, text) does not move with the camera: show
        // it where it is, from the newest picture if it is there.
        if (maskAt(u_maskNext) > 0.0 && u_nextValid > 0.5)
        {
            o_color = vec4(texture(u_next, uv).rgb, 1.0);
            return;
        }
        if (maskAt(u_mask) > 0.0)
        {
            o_color = vec4(texture(u_source, uv).rgb, 1.0);
            return;
        }
    }
    // Static pixels: identical at the same screen position in both pictures
    // (with their neighbours) do not move - the HUD, the minimap, text. The
    // overlay mask misses most of VCS's HUD, so this is what keeps it still;
    // in the moving world a pixel almost never matches itself in place, and
    // where it does (flat sky) showing it unwarped looks the same anyway.
    if (u_nextValid > 0.5)
    {
        vec2 texel = 1.0 / vec2(textureSize(u_next, 0));
        float diff = 0.0;
        for (int i = 0; i < 5; ++i)
        {
            vec2 o = i == 0 ? vec2(0.0) : (i == 1 ? vec2(texel.x, 0.0) : (i == 2 ? vec2(-texel.x, 0.0) :
                     (i == 3 ? vec2(0.0, texel.y) : vec2(0.0, -texel.y))));
            vec3 d = abs(texture(u_next, uv + o).rgb - texture(u_source, uv + o).rgb);
            diff = max(diff, max(d.r, max(d.g, d.b)));
        }
        if (diff < 0.02)
        {
            o_color = vec4(texture(u_next, uv).rgb, 1.0);
            return;
        }
    }
    // The homography is built in the picture's v-down convention (see the note
    // in ps2_camera.h) while gl_FragCoord is v-up, so flip around it. The
    // identity case is unaffected - which is why this only ever showed as
    // motion, and looked like the whole screen warping when the mouse moved.
    vec2 uvD = vec2(uv.x, 1.0 - uv.y);
    vec3 q = mat3(u_warp0, u_warp1, u_warp2) * vec3(uvD, 1.0);
    vec2 srcD = q.xy / q.z;
    vec2 src = vec2(srcD.x, 1.0 - srcD.y);
    // Two-way: the previous picture warped forward and the newest one warped
    // back, blended by how far the sweep has got. A single warped picture has
    // nothing for the strip a turn uncovers at the screen edge (it was
    // clamp-stretched: a smeared band appearing and vanishing every game frame)
    // and slides near and far things alike against the true parallax; with two,
    // an uncovered pixel comes from the picture that has it, and parallax
    // errors meet in the middle instead of sliding and snapping.
    vec3 r = mat3(u_back0, u_back1, u_back2) * vec3(uvD, 1.0);
    vec2 nxtD = r.xy / r.z;
    vec2 nxt = vec2(nxtD.x, 1.0 - nxtD.y);
    const float edge = 0.002;
    float inPrev = (all(greaterThanEqual(src, vec2(-edge))) && all(lessThanEqual(src, vec2(1.0 + edge))) && q.z > 0.0) ? 1.0 : 0.0;
    float inNext = (u_nextValid > 0.5 && all(greaterThanEqual(nxt, vec2(-edge))) && all(lessThanEqual(nxt, vec2(1.0 + edge))) && r.z > 0.0) ? 1.0 : 0.0;
    float wPrev = (1.0 - u_t) * inPrev;
    float wNext = u_t * inNext;
    if (wPrev + wNext <= 0.0)
    {
        // Neither covers it with weight: whichever covers it at all.
        wPrev = inPrev;
        wNext = inNext;
    }
    vec3 colour;
    if (wPrev + wNext <= 0.0)
        colour = texture(u_source, clamp(src, vec2(0.0), vec2(1.0))).rgb;
    else
        colour = (texture(u_source, clamp(src, vec2(0.0), vec2(1.0))).rgb * wPrev +
                  texture(u_next, clamp(nxt, vec2(0.0), vec2(1.0))).rgb * wNext) / (wPrev + wNext);
    o_color = vec4(colour, 1.0);
}
)";

        // The composed picture's overlay mask, at the composed size, fetched
        // exactly as the compose fetches it (u_rect1 of the top circuit).
        const char *kMaskSnapShader = R"(#version 450 core
layout(binding = 0) uniform sampler2D u_overlay;
uniform ivec4 u_rect;
uniform float u_valid;
out float o_mask;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    o_mask = (u_valid > 0.5 && p.x < u_rect.z && p.y < u_rect.w) ? texelFetch(u_overlay, u_rect.xy + p, 0).r : 0.0;
}
)";

        // Full-target triangle for load passes (rows limited by the scissor).
        const char *kFullscreenVertexShader = R"(#version 450 core
void main()
{
    vec2 p = vec2((gl_VertexID & 1) * 4 - 1, (gl_VertexID & 2) * 2 - 1);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

        // VRAM -> colour target.
        const char *kLoadColorShader = R"(
uniform uvec3 u_buffer; // block, bw, psm
uniform uint u_scale;   // target pixels per GS pixel (nearest upscale)
out vec4 o_color;
void main()
{
    uint x = uint(gl_FragCoord.x) / u_scale, y = uint(gl_FragCoord.y) / u_scale;
    uint raw = readVram(u_buffer.z, u_buffer.x, u_buffer.y, x, y);
    uint c = is16BitPsm(u_buffer.z) ? rgba5551To8888(raw)
           : (u_buffer.z == 0x01u ? ((raw & 0x00FFFFFFu) | 0x80000000u) : raw);
    o_color = unpackUnorm4x8(c);
}
)";

        // VRAM -> depth target.
        const char *kLoadDepthShader = R"(
uniform uvec3 u_buffer; // block, bw, psm
uniform uint u_scale;
void main()
{
    uint x = uint(gl_FragCoord.x) / u_scale, y = uint(gl_FragCoord.y) / u_scale;
    uint raw = readVram(u_buffer.z, u_buffer.x, u_buffer.y, x, y);
    gl_FragDepth = float(double(raw) * (1.0LF / 4294967296.0LF));
}
)";

        // Target rows -> VRAM.
        const char *kResolveShader = R"(
layout(local_size_x = 64, local_size_y = 4) in;
layout(binding = 0) uniform sampler2D u_source;
uniform uvec4 u_buffer; // block, bw, psm, depth
uniform uvec3 u_rows;   // first row, row count, width
uniform uint u_scale;   // each GS pixel takes the target pixel at its sample point
void main()
{
    uint x = gl_GlobalInvocationID.x, y = gl_GlobalInvocationID.y;
    if (x >= u_rows.z || y >= u_rows.y)
        return;
    y += u_rows.x;
    vec4 t = texelFetch(u_source, ivec2(x * u_scale, y * u_scale), 0);
    uint value;
    if (u_buffer.w != 0u)
    {
        double d = double(t.r) * 4294967296.0LF;
        value = d >= 4294967295.0LF ? 0xFFFFFFFFu : uint(floor(d + 0.5LF));
    }
    else
    {
        uint c = packUnorm4x8(t);
        value = is16BitPsm(u_buffer.z) ? rgba8888To5551(c) : c;
    }
    writeVram(u_buffer.z, u_buffer.x, u_buffer.y, x, y, value);
}
)";

        // Host -> local transfer of `count` pixels from the staging stream.
        const char *kHostToLocalShader = R"(
layout(local_size_x = 64) in;
layout(std430, binding = 3) readonly buffer StagingBuffer { uint staging[]; };
uniform uvec4 u_dest;  // DBP, DBW, DPSM, RRW
uniform uvec4 u_pos;   // DSAX, DSAY, first pixel, count
uniform uint u_bits;   // stream bits per pixel
uint stagingByte(uint i) { return (staging[i >> 2] >> ((i & 3u) * 8u)) & 0xFFu; }
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i >= u_pos.w)
        return;
    uint value;
    if (u_bits == 32u)
        value = staging[i];
    else if (u_bits == 24u)
        value = stagingByte(i * 3u) | (stagingByte(i * 3u + 1u) << 8) | (stagingByte(i * 3u + 2u) << 16);
    else if (u_bits == 16u)
        value = (staging[i >> 1] >> ((i & 1u) * 16u)) & 0xFFFFu;
    else if (u_bits == 8u)
        value = stagingByte(i);
    else
        value = (stagingByte(i >> 1) >> ((i & 1u) * 4u)) & 0xFu;
    uint pixel = u_pos.z + i;
    writeVram(u_dest.z, u_dest.x, u_dest.y, u_pos.x + pixel % u_dest.w, u_pos.y + pixel / u_dest.w, value);
}
)";

        // Local -> local transfer; `u_serial` copies in order on one thread
        // when source and destination overlap.
        const char *kLocalToLocalShader = R"(
layout(local_size_x = 64) in;
uniform uvec4 u_source; // SBP, SBW, SPSM, SSAX
uniform uvec4 u_dest;   // DBP, DBW, DPSM, DSAX
uniform uvec4 u_rect;   // RRW, RRH, SSAY, DSAY
uniform uvec2 u_mode;   // DIR, serial
void copyPixel(uint pixel)
{
    uint x = pixel % u_rect.x, y = pixel / u_rect.x;
    if ((u_mode.x & 2u) != 0u) x = u_rect.x - x - 1u;
    if ((u_mode.x & 1u) != 0u) y = u_rect.y - y - 1u;
    uint value = readVram(u_source.z, u_source.x, u_source.y, x + u_source.w, y + u_rect.z);
    writeVram(u_dest.z, u_dest.x, u_dest.y, x + u_dest.w, y + u_rect.w, value);
}
void main()
{
    uint total = u_rect.x * u_rect.y;
    if (u_mode.y != 0u)
    {
        if (gl_GlobalInvocationID.x == 0u)
            for (uint pixel = 0u; pixel < total; ++pixel)
                copyPixel(pixel);
        return;
    }
    if (gl_GlobalInvocationID.x < total)
        copyPixel(gl_GlobalInvocationID.x);
}
)";

        // CLUT buffer load (GSCpuBackend::LoadClutUnlocked); one entry per
        // invocation. The CLUT holds 512 16-bit entries, one per uint.
        const char *kClutLoadShader = R"(
layout(local_size_x = 256) in;
layout(std430, binding = 2) buffer ClutBuffer { uint clut[]; };
uniform uvec4 u_clut;  // CPSM, CBP, CSM, CSA
uniform uvec4 u_clut2; // CBW, COU, COV, four-bit
void main()
{
    uint entry = gl_LocalInvocationID.x;
    uint cpsm = u_clut.x;
    bool fourBit = u_clut2.w != 0u;
    bool sixteenBit = cpsm == 0x02u || cpsm == 0x0Au;
    uint entryCount = fourBit ? 16u : 256u;
    uint destinationBase = (u_clut.w & (sixteenBit ? 0x1Fu : 0x0Fu)) << 4;
    bool csm1Suffix = u_clut.z == 0u && !sixteenBit && !fourBit;
    uint firstEntry = csm1Suffix ? destinationBase : 0u;
    if (entry < firstEntry || entry >= entryCount)
        return;
    uint sourceX, sourceY, sourceWidth = 1u;
    if (u_clut.z == 0u)
    {
        uint index = (entry & ~0x18u) | ((entry & 0x08u) << 1) | ((entry & 0x10u) >> 1);
        sourceX = index & 0x0Fu;
        sourceY = index >> 4;
    }
    else
    {
        sourceWidth = u_clut2.x != 0u ? u_clut2.x : 1u;
        sourceX = (u_clut2.y << 4) + entry;
        sourceY = u_clut2.z;
    }
    uint raw = readVram(cpsm, u_clut.y, sourceWidth, sourceX, sourceY);
    uint destination = (csm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
    if (sixteenBit)
    {
        clut[destination] = raw & 0xFFFFu;
    }
    else
    {
        clut[destination] = raw & 0xFFFFu;
        clut[destination + 256u] = raw >> 16;
    }
}
)";

        GLuint compileShader(GL &gl, GLenum type, const std::string &source)
        {
            GLuint shader = gl.glCreateShader(type);
            const char *text = source.c_str();
            gl.glShaderSource(shader, 1, &text, nullptr);
            gl.glCompileShader(shader);
            GLint ok = 0;
            gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (!ok)
            {
                char log[4096];
                gl.glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
                std::cerr << "[gs:gpu] shader compile error:\n"
                          << log << std::endl;
            }
            return shader;
        }

        GLuint linkProgram(GL &gl, std::initializer_list<GLuint> shaders)
        {
            GLuint program = gl.glCreateProgram();
            for (GLuint shader : shaders)
                gl.glAttachShader(program, shader);
            gl.glLinkProgram(program);
            GLint ok = 0;
            gl.glGetProgramiv(program, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                char log[4096];
                gl.glGetProgramInfoLog(program, sizeof(log), nullptr, log);
                std::cerr << "[gs:gpu] program link error:\n"
                          << log << std::endl;
            }
            for (GLuint shader : shaders)
                gl.glDeleteShader(shader);
            return program;
        }
    }

    bool Renderer::DrawKey::operator==(const DrawKey &other) const
    {
        return std::memcmp(this, &other, sizeof(DrawKey)) == 0;
    }

    Renderer::Renderer()
        : m_cpu(std::make_unique<GSCpuBackend>())
    {
        std::string error;
        if (m_context.create(error))
        {
            const auto *renderer = reinterpret_cast<const char *>(m_context.gl.glGetString(GL_RENDERER));
            const auto *version = reinterpret_cast<const char *>(m_context.gl.glGetString(GL_VERSION));
            std::cerr << "[gs:gpu] OpenGL " << (version ? version : "?") << " on " << (renderer ? renderer : "?")
                      << (m_context.sharedWithHost() ? " [shares the window's context]" : " [own context]") << std::endl;
            std::snprintf(g_gsGpuInfo, sizeof(g_gsGpuInfo), "OpenGL %s on %s%s", version ? version : "?",
                          renderer ? renderer : "?", m_context.sharedWithHost() ? ", sharing the window's context" : "");
            m_gpu = std::getenv("PS2X_GS_GPU_CPU") == nullptr; // debug: keep the CPU path
            if (m_gpu)
                initGpuObjects();
            g_gsGpuActive.store(m_gpu, std::memory_order_relaxed);
            if (const char *env = std::getenv("PS2X_GS_SCALE"))
                g_gsGpuScaleRequest.store(static_cast<uint32_t>(std::atoi(env)), std::memory_order_relaxed);
            // PS2X_GS_PRESENT_SIZE=<w>x<h>: pretend the host window is this
            // size (tests the readback cap without a window).
            if (const char *env = std::getenv("PS2X_GS_PRESENT_SIZE"))
            {
                unsigned w = 0, h = 0;
                if (std::sscanf(env, "%ux%u", &w, &h) == 2)
                {
                    g_gsGpuPresentWidth.store(w, std::memory_order_relaxed);
                    g_gsGpuPresentHeight.store(h, std::memory_order_relaxed);
                }
            }
            m_scale = std::clamp<uint32_t>(g_gsGpuScaleRequest.load(std::memory_order_relaxed), 1u, 8u);
            m_requestedScale.store(m_scale, std::memory_order_relaxed);
        }
        else
        {
            std::cerr << "[gs:gpu] no GPU context (" << error << "); using the CPU rasterizer" << std::endl;
        }
    }

    Renderer::~Renderer()
    {
        stopReplacementWorker();
        if (m_gpu && m_vram)
        {
            flushBatch();
            readbackVram(); // leave local memory complete for the owner
        }
        if (m_gpu)
        {
            destroyReadbackRing();
            destroyStreamRing(m_vertexRing);
            destroyStreamRing(m_stagingRing);
        }
        if (std::getenv("PS2X_TEXREPL_STATS") && replacementsActive())
            emitTexreplStats();
        if (std::getenv("PS2X_GS_GPU_STATS"))
        {
            std::cerr << "[gs:gpu] draws=" << m_statDraws << " resolves=" << m_statResolves << " loads=" << m_statLoads
                      << " readbacks=" << m_statReadbacks << " clutLoads=" << m_statClutLoads
                      << " transfers=" << m_statTransfers << std::endl;
            std::cerr << "  time: readback=" << m_timeReadback << " draw=" << m_timeDraw << " present=" << m_timePresent
                      << std::endl;
            for (auto &t : m_targets)
                std::cerr << "  target block=" << t->block << " bw=" << t->bw << " psm=" << int(t->psm) << " depth=" << t->depth
                          << " rows=" << t->usedRows << " bytes=[" << t->beginByte << "," << t->endByte << ")" << std::endl;
            for (auto &r : m_statResolveReasons)
                std::cerr << "  resolves[" << r.first << "]=" << r.second << std::endl;
        }
    }

    void Renderer::setUniformBits(Program &program, GLint location, UniformKind kind, uint32_t count,
                                  const uint32_t *bits)
    {
        if (location < 0)
            return; // inactive uniform: nothing to set
        if (static_cast<size_t>(location) >= program.uniforms.size())
            program.uniforms.resize(static_cast<size_t>(location) + 1u);
        UniformSlot &slot = program.uniforms[static_cast<size_t>(location)];
        if (slot.valid != 0u && slot.count == count && slot.kind == kind &&
            std::memcmp(slot.bits, bits, count * sizeof(uint32_t)) == 0)
        {
            ++g_gsGpuUniformSkips;
            return;
        }
        slot.valid = 1u;
        slot.count = static_cast<uint8_t>(count);
        slot.kind = kind;
        std::memcpy(slot.bits, bits, count * sizeof(uint32_t));
        GL &gl = m_context.gl;
        switch (kind)
        {
        case UniformKind::Int:
            switch (count)
            {
            case 1u: gl.glProgramUniform1i(program.id, location, static_cast<GLint>(bits[0])); break;
            case 2u: gl.glProgramUniform2i(program.id, location, static_cast<GLint>(bits[0]), static_cast<GLint>(bits[1])); break;
            case 4u: gl.glProgramUniform4i(program.id, location, static_cast<GLint>(bits[0]), static_cast<GLint>(bits[1]), static_cast<GLint>(bits[2]), static_cast<GLint>(bits[3])); break;
            default: break;
            }
            break;
        case UniformKind::Uint:
            switch (count)
            {
            case 1u: gl.glProgramUniform1ui(program.id, location, bits[0]); break;
            case 2u: gl.glProgramUniform2ui(program.id, location, bits[0], bits[1]); break;
            case 3u: gl.glProgramUniform3ui(program.id, location, bits[0], bits[1], bits[2]); break;
            case 4u: gl.glProgramUniform4ui(program.id, location, bits[0], bits[1], bits[2], bits[3]); break;
            default: break;
            }
            break;
        case UniformKind::Float:
        {
            float f[4] = {};
            std::memcpy(f, bits, count * sizeof(uint32_t));
            switch (count)
            {
            case 1u: gl.glProgramUniform1f(program.id, location, f[0]); break;
            case 2u: gl.glProgramUniform2f(program.id, location, f[0], f[1]); break;
            case 3u: gl.glProgramUniform3f(program.id, location, f[0], f[1], f[2]); break;
            case 4u: gl.glProgramUniform4f(program.id, location, f[0], f[1], f[2], f[3]); break;
            default: break;
            }
            break;
        }
        }
    }

    GLint Renderer::uniform(Program &program, const char *name)
    {
        auto it = program.locations.find(name);
        if (it != program.locations.end())
            return it->second;
        const GLint location = m_context.gl.glGetUniformLocation(program.id, name);
        program.locations.emplace(name, location);
        return location;
    }

    void Renderer::initGpuObjects()
    {
        clearUniformCaches(); // program objects may be new
        GL &gl = m_context.gl;
        gl.glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

        const std::string header = std::string("#version 450 core\n") + kVramGlsl;
        m_draw.id = linkProgram(gl, {compileShader(gl, GL_VERTEX_SHADER, kVertexShader),
                                     compileShader(gl, GL_FRAGMENT_SHADER, header + kFragmentShader)});
        m_loadColor.id = linkProgram(gl, {compileShader(gl, GL_VERTEX_SHADER, kFullscreenVertexShader),
                                          compileShader(gl, GL_FRAGMENT_SHADER, header + kLoadColorShader)});
        m_loadDepth.id = linkProgram(gl, {compileShader(gl, GL_VERTEX_SHADER, kFullscreenVertexShader),
                                          compileShader(gl, GL_FRAGMENT_SHADER, header + kLoadDepthShader)});
        m_resolve.id = linkProgram(gl, {compileShader(gl, GL_COMPUTE_SHADER, header + kResolveShader)});
        m_hostToLocal.id = linkProgram(gl, {compileShader(gl, GL_COMPUTE_SHADER, header + kHostToLocalShader)});
        m_localToLocal.id = linkProgram(gl, {compileShader(gl, GL_COMPUTE_SHADER, header + kLocalToLocalShader)});
        m_clutLoad.id = linkProgram(gl, {compileShader(gl, GL_COMPUTE_SHADER, header + kClutLoadShader)});
        m_compose.id = linkProgram(gl, {compileShader(gl, GL_VERTEX_SHADER, kFullscreenVertexShader),
                                        compileShader(gl, GL_FRAGMENT_SHADER, kComposeShader)});
        m_mask.id = linkProgram(gl, {compileShader(gl, GL_VERTEX_SHADER, kVertexShader),
                                     compileShader(gl, GL_FRAGMENT_SHADER, kMaskFragmentShader)});
        m_maskCopy.id = linkProgram(gl, {compileShader(gl, GL_VERTEX_SHADER, kVertexShader),
                                         compileShader(gl, GL_FRAGMENT_SHADER, kMaskCopyFragmentShader)});
        m_blur.id = linkProgram(gl, {compileShader(gl, GL_VERTEX_SHADER, kFullscreenVertexShader),
                                     compileShader(gl, GL_FRAGMENT_SHADER, kBlurShader)});
        m_synth.id = linkProgram(gl, {compileShader(gl, GL_VERTEX_SHADER, kFullscreenVertexShader),
                                      compileShader(gl, GL_FRAGMENT_SHADER, kSynthShader)});
        m_maskSnap.id = linkProgram(gl, {compileShader(gl, GL_VERTEX_SHADER, kFullscreenVertexShader),
                                         compileShader(gl, GL_FRAGMENT_SHADER, kMaskSnapShader)});
        // A source target that has no mask yet still has to write something: an
        // explicit zero, so the copy overwrites rather than leaving stale bits.
        if (m_zeroMask == 0)
        {
            gl.glCreateTextures(GL_TEXTURE_2D, 1, &m_zeroMask);
            gl.glTextureStorage2D(m_zeroMask, 1, GL_R8, 1, 1);
            gl.glTextureParameteri(m_zeroMask, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl.glTextureParameteri(m_zeroMask, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            const uint8_t zero = 0u;
            gl.glTextureSubImage2D(m_zeroMask, 0, 0, 0, 1, 1, GL_RED, GL_UNSIGNED_BYTE, &zero);
        }
        // PS2X_OVERLAY_MASK=1: build the screen-space overlay mask. It is what
        // the motion blur and frame interpolation need to leave the HUD alone;
        // opt-in until one of them owns it.
        m_maskEnabled = std::getenv("PS2X_OVERLAY_MASK") != nullptr;

        gl.glCreateBuffers(1, &m_vramBuffer);
        gl.glNamedBufferStorage(m_vramBuffer, kLocalMemoryBytes, nullptr, GL_DYNAMIC_STORAGE_BIT);
        const std::vector<uint32_t> swizzle = buildSwizzleTables();
        gl.glCreateBuffers(1, &m_swizzleBuffer);
        gl.glNamedBufferStorage(m_swizzleBuffer, static_cast<GLsizeiptr>(swizzle.size() * sizeof(uint32_t)), swizzle.data(), 0);
        const std::vector<uint32_t> clut(512u, 0u);
        gl.glCreateBuffers(1, &m_clutBuffer);
        gl.glNamedBufferStorage(m_clutBuffer, static_cast<GLsizeiptr>(clut.size() * sizeof(uint32_t)), clut.data(), 0);
        gl.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_vramBuffer);
        gl.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_swizzleBuffer);
        gl.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_clutBuffer);

        gl.glCreateVertexArrays(1, &m_emptyVao);
        {
            const char *env = std::getenv("PS2X_GS_STREAM_RING");
            m_streamRing = env == nullptr || std::atoi(env) != 0;
            gl.glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &m_ssboAlign);
            m_ssboAlign = std::max<GLint>(m_ssboAlign, 4);
            for (int i = 0; i < 8; ++i)
            {
                const bool linear = (i & 4) != 0, clampS = (i & 2) != 0, clampT = (i & 1) != 0;
                gl.glCreateSamplers(1, &m_replSamplers[i]);
                gl.glSamplerParameteri(m_replSamplers[i], GL_TEXTURE_MIN_FILTER,
                                       linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
                gl.glSamplerParameteri(m_replSamplers[i], GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
                gl.glSamplerParameteri(m_replSamplers[i], GL_TEXTURE_WRAP_S, clampS ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                gl.glSamplerParameteri(m_replSamplers[i], GL_TEXTURE_WRAP_T, clampT ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            }
        }
        gl.glCreateVertexArrays(1, &m_vao);
        gl.glEnableVertexArrayAttrib(m_vao, 0);
        gl.glVertexArrayAttribFormat(m_vao, 0, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, x));
        gl.glEnableVertexArrayAttrib(m_vao, 1);
        gl.glVertexArrayAttribIFormat(m_vao, 1, 1, GL_UNSIGNED_INT, offsetof(Vertex, z));
        gl.glEnableVertexArrayAttrib(m_vao, 2);
        gl.glVertexArrayAttribFormat(m_vao, 2, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(Vertex, rgba));
        gl.glEnableVertexArrayAttrib(m_vao, 3);
        gl.glVertexArrayAttribFormat(m_vao, 3, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, s));
        gl.glEnableVertexArrayAttrib(m_vao, 4);
        gl.glVertexArrayAttribFormat(m_vao, 4, 1, GL_FLOAT, GL_FALSE, offsetof(Vertex, fog));
        for (GLuint attr = 0; attr < 5; ++attr)
            gl.glVertexArrayAttribBinding(m_vao, attr, 0);
    }

    // ------------------------------------------------------------------ targets

    Renderer::Target &Renderer::findOrCreateTarget(bool depth, uint32_t block, uint32_t bw, uint8_t psm)
    {
        const bool bits16 = is16Bit(psm);
        for (auto &target : m_targets)
        {
            if (target->depth == depth && target->block == block && target->bw == std::max<uint32_t>(bw, 1u) &&
                is16Bit(target->psm) == bits16)
            {
                if (target->psm != psm)
                {
                    // Same layout class, different format (e.g. CT32 <-> CT24):
                    // the texture's interpretation changes.
                    resolveTarget(*target);
                    resolveGroupEnd();
                    target->psm = psm;
                    if (target->usedRows != 0u)
                    {
                        target->staleY0 = 0u;
                        target->staleY1 = target->usedRows - 1u;
                    }
                }
                return *target;
            }
        }
        auto target = std::make_unique<Target>();
        target->depth = depth;
        target->block = block;
        target->bw = std::max<uint32_t>(bw, 1u);
        target->psm = psm;
        target->width = target->bw * 64u;
        const uint32_t bytesPerPixel = bits16 ? 2u : 4u;
        const uint32_t available = kLocalMemoryBytes - std::min(kLocalMemoryBytes, block * 256u);
        target->height = std::clamp<uint32_t>(available / (target->width * bytesPerPixel), 1u, kMaxTargetHeight);

        GL &gl = m_context.gl;
        gl.glCreateTextures(GL_TEXTURE_2D, 1, &target->texture);
        gl.glTextureStorage2D(target->texture, 1, depth ? GL_DEPTH_COMPONENT32F : GL_RGBA8,
                              static_cast<GLsizei>(target->width * m_scale), static_cast<GLsizei>(target->height * m_scale));
        gl.glTextureParameteri(target->texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.glTextureParameteri(target->texture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.glCreateFramebuffers(1, &target->loadFramebuffer);
        if (depth)
        {
            gl.glNamedFramebufferTexture(target->loadFramebuffer, GL_DEPTH_ATTACHMENT, target->texture, 0);
            gl.glNamedFramebufferDrawBuffer(target->loadFramebuffer, GL_NONE);
        }
        else
        {
            gl.glNamedFramebufferTexture(target->loadFramebuffer, GL_COLOR_ATTACHMENT0, target->texture, 0);
            gl.glNamedFramebufferDrawBuffer(target->loadFramebuffer, GL_COLOR_ATTACHMENT0);
        }
        m_targets.push_back(std::move(target));
        return *m_targets.back();
    }

    Renderer::Target &Renderer::colorTarget(const GSFrameReg &frame)
    {
        return findOrCreateTarget(false, frame.fbp * 32u, frame.fbw, frame.psm);
    }

    Renderer::Target &Renderer::depthTarget(const GSZbufReg &zbuf, uint32_t bw)
    {
        return findOrCreateTarget(true, zbuf.zbp * 32u, bw, zbuf.psm);
    }

    GLuint Renderer::framebufferFor(const Target &color, const Target *depth)
    {
        const uint64_t key = (static_cast<uint64_t>(color.texture) << 32) | (depth ? depth->texture : 0u);
        auto it = m_framebuffers.find(key);
        if (it != m_framebuffers.end())
            return it->second;
        GL &gl = m_context.gl;
        GLuint fbo = 0;
        gl.glCreateFramebuffers(1, &fbo);
        gl.glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, color.texture, 0);
        if (depth)
            gl.glNamedFramebufferTexture(fbo, GL_DEPTH_ATTACHMENT, depth->texture, 0);
        gl.glNamedFramebufferDrawBuffer(fbo, GL_COLOR_ATTACHMENT0);
        if (gl.glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            std::cerr << "[gs:gpu] incomplete framebuffer (color " << color.width << "x" << color.height << ", depth "
                      << (depth ? depth->width : 0u) << "x" << (depth ? depth->height : 0u) << ")" << std::endl;
        m_framebuffers.emplace(key, fbo);
        return fbo;
    }

    void Renderer::loadRows(Target &target, uint32_t y0, uint32_t rows)
    {
        if (rows == 0u)
            return;
        vramBarrierBeforeRead(); // this reads VRAM
        ++m_statLoads;
        // PS2X_GS_LOAD_TRACE=<present>: every load of that frame, with why.
        static const char *s_loadTrace = std::getenv("PS2X_GS_LOAD_TRACE");
        if (s_loadTrace != nullptr && std::strtoull(s_loadTrace, nullptr, 10) == m_presentIndex)
            std::fprintf(stderr, "[gs:load] block=0x%x bw=%u psm=0x%x depth=%d y0=%u rows=%u of %u reason=%s\n", target.block,
                         target.bw, target.psm, target.depth ? 1 : 0, y0, rows, target.usedRows, m_syncReason);
        g_gsGpuLoads.fetch_add(1u, std::memory_order_relaxed);
        g_gsGpuLoadRows.fetch_add(rows, std::memory_order_relaxed);
        GL &gl = m_context.gl;
        Program &program = target.depth ? m_loadDepth : m_loadColor;
        useProgram(program.id);
        setUniform3ui(program, uniform(program, "u_buffer"), target.block, target.bw, target.psm);
        setUniform1ui(program, uniform(program, "u_scale"), m_scale);
        bindFramebuffer(target.loadFramebuffer);
        setViewport(0, 0, static_cast<GLsizei>(target.width * m_scale), static_cast<GLsizei>(target.height * m_scale));
        setScissorTest(true);
        setScissor(0, static_cast<GLint>(y0 * m_scale), static_cast<GLsizei>(target.width * m_scale),
                     static_cast<GLsizei>(rows * m_scale));
        setBlend(false);
        if (target.depth)
        {
            setDepthTest(true);
            setDepthFunc(GL_ALWAYS);
            setDepthMask(GL_TRUE);
        }
        else
        {
            setDepthTest(false);
            setColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }
        bindVao(m_emptyVao);
        gpuMark(GpuWork::Load);
        gl.glDrawArrays(GL_TRIANGLES, 0, 3);
        gpuEnd();
        m_framebufferWriteSinceBarrier = true;
    }

    // PS2X_GS_GPU_SKIP_MEMBARRIER: diagnostic only. Drops BOTH resolve barriers
    // (gating just one leaves the other synchronising, so that test reads zero
    // either way). Expect visual artifacts with it on.
    void Renderer::resolveBarrierBegin()
    {
        m_resolveGroupOpen = true;
        // Only a draw into a framebuffer since the last barrier can make this
        // resolve's texture read stale, so that is the condition, not the
        // resolve count.
        if (!m_framebufferWriteSinceBarrier)
            return;
        m_framebufferWriteSinceBarrier = false;
        static const bool s_skip = std::getenv("PS2X_GS_GPU_SKIP_MEMBARRIER") != nullptr;
        if (s_skip)
            return;
        // Ordering prior framebuffer writes against this resolve's texture read.
        // Only GL_FRAMEBUFFER_BARRIER_BIT is needed - the old pair also carried
        // GL_SHADER_STORAGE_BARRIER_BIT, which is about shader storage writes
        // and has nothing to do with a read.
        //
        // PS2X_GS_TEXBARRIER=1: use glTextureBarrier() instead. It is the
        // precise ARB_texture_barrier tool for framebuffer -> texture feedback,
        // but it is specified for *drawing* commands' fetches, and this read is
        // a compute dispatch, so it is not guaranteed here. Try it, do not
        // assume it.
        static const bool s_texBarrier = std::getenv("PS2X_GS_TEXBARRIER") != nullptr;
        if (s_texBarrier)
        {
            g_gsGpuBarriers.fetch_add(1u, std::memory_order_relaxed);
            m_context.gl.glTextureBarrier();
        }
        else
        {
            g_gsGpuMemBarriers.fetch_add(1u, std::memory_order_relaxed);
            m_context.gl.glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT);
        }
    }

    void Renderer::resolveGroupEnd()
    {
        m_resolveGroupOpen = false;
        // Not a barrier here: the write is only ordered when something reads
        // VRAM, which is vramBarrierBeforeRead(). Resolves outnumber reads, so
        // this is where the count comes down.
        m_vramWriteSinceBarrier = true;
    }

    void Renderer::vramBarrierBeforeRead()
    {
        if (!m_vramWriteSinceBarrier)
            return;
        m_vramWriteSinceBarrier = false;
        static const bool s_skip = std::getenv("PS2X_GS_GPU_SKIP_MEMBARRIER") != nullptr;
        if (s_skip)
            return;
        // Ordering the resolves' VRAM writes against the read that follows.
        // Only GL_SHADER_STORAGE_BARRIER_BIT is needed: GL_BUFFER_UPDATE_BARRIER_BIT
        // is about glBufferSubData-style updates, not shader storage writes.
        g_gsGpuMemBarriers.fetch_add(1u, std::memory_order_relaxed);
        m_context.gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }

    void Renderer::resolveTarget(Target &target)
    {
        if (!target.gpuDirty())
            return;
        // PS2X_GS_GPU_SKIP_RESOLVE: diagnostic only, gives the ceiling. Local
        // memory stops being brought up to date, so expect a wrong picture;
        // the point is the frame rate the driver could reach without this work.
        static const bool s_skipResolve = std::getenv("PS2X_GS_GPU_SKIP_RESOLVE") != nullptr;
        if (s_skipResolve)
        {
            target.dirtyY0 = ~0u;
            target.dirtyY1 = 0;
            return;
        }
        ++m_statResolves;
        {
            static const char *s_loadTrace = std::getenv("PS2X_GS_LOAD_TRACE");
            if (s_loadTrace != nullptr && std::strtoull(s_loadTrace, nullptr, 10) == m_presentIndex)
                std::fprintf(stderr, "[gs:resolve] block=0x%x bw=%u psm=0x%x depth=%d y=%u..%u reason=%s\n", target.block,
                             target.bw, target.psm, target.depth ? 1 : 0, target.dirtyY0, target.dirtyY1, m_syncReason);
        }
        g_gsGpuResolveRows.fetch_add(std::min(target.dirtyY1, target.height - 1u) - target.dirtyY0 + 1u,
                                     std::memory_order_relaxed);
        ++m_statResolveReasons[m_syncReason];
        g_gsGpuResolves.fetch_add(1u, std::memory_order_relaxed);
        if (m_syncReason == "alias")
            g_gsGpuSyncAlias.fetch_add(1u, std::memory_order_relaxed);
        else if (m_syncReason == "texture")
            g_gsGpuSyncTexture.fetch_add(1u, std::memory_order_relaxed);
        else if (m_syncReason == "stale")
            g_gsGpuSyncStale.fetch_add(1u, std::memory_order_relaxed);
        GL &gl = m_context.gl;
        resolveBarrierBegin();
        const uint32_t y0 = target.dirtyY0;
        const uint32_t rows = std::min(target.dirtyY1, target.height - 1u) - y0 + 1u;
        useProgram(m_resolve.id);
        setUniform4ui(m_resolve, uniform(m_resolve, "u_buffer"), target.block, target.bw, target.psm,
                               target.depth ? 1u : 0u);
        setUniform3ui(m_resolve, uniform(m_resolve, "u_rows"), y0, rows, target.width);
        setUniform1ui(m_resolve, uniform(m_resolve, "u_scale"), m_scale);
        gl.glBindTextureUnit(0, target.texture);
        gpuMark(GpuWork::Resolve);
        gl.glDispatchCompute((target.width + 63u) / 64u, (rows + 3u) / 4u, 1u);
        gpuEnd();
        // The matching barrier is issued by resolveGroupEnd() at the end of the
        // call site's group, not here.
        if (replacementsActive())
        {
            uint32_t begin = 0, end = 0;
            rowsToBytes(target, y0, y0 + rows - 1u, begin, end);
            markPagesWritten(begin, end, true);
        }
        target.dirtyY0 = ~0u;
        target.dirtyY1 = 0;
    }

    void Renderer::rowsToBytes(const Target &target, uint32_t y0, uint32_t y1, uint32_t &begin, uint32_t &end) const
    {
        const PageShape shape = pageShape(target.psm);
        const uint32_t rowBytes = (target.bw * 64u + shape.width - 1u) / shape.width * kPageBytes;
        const uint32_t baseBytes = target.block * 256u;
        const uint32_t base = baseBytes & ~(kPageBytes - 1u);
        const uint32_t misalign = (baseBytes & (kPageBytes - 1u)) != 0u ? kPageBytes : 0u;
        begin = static_cast<uint32_t>(std::min<uint64_t>(base + uint64_t(y0 / shape.height) * rowBytes, kLocalMemoryBytes));
        end = static_cast<uint32_t>(
            std::min<uint64_t>(base + uint64_t(y1 / shape.height + 1u) * rowBytes + misalign, kLocalMemoryBytes));
    }

    bool Renderer::bytesToRows(const Target &target, uint32_t begin, uint32_t end, uint32_t &y0, uint32_t &y1) const
    {
        const uint32_t lo = std::max(begin, target.beginByte), hi = std::min(end, target.endByte);
        if (lo >= hi || target.usedRows == 0u)
            return false;
        const PageShape shape = pageShape(target.psm);
        const uint32_t rowBytes = (target.bw * 64u + shape.width - 1u) / shape.width * kPageBytes;
        const uint32_t baseBytes = target.block * 256u;
        const uint32_t base = baseBytes & ~(kPageBytes - 1u);
        const uint32_t misalign = (baseBytes & (kPageBytes - 1u)) != 0u ? kPageBytes : 0u;
        // With a misaligned base a memory page holds parts of two buffer pages.
        const uint32_t first = (std::max(lo, base + misalign) - misalign - base) / rowBytes;
        const uint32_t last = (hi - 1u - base) / rowBytes;
        y0 = first * shape.height;
        y1 = std::min((last + 1u) * shape.height - 1u, target.usedRows - 1u);
        return y0 <= y1;
    }

    void Renderer::resolveRange(uint32_t beginByte, uint32_t endByte)
    {
        if (!m_gpu)
            return;
        if (!m_inFlush)
            flushBatch();
        for (auto &target : m_targets)
        {
            if (!target->gpuDirty())
                continue;
            uint32_t begin = 0, end = 0;
            rowsToBytes(*target, target->dirtyY0, target->dirtyY1, begin, end);
            if (begin < endByte && beginByte < end)
                resolveTarget(*target);
        }
        resolveGroupEnd();
    }

    void Renderer::markStaleRows(Target &target, uint32_t beginByte, uint32_t endByte)
    {
        uint32_t y0 = 0, y1 = 0;
        if (!bytesToRows(target, beginByte, endByte, y0, y1))
            return;
        {
            static const char *s_loadTrace = std::getenv("PS2X_GS_LOAD_TRACE");
            if (s_loadTrace != nullptr && std::strtoull(s_loadTrace, nullptr, 10) == m_presentIndex)
                std::fprintf(stderr, "[gs:stale] block=0x%x psm=0x%x depth=%d bytes=0x%x..0x%x rows=%u..%u reason=%s\n",
                             target.block, target.psm, target.depth ? 1 : 0, beginByte, endByte, y0, y1, m_syncReason);
        }
        if (target.gpuDirty())
        {
            // Attribute it here too: resolveTarget records m_syncReason, and
            // without this the resolves this path triggers are blamed on
            // whatever operation ran before them.
            m_syncReason = "stale";
            resolveTarget(target); // keep its drawn rows; VRAM there is current
            resolveGroupEnd();
        }
        target.staleY0 = std::min(target.staleY0, y0);
        target.staleY1 = std::max(target.staleY1, y1);
    }

    void Renderer::loadStaleRows(Target &target, uint32_t needY0, uint32_t needY1)
    {
        if (!target.stale())
            return;
        // PS2X_GS_FULL_STALE_LOAD=1: the old behaviour, every stale row on any
        // need (a fallback, and the A/B for the change below).
        static const bool s_fullLoad = std::getenv("PS2X_GS_FULL_STALE_LOAD") != nullptr;
        if (s_fullLoad)
        {
            needY0 = 0u;
            needY1 = ~0u;
        }
        uint32_t y0 = target.staleY0, y1 = std::min(target.staleY1, target.usedRows - 1u);
        if (y0 > y1)
        {
            target.staleY0 = ~0u;
            target.staleY1 = 0;
            return;
        }
        // Only the rows the caller needs. A target is often taller than what is
        // drawn or shown (the display buffer gets grown into the memory below
        // it, where the game streams textures every frame), and reloading that
        // part on every draw was most of the GPU's load work - at an upscaled
        // size, 16x the pixels at 4x. The stale range is one interval, so only
        // a cut at either end can be kept; a window inside it loads it all.
        const uint32_t a = std::max(y0, needY0), b = std::min(y1, needY1);
        if (a > b)
            return; // nothing needed is stale
        if (a == y0 && b < y1)
        {
            target.staleY0 = b + 1u; // the tail stays stale
            y1 = b;
        }
        else if (b == y1 && a > y0)
        {
            target.staleY1 = a - 1u; // the head stays stale
            y0 = a;
        }
        else
        {
            target.staleY0 = ~0u;
            target.staleY1 = 0;
        }
        uint32_t begin = 0, end = 0;
        rowsToBytes(target, y0, y1, begin, end);
        m_syncReason = "stale";
        resolveRange(begin, end); // other targets' data there
        loadRows(target, y0, y1 - y0 + 1u);
    }

    Renderer::Target *Renderer::textureSourceTarget(uint32_t tbp0, uint32_t tbw, uint32_t tpsm, uint32_t rows,
                                                    uint32_t needY0, uint32_t needY1)
    {
        const bool tex16 = tpsm == PSM_CT16 || tpsm == PSM_CT16S;
        const bool tex32 = tpsm == PSM_CT32 || tpsm == PSM_CT24;
        if (!tex16 && !tex32)
            return nullptr; // palettized / Z reinterpretations read local memory
        for (auto &candidate : m_targets)
        {
            Target &target = *candidate;
            if (target.depth || target.block != tbp0 || target.bw != std::max<uint32_t>(tbw, 1u) || target.usedRows == 0u)
                continue;
            if (tex16 ? target.psm != tpsm : (target.psm != PSM_CT32 && target.psm != PSM_CT24))
                continue;
            if (rows > target.height)
                return nullptr;
            // Rows it doesn't mirror yet come from VRAM; rows another view of
            // the memory changed since (tracked as stale) are reloaded. Only the
            // rows this draw samples: a texture's height is a power of two, so a
            // 448-row display read as a texture is 1024 rows tall.
            static const bool s_fullLoad = std::getenv("PS2X_GS_FULL_STALE_LOAD") != nullptr;
            needY1 = s_fullLoad ? rows - 1u : std::min(needY1, rows - 1u);
            growTarget(target, needY1 + 1u);
            loadStaleRows(target, needY0, needY1);
            return &target;
        }
        return nullptr;
    }

    GLuint Renderer::feedbackCopy(const Target &target)
    {
        GL &gl = m_context.gl;
        const uint32_t width = target.width * m_scale, height = target.height * m_scale;
        if (m_feedbackTexture == 0 || m_feedbackWidth < width || m_feedbackHeight < height)
        {
            if (m_feedbackTexture != 0)
                gl.glDeleteTextures(1, &m_feedbackTexture);
            m_feedbackWidth = std::max(m_feedbackWidth, width);
            m_feedbackHeight = std::max(m_feedbackHeight, height);
            gl.glCreateTextures(GL_TEXTURE_2D, 1, &m_feedbackTexture);
            gl.glTextureStorage2D(m_feedbackTexture, 1, GL_RGBA8, static_cast<GLsizei>(m_feedbackWidth),
                                  static_cast<GLsizei>(m_feedbackHeight));
        }
        gpuMark(GpuWork::Feedback);
        gl.glCopyImageSubData(target.texture, GL_TEXTURE_2D, 0, 0, 0, 0, m_feedbackTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
                              static_cast<GLsizei>(width), static_cast<GLsizei>(target.usedRows * m_scale), 1);
        gpuEnd();
        return m_feedbackTexture;
    }

    void Renderer::vramWroteRange(uint32_t beginByte, uint32_t endByte)
    {
        if (!m_gpu)
            return;
        for (auto &target : m_targets)
            markStaleRows(*target, beginByte, endByte);
    }

    void Renderer::growTarget(Target &target, uint32_t needRows)
    {
        const uint32_t oldRows = target.usedRows;
        const uint32_t newRows = std::min(needRows, target.height);
        if (newRows <= oldRows)
            return;
        uint32_t begin = 0, end = 0;
        rowsToBytes(target, oldRows, newRows - 1u, begin, end);
        m_syncReason = "grow";
        resolveRange(begin, end);
        target.usedRows = newRows;
        bufferRange(target.block, target.bw, newRows, target.psm, target.beginByte, target.endByte);
        loadRows(target, oldRows, newRows - oldRows);
    }

    void Renderer::prepareTargetForDraw(Target &target, int32_t drawY0, int32_t drawY1)
    {
        growTarget(target, static_cast<uint32_t>(std::max(0, drawY1 + 1)));
        if (drawY1 >= drawY0 && drawY1 >= 0 && target.usedRows != 0u)
        {
            uint32_t begin = 0, end = 0;
            rowsToBytes(target, static_cast<uint32_t>(std::max(0, drawY0)),
                        std::min(static_cast<uint32_t>(drawY1), target.usedRows - 1u), begin, end);

            // Other views of the memory drawn here: their drawn data reaches
            // VRAM first (oldest first, so the newest wins where they
            // overlap), this target picks it up, and they go stale.
            std::vector<Target *> pending;
            for (auto &other : m_targets)
            {
                if (other.get() == &target || other->usedRows == 0u || !other->gpuDirty())
                    continue;
                uint32_t y0 = 0, y1 = 0;
                if (bytesToRows(*other, begin, end, y0, y1))
                    pending.push_back(other.get());
            }
            if (!pending.empty())
            {
                std::vector<std::pair<uint32_t, uint32_t>> ranges;
                for (Target *other : pending)
                {
                    uint32_t b = 0, e = 0;
                    rowsToBytes(*other, other->dirtyY0, other->dirtyY1, b, e);
                    ranges.emplace_back(b, e);
                }
                if (target.gpuDirty())
                    pending.push_back(&target);
                std::sort(pending.begin(), pending.end(),
                          [](const Target *a, const Target *b)
                          { return a->drawSeq < b->drawSeq; });
                m_syncReason = "alias";
                for (Target *t : pending)
                    resolveTarget(*t);
                resolveGroupEnd();
                for (const auto &range : ranges)
                    markStaleRows(target, range.first, range.second);
            }
            for (auto &other : m_targets)
                if (other.get() != &target)
                    markStaleRows(*other, begin, end);
            // The rows drawn must be current; the rest of the target can stay
            // stale - unless the dirty range (one interval) would then span
            // stale rows, which a resolve would write back over newer memory.
            const uint32_t y0 = static_cast<uint32_t>(std::max(0, drawY0)), y1 = static_cast<uint32_t>(drawY1);
            loadStaleRows(target, y0, y1);
            if (target.stale())
            {
                const uint32_t d0 = std::min(target.dirtyY0, y0), d1 = target.gpuDirty() ? std::max(target.dirtyY1, y1) : y1;
                if (target.staleY0 <= d1 && d0 <= target.staleY1)
                    loadStaleRows(target);
            }
            return;
        }
        loadStaleRows(target);
    }

    // -------------------------------------------------------------------- draws

    void Renderer::applyDrawState(const DrawKey &key, const GSDrawState &state)
    {
        GL &gl = m_context.gl;
        const GSContext &ctx = state.context;

        Target &color = colorTarget(ctx.frame);
        const uint32_t ztst = static_cast<uint32_t>((ctx.test >> 17) & 3u);
        const bool useDepth = ztst != 1u || !ctx.zbuf.zmask;
        Target *depth = useDepth ? &depthTarget(ctx.zbuf, ctx.frame.fbw) : nullptr;
        // Rows this draw can touch: scissor clipped to the geometry.
        const int32_t drawY0 = std::max(key.scissor[1], m_batchY0);
        const int32_t drawY1 = std::min(key.scissor[3], m_batchY1);
        prepareTargetForDraw(color, drawY0, drawY1);
        if (depth)
            prepareTargetForDraw(*depth, drawY0, drawY1);
        m_drawColor = &color;
        m_drawDepth = depth;

        // Texture: a colour target holding it is sampled directly (keeps an
        // upscaled image's detail through render-to-texture passes);
        // otherwise it is read from VRAM, where targets drawn over it go first.
        GLuint textureSource = 0;
        GLuint replacement = 0;
        m_textureSourceTarget = nullptr;
        m_textureSourceWidth = m_textureSourceHeight = 0;
        if (state.prim.tme)
        {
            if (replacementsActive())
                uploadLoadedReplacements();
            static const bool s_vramTextures = std::getenv("PS2X_GS_GPU_NO_RT_TEXTURE") != nullptr;
            // The rows the batch samples, from its texture coordinates (texel
            // v = t / q * height). Unknown (wrapping, region modes, q <= 0):
            // the whole texture.
            uint32_t needY0 = 0u, needY1 = state.textureHeight - 1u;
            {
                const uint32_t wmt = static_cast<uint32_t>((ctx.clamp >> 2) & 3u);
                const float texH = static_cast<float>(state.textureHeight);
                float vMin = 1e30f, vMax = -1e30f;
                bool known = wmt <= 1u && !m_vertices.empty();
                for (const Vertex &v : m_vertices)
                {
                    if (!(v.q > 0.0f))
                    {
                        known = false;
                        break;
                    }
                    const float tv = v.t / v.q * texH;
                    vMin = std::min(vMin, tv);
                    vMax = std::max(vMax, tv);
                }
                // REPEAT only stays inside when the range does; CLAMP clamps.
                if (known && wmt == 0u && (vMin < 0.0f || vMax > texH))
                    known = false;
                if (known && vMin == vMin && vMax == vMax)
                {
                    // A texel either side for bilinear filtering.
                    const float lo = std::clamp(std::floor(vMin) - 1.0f, 0.0f, texH - 1.0f);
                    const float hi = std::clamp(std::ceil(vMax) + 1.0f, 0.0f, texH - 1.0f);
                    needY0 = static_cast<uint32_t>(lo);
                    needY1 = static_cast<uint32_t>(hi);
                }
            }
            Target *source = s_vramTextures ? nullptr
                                            : textureSourceTarget(key.tbp0, key.tbw, key.tpsm, state.textureHeight,
                                                                  needY0, needY1);
            if (source)
            {
                m_textureSourceTarget = source;
                m_textureSourceWidth = state.textureWidth;
                m_textureSourceHeight = state.textureHeight;
                textureSource = source == &color ? feedbackCopy(color) : source->texture;
            }
            else if (replacementsActive() && (replacement = replacementTexture(state)) != 0)
            {
                // Sampled from the replacement image, not local memory.
            }
            else
            {
                uint32_t begin = 0, end = 0;
                bufferRange(key.tbp0, key.tbw, state.textureHeight, static_cast<uint8_t>(key.tpsm), begin, end);
                m_syncReason = "texture";
                resolveRange(begin, end);
            }
        }

        Program &p = m_draw;
        useProgram(p.id);
        bindFramebuffer(framebufferFor(color, depth));
        const int scale = static_cast<int>(m_scale);
        setViewport(0, 0, static_cast<GLsizei>(color.width * m_scale), static_cast<GLsizei>(color.height * m_scale));
        setScissorTest(true);
        const int x0 = std::max(0, key.scissor[0]), y0 = std::max(0, key.scissor[1]);
        const int x1 = std::min<int>(key.scissor[2], static_cast<int>(color.width) - 1);
        const int y1 = std::min<int>(key.scissor[3], static_cast<int>(color.height) - 1);
        setScissor(x0 * scale, y0 * scale, std::max(0, x1 - x0 + 1) * scale, std::max(0, y1 - y0 + 1) * scale);
        m_drawY0 = std::max(y0, drawY0);
        m_drawY1 = std::min(y1, drawY1);

        // Depth: GS Z grows towards the viewer.
        if (depth)
        {
            setDepthTest(true);
            static const GLenum funcs[4] = {GL_NEVER, GL_ALWAYS, GL_GEQUAL, GL_GREATER};
            setDepthFunc(funcs[ztst]);
            setDepthMask(ctx.zbuf.zmask ? GL_FALSE : GL_TRUE);
        }
        else
        {
            setDepthTest(false);
        }

        // Frame write mask (whole channels only for now).
        const uint32_t fbmsk = ctx.frame.fbmsk;
        setColorMask((fbmsk & 0xFFu) != 0xFFu, (fbmsk & 0xFF00u) != 0xFF00u, (fbmsk & 0xFF0000u) != 0xFF0000u,
                       (fbmsk & 0xFF000000u) != 0xFF000000u && ctx.frame.psm != PSM_CT24);

        // Blend: Cv = (A - B) * C / 128 + D, A/B/D in {Cs, Cd, 0}, C in {As, Ad, FIX}.
        auto set1 = [&](const char *name, int value)
        { setUniform1i(p, uniform(p, name), value); };
        const uint64_t alpha = ctx.alpha;
        const uint32_t a = alpha & 3u, b = (alpha >> 2) & 3u, c = (alpha >> 4) & 3u, d = (alpha >> 6) & 3u;
        const float sk = float(a == 0u) - float(b == 0u), sc = float(d == 0u);
        const float dk = float(a == 1u) - float(b == 1u), dc = float(d == 1u);
        if (state.prim.abe)
        {
            setBlend(true);
            GLenum equation = GL_FUNC_ADD;
            if (dk < 0.0f && dc == 0.0f)
                equation = GL_FUNC_SUBTRACT; // Cs*as - Cd*|ad|
            else if (sk < 0.0f && sc == 0.0f)
                equation = GL_FUNC_REVERSE_SUBTRACT; // Cd*ad - Cs*|as|
            gl.glBlendEquationSeparate(equation, GL_FUNC_ADD);
            gl.glBlendFuncSeparate(GL_ONE, GL_SRC1_COLOR, GL_ONE, GL_ZERO);
        }
        else
        {
            setBlend(false);
        }
        setUniform4f(p, uniform(p, "u_blend"), sk, sc, dk, dc);
        set1("u_abe", state.prim.abe ? 1 : 0);
        set1("u_csel", static_cast<int>(c));
        set1("u_pabe", state.pabe ? 1 : 0);
        setUniform2f(p, uniform(p, "u_targetSize"), static_cast<float>(color.width),
                              static_cast<float>(color.height));
        setUniform1f(p, uniform(p, "u_fix"), static_cast<float>((alpha >> 32) & 0xFFu));

        // Tests.
        const uint64_t test = ctx.test;
        set1("u_ate", static_cast<int>(test & 1u));
        set1("u_atst", static_cast<int>((test >> 1) & 7u));
        set1("u_aref", static_cast<int>((test >> 4) & 0xFFu));
        set1("u_afail", static_cast<int>((test >> 12) & 3u));
        const bool date = ((test >> 14) & 1u) != 0u;
        set1("u_date", date ? 1 : 0);
        set1("u_datm", static_cast<int>((test >> 15) & 1u));
        set1("u_fba", (ctx.fba & 1u) != 0u && ctx.frame.psm == PSM_CT32 ? 1 : 0);

        // Destination reads (DATE, Ad blending) sample the bound target.
        const bool readsTarget = date || (state.prim.abe && c == 1u);
        if (readsTarget)
        {
            gl.glBindTextureUnit(1, color.texture);
            g_gsGpuBarriers.fetch_add(1u, std::memory_order_relaxed);
            gl.glTextureBarrier();
        }

        // Texture.
        set1("u_tme", state.prim.tme ? 1 : 0);
        set1("u_fge", state.prim.fge ? 1 : 0);
        set1("u_tfx", static_cast<int>(ctx.tex0.tfx));
        set1("u_tcc", static_cast<int>(ctx.tex0.tcc));
        setUniform3f(p, uniform(p, "u_fogColor"), float(state.fogR), float(state.fogG), float(state.fogB));
        if (state.prim.tme)
        {
            const uint64_t clamp = ctx.clamp;
            setUniform4ui(p, uniform(p, "u_tex"), ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, ctx.tex0.cpsm);
            setUniform4ui(p, uniform(p, "u_tex2"), ctx.tex0.csm, ctx.tex0.csa,
                                   static_cast<uint32_t>(clamp & 0xFu), state.linearFilter ? 1u : 0u);
            setUniform4i(p, uniform(p, "u_region"), static_cast<int>((clamp >> 4) & 0x3FFu),
                                  static_cast<int>((clamp >> 14) & 0x3FFu), static_cast<int>((clamp >> 24) & 0x3FFu),
                                  static_cast<int>((clamp >> 34) & 0x3FFu));
            setUniform2i(p, uniform(p, "u_texSize"), static_cast<int>(state.textureWidth),
                                  static_cast<int>(state.textureHeight));
            setUniform3ui(p, uniform(p, "u_texa"), state.texa.ta0, state.texa.ta1, state.texa.aem ? 1u : 0u);
        }
        set1("u_rtScale", textureSource != 0 ? scale : 0);
        if (textureSource != 0)
            gl.glBindTextureUnit(3, textureSource);
        set1("u_replace", replacement != 0 ? 1 : 0);
        if (replacement != 0)
        {
            // The game's filter and wrap modes (region modes never get here).
            // As a sampler object: changing the texture's own parameters on
            // every draw costs the driver a revalidation each time.
            const bool linear = state.linearFilter;
            const int sampler = (linear ? 4 : 0) | ((ctx.clamp & 3u) == 1u ? 2 : 0) | (((ctx.clamp >> 2) & 3u) == 1u ? 1 : 0);
            gl.glBindSampler(4, m_replSamplers[sampler]);
            gl.glBindTextureUnit(4, replacement);
        }
        bindVao(m_vao);
    }

    // Creates the target's overlay mask if needed, clears it once per frame, and
    // returns its framebuffer (0 if the target cannot have one). The clear
    // matters because a copy only overwrites the region it covers.
    GLuint Renderer::ensureTargetMask(GL &gl, Target &target)
    {
        if (target.depth || target.texture == 0)
            return 0;
        const uint32_t w = target.width * m_scale, h = target.height * m_scale;
        if (target.maskTexture == 0)
        {
            gl.glCreateTextures(GL_TEXTURE_2D, 1, &target.maskTexture);
            gl.glTextureStorage2D(target.maskTexture, 1, GL_R8, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
            gl.glTextureParameteri(target.maskTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl.glTextureParameteri(target.maskTexture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl.glTextureParameteri(target.maskTexture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.glTextureParameteri(target.maskTexture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl.glCreateFramebuffers(1, &target.maskFramebuffer);
            gl.glNamedFramebufferTexture(target.maskFramebuffer, GL_COLOR_ATTACHMENT0, target.maskTexture, 0);
            gl.glNamedFramebufferDrawBuffer(target.maskFramebuffer, GL_COLOR_ATTACHMENT0);
            target.maskFrame = ~0ull;
        }
        if (target.maskFrame != m_presentIndex)
        {
            target.maskFrame = m_presentIndex;
            const GLfloat zero = 0.0f;
            gl.glClearNamedFramebufferfv(target.maskFramebuffer, GL_COLOR, 0, &zero);
        }
        return target.maskFramebuffer;
    }

    // Marks the pending batch in the target's overlay mask. Only called for
    // primitives with no perspective (see the note in flushBatch): the HUD and
    // the minimap are drawn in screen space, so they do not move with the camera
    // and the blur/interpolation passes must leave them alone.
    void Renderer::drawOverlayMask(GL &gl, Target &target)
    {
        if (ensureTargetMask(gl, target) == 0)
            return;
        // Remember the widest target overlays are drawn into - the scene
        // framebuffer. The compose reads the mask from there, not from the
        // display: the copies that carry it to the display happen on only some
        // frames, and the narrow targets (the minimap's own 128-wide one) are not
        // the frame.
        if (m_overlayTarget == nullptr || target.width > m_overlayTarget->width)
            m_overlayTarget = &target;
        Program &p = m_mask;
        useProgram(p.id);
        setUniform2f(p, uniform(p, "u_targetSize"), static_cast<float>(target.width), static_cast<float>(target.height));
        bindFramebuffer(target.maskFramebuffer);
        setBlend(false);
        setDepthTest(false);
        setColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        // Viewport, scissor and VAO are the ones applyDrawState just set for this
        // same target, and the next draw re-applies all of it.
        // No barrier after: the mask is only ever sampled by later draws, which
        // GL orders after this render without one. (It used to issue
        // glMemoryBarrier here for every overlay draw.)
        gl.glDrawArrays(GL_TRIANGLES, m_firstVertex, static_cast<GLsizei>(m_vertices.size()));
    }

    // A screen-space pass that reads one target and writes another is a copy of
    // the frame, and the overlay mask has to travel with it: the HUD is in those
    // pixels, and by compose time the display has been copied through at least
    // one other buffer, so a mask left on the scene target is invisible.
    void Renderer::drawMaskCopy(GL &gl, Target &dst, Target &src)
    {
        if (ensureTargetMask(gl, dst) == 0)
            return;
        // Clearing the source here too means a source with no overlays this frame
        // copies zeros rather than the previous frame's bits.
        ensureTargetMask(gl, src);
        Program &p = m_maskCopy;
        useProgram(p.id);
        setUniform2f(p, uniform(p, "u_targetSize"), static_cast<float>(dst.width), static_cast<float>(dst.height));
        setUniform2f(p, uniform(p, "u_srcSize"), static_cast<float>(m_textureSourceWidth),
                     static_cast<float>(m_textureSourceHeight));
        setUniform1f(p, uniform(p, "u_srcScale"), static_cast<float>(m_scale));
        gl.glBindTextureUnit(0, src.maskTexture != 0 ? src.maskTexture : m_zeroMask);
        bindFramebuffer(dst.maskFramebuffer);
        setBlend(false);
        setDepthTest(false);
        setColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl.glDrawArrays(GL_TRIANGLES, m_firstVertex, static_cast<GLsizei>(m_vertices.size()));
    }

    void Renderer::flushBatch()
    {
        if (!m_gpu || m_vertices.empty())
        {
            m_vertices.clear();
            m_haveKey = false;
            return;
        }
        if (((m_keyState.context.test >> 17) & 3u) == 0u) // ZTST NEVER: nothing drawn
        {
            m_vertices.clear();
            m_haveKey = false;
            return;
        }
        // Motion blur off: drop the draws that read the game's blur buffers.
        //
        // VCS's motion blur is not one feedback quad. It accumulates previous
        // frames into two 256x256 buffers (adjacent in VRAM, TEX0 base 13440
        // and 14464) in a ~9-pass ping-pong per frame, then blends the result
        // over the display. Every pass - both the textured ones and the
        // texture-less ones, which still carry the buffer in TEX0 - names one
        // of those two as its texture base, so one test drops the whole effect
        // and its cost. Skipping only the accumulation would leave the buffers
        // stale and show a frozen ghost; skipping only the display blend would
        // keep the cost. See PS2X_GS_TRACE_FULLSCREEN for the draw stream.
        static const bool s_forceBlurOff = std::getenv("PS2X_MOTIONBLUR_OFF") != nullptr;
        if (s_forceBlurOff || g_gsMotionBlurOff.load(std::memory_order_relaxed))
        {
            static const uint32_t s_blurBase0 = []()
            {
                const char *env = std::getenv("PS2X_MOTIONBLUR_BUFS");
                return env ? static_cast<uint32_t>(std::strtoul(env, nullptr, 10)) : g_gsMotionBlurBase[0].load(std::memory_order_relaxed);
            }();
            static const uint32_t s_blurBase1 = []()
            {
                const char *env = std::getenv("PS2X_MOTIONBLUR_BUFS");
                if (!env)
                    return g_gsMotionBlurBase[1].load(std::memory_order_relaxed);
                const char *comma = std::strchr(env, ',');
                return comma ? static_cast<uint32_t>(std::strtoul(comma + 1, nullptr, 10)) : g_gsMotionBlurBase[1].load(std::memory_order_relaxed);
            }();
            // The buffers are per game (g_gsMotionBlurBase, set by the game
            // override; VCS's two accumulation buffers by default). LCS's trail is
            // one full-screen blend, (Cs - Cd) * FIX + Cd, of an earlier picture
            // over the display - from its 512x512 copy at TEX0 12288 or, in some
            // scenes, straight from the front buffer at TEX0 0; with
            // g_gsMotionBlurFrontTrail base 0 is matched together with that blend
            // so ordinary textures there are left alone.
            const uint32_t tbp = m_keyState.context.tex0.tbp0;
            constexpr uint32_t kTrailBlend = 0u | (1u << 2) | (2u << 4) | (1u << 6); // A=Cs B=Cd C=FIX D=Cd
            const bool trailFromFront = g_gsMotionBlurFrontTrail.load(std::memory_order_relaxed) && tbp == 0u && m_keyState.prim.tme && m_keyState.prim.abe &&
                                        (m_keyState.context.alpha & 0xFFu) == kTrailBlend &&
                                        m_keyState.context.frame.fbp == 140u;
            if (tbp == s_blurBase0 || tbp == s_blurBase1 || trailFromFront)
            {
                m_vertices.clear();
                m_haveKey = false;
                ++m_statBlurSkipped;
                return;
            }
        }
        float minY = m_vertices[0].y, maxY = m_vertices[0].y;
        float minX = m_vertices[0].x, maxX = m_vertices[0].x;
        // A screen-space overlay has no perspective, so its STQ q stays exactly 1:
        // measured on a gameplay capture, every HUD draw is q == 1.0 and every
        // world draw is 0.006..0.11. But a full-frame pass - the game's own blur
        // display blend, a display copy - is also q == 1, and those are the frame
        // itself, not an overlay. Width tells them apart: the frame is full width
        // and an overlay never is (the widest HUD draw measured is 557 of 640).
        bool overlay = true;
        for (const Vertex &v : m_vertices)
        {
            minY = std::min(minY, v.y);
            maxY = std::max(maxY, v.y);
            minX = std::min(minX, v.x);
            maxX = std::max(maxX, v.x);
            overlay = overlay && std::fabs(v.q - 1.0f) < 1e-4f;
        }
        if (m_drawColor != nullptr && m_drawColor->width != 0 &&
            maxX - minX >= static_cast<float>(m_drawColor->width) * 0.95f)
            overlay = false;
        m_batchX0 = static_cast<int32_t>(std::floor(minX));
        m_batchX1 = static_cast<int32_t>(std::ceil(maxX)) - 1;
        // PS2X_GS_TRACE_FULLSCREEN: log every draw covering most of its target.
        // The motion-blur signature is a screen-sized quad; this says whether
        // one exists, what it samples, and how it blends.
        {
            static const bool s_traceFull = std::getenv("PS2X_GS_TRACE_FULLSCREEN") != nullptr;
            if (s_traceFull)
            {
                const Target *col = m_drawColor;
                const GSContext &c = m_keyState.context;
                const int32_t bw = col ? static_cast<int32_t>(col->width) : 0;
                const int32_t bh = col ? static_cast<int32_t>(col->height) : 0;
                const int32_t w = m_batchX1 - m_batchX0 + 1, h = m_batchY1 - m_batchY0 + 1;
                if (bw != 0 && w * 2 >= bw && h * 2 >= bh)
                    std::fprintf(stderr,
                                 "[gs:full] target=%dx%d %dx%d at (%d,%d) tme=%d abe=%d a=%d,%d,%d,%d date=%d fbp=%u fbw=%u "
                                 "tbp=%u tbw=%u tw=%u th=%u\n",
                                 bw, bh, w, h, m_batchX0, m_batchY0, m_keyState.prim.tme ? 1 : 0, m_keyState.prim.abe ? 1 : 0,
                                 static_cast<int>(c.alpha & 3u), static_cast<int>((c.alpha >> 2) & 3u),
                                 static_cast<int>((c.alpha >> 4) & 3u), static_cast<int>((c.alpha >> 6) & 3u),
                                 static_cast<int>((c.test >> 14) & 1u), c.frame.fbp, c.frame.fbw, c.tex0.tbp0, c.tex0.tbw, c.tex0.tw,
                                 c.tex0.th);
            }
        }
        // PS2X_GS_TRACE_DRAWS=<presentIndex>: log every draw of one presented
        // frame in order - index, target, texture, depth state, geometry bounds,
        // and the per-primitive perspective divisor and Z range. The last two are
        // the candidates for telling screen-space overlays (q == 1, constant Z)
        // from world geometry, which the blur and interpolation both need.
        // PS2X_FRAME_INTERP_TEST=1: make one synthesized frame and write it (and
        // the frame it came from) out as PGMs, so the warp can be checked by eye
        // from a capture, which has no game running to drive the host path.
        {
            static const bool s_interpTest = std::getenv("PS2X_FRAME_INTERP_TEST") != nullptr;
            static const uint64_t s_testFrame = []()
            {
                const char *env = std::getenv("PS2X_FRAME_INTERP_TEST_FRAME");
                return env ? std::strtoull(env, nullptr, 10) : 45ull;
            }();
            if (s_interpTest && m_presentIndex == s_testFrame && m_prevComposedTexture != 0)
            {
                const uint32_t w = m_prevComposedWidth, h = m_prevComposedHeight;
                std::vector<uint8_t> before(static_cast<size_t>(w) * h * 4u, 0u);
                GL &gl = m_context.gl;
                gl.glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
                gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
                gl.glGetTextureSubImage(m_prevComposedTexture, 0, 0, 0, 0, static_cast<GLsizei>(w),
                                        static_cast<GLsizei>(h), 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                        static_cast<GLsizei>(before.size()), before.data());
                uint32_t sw = 0, sh = 0;
                static const float s_testPhase = []()
                {
                    const char *env = std::getenv("PS2X_FRAME_INTERP_PHASE");
                    return env ? static_cast<float>(std::atof(env)) : 0.5f;
                }();
                const GLuint synth = synthesizeHostFrame(s_testPhase, sw, sh);
                if (synth != 0)
                {
                    std::vector<uint8_t> after(static_cast<size_t>(sw) * sh * 4u, 0u);
                    gl.glGetTextureSubImage(synth, 0, 0, 0, 0, static_cast<GLsizei>(sw), static_cast<GLsizei>(sh), 1,
                                            GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(after.size()), after.data());
                    if (const char *dir = std::getenv("PS2X_FRAME_INTERP_DUMP"))
                    {
                        char path[1024];
                        std::snprintf(path, sizeof(path), "%s_source.ppm", dir);
                        if (FILE *f = std::fopen(path, "wb"))
                        {
                            std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                            for (size_t i = 0; i + 3u < before.size(); i += 4u)
                                std::fwrite(&before[i], 1, 3, f);
                            std::fclose(f);
                        }
                        std::snprintf(path, sizeof(path), "%s_synth.ppm", dir);
                        if (FILE *f = std::fopen(path, "wb"))
                        {
                            std::fprintf(f, "P6\n%u %u\n255\n", sw, sh);
                            for (size_t i = 0; i + 3u < after.size(); i += 4u)
                                std::fwrite(&after[i], 1, 3, f);
                            std::fclose(f);
                        }
                        std::cerr << "[gs] frame interp: wrote " << dir << "_source.ppm and " << dir
                                  << "_synth.ppm (" << sw << "x" << sh << ")" << std::endl;
                    }
                }
            }
        }
        {
            static const char *s_traceDraws = std::getenv("PS2X_GS_TRACE_DRAWS");
            if (s_traceDraws != nullptr &&
                static_cast<uint64_t>(std::strtoull(s_traceDraws, nullptr, 10)) == m_presentIndex)
            {
                const Target *col = m_drawColor;
                const GSContext &c = m_keyState.context;
                float qmin = m_vertices[0].q, qmax = m_vertices[0].q;
                uint32_t zmin = m_vertices[0].z, zmax = m_vertices[0].z;
                float smin = m_vertices[0].s, smax = m_vertices[0].s;
                float tmin = m_vertices[0].t, tmax = m_vertices[0].t;
                for (const Vertex &v : m_vertices)
                {
                    qmin = std::min(qmin, v.q);
                    qmax = std::max(qmax, v.q);
                    zmin = std::min(zmin, v.z);
                    zmax = std::max(zmax, v.z);
                    smin = std::min(smin, v.s);
                    smax = std::max(smax, v.s);
                    tmin = std::min(tmin, v.t);
                    tmax = std::max(tmax, v.t);
                }
                std::fprintf(stderr,
                             "[gs:draw] n=%llu target=%dx%d %dx%d at (%d,%d) ztst=%u zw=%u tme=%d abe=%d tbp=%u tw=%u "
                             "th=%u psm=%u fbp=%u q=%.4f..%.4f z=%u..%u st=(%.3f,%.3f)..(%.3f,%.3f) v=%zu\n",
                             (unsigned long long)++m_frameDrawIndex, col ? static_cast<int>(col->width) : 0,
                             col ? static_cast<int>(col->height) : 0, m_batchX1 - m_batchX0 + 1,
                             m_batchY1 - m_batchY0 + 1, m_batchX0, m_batchY0, (c.test >> 17) & 3u, c.zbuf.zmask,
                             m_keyState.prim.tme ? 1 : 0, m_keyState.prim.abe ? 1 : 0, c.tex0.tbp0, c.tex0.tw, c.tex0.th,
                             c.tex0.psm, c.frame.fbp, static_cast<double>(qmin), static_cast<double>(qmax),
                             static_cast<unsigned>(zmin), static_cast<unsigned>(zmax), static_cast<double>(smin),
                             static_cast<double>(tmin), static_cast<double>(smax), static_cast<double>(tmax),
                             m_vertices.size());
            }
        }
        m_batchY0 = static_cast<int32_t>(std::floor(minY));
        m_batchY1 = static_cast<int32_t>(std::ceil(maxY)) - 1; // an edge at y covers rows < y
        GL &gl = m_context.gl;
        m_inFlush = true;
        applyDrawState(m_key, m_keyState);
        m_inFlush = false;

        ScopeTimer timer{m_timeDraw};
        const size_t bytes = m_vertices.size() * sizeof(Vertex);
        size_t ringOffset = SIZE_MAX;
        if (m_streamRing)
        {
            bool recreated = false;
            ringOffset = ringAlloc(m_vertexRing, bytes, sizeof(Vertex), 64u << 20, recreated);
            if (ringOffset != SIZE_MAX)
            {
                if (recreated)
                    gl.glVertexArrayVertexBuffer(m_vao, 0, m_vertexRing.buffer, 0, sizeof(Vertex));
                std::memcpy(m_vertexRing.base + ringOffset, m_vertices.data(), bytes);
                m_firstVertex = static_cast<GLint>(ringOffset / sizeof(Vertex));
            }
            else
                m_streamRing = false; // could not map: the old path from here on
        }
        if (ringOffset == SIZE_MAX)
        {
            if (bytes > m_vboCapacity)
            {
                if (m_vbo)
                    gl.glDeleteBuffers(1, &m_vbo);
                m_vboCapacity = std::max<size_t>(bytes * 2u, 1u << 20);
                gl.glCreateBuffers(1, &m_vbo);
                gl.glNamedBufferStorage(m_vbo, static_cast<GLsizeiptr>(m_vboCapacity), nullptr, GL_DYNAMIC_STORAGE_BIT);
            }
            gl.glNamedBufferSubData(m_vbo, 0, static_cast<GLsizeiptr>(bytes), m_vertices.data());
            gl.glVertexArrayVertexBuffer(m_vao, 0, m_vbo, 0, sizeof(Vertex));
            m_firstVertex = 0;
        }
        gpuMark(GpuWork::Draw);
        gl.glDrawArrays(GL_TRIANGLES, m_firstVertex, static_cast<GLsizei>(m_vertices.size()));
        gpuEnd();
        if (m_maskEnabled && m_drawColor != nullptr)
        {
            // An overlay marks itself; a copy of another target carries that
            // target's overlays with it, which is how the mask reaches the
            // display the compose reads.
            if (overlay)
                drawOverlayMask(gl, *m_drawColor);
            else if (m_textureSourceTarget != nullptr)
                drawMaskCopy(gl, *m_drawColor, *m_textureSourceTarget);
        }
        m_framebufferWriteSinceBarrier = true;
        ++m_statDraws;
        g_gsGpuDraws.fetch_add(1u, std::memory_order_relaxed);
        g_gsGpuDrawRows.fetch_add(static_cast<uint64_t>(std::max(0, m_batchY1 - m_batchY0 + 1)), std::memory_order_relaxed);
        g_gsGpuTargets.store(static_cast<uint64_t>(m_targets.size()), std::memory_order_relaxed);

        // The targets now hold data VRAM lacks in the drawn rows.
        auto markDirty = [&](Target *target)
        {
            if (!target)
                return;
            target->drawSeq = ++m_drawSeq;
            target->dirtyY0 = std::min<uint32_t>(target->dirtyY0, static_cast<uint32_t>(std::max(0, m_drawY0)));
            target->dirtyY1 = std::max<uint32_t>(target->dirtyY1, static_cast<uint32_t>(std::max(0, m_drawY1)));
        };
        markDirty(m_drawColor);
        if (m_drawDepth && !m_keyState.context.zbuf.zmask)
            markDirty(m_drawDepth);

        m_vertices.clear();
        m_haveKey = false;
    }

    void Renderer::submit(const GSPrimitiveBatch &batch)
    {
        if (!m_gpu)
        {
            m_cpu->Submit(batch);
            return;
        }
        const GSDrawState &state = batch.state;
        const GSContext &ctx = state.context;
        const GSPrimType type = state.prim.type;
        if (type == GS_PRIM_POINT || type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP)
            return; // TODO: points and lines (unused by VCS so far)

        DrawKey key{};
        key.frameBlock = ctx.frame.fbp * 32u;
        key.frameBw = ctx.frame.fbw;
        key.framePsm = ctx.frame.psm;
        key.fbmsk = ctx.frame.fbmsk;
        key.zBlock = ctx.zbuf.zbp * 32u;
        key.zPsm = ctx.zbuf.psm;
        key.zmask = ctx.zbuf.zmask;
        key.zte = static_cast<uint32_t>((ctx.test >> 16) & 1u);
        key.scissor[0] = ctx.scissor.x0;
        key.scissor[1] = ctx.scissor.y0;
        key.scissor[2] = ctx.scissor.x1;
        key.scissor[3] = ctx.scissor.y1;
        key.test = ctx.test;
        key.alpha = state.prim.abe ? ctx.alpha : 0u;
        key.fba = ctx.fba;
        key.tme = state.prim.tme;
        key.fge = state.prim.fge;
        key.abe = state.prim.abe;
        key.pabe = state.pabe;
        if (state.prim.tme)
        {
            key.clamp = ctx.clamp;
            key.tex1 = ctx.tex1;
            key.tbp0 = ctx.tex0.tbp0;
            key.tbw = ctx.tex0.tbw;
            key.tpsm = ctx.tex0.psm;
            key.tw = ctx.tex0.tw;
            key.th = ctx.tex0.th;
            key.tcc = ctx.tex0.tcc;
            key.tfx = ctx.tex0.tfx;
            if (isPalettized(ctx.tex0.psm))
            {
                key.cbp = ctx.tex0.cbp;
                key.cpsm = ctx.tex0.cpsm;
                key.csm = ctx.tex0.csm;
                key.csa = ctx.tex0.csa;
                key.clutVersion = m_clutVersion;
            }
            key.ta0 = state.texa.ta0;
            key.ta1 = state.texa.ta1;
            key.aem = state.texa.aem;
            key.linear = state.linearFilter;
        }
        if (state.prim.fge)
            key.fogColor = state.fogR | (state.fogG << 8) | (state.fogB << 16);

        if (m_haveKey && !(key == m_key))
            flushBatch();
        if (!m_haveKey)
        {
            m_key = key;
            m_keyState = state;
            m_haveKey = true;
        }

        // GS pixel centres are at integer coordinates, GL's at +0.5: shifting
        // vertices by half a pixel makes GL sample where the GS does (off by
        // half a pixel, every render-to-texture pass drifts the image).
        // Upscaled, the GS sample point of a pixel is its first sub-pixel's
        // centre: half a target pixel.
        const float halfPixel = 0.5f / static_cast<float>(m_scale);
        const float ofx = static_cast<float>(ctx.xyoffset.ofx) / 16.0f - halfPixel;
        const float ofy = static_cast<float>(ctx.xyoffset.ofy) / 16.0f - halfPixel;
        const float texW = static_cast<float>(state.textureWidth);
        const float texH = static_cast<float>(state.textureHeight);
        auto makeVertex = [&](const GSVertex &v, const GSVertex &colorSource) -> Vertex
        {
            Vertex out{};
            out.x = v.x - ofx;
            out.y = v.y - ofy;
            out.z = static_cast<uint32_t>(std::clamp(v.z + 0.5, 0.0, 4294967295.0));
            out.rgba = colorSource.r | (colorSource.g << 8) | (colorSource.b << 16) | (static_cast<uint32_t>(colorSource.a) << 24);
            if (state.prim.fst)
            {
                out.s = (static_cast<float>(v.u) / 16.0f) / texW;
                out.t = (static_cast<float>(v.v) / 16.0f) / texH;
                out.q = 1.0f;
            }
            else
            {
                out.s = v.s;
                out.t = v.t;
                out.q = v.q;
            }
            out.fog = static_cast<float>(v.fog) / 255.0f;
            return out;
        };

        if (type == GS_PRIM_SPRITE)
        {
            if (batch.vertexCount < 2u)
                return;
            const GSVertex &v0 = batch.vertices[0];
            const GSVertex &v1 = batch.vertices[1];
            Vertex a = makeVertex(v0, v1);
            Vertex b = makeVertex(v1, v1);
            // Sprite corners: texel coordinates per corner (STQ divided per vertex).
            if (state.prim.fst)
            {
                a.s = (static_cast<float>(v0.u) / 16.0f) / texW;
                a.t = (static_cast<float>(v0.v) / 16.0f) / texH;
                b.s = (static_cast<float>(v1.u) / 16.0f) / texW;
                b.t = (static_cast<float>(v1.v) / 16.0f) / texH;
            }
            else
            {
                const float q0 = std::fabs(v0.q) > 0.0f ? std::fabs(v0.q) : 1.0f;
                const float q1 = std::fabs(v1.q) > 0.0f ? std::fabs(v1.q) : 1.0f;
                a.s = v0.s / q0;
                a.t = v0.t / q0;
                b.s = v1.s / q1;
                b.t = v1.t / q1;
            }
            a.q = b.q = 1.0f;
            a.z = b.z;
            a.fog = b.fog;
            Vertex c = a, d = a;
            c.x = b.x; // (x1, y0)
            c.s = b.s;
            d.y = b.y; // (x0, y1)
            d.t = b.t;
            m_vertices.push_back(a);
            m_vertices.push_back(c);
            m_vertices.push_back(d);
            m_vertices.push_back(c);
            m_vertices.push_back(b);
            m_vertices.push_back(d);
            return;
        }

        if (batch.vertexCount < 3u)
            return;
        // Flat shading uses the last vertex's colour.
        const GSVertex &flat = batch.vertices[2];
        for (int i = 0; i < 3; ++i)
            m_vertices.push_back(makeVertex(batch.vertices[i], state.prim.iip ? batch.vertices[i] : flat));
    }

    // ------------------------------------------------------------- memory side

    void Renderer::readbackVram()
    {
        vramBarrierBeforeRead(); // this reads VRAM
        if (!m_gpu || !m_vram)
            return;
        flushBatch();
        m_syncReason = "readback";
        resolveRange(0u, kLocalMemoryBytes);
        ++m_statReadbacks;
        g_gsGpuReadbacks.fetch_add(1u, std::memory_order_relaxed);
        ScopeTimer timer{m_timeReadback};
        GL &gl = m_context.gl;
        gl.glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        gl.glGetNamedBufferSubData(m_vramBuffer, 0, kLocalMemoryBytes, m_vram);
        std::fill(m_pageCpuStale.begin(), m_pageCpuStale.end(), uint8_t{0});
    }

    void Renderer::uploadVram(uint32_t beginByte, uint32_t endByte)
    {
        if (!m_gpu || !m_vram || beginByte >= endByte)
            return;
        flushBatch();
        m_context.gl.glNamedBufferSubData(m_vramBuffer, beginByte, endByte - beginByte, m_vram + beginByte);
        vramWroteRange(beginByte, endByte);
        markPagesWritten(beginByte, endByte, false);
    }

    void Renderer::initialize(uint8_t *vram, uint32_t vramSize)
    {
        invalidateGlState(); // a fresh context has no state cached

        m_vram = vram;
        m_cpu->Initialize(vram, vramSize);
        if (m_gpu && m_vram)
            m_context.gl.glNamedBufferSubData(m_vramBuffer, 0, std::min(vramSize, kLocalMemoryBytes), m_vram);
    }

    void Renderer::reset()
    {
        invalidateGlState(); // a fresh context has no state cached

        flushBatch();
        m_cpu->Reset();
        m_clutCbp[0] = m_clutCbp[1] = 0u;
        m_transferState = {};
        m_transferState.direction = 3u;
        m_cpuTransfer = false;
        if (m_gpu && m_vram)
        {
            for (auto &target : m_targets)
            {
                target->dirtyY0 = ~0u;
                target->dirtyY1 = 0;
            }
            uploadVram(0u, kLocalMemoryBytes);
        }
    }

    void Renderer::loadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
    {
        if (!m_gpu)
        {
            m_cpu->LoadClut(tex0, texclut);
            return;
        }
        if (replacementsActive())
            m_cpu->LoadClut(tex0, texclut); // the palette hash reads the CPU CLUT
        const bool fourBit = tex0.psm == PSM_T4 || tex0.psm == PSM_T4HL || tex0.psm == PSM_T4HH;
        const bool eightBit = tex0.psm == PSM_T8 || tex0.psm == PSM_T8H;
        if (!fourBit && !eightBit)
            return;
        switch (tex0.cld)
        {
        case 1u:
            break;
        case 2u:
            m_clutCbp[0] = tex0.cbp;
            break;
        case 3u:
            m_clutCbp[1] = tex0.cbp;
            break;
        case 4u:
            if (m_clutCbp[0] == tex0.cbp)
                return;
            m_clutCbp[0] = tex0.cbp;
            break;
        case 5u:
            if (m_clutCbp[1] == tex0.cbp)
                return;
            m_clutCbp[1] = tex0.cbp;
            break;
        default:
            return;
        }
        const bool sixteenBit = tex0.cpsm == PSM_CT16 || tex0.cpsm == PSM_CT16S;
        const bool thirtyTwoBit = tex0.cpsm == PSM_CT32 || tex0.cpsm == PSM_CT24;
        if (!sixteenBit && !thirtyTwoBit)
            return;

        flushBatch(); // queued draws use the previous CLUT
        uint32_t begin = 0, end = 0;
        const uint32_t sourceWidth = tex0.csm == 0u ? 1u : std::max<uint32_t>(texclut.cbw, 1u);
        const uint32_t sourceRows = tex0.csm == 0u ? 16u : static_cast<uint32_t>(texclut.cov) + 1u;
        bufferRange(tex0.cbp, sourceWidth, sourceRows, tex0.cpsm, begin, end);
        m_syncReason = "clut";
        resolveRange(begin, end);

        ++m_statClutLoads;
        ++m_clutVersion;
        GL &gl = m_context.gl;
        // Draws already submitted still read the old CLUT/VRAM: without this
        // the compute write can overtake their fragment shaders (write after read).
        gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        useProgram(m_clutLoad.id);
        setUniform4ui(m_clutLoad, uniform(m_clutLoad, "u_clut"), tex0.cpsm, tex0.cbp, tex0.csm, tex0.csa);
        setUniform4ui(m_clutLoad, uniform(m_clutLoad, "u_clut2"), texclut.cbw, texclut.cou, texclut.cov,
                               fourBit ? 1u : 0u);
        gpuMark(GpuWork::Transfer);
        gl.glDispatchCompute(1u, 1u, 1u);
        gpuEnd();
        gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }

    void Renderer::dispatchHostToLocal(const uint8_t *data, uint32_t pixels, uint32_t streamBytes)
    {
        GL &gl = m_context.gl;
        const size_t bytes = (streamBytes + 3u) & ~3u;
        size_t ringOffset = SIZE_MAX;
        if (m_streamRing)
        {
            bool recreated = false;
            ringOffset = ringAlloc(m_stagingRing, bytes, static_cast<size_t>(m_ssboAlign), 64u << 20, recreated);
            if (ringOffset != SIZE_MAX)
            {
                std::memcpy(m_stagingRing.base + ringOffset, data, streamBytes);
                if (bytes != streamBytes)
                    std::memset(m_stagingRing.base + ringOffset + streamBytes, 0, bytes - streamBytes);
                gl.glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 3, m_stagingRing.buffer, static_cast<GLintptr>(ringOffset),
                                     static_cast<GLsizeiptr>(bytes));
            }
        }
        if (ringOffset == SIZE_MAX)
        {
            if (bytes > m_stagingCapacity)
            {
                if (m_stagingBuffer)
                    gl.glDeleteBuffers(1, &m_stagingBuffer);
                m_stagingCapacity = std::max<size_t>(bytes, 1u << 20);
                gl.glCreateBuffers(1, &m_stagingBuffer);
                gl.glNamedBufferStorage(m_stagingBuffer, static_cast<GLsizeiptr>(m_stagingCapacity), nullptr,
                                        GL_DYNAMIC_STORAGE_BIT);
            }
            gl.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_stagingBuffer);
            if (streamBytes & 3u)
            {
                std::vector<uint8_t> padded(bytes, 0u);
                std::memcpy(padded.data(), data, streamBytes);
                gl.glNamedBufferSubData(m_stagingBuffer, 0, static_cast<GLsizeiptr>(bytes), padded.data());
            }
            else
            {
                gl.glNamedBufferSubData(m_stagingBuffer, 0, static_cast<GLsizeiptr>(bytes), data);
            }
        }
        const auto &buf = m_transfer.bitbltbuf;
        gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); // earlier draws finish reading VRAM first
        useProgram(m_hostToLocal.id);
        setUniform4ui(m_hostToLocal, uniform(m_hostToLocal, "u_dest"), buf.dbp,
                               std::max<uint32_t>(buf.dbw, 1u), buf.dpsm, m_transfer.trxreg.rrw);
        setUniform4ui(m_hostToLocal, uniform(m_hostToLocal, "u_pos"), m_transfer.trxpos.dsax,
                               m_transfer.trxpos.dsay, m_transferState.copiedPixels, pixels);
        setUniform1ui(m_hostToLocal, uniform(m_hostToLocal, "u_bits"), bitsPerPixel(buf.dpsm));
        gpuMark(GpuWork::Transfer);
        gl.glDispatchCompute((pixels + 63u) / 64u, 1u, 1u);
        gpuEnd();
        gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    }

    void Renderer::dispatchLocalToLocal(const GSTransferCommand &command)
    {
        const auto &buf = command.bitbltbuf;
        const uint32_t rrw = command.trxreg.rrw, rrh = command.trxreg.rrh;
        if (rrw == 0u || rrh == 0u)
            return;
        uint32_t srcBegin = 0, srcEnd = 0, dstBegin = 0, dstEnd = 0;
        bufferRange(buf.sbp, buf.sbw, command.trxpos.ssay + rrh, buf.spsm, srcBegin, srcEnd);
        bufferRange(buf.dbp, buf.dbw, command.trxpos.dsay + rrh, buf.dpsm, dstBegin, dstEnd);
        const bool overlap = srcBegin < dstEnd && dstBegin < srcEnd;
        GL &gl = m_context.gl;
        gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); // earlier draws finish reading VRAM first
        useProgram(m_localToLocal.id);
        setUniform4ui(m_localToLocal, uniform(m_localToLocal, "u_source"), buf.sbp,
                               std::max<uint32_t>(buf.sbw, 1u), buf.spsm, command.trxpos.ssax);
        setUniform4ui(m_localToLocal, uniform(m_localToLocal, "u_dest"), buf.dbp,
                               std::max<uint32_t>(buf.dbw, 1u), buf.dpsm, command.trxpos.dsax);
        setUniform4ui(m_localToLocal, uniform(m_localToLocal, "u_rect"), rrw, rrh, command.trxpos.ssay,
                               command.trxpos.dsay);
        setUniform2ui(m_localToLocal, uniform(m_localToLocal, "u_mode"), command.trxpos.dir,
                               overlap ? 1u : 0u);
        gpuMark(GpuWork::Transfer);
        gl.glDispatchCompute(overlap ? 1u : (rrw * rrh + 63u) / 64u, 1u, 1u);
        gpuEnd();
        gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    }

    void Renderer::beginTransfer(const GSTransferCommand &command)
    {
        if (!m_gpu)
        {
            m_cpu->BeginTransfer(command);
            return;
        }
        flushBatch();
        ++m_statTransfers;
        m_transfer = command;
        m_cpuTransfer = false;
        m_transferState = {};
        m_transferState.x = command.trxpos.dsax;
        m_transferState.y = command.trxpos.dsay;
        m_transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * command.trxreg.rrh;
        m_transferState.direction = command.direction;
        const auto &buf = command.bitbltbuf;

        if (command.direction == 0u || command.direction == 2u)
        {
            // Partial writes must merge with drawn data.
            uint32_t begin = 0, end = 0;
            bufferRange(buf.dbp, buf.dbw, static_cast<uint32_t>(command.trxpos.dsay) + command.trxreg.rrh, buf.dpsm,
                        begin, end);
            m_syncReason = "transferDst";
            resolveRange(begin, end);
        }
        if (command.direction == 1u || command.direction == 2u)
        {
            uint32_t begin = 0, end = 0;
            bufferRange(buf.sbp, buf.sbw, static_cast<uint32_t>(command.trxpos.ssay) + command.trxreg.rrh, buf.spsm,
                        begin, end);
            m_syncReason = "transferSrc";
            resolveRange(begin, end);
        }

        if (replacementsActive() && (command.direction == 0u || command.direction == 2u))
            m_cpu->BeginTransfer(command); // keeps the CPU copy (texture hashing) in step
        if (command.direction == 1u)
        {
            // Local -> host: the CPU backend reads the refreshed local memory.
            readbackVram();
            m_cpu->BeginTransfer(command);
            m_cpuTransfer = true;
        }
        else if (command.direction == 2u)
        {
            dispatchLocalToLocal(command);
            uint32_t begin = 0, end = 0;
            bufferRange(buf.dbp, buf.dbw, static_cast<uint32_t>(command.trxpos.dsay) + command.trxreg.rrh, buf.dpsm,
                        begin, end);
            vramWroteRange(begin, end);
            markPagesWritten(begin, end, true); // the source may hold GPU-drawn data
            m_transferState.copiedPixels = m_transferState.totalPixels;
            m_transferState.direction = 3u;
        }
        else if (command.direction == 0u && (m_transferState.totalPixels == 0u || command.trxreg.rrw == 0u))
        {
            m_transferState.direction = 3u;
        }
    }

    void Renderer::uploadImage(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!m_gpu)
        {
            m_cpu->UploadImage(data, sizeBytes);
            return;
        }
        if (!data || sizeBytes == 0u || m_transferState.direction != 0u)
            return;
        // Whole pixels only (a partial trailing pixel is dropped, as on the CPU path).
        const uint32_t bits = bitsPerPixel(m_transfer.bitbltbuf.dpsm);
        const uint32_t remaining = m_transferState.totalPixels - m_transferState.copiedPixels;
        uint32_t pixels = 0;
        if (bits == 4u)
            pixels = std::min<uint32_t>(remaining, sizeBytes * 2u);
        else
            pixels = std::min<uint32_t>(remaining, sizeBytes / (bits / 8u));
        if (pixels == 0u)
            return;
        const uint32_t streamBytes = bits == 4u ? (pixels + 1u) / 2u : pixels * (bits / 8u);
        flushBatch();
        dispatchHostToLocal(data, pixels, streamBytes);
        if (replacementsActive())
        {
            ScopeTimer timer{g_texreplMirrorSecs};
            m_cpu->UploadImage(data, sizeBytes);
            g_texreplMirrorBytes[m_transfer.bitbltbuf.dpsm & 0x3Fu] += streamBytes;
        }

        const auto &buf = m_transfer.bitbltbuf;
        uint32_t begin = 0, end = 0;
        bufferRange(buf.dbp, buf.dbw, static_cast<uint32_t>(m_transfer.trxpos.dsay) + m_transfer.trxreg.rrh, buf.dpsm,
                    begin, end);
        vramWroteRange(begin, end);
        markPagesWritten(begin, end, false);

        m_transferState.copiedPixels += pixels;
        if (m_transferState.copiedPixels >= m_transferState.totalPixels)
        {
            m_transferState.direction = 3u;
            m_transferState.totalPixels = 0u;
        }
        else
        {
            const uint32_t rrw = m_transfer.trxreg.rrw;
            m_transferState.x = m_transfer.trxpos.dsax + (m_transferState.copiedPixels % rrw);
            m_transferState.y = m_transfer.trxpos.dsay + (m_transferState.copiedPixels / rrw);
        }
    }

    void Renderer::textureFlush()
    {
        if (!m_gpu)
            m_cpu->TextureFlush();
    }

    void Renderer::syncLocalMemory()
    {
        readbackVram();
    }

    // ------------------------------------------------------ texture replacement

    void Renderer::markPagesWritten(uint32_t beginByte, uint32_t endByte, bool cpuStale)
    {
        if (!replacementsActive() || beginByte >= endByte)
            return;
        const uint32_t first = beginByte / kPageBytes, last = std::min<uint32_t>((endByte - 1u) / kPageBytes, 511u);
        // An upload makes the CPU copy current there again (a page only
        // partly rewritten hashes mixed data: a miss, never a wrong match).
        for (uint32_t page = first; page <= last; ++page)
        {
            ++m_pageGeneration[page];
            m_pageCpuStale[page] = cpuStale ? 1u : 0u;
        }
    }

    void Renderer::updateReplacements()
    {
        const uint64_t version = gsrepl::currentVersion();
        if (version == m_replVersion)
            return;
        m_replVersion = version;
        const bool wasActive = replacementsActive();
        m_replIndex = gsrepl::current();
        GL &gl = m_context.gl;
        {
            std::lock_guard lock(m_replMutex);
            m_replRequests.clear();
            m_replResults.clear();
            m_replResultCount.store(0u, std::memory_order_relaxed);
        }
        m_replPendingUploads.clear();
        // Keep the uploads whose file is unchanged: rewriting the prompt art
        // must not reload the (possibly huge) texture pack with it.
        for (auto it = m_replacements.begin(); it != m_replacements.end();)
        {
            bool keep = false;
            if (m_replIndex)
            {
                const auto file = m_replIndex->files.find(it->first);
                keep = file != m_replIndex->files.end() && it->second.texture != 0 &&
                       file->second.stamp == it->second.stamp;
            }
            if (keep)
            {
                ++it;
                continue;
            }
            if (it->second.texture != 0)
                gl.glDeleteTextures(1, &it->second.texture);
            it = m_replacements.erase(it);
        }
        m_textureHashes.clear();
        m_clutHashKey = ~0ull;
        if (!replacementsActive())
        {
            stopReplacementWorker();
            return;
        }
        std::cerr << "[gs:gpu] texture replacement: " << m_replIndex->files.size() << " textures" << std::endl;
        if (!wasActive)
        {
            // The CPU copy of local memory becomes the hashing source: bring
            // it up to date; uploads keep it so from here on.
            readbackVram();
            std::fill(m_pageGeneration.begin(), m_pageGeneration.end(), 0u);
        }
        if (!m_replThread.joinable())
        {
            m_replStop = false;
            m_replThread = std::thread(
                [this]()
                {
                    ThreadNaming::SetCurrentThreadName("TexRepl");
                    replacementWorker();
                });
        }
    }

    void Renderer::stopReplacementWorker()
    {
        if (!m_replThread.joinable())
            return;
        {
            std::lock_guard lock(m_replMutex);
            m_replStop = true;
        }
        m_replCv.notify_all();
        m_replThread.join();
    }

    void Renderer::replacementWorker()
    {
        for (;;)
        {
            std::pair<gsrepl::Key, std::string> request;
            {
                std::unique_lock lock(m_replMutex);
                m_replCv.wait(lock, [this]() { return m_replStop || !m_replRequests.empty(); });
                if (m_replStop)
                    return;
                request = std::move(m_replRequests.front());
                m_replRequests.pop_front();
            }
            auto image = std::make_unique<gsrepl::Image>();
            std::string error;
            if (!gsrepl::loadImage(request.second, *image, error))
            {
                std::cerr << "[gs:gpu] replacement " << request.second << ": " << error << std::endl;
                image.reset();
            }
            std::lock_guard lock(m_replMutex);
            m_replResults.emplace_back(request.first, std::move(image));
            m_replResultCount.fetch_add(1u, std::memory_order_release);
        }
    }

    void Renderer::uploadLoadedReplacements()
    {
        // At most this much texture data per frame: entering a new area can
        // finish dozens of replacements at once, and uploading them all in one
        // frame is a visible hitch (the originals show a few frames longer).
        constexpr size_t kUploadBytesPerFrame = 6u * 1024u * 1024u;
        if (m_replResultCount.load(std::memory_order_acquire) != 0u)
        {
            std::lock_guard lock(m_replMutex);
            for (auto &result : m_replResults)
                m_replPendingUploads.push_back(std::move(result));
            m_replResults.clear();
            m_replResultCount.store(0u, std::memory_order_relaxed);
        }
        if (m_replPendingUploads.empty() || m_replUploadedThisFrame >= kUploadBytesPerFrame)
            return;
        GL &gl = m_context.gl;
        while (!m_replPendingUploads.empty() && m_replUploadedThisFrame < kUploadBytesPerFrame)
        {
            auto result = std::move(m_replPendingUploads.front());
            m_replPendingUploads.pop_front();
            auto it = m_replacements.find(result.first);
            if (it == m_replacements.end() || !result.second || result.second->levels.empty())
                continue; // index changed meanwhile, or the file failed (stays "requested")
            const gsrepl::Image &image = *result.second;
            for (const auto &l : image.levels)
                m_replUploadedThisFrame += l.data.size();
            GLuint texture = 0;
            gl.glCreateTextures(GL_TEXTURE_2D, 1, &texture);
            gl.glTextureStorage2D(texture, static_cast<GLsizei>(image.levels.size()), image.glFormat,
                                  static_cast<GLsizei>(image.levels[0].width), static_cast<GLsizei>(image.levels[0].height));
            for (size_t level = 0; level < image.levels.size(); ++level)
            {
                const auto &l = image.levels[level];
                if (image.compressed)
                    gl.glCompressedTextureSubImage2D(texture, static_cast<GLint>(level), 0, 0, static_cast<GLsizei>(l.width),
                                                     static_cast<GLsizei>(l.height), image.glFormat,
                                                     static_cast<GLsizei>(l.data.size()), l.data.data());
                else
                    gl.glTextureSubImage2D(texture, static_cast<GLint>(level), 0, 0, static_cast<GLsizei>(l.width),
                                           static_cast<GLsizei>(l.height), GL_RGBA, GL_UNSIGNED_BYTE, l.data.data());
            }
            gl.glTextureParameteri(texture, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(image.levels.size()) - 1);
            gl.glTextureParameterf(texture, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, replacementAniso());
            it->second.texture = texture;
            ++m_statReplLoaded;
        }
    }

    uint64_t Renderer::hashTextureMemory(const GSDrawState &state)
    {
        // PCSX2 HashTextureLevel(): whole GS blocks of the texture in block
        // order when it covers whole blocks of a format using all 32 bits of
        // its words; otherwise the expanded texels (palette indices / RGBA).
        const GSTex0Reg &tex0 = state.context.tex0;
        const uint32_t psm = tex0.psm, tbp = tex0.tbp0, tbw = tex0.tbw;
        const uint32_t width = 1u << std::min<uint32_t>(tex0.tw, 10u), height = 1u << std::min<uint32_t>(tex0.th, 10u);
        uint32_t bsx = 8, bsy = 8;
        if (psm == PSM_CT16 || psm == PSM_CT16S || psm == 0x32u || psm == 0x3Au)
            bsx = 16, bsy = 8;
        else if (psm == PSM_T8)
            bsx = 16, bsy = 16;
        else if (psm == PSM_T4)
            bsx = 32, bsy = 16;
        const bool fullWords = psm == PSM_CT32 || psm == 0x30u || psm == PSM_T8 || psm == PSM_T4;
        ++m_statReplHashes;
        XXH3_state_t st;
        XXH3_64bits_reset(&st);
        if (width >= bsx && height >= bsy && fullWords)
        {
            for (uint32_t y = 0; y < height; y += bsy)
                for (uint32_t x = 0; x < width; x += bsx)
                    XXH3_64bits_update(&st, m_vram + (GSMem::PixelByteAddress(psm, tbp, tbw, x, y) & ~255u), 256u);
            g_texreplHashBytes[psm & 0x3Fu] += static_cast<uint64_t>(width / bsx) * (height / bsy) * 256u;
            return XXH3_64bits_digest(&st);
        }
        const bool palettized = isPalettized(static_cast<uint8_t>(psm));
        std::vector<uint8_t> row(static_cast<size_t>(width) * (palettized ? 1u : 4u));
        const uint32_t ta0 = state.texa.ta0, ta1 = state.texa.ta1;
        const bool aem = state.texa.aem;
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t raw = m_cpu->ReadVram(psm, tbp, tbw, x, y);
                if (palettized)
                {
                    row[x] = static_cast<uint8_t>(raw);
                    continue;
                }
                uint32_t c = raw;
                if (psm == PSM_CT24 || psm == 0x31u)
                    c = (raw & 0x00FFFFFFu) | ((aem && (raw & 0x00FFFFFFu) == 0u) ? 0u : (ta0 << 24));
                else if (is16Bit(static_cast<uint8_t>(psm)))
                {
                    const uint32_t a = (raw & 0x8000u) ? ta1 : ((aem && (raw & 0x7FFFu) == 0u) ? 0u : ta0);
                    c = ((raw & 0x1Fu) << 3) | (((raw >> 5) & 0x1Fu) << 11) | (((raw >> 10) & 0x1Fu) << 19) | (a << 24);
                }
                std::memcpy(row.data() + x * 4u, &c, 4u);
            }
            XXH3_64bits_update(&st, row.data(), row.size());
        }
        g_texreplHashBytes[psm & 0x3Fu] += static_cast<uint64_t>(width) * height * (palettized ? 1u : 4u);
        return XXH3_64bits_digest(&st);
    }

    uint64_t Renderer::hashClut(const GSDrawState &state)
    {
        // PCSX2 GSClut::Read32 buffer: 16 (4-bit) or 256 (8-bit) 32-bit
        // entries in index order; 16-bit palettes expanded with TEXA.
        const GSTex0Reg &tex0 = state.context.tex0;
        const bool fourBit = tex0.psm == PSM_T4 || tex0.psm == PSM_T4HL || tex0.psm == PSM_T4HH;
        const bool sixteen = tex0.cpsm == PSM_CT16 || tex0.cpsm == PSM_CT16S;
        const uint64_t key = (m_clutVersion << 24) ^ (uint64_t(tex0.csa) << 1) ^ (fourBit ? 1u : 0u) ^
                             (uint64_t(tex0.cpsm) << 8) ^
                             (sixteen ? (uint64_t(state.texa.ta0) << 40 ^ uint64_t(state.texa.ta1) << 48 ^ uint64_t(state.texa.aem) << 56) : 0u);
        if (key == m_clutHashKey)
            return m_clutHash;
        const auto &clut = m_cpu->clutTable();
        uint32_t entries[256];
        const uint32_t count = fourBit ? 16u : 256u;
        if (!sixteen)
        {
            const uint32_t csa = (tex0.csa & 15u) << 4;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t e = fourBit ? csa + i : std::min((i & ~15u) + csa, 240u) + (i & 15u);
                entries[i] = uint32_t(clut[e & 0xFFu]) | (uint32_t(clut[(e & 0xFFu) + 256u]) << 16);
            }
        }
        else
        {
            const uint32_t base = static_cast<uint32_t>(tex0.csa) << 4;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t raw = clut[(base + i) & 0x1FFu];
                const uint32_t a = (raw & 0x8000u) ? state.texa.ta1 : ((state.texa.aem && (raw & 0x7FFFu) == 0u) ? 0u : state.texa.ta0);
                entries[i] = ((raw & 0x1Fu) << 3) | (((raw >> 5) & 0x1Fu) << 11) | (((raw >> 10) & 0x1Fu) << 19) | (a << 24);
            }
        }
        m_clutHashKey = key;
        m_clutHash = XXH3_64bits(entries, count * 4u);
        return m_clutHash;
    }

    void Renderer::dumpReplacementTexture(const GSDrawState &state, const gsrepl::Key &key)
    {
        const char *dir = std::getenv("PS2X_TEXREPL_DUMP");
        if (!dir || !*dir || !m_cpu || !m_vram)
            return;
        const GSContext &ctx = state.context;
        const GSTex0Reg &tex0 = ctx.tex0;
        const uint32_t psm = tex0.psm & 0x3Fu;
        const bool palettized = isPalettized(static_cast<uint8_t>(psm));
        // PS2X_TEXREPL_DUMP_ALL=1: decode best effort even for formats the
        // matcher does not support (raw 32-bit words), so pack authoring can
        // see every drawn texture.
        static const bool s_anyFormat = std::getenv("PS2X_TEXREPL_DUMP_ALL") != nullptr;
        if (!palettized && psm != PSM_CT32 && psm != PSM_CT24 && psm != PSM_CT16 && psm != PSM_CT16S && !s_anyFormat)
            return;
        const uint32_t width = 1u << std::min<uint32_t>(tex0.tw, 10u);
        const uint32_t height = 1u << std::min<uint32_t>(tex0.th, 10u);
        if (width == 0u || height == 0u)
            return;
        // Skip very large textures while hunting glyphs: copying 1-4 MB
        // buffers per dump stutters the render thread. Raise via
        // PS2X_TEXREPL_DUMP_MAX (edge length, default 512) when needed.
        static const uint32_t s_maxEdge = []()
        {
            const char *env = std::getenv("PS2X_TEXREPL_DUMP_MAX");
            return env ? static_cast<uint32_t>(std::strtoul(env, nullptr, 10)) : 512u;
        }();
        if (width > s_maxEdge || height > s_maxEdge)
            return;
        // Palette for palettized textures, expanded like hashClut does.
        uint32_t palette[256] = {};
        if (palettized)
        {
            const bool fourBit = tex0.psm == PSM_T4 || tex0.psm == PSM_T4HL || tex0.psm == PSM_T4HH;
            const bool sixteen = tex0.cpsm == PSM_CT16 || tex0.cpsm == PSM_CT16S;
            const auto &clut = m_cpu->clutTable();
            const uint32_t count = fourBit ? 16u : 256u;
            if (!sixteen)
            {
                const uint32_t csa = (tex0.csa & 15u) << 4;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t e = fourBit ? csa + i : std::min((i & ~15u) + csa, 240u) + (i & 15u);
                    const uint32_t lo = clut[e & 0xFFu], hi = clut[(e & 0xFFu) + 256u];
                    palette[i] = (lo & 0xFFu) | ((lo >> 8) << 8) | ((hi & 0xFFu) << 16) | ((hi >> 8) << 24);
                }
            }
            else
            {
                const uint32_t base = static_cast<uint32_t>(tex0.csa) << 4;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t raw = clut[(base + i) & 0x1FFu];
                    const uint32_t a = (raw & 0x8000u) ? state.texa.ta1
                                                       : ((state.texa.aem && (raw & 0x7FFFu) == 0u) ? 0u : state.texa.ta0);
                    palette[i] = ((raw & 0x1Fu) << 3) | (((raw >> 5) & 0x1Fu) << 11) | (((raw >> 10) & 0x1Fu) << 19) | (a << 24);
                }
            }
        }
        std::vector<uint8_t> rgba(size_t(width) * height * 4u);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t raw = m_cpu->ReadVram(psm, tex0.tbp0, tex0.tbw, x, y);
                uint32_t c = raw;
                if (palettized)
                    c = palette[raw & 0xFFu];
                else if (psm == PSM_CT24)
                    c = (raw & 0x00FFFFFFu) | ((state.texa.aem && (raw & 0x00FFFFFFu) == 0u) ? 0u : (state.texa.ta0 << 24));
                else if (is16Bit(static_cast<uint8_t>(psm)))
                {
                    const uint32_t a = (raw & 0x8000u) ? state.texa.ta1
                                                       : ((state.texa.aem && (raw & 0x7FFFu) == 0u) ? 0u : state.texa.ta0);
                    c = ((raw & 0x1Fu) << 3) | (((raw >> 5) & 0x1Fu) << 11) | (((raw >> 10) & 0x1Fu) << 19) | (a << 24);
                }
                std::memcpy(rgba.data() + (size_t(y) * width + x) * 4u, &c, 4u);
            }
        DumpJob job;
        job.rgba = std::move(rgba);
        job.width = static_cast<int>(width);
        job.height = static_cast<int>(height);
        job.tbp0 = tex0.tbp0;
        job.tbw = tex0.tbw;
        job.psm = tex0.psm;
        job.tw = tex0.tw;
        job.th = tex0.th;
        job.frame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        std::snprintf(job.name, sizeof(job.name), "%016llx-%016llx-%08x.png", static_cast<unsigned long long>(key.tex0Hash),
                      static_cast<unsigned long long>(key.clutHash), key.bits);
        // Encoded off-thread: PNG compression on the render thread stutters.
        enqueueDump(std::move(job));
    }

    GLuint Renderer::replacementTexture(const GSDrawState &state)
    {
        const GSContext &ctx = state.context;
        const GSTex0Reg &tex0 = ctx.tex0;
        const uint32_t psm = tex0.psm;
        const bool palettized = isPalettized(static_cast<uint8_t>(psm));
        // PS2X_TEXREPL_DUMP_ALL=1: dump textures the matcher refuses
        // (unsupported formats, region clamp) so pack authoring sees every
        // drawn texture; GPU-drawn (CPU-stale) pages are still skipped - the
        // CPU copy holds stale bytes there.
        static const bool s_dumpAll = std::getenv("PS2X_TEXREPL_DUMP_ALL") != nullptr && std::getenv("PS2X_TEXREPL_DUMP") != nullptr;
        if (s_dumpAll)
        {
            const bool supported = palettized || psm == PSM_CT32 || psm == PSM_CT24 || psm == PSM_CT16 || psm == PSM_CT16S;
            const bool unclamped = (ctx.clamp & 3u) < 2u && ((ctx.clamp >> 2) & 3u) < 2u;
            if (!supported || !unclamped)
            {
                uint32_t begin = 0, end = 0;
                bufferRange(tex0.tbp0, tex0.tbw, 1u << std::min<uint32_t>(tex0.th, 10u), static_cast<uint8_t>(psm), begin, end);
                if (begin < end)
                {
                    const bool texa = !palettized && psm != PSM_CT32;
                    gsrepl::Key key;
                    key.tex0Hash = hashTextureMemory(state);
                    key.clutHash = palettized ? hashClut(state) : 0u;
                    key.bits = gsrepl::makeBits(static_cast<uint8_t>(psm), tex0.tw, tex0.th, texa ? state.texa.ta0 : 0u,
                                                texa ? state.texa.aem : 0u, texa ? state.texa.ta1 : 0u);
                    if (m_dumpedTextures.insert(key).second)
                        dumpReplacementTexture(state, key);
                }
            }
        }
        if (!palettized && psm != PSM_CT32 && psm != PSM_CT24 && psm != PSM_CT16 && psm != PSM_CT16S)
            return 0;
        if ((ctx.clamp & 3u) >= 2u || ((ctx.clamp >> 2) & 3u) >= 2u)
            return 0; // region clamp / repeat: not supported by replacements
        ++m_statReplDraws;
        uint32_t begin = 0, end = 0;
        bufferRange(tex0.tbp0, tex0.tbw, 1u << std::min<uint32_t>(tex0.th, 10u), static_cast<uint8_t>(psm), begin, end);
        if (begin >= end)
            return 0;
        uint64_t generation = 1;
        for (uint32_t page = begin / kPageBytes; page <= std::min<uint32_t>((end - 1u) / kPageBytes, 511u); ++page)
        {
            if (m_pageCpuStale[page])
                return 0; // drawn by the GPU (render-to-texture): never a replacement
            generation += m_pageGeneration[page];
        }
        const bool texa = !palettized && psm != PSM_CT32;
        const uint64_t texKey = uint64_t(tex0.tbp0) | uint64_t(tex0.tbw) << 14 | uint64_t(psm) << 20 | uint64_t(tex0.tw) << 26 |
                                uint64_t(tex0.th) << 30 |
                                (texa ? (uint64_t(state.texa.ta0) << 34 | uint64_t(state.texa.aem) << 42 | uint64_t(state.texa.ta1) << 43) : 0u);
        HashedTexture &hashed = m_textureHashes[texKey];
        if (hashed.generation != generation)
        {
            ScopeTimer timer{g_texreplHashSecs};
            hashed.hash = hashTextureMemory(state);
            hashed.generation = generation;
            hashed.resolved = false; // the bytes changed: resolve again
            hashed.replacement = 0;
        }
        gsrepl::Key key;
        key.tex0Hash = hashed.hash;
        if (palettized)
        {
            ScopeTimer timer{g_texreplHashSecs};
            key.clutHash = hashClut(state);
        }
        else
            key.clutHash = 0u;
        key.bits = gsrepl::makeBits(psm, tex0.tw, tex0.th, texa ? state.texa.ta0 : 0u, texa ? state.texa.aem : 0u,
                                    texa ? state.texa.ta1 : 0u);
        if (const char *dumpDir = std::getenv("PS2X_TEXREPL_DUMP"); dumpDir && *dumpDir &&
                                                                        m_dumpedTextures.insert(key).second)
            dumpReplacementTexture(state, key);
        // Settled answer from a previous draw of this same texture: one lookup
        // instead of two, and no Key comparison in either map.
        if (hashed.resolved)
        {
            if (hashed.replacement != 0)
                ++m_statReplHits;
            else
                ++m_statReplMisses;
            return hashed.replacement;
        }
        const auto file = m_replIndex->files.find(key);
        if (file == m_replIndex->files.end())
        {
            ++m_statReplMisses;
            hashed.resolved = true; // no replacement for this texture at all
            static const bool s_logMisses = std::getenv("PS2X_TEXREPL_LOG_MISSES") != nullptr;
            if (s_logMisses)
                std::fprintf(stderr, "[texrepl] miss %016llx-%016llx-%08x tbp=%u tbw=%u\n", (unsigned long long)key.tex0Hash,
                             (unsigned long long)key.clutHash, key.bits, tex0.tbp0, tex0.tbw);
            return 0;
        }
        ++m_statReplHits;
        Replacement &replacement = m_replacements[key];
        if (replacement.texture != 0)
        {
            hashed.resolved = true;
            hashed.replacement = replacement.texture;
            return replacement.texture;
        }
        if (!replacement.requested)
        {
            replacement.requested = true;
            replacement.stamp = file->second.stamp;
            // The prompt art loads inline, so a pad context change swaps the
            // prompts within the same frame instead of showing the previous
            // keys until the worker catches up. Pack textures always load on
            // the worker: loading them here would stall the render thread
            // while new textures stream in.
            if (file->second.inlineLoad)
            {
                gsrepl::Image image;
                std::string error;
                if (gsrepl::loadImage(file->second.path, image, error) && !image.levels.empty())
                {
                    GL &gl = m_context.gl;
                    GLuint texture = 0;
                    gl.glCreateTextures(GL_TEXTURE_2D, 1, &texture);
                    gl.glTextureStorage2D(texture, static_cast<GLsizei>(image.levels.size()), image.glFormat,
                                          static_cast<GLsizei>(image.levels[0].width),
                                          static_cast<GLsizei>(image.levels[0].height));
                    for (size_t level = 0; level < image.levels.size(); ++level)
                    {
                        const auto &l = image.levels[level];
                        if (image.compressed)
                            gl.glCompressedTextureSubImage2D(texture, static_cast<GLint>(level), 0, 0,
                                                             static_cast<GLsizei>(l.width),
                                                             static_cast<GLsizei>(l.height), image.glFormat,
                                                             static_cast<GLsizei>(l.data.size()), l.data.data());
                        else
                            gl.glTextureSubImage2D(texture, static_cast<GLint>(level), 0, 0,
                                                   static_cast<GLsizei>(l.width), static_cast<GLsizei>(l.height), GL_RGBA,
                                                   GL_UNSIGNED_BYTE, l.data.data());
                    }
                    gl.glTextureParameteri(texture, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(image.levels.size()) - 1);
                    gl.glTextureParameterf(texture, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, replacementAniso());
                    replacement.texture = texture;
                    ++m_statReplLoaded;
                    hashed.resolved = true;
                    hashed.replacement = texture;
                    return replacement.texture;
                }
                std::cerr << "[gs:gpu] replacement " << file->second.path << ": " << error << std::endl;
                return 0; // stays "requested": the game's own art shows
            }
            {
                std::lock_guard lock(m_replMutex);
                m_replRequests.emplace_back(key, file->second.path);
            }
            m_replCv.notify_one();
        }
        return 0; // loading: the original texture this time
    }

    void Renderer::emitTexreplStats()
    {
        std::cerr << "[texrepl] draws=" << m_statReplDraws << " hits=" << m_statReplHits << " misses=" << m_statReplMisses
                  << " loaded=" << m_statReplLoaded << " hashed=" << m_statReplHashes
                  << " textures=" << m_textureHashes.size() << " pendingUploads=" << m_replPendingUploads.size()
                  << " mirrorMs=" << static_cast<int>(g_texreplMirrorSecs * 1000.0)
                  << " hashMs=" << static_cast<int>(g_texreplHashSecs * 1000.0);
        for (int pass = 0; pass < 2; ++pass)
        {
            uint64_t *bytes = pass == 0 ? g_texreplMirrorBytes : g_texreplHashBytes;
            std::cerr << (pass == 0 ? " mirrorTop=" : " hashTop=");
            for (int n = 0; n < 3; ++n)
            {
                int best = -1;
                for (int psm = 0; psm < 64; ++psm)
                    if (bytes[psm] != 0u && (best < 0 || bytes[psm] > bytes[best]))
                        best = psm;
                if (best < 0)
                    break;
                std::cerr << (n ? "," : "") << "psm" << std::hex << best << std::dec << ":" << (bytes[best] >> 20)
                          << "MB";
                bytes[best] = 0u;
            }
        }
        std::memset(g_texreplMirrorBytes, 0, sizeof(g_texreplMirrorBytes));
        std::memset(g_texreplHashBytes, 0, sizeof(g_texreplHashBytes));
        g_texreplMirrorSecs = 0.0;
        g_texreplHashSecs = 0.0;
        std::cerr << std::endl;
    }

    PresentationFrame Renderer::present(const GSPresentationRequest &request)
    {
        {
            static const bool s_timers = std::getenv("PS2X_GS_GPU_TIMERS") != nullptr;
            m_gpuTimers = s_timers && m_gpu;
        }
        ++m_presentIndex;
        m_frameDrawIndex = 0;
        // The overlay mask is what keeps the HUD out of the motion blur, so it is
        // built whenever the blur is on; PS2X_OVERLAY_MASK forces it for testing.
        {
            static const bool s_force = std::getenv("PS2X_OVERLAY_MASK") != nullptr;
            m_maskEnabled = s_force || g_gsModernBlur.load(std::memory_order_relaxed) ||
                            g_gsFrameInterp.load(std::memory_order_relaxed);
        }
        // PS2X_OVERLAY_MASK_DUMP=<path>: once, write the frame's overlay mask out
        // as a PGM so the 3D/overlay split can be checked by eye. Dumped here, at
        // the end of a frame, so it holds every overlay draw.
        if (m_maskEnabled)
        {
            static const char *s_dump = std::getenv("PS2X_OVERLAY_MASK_DUMP");
            // Late enough that the frame has been drawn and copied, like the
            // presents the compose debug uses.
            static const uint64_t s_dumpFrame = []()
            {
                const char *env = std::getenv("PS2X_OVERLAY_MASK_DUMP_FRAME");
                return env ? std::strtoull(env, nullptr, 10) : 0ull;
            }();
            const bool s_dumpRange = s_dumpFrame == 0ull ? (m_presentIndex >= 40u && m_presentIndex <= 46u)
                                                         : m_presentIndex == s_dumpFrame;
            if (s_dump != nullptr && s_dumpRange)
            {
                for (auto &target : m_targets)
                {
                    // The draws set maskFrame to the present index as it was
                    // during the frame; present() has just incremented it.
                    if (target->maskTexture == 0 || target->maskFrame + 1u != m_presentIndex)
                        continue;
                    const uint32_t w = target->width * m_scale, h = target->height * m_scale;
                    std::vector<uint8_t> px(static_cast<size_t>(w) * h, 0u);
                    GL &gl = m_context.gl;
                    gl.glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
                    gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    gl.glGetTextureSubImage(target->maskTexture, 0, 0, 0, 0, static_cast<GLsizei>(w),
                                            static_cast<GLsizei>(h), 1, GL_RED, GL_UNSIGNED_BYTE,
                                            static_cast<GLsizei>(px.size()), px.data());
                    // One file per target: the mask is built on the scene target
                    // and propagated through the copies, so seeing each hop says
                    // where a mis-mapping creeps in.
                    char path[1024];
                    std::snprintf(path, sizeof(path), "%s.%llu_%u_%ux%u.pgm", s_dump,
                                  (unsigned long long)m_presentIndex, target->block, w, h);
                    if (FILE *f = std::fopen(path, "wb"))
                    {
                        std::fprintf(f, "P5\n%u %u\n255\n", w, h);
                        std::fwrite(px.data(), 1, px.size(), f);
                        std::fclose(f);
                    }
                    std::cerr << "[gs] overlay mask: dumped " << w << "x" << h << " (block " << target->block << ") to "
                              << path << std::endl;
                }
            }
        }
        {
            static const char *s_traceDraws = std::getenv("PS2X_GS_TRACE_DRAWS");
            // Mark the *start* of the traced frame, so the draws that follow are
            // that frame's. (This used to fire on the next present, which made the
            // trace and the mask dump describe different frames - they were off by
            // one, and comparisons between them were meaningless.)
            if (s_traceDraws != nullptr &&
                static_cast<uint64_t>(std::strtoull(s_traceDraws, nullptr, 10)) == m_presentIndex)
                std::fprintf(stderr, "[gs:present] --- start of frame %llu ---\n", (unsigned long long)m_presentIndex);
        }
        // Presentation reads local memory (both circuits, preferred source).
        m_syncReason = "present";
        m_replUploadedThisFrame = 0;
        updateReplacements();
        if (replacementsActive())
        {
            static const bool s_stats = std::getenv("PS2X_TEXREPL_STATS") != nullptr;
            static const uint32_t s_interval = []()
            {
                const char *env = std::getenv("PS2X_TEXREPL_STATS");
                if (!env || !*env || std::strcmp(env, "1") == 0)
                    return 300u;
                return static_cast<uint32_t>(std::strtoul(env, nullptr, 10));
            }();
            static uint32_t s_presents = 0;
            if (s_stats && ++s_presents % s_interval == 0u)
                emitTexreplStats();
        }
        PresentationFrame frame;
        // PS2X_GS_GPU_COMPOSE=1 (default on): compose the display on the GPU
        // at scale 1 as well. The CPU fallback costs a full local-memory
        // readback plus a CPU merge every frame; the GPU path composes into a
        // texture and transfers only the display. Set to 0 for the CPU
        // reference path.
        static const bool s_gpuComposeLowScale = []()
        {
            const char *env = std::getenv("PS2X_GS_GPU_COMPOSE");
            return env == nullptr || std::atoi(env) != 0;
        }();
        if (m_gpu && (m_scale > 1u || s_gpuComposeLowScale))
        {
            // Upscaled: compose from the render targets; local memory (and
            // the CPU composition) only for displays the GPU didn't draw.
            flushBatch();
            ScopeTimer timer{m_timePresent};
            upscalePresentation(request, frame);
            gpuEnd();
        }
        if (!frame && !frame.unchanged)
        {
            readbackVram();
            m_cpu->Sync(GSSyncReason::Presentation);
            m_cpu->TextureFlush();
            ScopeTimer timer{m_timePresent};
            frame = m_cpu->Present(request);
        }
        if (const uint32_t requested = g_gsGpuScaleRequest.load(std::memory_order_relaxed); requested != 0u)
            setScale(requested);
        applyPendingScale(); // between frames
        gpuTimersFrameEnd();
        return frame;
    }

    // Sizes the compose target for the motion blur. Grow-only, like the present
    // pair: the display size churns through the logos and the intro, and
    // recreating this on every change would hitch exactly there.
    void Renderer::ensureBlurTargets(GL &gl, uint32_t width, uint32_t height)
    {
        if (m_composeTexture == 0 || m_composeWidth < width || m_composeHeight < height)
        {
            if (m_composeTexture != 0)
            {
                gl.glDeleteFramebuffers(1, &m_composeFramebuffer);
                gl.glDeleteTextures(1, &m_composeTexture);
            }
            m_composeWidth = std::max(m_composeWidth, width);
            m_composeHeight = std::max(m_composeHeight, height);
            gl.glCreateTextures(GL_TEXTURE_2D, 1, &m_composeTexture);
            gl.glTextureStorage2D(m_composeTexture, 1, GL_RGBA8, static_cast<GLsizei>(m_composeWidth),
                                  static_cast<GLsizei>(m_composeHeight));
            gl.glTextureParameteri(m_composeTexture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl.glTextureParameteri(m_composeTexture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl.glTextureParameteri(m_composeTexture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.glTextureParameteri(m_composeTexture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl.glCreateFramebuffers(1, &m_composeFramebuffer);
            gl.glNamedFramebufferTexture(m_composeFramebuffer, GL_COLOR_ATTACHMENT0, m_composeTexture, 0);
            gl.glNamedFramebufferDrawBuffer(m_composeFramebuffer, GL_COLOR_ATTACHMENT0);
        }
    }

    void Renderer::destroyBlurTargets()
    {
        GL &gl = m_context.gl;
        if (m_composeTexture != 0)
        {
            gl.glDeleteFramebuffers(1, &m_composeFramebuffer);
            gl.glDeleteTextures(1, &m_composeTexture);
            m_composeTexture = m_composeFramebuffer = 0;
        }
    }

    void Renderer::blurCompose(GL &gl, uint32_t width, uint32_t height, GLuint destinationFramebuffer)
    {
        // The present path publishes the camera after the compose, so the value
        // here is the one this frame was built with. Read it twice around the
        // generation counter: the game thread writes it, and a torn read would
        // mix one frame's orientation with another's position.
        Ps2CameraState cur;
        static const char *s_fakeCamera = std::getenv("PS2X_BLUR_FAKE_CAMERA");
        if (s_fakeCamera != nullptr)
        {
            // A synthetic camera, for checking the blur without the game running
            // (a GS capture has no camera of its own): a level camera yawing by
            // this many degrees per presented frame, which smears the world
            // horizontally. Test hook only.
            const float yaw = static_cast<float>(std::atof(s_fakeCamera)) * static_cast<float>(m_presentIndex) *
                              3.14159265f / 180.0f;
            cur.front[0] = std::cos(yaw);
            cur.front[1] = std::sin(yaw);
            cur.front[2] = 0.0f;
            cur.up[0] = 0.0f;
            cur.up[1] = 0.0f;
            cur.up[2] = 1.0f;
            cur.pos[0] = 0.0f;
            cur.pos[1] = 0.0f;
            cur.pos[2] = 16.0f;
            cur.viewX = 0.7f;
            cur.viewY = 0.525f;
            cur.valid = true;
        }
        else
        {
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                const uint64_t before = g_ps2CameraFrame.load(std::memory_order_relaxed);
                cur = g_ps2Camera;
                if (g_ps2CameraFrame.load(std::memory_order_relaxed) == before)
                    break;
            }
        }

        // Depth in world units: 1 unit is about 0.1 m, so 200 is ~20 m, roughly
        // what fills a chase-camera frame. It only scales the translation part -
        // rotation is exact - so raising it is how "too smeary while driving" is
        // dialled back without touching turns.
        static const float s_depth = []()
        {
            const char *env = std::getenv("PS2X_BLUR_DEPTH");
            return env ? static_cast<float>(std::atof(env)) : 200.0f;
        }();

        const Ps2CameraHomography warp = ps2CameraHomography(m_blurPrevCamera, cur, s_depth);
        m_blurPrevCamera = cur;

        // No usable camera (the intro is a cutscene with none) means no motion to
        // smear along: shutter 0 passes the frame through.
        const float shutter = warp.usable ? g_gsModernBlurStrength.load(std::memory_order_relaxed) : 0.0f;

        ensureBlurTargets(gl, width, height);
        {
            static bool s_reported = false;
            if (!s_reported)
            {
                s_reported = true;
                std::cerr << "[gs] motion blur: " << (warp.usable ? "warp" : "no camera") << ", shutter " << shutter
                          << ", depth " << s_depth << ", " << width << "x" << height << std::endl;
            }
        }
        if (m_blur.id == 0)
        {
            // A compile failure: still get the frame out, unblurred.
            gl.glBlitNamedFramebuffer(m_composeFramebuffer, destinationFramebuffer, 0, 0, static_cast<GLint>(width),
                                      static_cast<GLint>(height), 0, 0, static_cast<GLint>(width),
                                      static_cast<GLint>(height), GL_COLOR_BUFFER_BIT, GL_NEAREST);
            return;
        }
        Program &p = m_blur;
        useProgram(p.id);
        setUniform3f(p, uniform(p, "u_warp0"), warp.m[0], warp.m[1], warp.m[2]);
        setUniform3f(p, uniform(p, "u_warp1"), warp.m[3], warp.m[4], warp.m[5]);
        setUniform3f(p, uniform(p, "u_warp2"), warp.m[6], warp.m[7], warp.m[8]);
        setUniform2f(p, uniform(p, "u_size"), static_cast<float>(width), static_cast<float>(height));
        setUniform1f(p, uniform(p, "u_shutter"), shutter);
        gl.glBindTextureUnit(0, m_composeTexture);
        bindFramebuffer(destinationFramebuffer);
        setViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        setScissorTest(false);
        setBlend(false);
        setDepthTest(false);
        setColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        bindVao(m_emptyVao);
        gl.glDrawArrays(GL_TRIANGLES, 0, 3);
        gl.glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    }

    // Reads the published camera, keeping the previous one. Called once per
    // compose whatever else is enabled: the interpolation deltas are between
    // composed frames, so this must not depend on the blur being on.
    // Seconds on the render thread's steady clock.
    static double interpNow()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void Renderer::trackCamera(const GSPresentationRequest &request)
    {
        m_interpShifted = false;
        Ps2CameraState cur;
        // The same synthetic camera the blur uses, for testing from a capture
        // (which has no game running to publish a real one).
        static const char *s_fake = std::getenv("PS2X_BLUR_FAKE_CAMERA");
        if (s_fake != nullptr)
        {
            const float yaw = static_cast<float>(std::atof(s_fake)) * static_cast<float>(m_presentIndex) *
                              3.14159265f / 180.0f;
            cur.front[0] = std::cos(yaw);
            cur.front[1] = std::sin(yaw);
            cur.front[2] = 0.0f;
            cur.up[0] = 0.0f;
            cur.up[1] = 0.0f;
            cur.up[2] = 1.0f;
            cur.pos[0] = 0.0f;
            cur.pos[1] = 0.0f;
            cur.pos[2] = 16.0f;
            cur.viewX = 0.7f;
            cur.viewY = 0.525f;
            cur.valid = true;
        }
        else if (request.hasCamera)
        {
            // The camera latched with this very picture, on the game thread.
            cur = request.camera;
        }
        else
        {
            // No camera with the request (a replay): the last published one.
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                const uint64_t before = g_ps2CameraFrame.load(std::memory_order_relaxed);
                cur = g_ps2Camera;
                if (g_ps2CameraFrame.load(std::memory_order_relaxed) == before)
                    break;
            }
        }
        m_requestCamera = cur;
    }

    static bool sameCamera(const Ps2CameraState &a, const Ps2CameraState &b)
    {
        for (int i = 0; i < 3; ++i)
            if (a.front[i] != b.front[i] || a.pos[i] != b.pos[i] || a.up[i] != b.up[i])
                return false;
        return a.viewX == b.viewX && a.viewY == b.viewY && a.valid == b.valid;
    }

    // Frame interpolation pairs each picture with its camera by *picture
    // change*, which the renderer sees exactly (the composed source's drawSeq).
    // VCS draws a new view every 2-4 VBlanks while a picture is composed every
    // VBlank, and the camera that comes with a picture can arrive a compose
    // early or late - pairing by camera change instead made the "previous
    // picture" sometimes already the new one, so the sweep overshot a whole
    // step and snapped back: the jitter.
    //   picture changed -> the old picture and its camera start the pair, and
    //                      the pair waits (pending) for the new picture's camera
    //   pending         -> the first camera that differs is the new picture's,
    //                      early or late (measured: at most one compose late);
    //                      none by the next compose: a still camera
    void Renderer::interpPair(uint64_t contentSeq)
    {
        m_interpShifted = false;
        m_interpPairedNow = false;
        const bool pictureChanged = contentSeq != m_interpContentSeq;
        m_interpContentSeq = contentSeq;
        if (pictureChanged)
        {
            m_interpPrevPrevCamera = m_interpPrevCamera;
            m_interpPrevCamera = m_interpCamera; // the camera the old picture was shown with
            m_interpPending = true;
            m_interpPendingComposes = 0;
            m_interpShifted = true;
        }
        if (!m_interpPending)
            return;
        const bool cameraArrived = !sameCamera(m_requestCamera, m_interpPrevCamera);
        if (cameraArrived || ++m_interpPendingComposes > 1)
        {
            m_interpCamera = cameraArrived ? m_requestCamera : m_interpPrevCamera;
            // A cut, or the game snapping the camera round (it recentres by
            // 100+ degrees in a frame or two): warping the old picture across
            // that only smears it. Show plain pictures until the next pair.
            if (cameraArrived)
            {
                const Ps2CameraState &a = m_interpPrevCamera, &b = m_interpCamera;
                const float dot = a.front[0] * b.front[0] + a.front[1] * b.front[1] + a.front[2] * b.front[2];
                const float dx = a.pos[0] - b.pos[0], dy = a.pos[1] - b.pos[1], dz = a.pos[2] - b.pos[2];
                if (dot < 0.9659f /* 15 degrees */ || dx * dx + dy * dy + dz * dz > 9.0f /* 3 units */)
                    m_interpPrevCamera = m_interpCamera;
            }
            m_interpPending = false;
            const double now = interpNow();
            const double gap = now - m_interpPairTime;
            // The interval between game frames, averaged so the sweep has a
            // steady length; a long gap (a load, a pause) is not a frame time.
            if (m_interpHavePrev && gap > 0.004 && gap < 0.25)
                m_interpInterval = std::clamp(m_interpInterval * 0.7 + gap * 0.3, 1.0 / 60.0, 1.0 / 10.0);
            m_interpPairTime = now;
            m_interpHavePrev = m_interpHavePrev || m_interpCamera.valid;
            m_interpPairedNow = cameraArrived;
        }
    }

    // PS2X_GS_PRESENT_SHARE: -1 automatic, 1 forced shared, 0 forced readback.
    // Used by both the present and the synthesis, which must agree on which path
    // is active: the host draws the composed picture at the readback size, so a
    // synthesized frame handed over at the composed size is resampled differently
    // and the picture pulses between the two.
    static int presentShareSetting()
    {
        static const int s_setting = []()
        {
            const char *env = std::getenv("PS2X_GS_PRESENT_SHARE");
            return env != nullptr ? (std::atoi(env) != 0 ? 1 : 0) : -1;
        }();
        return s_setting;
    }

    // The camera at fraction t of the way from prev to cur, and the homography
    // from the previous view to that camera. The synthesized frame is warped
    // *between* the previous and current pictures, never beyond the newest one -
    // extrapolating past it predicts the future, and a camera that accelerates
    // makes the whole picture wobble around the static HUD.
    static bool interpolateCameraHomography(const Ps2CameraState &prev, const Ps2CameraState &cur, float t,
                                            float depth, float out[9], float back[9])
    {
        // Basis rows (right, up, front), the same convention as ps2CameraHomography.
        const auto basis = [](const Ps2CameraState &c, float R[9]) -> bool
        {
            const float f[3] = {c.front[0], c.front[1], c.front[2]};
            const float u[3] = {c.up[0], c.up[1], c.up[2]};
            float r[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
            const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            if (!(rl > 1e-4f) || !(fl > 1e-4f))
                return false;
            for (int i = 0; i < 3; ++i)
            {
                r[i] /= rl;
                R[6 + i] = f[i] / fl;
            }
            R[3] = r[1] * R[8] - r[2] * R[7];
            R[4] = r[2] * R[6] - r[0] * R[8];
            R[5] = r[0] * R[7] - r[1] * R[6];
            R[0] = r[0];
            R[1] = r[1];
            R[2] = r[2];
            return true;
        };
        float Rp[9], Rc[9];
        if (!basis(prev, Rp) || !basis(cur, Rc))
            return false;
        // S = Rp * Rc^T maps the current frame into the previous one; the
        // camera's step from prev to cur is S^T. Walk t of that step from prev.
        float S[9];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                S[i * 3 + j] = Rp[i * 3] * Rc[j * 3] + Rp[i * 3 + 1] * Rc[j * 3 + 1] + Rp[i * 3 + 2] * Rc[j * 3 + 2];
        const float cosA = std::clamp((S[0] + S[4] + S[8] - 1.0f) * 0.5f, -1.0f, 1.0f);
        const float ang = std::acos(cosA);
        if (ang < 1e-4f)
            return false; // barely moving: the blend below is exact enough
        const float s = 0.5f / std::sin(ang);
        const float ax = (S[7] - S[5]) * s, ay = (S[2] - S[6]) * s, az = (S[3] - S[1]) * s;
        // R = Rot(axis, -t*ang) * Rp
        const float th = -t * ang;
        const float c = std::cos(th), sn = std::sin(th), ic = 1.0f - c;
        const float Rot[9] = {c + ax * ax * ic,     ax * ay * ic - az * sn, ax * az * ic + ay * sn,
                              ay * ax * ic + az * sn, c + ay * ay * ic,      ay * az * ic - ax * sn,
                              az * ax * ic - ay * sn, az * ay * ic + ax * sn, c + az * az * ic};
        float Ri[9];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                Ri[i * 3 + j] = Rot[i * 3] * Rp[j] + Rot[i * 3 + 1] * Rp[3 + j] + Rot[i * 3 + 2] * Rp[6 + j];
        Ps2CameraState mid = prev;
        for (int i = 0; i < 3; ++i)
        {
            mid.front[i] = Ri[6 + i];
            mid.up[i] = Ri[3 + i];
            mid.pos[i] = prev.pos[i] + t * (cur.pos[i] - prev.pos[i]);
        }
        // The shader samples the source at q for each output pixel, so it needs
        // the mapping output(mid) -> source(prev). (a, b) maps b -> a, so that is
        // (prev, mid) - not (mid, prev), which is its inverse. The inverse is the
        // identity at t = 0, which is why a still camera looked right and every
        // motion went the wrong way.
        const Ps2CameraHomography hm = ps2CameraHomography(prev, mid, depth);
        const Ps2CameraHomography hb = ps2CameraHomography(cur, mid, depth); // output(mid) -> newest
        if (!hm.usable || !hb.usable)
            return false;
        const float inv = hm.m[8] != 0.0f ? 1.0f / hm.m[8] : 1.0f;
        const float invB = hb.m[8] != 0.0f ? 1.0f / hb.m[8] : 1.0f;
        for (int i = 0; i < 9; ++i)
        {
            out[i] = hm.m[i] * inv;
            back[i] = hb.m[i] * invB;
        }
        return true;
    }

    // PS2X_INTERP_ALIGN=1: at a new game frame, how well does the previous
    // picture warped fully (t=1) to the new camera match the new picture? With
    // the camera paired to the right picture, the warp should beat the unwarped
    // picture; if the camera runs a frame ahead, warping with the pair one frame
    // earlier wins instead. Mean |luma| difference over the central 60%.
    void Renderer::interpAlignCheck()
    {
        static uint64_t s_n = 0;
        if ((++s_n % 4u) != 0u || m_prevComposedTexture == 0 || m_lastComposedTexture == 0 || !m_interpHavePrev ||
            !m_interpPrevCamera.valid || !m_interpCamera.valid)
            return;
        GL &gl = m_context.gl;
        const auto read = [&](GLuint texture, uint32_t w, uint32_t h, std::vector<uint8_t> &out)
        {
            out.assign(size_t(w) * h * 4u, 0u);
            gl.glGetTextureSubImage(texture, 0, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 1, GL_RGBA,
                                    GL_UNSIGNED_BYTE, static_cast<GLsizei>(out.size()), out.data());
        };
        const auto luma = [](const std::vector<uint8_t> &img, uint32_t w, uint32_t h, float u, float v)
        {
            const uint32_t x = std::min(w - 1u, static_cast<uint32_t>(u * w));
            const uint32_t y = std::min(h - 1u, static_cast<uint32_t>(v * h));
            const uint8_t *p = img.data() + (size_t(y) * w + x) * 4u;
            return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
        };
        const uint32_t w = m_lastComposedWidth, h = m_lastComposedHeight;
        std::vector<uint8_t> cur, prev, warped;
        read(m_lastComposedTexture, w, h, cur);
        read(m_prevComposedTexture, m_prevComposedWidth, m_prevComposedHeight, prev);
        const auto error = [&](const std::vector<uint8_t> &img, uint32_t iw, uint32_t ih)
        {
            double sum = 0.0;
            int n = 0;
            for (int j = 0; j < 48; ++j)
                for (int i = 0; i < 64; ++i)
                {
                    const float u = 0.2f + 0.6f * (i + 0.5f) / 64.0f, v = 0.2f + 0.6f * (j + 0.5f) / 48.0f;
                    sum += std::fabs(luma(img, iw, ih, u, v) - luma(cur, w, h, u, v));
                    ++n;
                }
            return sum / n;
        };
        const double eIdentity = error(prev, m_prevComposedWidth, m_prevComposedHeight);
        const auto warpError = [&]() -> double
        {
            uint32_t sw = 0, sh = 0;
            const GLuint synth = synthesizeHostFrame(1.0f, sw, sh);
            if (synth == 0 || sw == 0 || sh == 0)
                return -1.0;
            read(synth, sw, sh, warped);
            return error(warped, sw, sh);
        };
        const double eNow = warpError();
        // PS2X_INTERP_DUMP_PAIR=<prefix>: once, the previous picture, the frame
        // halfway between, and the newest picture as PPMs (a visual check).
        if (const char *prefix = std::getenv("PS2X_INTERP_DUMP_PAIR"))
        {
            static int s_dumped = 0;
            const float yawStep = std::fabs(std::atan2(m_interpCamera.front[1], m_interpCamera.front[0]) -
                                            std::atan2(m_interpPrevCamera.front[1], m_interpPrevCamera.front[0]));
            if (s_dumped < 1 && yawStep > 0.02f && yawStep < 0.5f)
            {
                ++s_dumped;
                const auto writePpm = [&](const std::string &path, const std::vector<uint8_t> &img, uint32_t iw, uint32_t ih)
                {
                    if (FILE *f = std::fopen(ps2x::narrow(path).c_str(), "wb"))
                    {
                        std::fprintf(f, "P6\n%u %u\n255\n", iw, ih);
                        for (uint32_t y = 0; y < ih; ++y)
                            for (uint32_t x = 0; x < iw; ++x)
                                std::fwrite(img.data() + (size_t(ih - 1u - y) * iw + x) * 4u, 1, 3, f);
                        std::fclose(f);
                    }
                };
                writePpm(std::string(prefix) + "_prev.ppm", prev, m_prevComposedWidth, m_prevComposedHeight);
                writePpm(std::string(prefix) + "_next.ppm", cur, w, h);
                uint32_t sw = 0, sh = 0;
                const GLuint mid = synthesizeHostFrame(0.5f, sw, sh);
                if (mid != 0)
                {
                    read(mid, sw, sh, warped);
                    writePpm(std::string(prefix) + "_mid.ppm", warped, sw, sh);
                }
                for (int i = 0; i < 2 && m_interpMask[i] != 0; ++i)
                {
                    std::vector<uint8_t> m(size_t(m_interpMaskWidth) * m_interpMaskHeight);
                    gl.glGetTextureSubImage(m_interpMask[i], 0, 0, 0, 0, static_cast<GLsizei>(m_interpMaskWidth),
                                            static_cast<GLsizei>(m_interpMaskHeight), 1, GL_RED, GL_UNSIGNED_BYTE,
                                            static_cast<GLsizei>(m.size()), m.data());
                    size_t on = 0;
                    for (uint8_t v : m)
                        on += v != 0;
                    if (FILE *f = std::fopen((std::string(prefix) + "_mask" + std::to_string(i) + ".pgm").c_str(), "wb"))
                    {
                        std::fprintf(f, "P5\n%u %u\n255\n", m_interpMaskWidth, m_interpMaskHeight);
                        for (uint32_t y = 0; y < m_interpMaskHeight; ++y)
                            std::fwrite(m.data() + size_t(m_interpMaskHeight - 1u - y) * m_interpMaskWidth, 1, m_interpMaskWidth, f);
                        std::fclose(f);
                    }
                    std::cerr << "[interp] mask[" << i << "] (" << (i == m_interpMaskCur ? "newest" : "previous")
                              << ") " << m_interpMaskWidth << "x" << m_interpMaskHeight << " set=" << on << std::endl;
                }
                {
                    Target *mt = m_overlayTarget;
                    std::cerr << "[interp] overlayTarget=" << (mt ? mt->block : 0u) << " maskTex=" << (mt ? mt->maskTexture : 0u)
                              << " maskEnabled=" << m_maskEnabled << std::endl;
                }
                std::cerr << "[interp] dumped a pair, yaw step " << yawStep * 57.29578f << " deg" << std::endl;
            }
        }
        // FOV scale sweep (and the reversed direction): which projection makes
        // the warp match the new picture best?
        {
            const Ps2CameraState savedPrev = m_interpPrevCamera, savedCur = m_interpCamera;
            static const float kScales[] = {0.5f, 0.7f, 0.85f, 1.0f, 1.2f, 1.4f, 2.0f};
            std::ostringstream line;
            line << "[alignfov]";
            for (float scale : kScales)
            {
                m_interpPrevCamera.viewX = savedPrev.viewX * scale;
                m_interpPrevCamera.viewY = savedPrev.viewY * scale;
                m_interpCamera.viewX = savedCur.viewX * scale;
                m_interpCamera.viewY = savedCur.viewY * scale;
                line << " s" << scale << "=" << warpError();
            }
            m_interpPrevCamera = savedCur;
            m_interpCamera = savedPrev;
            line << " reversed=" << warpError() << " viewX=" << savedCur.viewX << " viewY=" << savedCur.viewY;
            m_interpPrevCamera = savedPrev;
            m_interpCamera = savedCur;
            std::cerr << line.str() << std::endl;
        }
        double eEarlier = -1.0;
        if (m_interpPrevPrevCamera.valid)
        {
            const Ps2CameraState savedPrev = m_interpPrevCamera, savedCur = m_interpCamera;
            m_interpCamera = m_interpPrevCamera;
            m_interpPrevCamera = m_interpPrevPrevCamera;
            eEarlier = warpError();
            m_interpPrevCamera = savedPrev;
            m_interpCamera = savedCur;
        }
        const float yaw = std::atan2(m_interpCamera.front[1], m_interpCamera.front[0]) -
                          std::atan2(m_interpPrevCamera.front[1], m_interpPrevCamera.front[0]);
        std::cerr << "[align] identity=" << eIdentity << " warpNow=" << eNow << " warpEarlierPair=" << eEarlier
                  << " yawStepDeg=" << yaw * 57.29578f << std::endl;
        invalidateGlState();
    }

    // Warps the last composed frame forward by half the last camera motion and
    // writes it into m_synthTexture. Overlays are copied, not warped: the HUD is
    // identical in the two pictures it sits between, so moving it would be wrong.
    // Returns the texture, or 0 if it cannot make one.
    static float cameraYawDeg(const Ps2CameraState &c)
    {
        return std::atan2(c.front[1], c.front[0]) * 57.29578f;
    }

    GLuint Renderer::synthesizeHostFrame(float phase, uint32_t &outWidth, uint32_t &outHeight)
    {
        if (m_synth.id == 0 || m_prevComposedTexture == 0 || !m_interpHavePrev || !m_interpPrevCamera.valid ||
            !m_interpCamera.valid || m_prevComposedWidth == 0 || m_prevComposedHeight == 0)
        {
            // PS2X_SYNTH_WHY=1: say which guard refused, so a synthesis that
            // silently does nothing can be told from one that never runs.
            static const bool s_why = std::getenv("PS2X_SYNTH_WHY") != nullptr;
            if (s_why)
            {
                static uint64_t s_n = 0;
                if ((s_n++ % 60u) == 0u)
                    std::cerr << "[gs] synth refused: program=" << m_synth.id << " prevComposed=" << m_prevComposedTexture
                              << " havePrev=" << m_interpHavePrev << " prevValid=" << m_interpPrevCamera.valid
                              << " curValid=" << m_interpCamera.valid << " size=" << m_lastComposedWidth << "x"
                              << m_lastComposedHeight << std::endl;
            }
            return 0;
        }

        static const float s_depth = []()
        {
            const char *env = std::getenv("PS2X_BLUR_DEPTH");
            // 15: VCS's camera orbits the player at close range, so the near
            // scene dominates the motion. PS2X_INTERP_ALIGN measured the warp
            // removing ~25% of the frame-to-frame difference at 8-20 vs ~10% at
            // 200 and ~2% for pure rotation.
            return env ? static_cast<float>(std::atof(env)) : 15.0f;
        }();
        // Interpolation: this frame shows the picture a fraction t of the way
        // from the previous composed frame to the current one, warped by the
        // interpolated camera. Extrapolation was tried first - warping past the
        // newest picture - and the owner saw it: a camera that accelerates makes
        // the prediction wrong, and the whole frame wobbles around the static
        // HUD. Both endpoints are known here, so the positions are always right.
        // phase < 0: the renderer's own phase - how far the sweep from the
        // previous game frame to the current one has got, on the clock that
        // timed the pair. The host's clock cannot know when the pair changed:
        // it reset the phase at its own VBlank, and when that came before the
        // compose it showed the *old* pair at t=0 - a step back, then forward.
        float t = phase;
        if (phase < 0.0f)
        {
            // A still camera: nothing to interpolate, show the newest picture.
            // (The next pair starts from exactly this picture, so nothing jumps.)
            if (!m_interpPending && sameCamera(m_interpPrevCamera, m_interpCamera))
            {
                g_gsInterpLastPhase.store(1.0f, std::memory_order_relaxed);
                g_gsInterpShownYaw.store(cameraYawDeg(m_interpCamera), std::memory_order_relaxed);
                return 0;
            }
            const double elapsed = (m_synthDisplayTime > 0.0 ? m_synthDisplayTime : interpNow()) - m_interpPairTime;
            // Waiting for the new picture's camera: hold the old picture (t=0),
            // which is what is already on screen.
            t = m_interpPending ? 0.0f : static_cast<float>(elapsed / m_interpInterval);
            // Swept through: the current picture *is* this view, with its own
            // up-to-date content - show it as it is.
            if (t >= 1.0f)
            {
                g_gsInterpLastPhase.store(1.0f, std::memory_order_relaxed);
                g_gsInterpShownYaw.store(cameraYawDeg(m_interpCamera), std::memory_order_relaxed);
                return 0;
            }
        }
        t = std::clamp(t, 0.0f, 1.0f);
        g_gsInterpLastPhase.store(t, std::memory_order_relaxed);
        {
            float a = cameraYawDeg(m_interpPrevCamera), b = cameraYawDeg(m_interpCamera);
            if (b - a > 180.0f)
                b -= 360.0f;
            else if (a - b > 180.0f)
                b += 360.0f;
            g_gsInterpShownYaw.store(a + t * (b - a), std::memory_order_relaxed);
        }
        float ext[9], back[9];
        const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        if (t == 0.0f)
        {
            // The old picture exactly as it was (the newest is not blended in).
            std::copy(identity, identity + 9, ext);
            std::copy(identity, identity + 9, back);
        }
        else if (!interpolateCameraHomography(m_interpPrevCamera, m_interpCamera, t, s_depth, ext, back))
        {
            // Barely moving: the linear blend between the two views is exact
            // enough, and it needs no axis-angle path.
            const Ps2CameraHomography hf = ps2CameraHomography(m_interpPrevCamera, m_interpCamera, s_depth);
            const Ps2CameraHomography hr = ps2CameraHomography(m_interpCamera, m_interpPrevCamera, s_depth);
            if (!hf.usable || !hr.usable)
                return 0;
            const float inv = hf.m[8] != 0.0f ? 1.0f / hf.m[8] : 1.0f;
            const float invR = hr.m[8] != 0.0f ? 1.0f / hr.m[8] : 1.0f;
            for (int i = 0; i < 9; ++i)
            {
                ext[i] = t * hf.m[i] * inv + (1.0f - t) * identity[i];
                back[i] = (1.0f - t) * hr.m[i] * invR + t * identity[i];
            }
        }

        GL &gl = m_context.gl;
        const uint32_t w = m_prevComposedWidth, hgt = m_prevComposedHeight;
        // Render the warp at the size the host will draw it, not at the composed
        // size: at internal scale 4 that is a 7x smaller pass, and 180 of these
        // a second stalled the game's own GS feeding during cutscenes. The warp
        // works in normalised coordinates, so only the sampling grid changes.
        uint32_t rw = w, rh = hgt;
        if (!(m_context.sharedWithHost() && presentShareSetting() == 1))
        {
            const uint32_t hostWidth = g_gsGpuPresentWidth.load(std::memory_order_relaxed);
            const uint32_t hostHeight = g_gsGpuPresentHeight.load(std::memory_order_relaxed);
            if (hostWidth != 0u && hostHeight != 0u && hostWidth < w && hostHeight < hgt)
            {
                const double fit = std::min(static_cast<double>(hostWidth) / static_cast<double>(w),
                                            static_cast<double>(hostHeight) / static_cast<double>(hgt));
                rw = std::max<uint32_t>(64u, static_cast<uint32_t>(static_cast<double>(w) * fit) & ~1u);
                rh = std::max<uint32_t>(64u, static_cast<uint32_t>(static_cast<double>(hgt) * fit) & ~1u);
            }
        }
        if (m_synthTexture[0] == 0 || m_synthWidth < rw || m_synthHeight < rh)
        {
            for (int i = 0; i < kSynthRing; ++i)
            {
                if (m_synthTexture[i] != 0)
                {
                    gl.glDeleteFramebuffers(1, &m_synthFramebuffer[i]);
                    gl.glDeleteTextures(1, &m_synthTexture[i]);
                    m_synthTexture[i] = m_synthFramebuffer[i] = 0;
                }
            }
            m_synthWidth = std::max(m_synthWidth, rw);
            m_synthHeight = std::max(m_synthHeight, rh);
            for (int i = 0; i < kSynthRing; ++i)
            {
                gl.glCreateTextures(GL_TEXTURE_2D, 1, &m_synthTexture[i]);
                gl.glTextureStorage2D(m_synthTexture[i], 1, GL_RGBA8, static_cast<GLsizei>(m_synthWidth),
                                      static_cast<GLsizei>(m_synthHeight));
                gl.glTextureParameteri(m_synthTexture[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                gl.glTextureParameteri(m_synthTexture[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                gl.glTextureParameteri(m_synthTexture[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                gl.glTextureParameteri(m_synthTexture[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                gl.glCreateFramebuffers(1, &m_synthFramebuffer[i]);
                gl.glNamedFramebufferTexture(m_synthFramebuffer[i], GL_COLOR_ATTACHMENT0, m_synthTexture[i], 0);
                gl.glNamedFramebufferDrawBuffer(m_synthFramebuffer[i], GL_COLOR_ATTACHMENT0);
            }
            m_synthIndex = 0;
        }
        // Next slot in the ring: whatever is still in flight is left alone.
        m_synthIndex = (m_synthIndex + 1) % kSynthRing;

        const bool maskValid = m_interpMask[0] != 0 && m_interpMaskWidth == w && m_interpMaskHeight == hgt;
        Program &p = m_synth;
        useProgram(p.id);
        setUniform3f(p, uniform(p, "u_warp0"), ext[0], ext[1], ext[2]);
        setUniform3f(p, uniform(p, "u_warp1"), ext[3], ext[4], ext[5]);
        setUniform3f(p, uniform(p, "u_warp2"), ext[6], ext[7], ext[8]);
        setUniform3f(p, uniform(p, "u_back0"), back[0], back[1], back[2]);
        setUniform3f(p, uniform(p, "u_back1"), back[3], back[4], back[5]);
        setUniform3f(p, uniform(p, "u_back2"), back[6], back[7], back[8]);
        setUniform1f(p, uniform(p, "u_t"), t);
        // The newest picture, for the two-way blend: the same size as the
        // previous one, or it is left out.
        const bool nextValid = m_lastComposedTexture != 0 && m_lastComposedWidth == w && m_lastComposedHeight == hgt;
        setUniform1f(p, uniform(p, "u_nextValid"), nextValid ? 1.0f : 0.0f);
        gl.glBindTextureUnit(2, nextValid ? m_lastComposedTexture : m_prevComposedTexture);
        setUniform2f(p, uniform(p, "u_size"), static_cast<float>(rw), static_cast<float>(rh));

        setUniform2f(p, uniform(p, "u_maskCoordScale"), static_cast<float>(w) / static_cast<float>(rw),
                     static_cast<float>(hgt) / static_cast<float>(rh));
        setUniform1f(p, uniform(p, "u_maskValid"), maskValid ? 1.0f : 0.0f);
        gl.glBindTextureUnit(3, maskValid ? m_interpMask[m_interpMaskCur] : m_zeroMask);
        gl.glBindTextureUnit(0, m_prevComposedTexture);
        gl.glBindTextureUnit(1, maskValid ? m_interpMask[m_interpMaskCur ^ 1] : m_zeroMask);
        bindFramebuffer(m_synthFramebuffer[m_synthIndex]);
        setViewport(0, 0, static_cast<GLsizei>(rw), static_cast<GLsizei>(rh));
        setScissorTest(false);
        setBlend(false);
        setDepthTest(false);
        setColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        bindVao(m_emptyVao);
        gl.glDrawArrays(GL_TRIANGLES, 0, 3);
        // The host draws this texture from its own context next, so the writes
        // have to be visible: barrier, then submit. Without the flush the host can
        // sample it before the pass has run, which shows as a black frame - the
        // shared present path does the same thing for the same reason.
        gl.glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
        gl.glFlush();
        // PS2X_SYNTH_CHECK=1: every 60th synthesized frame, read back both the
        // frame this one was warped from and the synthesized frame itself, and
        // report how bright each is and how much of it is black. Three outcomes
        // mean three different things:
        //   source black, synth black  -> the composed frame was already black
        //   source fine, synth black   -> the warp pass itself is producing black
        //   both fine                  -> the renderer's output is right, so the
        //                                 black is introduced after it, on the way
        //                                 to the display
        // The readback is synchronous, so this is a diagnostic and not a setting:
        // it changes the very timing a race would depend on.
        static const bool s_check = std::getenv("PS2X_SYNTH_CHECK") != nullptr;
        if (s_check)
        {
            static uint64_t s_tick = 0;
            if ((++s_tick % 60u) == 0u)
            {
                auto report = [&](GLuint texture, uint32_t tw, uint32_t th, const char *what)
                {
                    if (texture == 0u || tw == 0u || th == 0u)
                        return;
                    std::vector<uint8_t> px(static_cast<size_t>(tw) * th * 4u, 0u);
                    gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
                    gl.glGetTextureSubImage(texture, 0, 0, 0, 0, static_cast<GLsizei>(tw), static_cast<GLsizei>(th), 1,
                                            GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(px.size()), px.data());
                    uint64_t sum = 0, black = 0;
                    for (size_t i = 0; i + 3u < px.size(); i += 4u)
                    {
                        const uint32_t luma = px[i] + px[i + 1u] + px[i + 2u];
                        sum += luma;
                        if (luma < 12u)
                            ++black;
                    }
                    const size_t n = static_cast<size_t>(tw) * th;
                    std::cerr << "[gs] synth check " << what << " " << tw << "x" << th << ": mean brightness "
                              << (n != 0u ? static_cast<double>(sum) / static_cast<double>(n * 3u) : 0.0) << ", black "
                              << (n != 0u ? 100.0 * static_cast<double>(black) / static_cast<double>(n) : 0.0) << "%"
                              << std::endl;
                };
                report(m_prevComposedTexture, m_prevComposedWidth, m_prevComposedHeight, "source");
                // The synthesized texture is at the draw size now, not the
                // composed size - reading it at the composed size returned a
                // GL error and a zero buffer, which read as "black".
                report(m_synthTexture[m_synthIndex], rw, rh, "synth ");
            }
        }
        outWidth = rw;
        outHeight = rh;
        // If the host cannot see our textures, come back as pixels instead. The host
        // draws the frame with its own context, so a texture we own is a name it
        // cannot resolve - which is why interpolation does not work on Windows today.
        // PS2X_INTERP_READBACK=1 forces this route anywhere, for testing it on Linux
        // where the share does work.
        m_synthPixelsValid = false;
        static const bool s_forceReadback = std::getenv("PS2X_INTERP_READBACK") != nullptr;
        if (!m_context.sharedWithHost() || s_forceReadback)
        {
            const size_t bytes = static_cast<size_t>(rw) * rh * 4u;
            m_synthPixels.resize(bytes);
            gl.glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
            gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
            gl.glGetTextureSubImage(m_synthTexture[m_synthIndex], 0, 0, 0, 0, static_cast<GLsizei>(rw),
                                    static_cast<GLsizei>(rh), 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                    static_cast<GLsizei>(bytes), m_synthPixels.data());
            m_synthPixelsWidth = rw;
            m_synthPixelsHeight = rh;
            m_synthPixelsValid = true;
        }
        return m_synthTexture[m_synthIndex];
    }

    void Renderer::upscalePresentation(const GSPresentationRequest &request, PresentationFrame &frame)
    {
        // Every composed frame is a sample for the interpolation, whatever else is
        // enabled - so this runs before any early return.
        trackCamera(request);
        gpuMark(GpuWork::Present);
        // The host draws the composed frame with raylib between our frames, and
        // if its context shares state with ours those calls change bindings
        // behind the cache's back. Dropping the cache here costs a handful of
        // re-applied calls per frame and removes that whole class of bug.
        invalidateGlState();

        // When every enabled circuit reads a buffer the GPU drew, compose
        // the display from the targets at full resolution (the CPU path's
        // merge); anything else is left to the CPU composition.
        // When the GPU cannot compose this picture (the FMV movies and some
        // cutscenes draw in a way the CPU path handles), the composed pair the
        // interpolation warps between goes stale - and with the display one
        // picture behind, that would freeze the screen on the last composable
        // picture while the movie's audio keeps playing. Drop the pair so the
        // host falls back to the raw picture, and require two fresh composes
        // before interpolating again.
        const auto dropInterpSource = [&]()
        {
            m_lastComposedTexture = 0;
            m_prevComposedTexture = 0;
            m_interpHavePrev = false;
        };
        // Not interpolating (cutscene, or the rate is 60): drop the pair, so
        // that when interpolation resumes it starts from two fresh composes
        // rather than warping across the gap.
        if (!g_gsFrameInterp.load(std::memory_order_relaxed))
            dropInterpSource();
        static const bool s_fieldBob = std::getenv("PS2X_FIELD_BOB") != nullptr;
        if (s_fieldBob)
        {
            dropInterpSource();
            return;
        }
        struct Circuit
        {
            Target *target = nullptr;
            uint32_t x = 0, y = 0, width = 0, height = 0;
        };
        auto circuit = [&](uint64_t dispfb, uint64_t display, Circuit &out) -> bool
        {
            const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
            const uint32_t fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
            const uint8_t psm = static_cast<uint8_t>((dispfb >> 15) & 0x1Fu);
            out.x = static_cast<uint32_t>((dispfb >> 32) & 0x7FFu);
            out.y = static_cast<uint32_t>((dispfb >> 43) & 0x7FFu);
            const uint32_t dw = static_cast<uint32_t>((display >> 32) & 0x0FFFu);
            const uint32_t dh = static_cast<uint32_t>((display >> 44) & 0x07FFu);
            const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0x0Fu);
            out.width = (dw + 1u) / (magh + 1u);
            out.height = dh + 1u;
            if (out.width < 64u || out.height < 64u || out.width > 640u || out.height > 512u)
                return false; // the CPU path substitutes defaults here
            for (auto &candidate : m_targets)
            {
                Target &target = *candidate;
                if (target.depth || target.block != fbp * 32u || target.bw != std::max<uint32_t>(fbw, 1u) ||
                    is16Bit(target.psm) != is16Bit(psm))
                    continue;
                if (out.x + out.width > target.width || out.y + out.height > target.usedRows)
                    return false;
                out.target = &target;
                return true;
            }
            return false;
        };
        const bool crt1 = (request.pmode & 1u) != 0u, crt2 = (request.pmode & 2u) != 0u;
        Circuit c1, c2;
        if ((crt1 && !circuit(request.dispfb1, request.display1, c1)) ||
            (crt2 && !circuit(request.dispfb2, request.display2, c2)) || (!crt1 && !crt2))
        {
            dropInterpSource();
            return;
        }
        // A game-specific display source substitution (see GS frontend) is
        // the CPU path's.
        if (request.hasPreferredSource && !(crt1 && crt2) &&
            request.preferredDestFbp == (crt1 ? c1 : c2).target->block / 32u)
        {
            dropInterpSource();
            return;
        }

        GL &gl = m_context.gl;
        const uint32_t s = m_scale;
        const uint32_t width = std::max(crt1 ? c1.width : 0u, crt2 ? c2.width : 0u) * s;
        const uint32_t height = std::max(crt1 ? c1.height : 0u, crt2 ? c2.height : 0u) * s;
        // When the renderer shares the host window's context, the display is
        // composed into one of two exactly-sized textures the host draws
        // directly: no readback, no upload. (Otherwise the growing texture
        // below feeds the CPU readback path.)
        // The pair below is grow-only: it used to be recreated on any
        // display-size change, which churned two full-size textures through the
        // logos/FMV/intro and hitched there.
        // On by default on Linux, where it was built and verified. On Windows
        // the two contexts share through WGL instead of GLX, and the host
        // drawing a texture owned by the renderer's context comes out corrupt
        // there, so Windows takes the readback path unless asked otherwise with
        // PS2X_GS_PRESENT_SHARE=1. PS2X_GS_PRESENT_SHARE=0 forces the readback
        // path everywhere (see the note in gl_context.cpp).
        // Which present path pays depends on the internal scale. The readback
        // path is cheap at native - it is capped to the window, GPU-downscaled
        // and hands the latch over rather than copying - but its cost is
        // proportional to pixels, so at 2x or more it dwarfs everything. The
        // shared path avoids the copy entirely but serialises the two GL
        // contexts, which costs more than the readback when there is little to
        // copy. Measured: at native the shared path was markedly worse (late
        // frames 108-250/600 readback vs 216-293/600 shared, underrun up to
        // 126k vs 163k per 10 s). So: shared only when it pays.
        // PS2X_GS_PRESENT_SHARE overrides: 1 forces shared, 0 forces readback.
        const int s_sharePresent = presentShareSetting();
        // Only when asked for. The shared path avoids a readback per frame, but
        // drawing the renderer's display texture from the host's context makes
        // the interpolated picture shimmer - the synthesized frames and the
        // composed ones end up rendered differently - and the readback path does
        // not. Correctness first: PS2X_GS_PRESENT_SHARE=1 opts in (and =0 is now
        // the same as unset). The renderer still shares the window's context
        // either way, which is what the synthesized frame needs.
        const bool sharePresent = m_context.sharedWithHost() && s_sharePresent == 1;
        {
            // Say once which path is in use: the choice depends on the internal
            // scale, so it is not obvious from the outside.
            static bool s_reported = false;
            if (!s_reported)
            {
                s_reported = true;
                std::cerr << "[gs] present: " << (sharePresent ? "shared texture" : "readback")
                          << " (scale " << m_scale << ", "
                          << (s_sharePresent < 0 ? "automatic" : "forced by PS2X_GS_PRESENT_SHARE") << ")" << std::endl;
            }
        }
        const uint32_t composedWidth = width, composedHeight = height;
        if (sharePresent)
        {
            // The host draws this texture with raylib, so its filtering has to
            // follow the "smooth filtering" setting here (raylib only sets
            // parameters on textures it loaded itself).
            const GLint filter = ps2HostSettings().smoothFiltering ? GL_LINEAR : GL_NEAREST;
            if (filter != m_presentSharedFilter)
            {
                m_presentSharedFilter = filter;
                for (GLuint texture : m_presentShared)
                {
                    if (texture == 0)
                        continue;
                    gl.glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, filter);
                    gl.glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, filter);
                }
            }
        }
        // Grow-only: the display size changes constantly through the logos, the
        // FMV movies and the intro (letterboxing, display-mode switches).
        // Recreating two full-size textures on every change churned VRAM and
        // hitched exactly there; the host samples the used sub-rect instead.
        if (sharePresent && (m_presentShared[0] == 0 || m_presentSharedWidth < width || m_presentSharedHeight < height))
        {
            for (int i = 0; i < 2; ++i)
            {
                if (m_presentShared[i] != 0)
                    gl.glDeleteTextures(1, &m_presentShared[i]);
                if (m_presentSharedFramebuffer[i] != 0)
                    gl.glDeleteFramebuffers(1, &m_presentSharedFramebuffer[i]);
                m_presentShared[i] = 0;
                m_presentSharedFramebuffer[i] = 0;
            }
            m_presentSharedWidth = std::max(m_presentSharedWidth, width);
            m_presentSharedHeight = std::max(m_presentSharedHeight, height);
            for (int i = 0; i < 2; ++i)
            {
                gl.glCreateTextures(GL_TEXTURE_2D, 1, &m_presentShared[i]);
                gl.glTextureStorage2D(m_presentShared[i], 1, GL_RGBA8, static_cast<GLsizei>(m_presentSharedWidth),
                                      static_cast<GLsizei>(m_presentSharedHeight));
                // The host samples this with raylib; it can override the filter.
                gl.glTextureParameteri(m_presentShared[i], GL_TEXTURE_MIN_FILTER,
                                       m_presentSharedFilter != 0 ? m_presentSharedFilter : GL_LINEAR);
                gl.glTextureParameteri(m_presentShared[i], GL_TEXTURE_MAG_FILTER,
                                       m_presentSharedFilter != 0 ? m_presentSharedFilter : GL_LINEAR);
                gl.glTextureParameteri(m_presentShared[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                gl.glTextureParameteri(m_presentShared[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                gl.glCreateFramebuffers(1, &m_presentSharedFramebuffer[i]);
                gl.glNamedFramebufferTexture(m_presentSharedFramebuffer[i], GL_COLOR_ATTACHMENT0, m_presentShared[i], 0);
                gl.glNamedFramebufferDrawBuffer(m_presentSharedFramebuffer[i], GL_COLOR_ATTACHMENT0);
            }
        }
        if (!sharePresent && (m_presentTexture == 0 || m_presentWidth < width || m_presentHeight < height))
        {
            if (m_presentTexture != 0)
            {
                gl.glDeleteFramebuffers(1, &m_presentFramebuffer);
                gl.glDeleteTextures(1, &m_presentTexture);
            }
            m_presentWidth = std::max(m_presentWidth, width);
            m_presentHeight = std::max(m_presentHeight, height);
            gl.glCreateTextures(GL_TEXTURE_2D, 1, &m_presentTexture);
            gl.glTextureStorage2D(m_presentTexture, 1, GL_RGBA8, static_cast<GLsizei>(m_presentWidth),
                                  static_cast<GLsizei>(m_presentHeight));
            gl.glCreateFramebuffers(1, &m_presentFramebuffer);
            gl.glNamedFramebufferTexture(m_presentFramebuffer, GL_COLOR_ATTACHMENT0, m_presentTexture, 0);
            gl.glNamedFramebufferDrawBuffer(m_presentFramebuffer, GL_COLOR_ATTACHMENT0);
        }
        if (crt1)
            loadStaleRows(*c1.target, c1.y, c1.y + c1.height - 1u);
        if (crt2)
            loadStaleRows(*c2.target, c2.y, c2.y + c2.height - 1u);

        gpuMark(GpuWork::Present); // the loads above closed it
        const bool both = crt1 && crt2;
        const Circuit &top = crt1 ? c1 : c2; // a lone circuit is drawn as "circuit 1" at full weight
        Program &p = m_compose;
        useProgram(p.id);
        setUniform4i(p, uniform(p, "u_rect1"), static_cast<GLint>(top.x * s), static_cast<GLint>(top.y * s),
                              static_cast<GLint>(top.width * s), static_cast<GLint>(top.height * s));
        if (both)
            setUniform4i(p, uniform(p, "u_rect2"), static_cast<GLint>(c2.x * s), static_cast<GLint>(c2.y * s),
                                  static_cast<GLint>(c2.width * s), static_cast<GLint>(c2.height * s));
        else
            setUniform4i(p, uniform(p, "u_rect2"), 0, 0, 0, 0);
        setUniform3f(p, uniform(p, "u_bg"), float(request.bgcolor & 0xFFu) / 255.0f,
                              float((request.bgcolor >> 8) & 0xFFu) / 255.0f, float((request.bgcolor >> 16) & 0xFFu) / 255.0f);
        const bool mmod = ((request.pmode >> 5) & 1u) != 0u, slbg = ((request.pmode >> 7) & 1u) != 0u;
        const float alp = static_cast<float>((request.pmode >> 8) & 0xFFu) / 255.0f;
        setUniform2f(p, uniform(p, "u_mode"), both ? (mmod ? alp : -1.0f) : 1.0f, both && slbg ? 1.0f : 0.0f);
        gl.glBindTextureUnit(0, top.target->texture);
        gl.glBindTextureUnit(1, both ? c2.target->texture : top.target->texture);
        // Motion blur ("Modern"): the compose draws into its own target and the
        // blur pass reads that and writes the display.
        const bool reproject = g_gsModernBlur.load(std::memory_order_relaxed) && m_blur.id != 0;
        // The overlay mask, so the blur leaves the HUD alone. The mask is per
        // target, and with two display circuits the compose blends them, so it is
        // only usable when both name the same target - which is what field mode
        // does (the two circuits are the two fields of one buffer).
        {
            static const bool s_overlayDebug = std::getenv("PS2X_OVERLAY_MASK_DEBUG") != nullptr;
            // Read the mask from the target the overlays were drawn into (the
            // scene), not from the display: the HUD lives in the scene framebuffer
            // and the copies that carry it to the display happen on only some
            // frames, so a mask read from the display is missing on the others.
            Target *maskTarget = m_overlayTarget != nullptr ? m_overlayTarget : top.target;
            const bool overlayValid = m_maskEnabled && maskTarget->maskTexture != 0;
            gl.glBindTextureUnit(2, overlayValid ? maskTarget->maskTexture : top.target->texture);
            setUniform1f(p, uniform(p, "u_overlayValid"), overlayValid ? 1.0f : 0.0f);
            // In alpha only when the blur will read it: the display texture itself
            // has to stay opaque.
            setUniform1f(p, uniform(p, "u_overlayToAlpha"), (reproject && overlayValid) ? 1.0f : 0.0f);
            setUniform1f(p, uniform(p, "u_overlayDebug"), s_overlayDebug ? 1.0f : 0.0f);
        }
        // Pair pictures with cameras (frame interpolation), by picture change.
        interpPair(both ? std::max(top.target->drawSeq, c2.target->drawSeq) : top.target->drawSeq);
        // When this picture starts a new game frame, the last composed picture -
        // the previous game frame - becomes the interpolation source. It is held
        // across the repeats that follow (2-4 VBlanks per game frame), so it is
        // always a copy of its own: the shared present ring is reused every
        // compose. Repeats of the same view leave it alone.
        static const bool s_interpTestKeep = std::getenv("PS2X_FRAME_INTERP_TEST") != nullptr;
        bool sourceCopied = false;
        const bool interpWanted = g_gsFrameInterp.load(std::memory_order_relaxed) || s_interpTestKeep;
        if (interpWanted && m_interpShifted && m_lastComposedTexture != 0 && m_lastComposedWidth == width &&
            m_lastComposedHeight == height)
        {
            GLuint sourceFramebuffer = 0;
            if (m_lastComposedTexture == m_presentTexture)
                sourceFramebuffer = m_presentFramebuffer;
            for (int i = 0; i < 2 && sourceFramebuffer == 0; ++i)
                if (m_lastComposedTexture == m_presentShared[i])
                    sourceFramebuffer = m_presentSharedFramebuffer[i];
            if (sourceFramebuffer != 0)
            {
                // glBlitNamedFramebuffer is affected by the scissor test, and the
                // game leaves its last scissor set from the draw that ended the
                // frame - so without this the copy covers only that small rect and
                // the rest of the interpolation source stays black. That is the
                // "frozen black frame from before the fade-in" the owner saw.
                setScissorTest(false);
                if (m_prevComposedTexture == 0 || m_prevComposedWidth != width || m_prevComposedHeight != height)
                {
                    if (m_prevComposedTexture != 0)
                    {
                        gl.glDeleteFramebuffers(1, &m_prevComposedFramebuffer);
                        gl.glDeleteTextures(1, &m_prevComposedTexture);
                    }
                    gl.glCreateTextures(GL_TEXTURE_2D, 1, &m_prevComposedTexture);
                    gl.glTextureStorage2D(m_prevComposedTexture, 1, GL_RGBA8, static_cast<GLsizei>(width),
                                          static_cast<GLsizei>(height));
                    gl.glTextureParameteri(m_prevComposedTexture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    gl.glTextureParameteri(m_prevComposedTexture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    gl.glCreateFramebuffers(1, &m_prevComposedFramebuffer);
                    gl.glNamedFramebufferTexture(m_prevComposedFramebuffer, GL_COLOR_ATTACHMENT0,
                                                 m_prevComposedTexture, 0);
                    gl.glNamedFramebufferDrawBuffer(m_prevComposedFramebuffer, GL_COLOR_ATTACHMENT0);
                }
                gl.glBlitNamedFramebuffer(sourceFramebuffer, m_prevComposedFramebuffer, 0, 0, static_cast<GLint>(width),
                                          static_cast<GLint>(height), 0, 0, static_cast<GLint>(width),
                                          static_cast<GLint>(height), GL_COLOR_BUFFER_BIT, GL_NEAREST);
                m_prevComposedWidth = width;
                m_prevComposedHeight = height;
                ++m_prevComposedCopies;
                sourceCopied = true;
            }
        }
        // The old picture's overlay mask goes with it (the newest slot is
        // rewritten after this compose).
        if (sourceCopied)
            m_interpMaskCur ^= 1;
        // A new game frame whose previous picture could not be kept (size
        // change, no composed picture yet): the cameras moved on but the source
        // did not, so warping it would be wrong. Show plain pictures until the
        // next game frame re-establishes the pair.
        if (interpWanted && m_interpShifted && !sourceCopied)
            m_interpHavePrev = false;
        const GLuint displayFramebuffer =
            sharePresent ? m_presentSharedFramebuffer[m_presentSharedIndex] : m_presentFramebuffer;
        if (reproject)
            ensureBlurTargets(gl, width, height);
        bindFramebuffer(reproject ? m_composeFramebuffer : displayFramebuffer);
        setViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        setScissorTest(false);
        setBlend(false);
        setDepthTest(false);
        setColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        bindVao(m_emptyVao);
        gl.glDrawArrays(GL_TRIANGLES, 0, 3);
        if (reproject)
            blurCompose(gl, width, height, displayFramebuffer);
        m_lastComposedTexture = sharePresent ? m_presentShared[m_presentSharedIndex] : m_presentTexture;
        m_lastComposedWidth = width;
        m_lastComposedHeight = height;
        g_gsComposedFrames.fetch_add(1u, std::memory_order_relaxed);
        if (interpWanted)
        {
            if (m_interpMask[0] == 0 || m_interpMaskWidth != width || m_interpMaskHeight != height)
            {
                for (int i = 0; i < 2; ++i)
                {
                    if (m_interpMask[i] != 0)
                    {
                        gl.glDeleteFramebuffers(1, &m_interpMaskFramebuffer[i]);
                        gl.glDeleteTextures(1, &m_interpMask[i]);
                    }
                    gl.glCreateTextures(GL_TEXTURE_2D, 1, &m_interpMask[i]);
                    gl.glTextureStorage2D(m_interpMask[i], 1, GL_R8, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
                    gl.glTextureParameteri(m_interpMask[i], GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    gl.glTextureParameteri(m_interpMask[i], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    gl.glCreateFramebuffers(1, &m_interpMaskFramebuffer[i]);
                    gl.glNamedFramebufferTexture(m_interpMaskFramebuffer[i], GL_COLOR_ATTACHMENT0, m_interpMask[i], 0);
                    gl.glNamedFramebufferDrawBuffer(m_interpMaskFramebuffer[i], GL_COLOR_ATTACHMENT0);
                    const float zero = 0.0f;
                    gl.glClearNamedFramebufferfv(m_interpMaskFramebuffer[i], GL_COLOR, 0, &zero);
                }
                m_interpMaskWidth = width;
                m_interpMaskHeight = height;
            }
            Target *snapTarget = m_overlayTarget != nullptr ? m_overlayTarget : top.target;
            const bool snapValid = m_maskEnabled && snapTarget->maskTexture != 0;
            Program &ms = m_maskSnap;
            useProgram(ms.id);
            const int sc = static_cast<int>(m_scale);
            setUniform4i(ms, uniform(ms, "u_rect"), static_cast<GLint>(top.x * sc), static_cast<GLint>(top.y * sc),
                         static_cast<GLint>(top.width * sc), static_cast<GLint>(top.height * sc));
            setUniform1f(ms, uniform(ms, "u_valid"), snapValid ? 1.0f : 0.0f);
            gl.glBindTextureUnit(0, snapValid ? snapTarget->maskTexture : m_zeroMask);
            bindFramebuffer(m_interpMaskFramebuffer[m_interpMaskCur]);
            setViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
            setScissorTest(false);
            setBlend(false);
            setDepthTest(false);
            setColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            bindVao(m_emptyVao);
            gl.glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        static const bool s_align = std::getenv("PS2X_INTERP_ALIGN") != nullptr;
        if (s_align && m_interpPairedNow)
            interpAlignCheck();
        // PS2X_INTERP_SEQ=1: per compose, whether the camera changed (c) and
        // whether the picture's content changed (p, one row across the middle),
        // to measure how far the picture lags the camera latched with it.
        static const bool s_seq = std::getenv("PS2X_INTERP_SEQ") != nullptr;
        if (s_seq && m_lastComposedTexture != 0)
        {
            static std::vector<uint8_t> s_row, s_lastRow;
            s_row.assign(size_t(width) * 4u, 0u);
            gl.glGetTextureSubImage(m_lastComposedTexture, 0, 0, static_cast<GLint>(height / 2u), 0,
                                    static_cast<GLsizei>(width), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                    static_cast<GLsizei>(s_row.size()), s_row.data());
            const bool pictureChanged = s_row != s_lastRow;
            s_lastRow = s_row;
            static std::string s_line;
            // p: picture changed, c: its camera arrived (pair complete), B: both
            s_line += m_interpPairedNow ? (m_interpShifted ? 'B' : 'c') : (m_interpShifted ? 'p' : '.');
            (void)pictureChanged;
            if (s_line.size() >= 80u)
            {
                std::cerr << "[seq] " << s_line << std::endl;
                s_line.clear();
            }
        }
        if (sharePresent)
        {
            // Make the write visible to the host's context (it draws this
            // texture next frame; the context's own ordering keeps the GPU
            // work ahead of the sample).
            gl.glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            gl.glFlush();
            frame.glTexture = m_presentShared[m_presentSharedIndex];
            frame.glWidth = m_presentSharedWidth;
            frame.glHeight = m_presentSharedHeight;
            frame.width = composedWidth;
            frame.height = composedHeight;
            frame.rowPixels = width;
            frame.displayFbp = frame.sourceFbp = (crt1 ? c1 : c2).target->block / 32u;
            m_presentSharedIndex ^= 1;
            return;
        }

        // The readback is the expensive part of the present and the frame is
        // only ever shown at the host window's size: when the window is
        // smaller than the upscaled frame, downscale on the GPU first (the
        // same bilinear filter the host would apply) and transfer only that.
        // At internal_scale 4 into a 1280x960 window this is ~4x less data.
        uint32_t readWidth = width, readHeight = height;
        GLuint readTexture = m_presentTexture;
        const uint32_t hostWidth = g_gsGpuPresentWidth.load(std::memory_order_relaxed);
        const uint32_t hostHeight = g_gsGpuPresentHeight.load(std::memory_order_relaxed);
        if (hostWidth != 0u && hostHeight != 0u && hostWidth < readWidth && hostHeight < readHeight)
        {
            const double fit = std::min(static_cast<double>(hostWidth) / static_cast<double>(width),
                                        static_cast<double>(hostHeight) / static_cast<double>(height));
            readWidth = std::max<uint32_t>(64u, static_cast<uint32_t>(static_cast<double>(width) * fit) & ~1u);
            readHeight = std::max<uint32_t>(64u, static_cast<uint32_t>(static_cast<double>(height) * fit) & ~1u);
            if (m_readbackTexture == 0 || m_readbackWidth < readWidth || m_readbackHeight < readHeight)
            {
                if (m_readbackTexture != 0)
                {
                    gl.glDeleteFramebuffers(1, &m_readbackFramebuffer);
                    gl.glDeleteTextures(1, &m_readbackTexture);
                }
                m_readbackWidth = std::max(m_readbackWidth, readWidth);
                m_readbackHeight = std::max(m_readbackHeight, readHeight);
                gl.glCreateTextures(GL_TEXTURE_2D, 1, &m_readbackTexture);
                gl.glTextureStorage2D(m_readbackTexture, 1, GL_RGBA8, static_cast<GLsizei>(m_readbackWidth),
                                      static_cast<GLsizei>(m_readbackHeight));
                gl.glCreateFramebuffers(1, &m_readbackFramebuffer);
                gl.glNamedFramebufferTexture(m_readbackFramebuffer, GL_COLOR_ATTACHMENT0, m_readbackTexture, 0);
                gl.glNamedFramebufferDrawBuffer(m_readbackFramebuffer, GL_COLOR_ATTACHMENT0);
            }
            gl.glBlitNamedFramebuffer(m_presentFramebuffer, m_readbackFramebuffer, 0, 0, static_cast<GLint>(width),
                                      static_cast<GLint>(height), 0, 0, static_cast<GLint>(readWidth),
                                      static_cast<GLint>(readHeight), GL_COLOR_BUFFER_BIT, GL_LINEAR);
            readTexture = m_readbackTexture;
        }
        gl.glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
        gl.glPixelStorei(GL_PACK_ALIGNMENT, 4);
        const uint32_t displayFbp = (crt1 ? c1 : c2).target->block / 32u;
        // Asynchronous by default. A read straight into client memory makes the
        // driver finish every command queued so far before it returns, so the
        // renderer (and the game behind it) waited out the whole frame on the
        // GPU before submitting the next: CPU and GPU never overlapped, and the
        // wait grew with the internal scale. On NVIDIA the wait is a spin in the
        // driver's own thread. Instead the copy goes into a pack buffer with a
        // fence, and the frame handed over is the one copied a present earlier,
        // which is done by the time it is wanted. One frame of display latency.
        // PS2X_GS_ASYNC_READBACK=0 restores the synchronous read.
        static const bool s_asyncReadback = []()
        {
            const char *env = std::getenv("PS2X_GS_ASYNC_READBACK");
            return env == nullptr || std::atoi(env) != 0;
        }();
        if (!s_asyncReadback)
        {
            std::vector<uint8_t> out(static_cast<size_t>(readWidth) * readHeight * 4u);
            gl.glGetTextureSubImage(readTexture, 0, 0, 0, 0, static_cast<GLsizei>(readWidth),
                                    static_cast<GLsizei>(readHeight), 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                    static_cast<GLsizei>(out.size()), out.data());
            frame.pixels = std::move(out);
            frame.width = readWidth;
            frame.height = readHeight;
            frame.rowPixels = readWidth;
            frame.displayFbp = frame.sourceFbp = displayFbp;
        }
        else
        {
            constexpr uint32_t kRing = sizeof(m_readbackRing) / sizeof(m_readbackRing[0]);
            const size_t bytes = static_cast<size_t>(readWidth) * readHeight * 4u;
            ReadbackSlot &slot = m_readbackRing[m_readbackNext];
            if (slot.fence != nullptr)
            {
                // Not collected (the ring wrapped): it is overwritten below.
                gl.glDeleteSync(slot.fence);
                slot.fence = nullptr;
            }
            if (slot.buffer == 0 || slot.capacity < bytes)
            {
                if (slot.buffer != 0)
                {
                    if (slot.mapped != nullptr)
                        gl.glUnmapNamedBuffer(slot.buffer);
                    gl.glDeleteBuffers(1, &slot.buffer);
                }
                slot.mapped = nullptr;
                // In client memory and mapped once: reading a finished copy is
                // then a plain memcpy, where mapping a device buffer per frame
                // could make the driver copy it down there and then.
                gl.glCreateBuffers(1, &slot.buffer);
                const GLbitfield persistent = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
                gl.glNamedBufferStorage(slot.buffer, static_cast<GLsizeiptr>(bytes), nullptr,
                                        persistent | GL_CLIENT_STORAGE_BIT);
                slot.mapped = static_cast<const uint8_t *>(
                    gl.glMapNamedBufferRange(slot.buffer, 0, static_cast<GLsizeiptr>(bytes), persistent));
                slot.capacity = bytes;
            }
            gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.buffer);
            gl.glGetTextureSubImage(readTexture, 0, 0, 0, 0, static_cast<GLsizei>(readWidth),
                                    static_cast<GLsizei>(readHeight), 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                    static_cast<GLsizei>(bytes), nullptr);
            gl.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            slot.fence = gl.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            slot.width = readWidth;
            slot.height = readHeight;
            slot.displayFbp = displayFbp;
            slot.serial = ++m_readbackSerial;
            // Start the GPU on it now rather than at the next flush point.
            gl.glFlush();
            m_readbackNext = (m_readbackNext + 1u) % kRing;

            // Hand over the newest copy the GPU has finished - never wait for
            // one. On a busy driver (an NVIDIA laptop in a dense cutscene) the
            // frame copied a present ago is often not done yet, and waiting for
            // it here held this thread, which backed up the GS frontend, the
            // VU1 worker and the game thread behind it. Nothing finished yet:
            // the host keeps the frame it has, and a later VBlank collects this
            // one (PollPresentation).
            // But never more than three presents in flight: with nothing else
            // holding it back this thread could queue seconds of work ahead
            // of a busy driver, and the newest *finished* frame then lagged
            // just as far - the picture froze for seconds while the game ran
            // on (seen on the laptop once the stream rings stopped stalling).
            // Waiting for the oldest copy keeps the GPU at most three frames
            // behind (two was tight enough that a busy driver's short bursts
            // stalled this thread, and the pipeline behind it, on the laptop).
            constexpr int kMaxInFlight = 3;
            {
                int pending = 0;
                ReadbackSlot *oldest = nullptr;
                for (ReadbackSlot &other : m_readbackRing)
                    if (other.fence != nullptr)
                    {
                        ++pending;
                        if (oldest == nullptr || other.serial < oldest->serial)
                            oldest = &other;
                    }
                // One copy is added per present, so one wait restores the cap.
                if (pending > kMaxInFlight)
                    gl.glClientWaitSync(oldest->fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            }
            uint32_t &s_unchangedStreak = m_readbackUnchangedStreak;
            if (ReadbackSlot *ready = newestFinishedReadback())
            {
                takeReadback(*ready, frame);
                readWidth = ready->width;
                readHeight = ready->height;
                // A visible hold: the picture did not change for this many presents.
                if (s_unchangedStreak >= 10u)
                    std::fprintf(stderr, "[gs] display held for %u presents waiting for the GPU\n", s_unchangedStreak);
                s_unchangedStreak = 0;
            }
            else
            {
                frame.unchanged = true;
                ++s_unchangedStreak;
            }
        }
        // PS2X_GS_PRESENT_DUMP=<prefix>: write presented frames 20..80 as binary
        // PPMs <prefix>_NNN.ppm, for A/B checks. Every present, so consecutive
        // frames can be compared - the field alternation shows there.
        if (const char *dumpPath = std::getenv("PS2X_GS_PRESENT_DUMP"))
        {
            static int s_presents = 0;
            ++s_presents;
            if (s_presents >= 20 && s_presents <= 80)
            {
                char path[1024];
                std::snprintf(path, sizeof(path), "%s_%03d.ppm", dumpPath, s_presents);
                if (FILE *f = std::fopen(path, "wb"))
                {
                    std::fprintf(f, "P6\n%u %u\n255\n", readWidth, readHeight);
                    for (size_t i = 0; i < frame.pixels.size(); i += 4u)
                    {
                        const uint8_t rgb[3] = {frame.pixels[i], frame.pixels[i + 1u], frame.pixels[i + 2u]};
                        std::fwrite(rgb, 1, 3, f);
                    }
                    std::fclose(f);
                }
            }
        }
    }

    Renderer::ReadbackSlot *Renderer::newestFinishedReadback()
    {
        GL &gl = m_context.gl;
        ReadbackSlot *newest = nullptr;
        for (ReadbackSlot &slot : m_readbackRing)
        {
            if (slot.fence == nullptr || (newest != nullptr && slot.serial < newest->serial))
                continue;
            const GLenum state = gl.glClientWaitSync(slot.fence, 0, 0);
            if (state == GL_ALREADY_SIGNALED || state == GL_CONDITION_SATISFIED)
                newest = &slot;
        }
        if (newest != nullptr)
            for (ReadbackSlot &slot : m_readbackRing)
                if (slot.fence != nullptr && slot.serial < newest->serial)
                {
                    // Superseded by a newer finished copy.
                    gl.glDeleteSync(slot.fence);
                    slot.fence = nullptr;
                }
        return newest;
    }

    void Renderer::takeReadback(ReadbackSlot &slot, PresentationFrame &frame)
    {
        GL &gl = m_context.gl;
        gl.glClientWaitSync(slot.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull); // finished already, normally
        gl.glDeleteSync(slot.fence);
        slot.fence = nullptr;
        const size_t bytes = static_cast<size_t>(slot.width) * slot.height * 4u;
        if (slot.mapped != nullptr)
        {
            frame.pixels = gsTakeFrameBuffer(); // reuses its capacity
            frame.pixels.assign(slot.mapped, slot.mapped + bytes);
        }
        else if (const void *mapped = gl.glMapNamedBufferRange(slot.buffer, 0, static_cast<GLsizeiptr>(bytes), GL_MAP_READ_BIT))
        {
            // One pass (resize would zero the buffer first).
            const uint8_t *src = static_cast<const uint8_t *>(mapped);
            frame.pixels = gsTakeFrameBuffer(); // reuses its capacity
            frame.pixels.assign(src, src + bytes);
            gl.glUnmapNamedBuffer(slot.buffer);
        }
        else
            frame.pixels.assign(bytes, 0u);
        frame.width = slot.width;
        frame.height = slot.height;
        frame.rowPixels = slot.width;
        frame.displayFbp = frame.sourceFbp = slot.displayFbp;
    }

    bool Renderer::collectPendingReadback(PresentationFrame &frame)
    {
        if (!m_gpu)
            return false;
        ReadbackSlot *ready = newestFinishedReadback();
        if (ready == nullptr)
            return false;
        takeReadback(*ready, frame);
        m_readbackUnchangedStreak = 0; // the picture moved on
        return true;
    }

    void Renderer::gpuMark(GpuWork work)
    {
        if (!m_gpuTimers)
            return;
        GL &gl = m_context.gl;
        if (m_gpuQueryOpen)
            gl.glEndQuery(GL_TIME_ELAPSED);
        GLuint query = 0;
        if (!m_gpuFreeQueries.empty())
        {
            query = m_gpuFreeQueries.back();
            m_gpuFreeQueries.pop_back();
        }
        else
            gl.glGenQueries(1, &query);
        gl.glBeginQuery(GL_TIME_ELAPSED, query);
        m_gpuQueryOpen = true;
        m_gpuMarks.push_back({query, work});
    }

    void Renderer::gpuEnd()
    {
        if (!m_gpuQueryOpen)
            return;
        m_context.gl.glEndQuery(GL_TIME_ELAPSED);
        m_gpuQueryOpen = false;
    }

    void Renderer::gpuTimersFrameEnd()
    {
        if (!m_gpuTimers)
            return;
        gpuEnd();
        GL &gl = m_context.gl;
        size_t done = 0;
        for (; done < m_gpuMarks.size(); ++done)
        {
            GLint available = 0;
            gl.glGetQueryObjectiv(m_gpuMarks[done].query, GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available)
                break;
            GLuint64 ns = 0;
            gl.glGetQueryObjectui64v(m_gpuMarks[done].query, GL_QUERY_RESULT, &ns);
            m_gpuWorkNs[static_cast<int>(m_gpuMarks[done].work)] += static_cast<double>(ns);
            m_gpuFreeQueries.push_back(m_gpuMarks[done].query);
        }
        m_gpuMarks.erase(m_gpuMarks.begin(), m_gpuMarks.begin() + static_cast<std::ptrdiff_t>(done));
        if (++m_gpuTimerFrames < 300u)
            return;
        static const char *kNames[] = {"draw", "load", "resolve", "feedback", "transfer", "present", "idle"};
        double total = 0.0;
        std::fprintf(stderr, "[gs:gputime] scale=%u ms/present:", m_scale);
        for (int i = 0; i < static_cast<int>(GpuWork::Idle); ++i)
        {
            std::fprintf(stderr, " %s=%.2f", kNames[i], m_gpuWorkNs[i] / 1e6 / m_gpuTimerFrames);
            total += m_gpuWorkNs[i];
            m_gpuWorkNs[i] = 0.0;
        }
        std::fprintf(stderr, " busy=%.2f ringStalls=%u\n", total / 1e6 / m_gpuTimerFrames, m_ringStalls);
        m_ringStalls = 0;
        m_gpuTimerFrames = 0;
    }

    size_t Renderer::ringAlloc(StreamRing &ring, size_t bytes, size_t align, size_t initialSize, bool &recreated)
    {
        // Eight segments of a 64 MiB ring: a stall needs the GPU most of a lap
        // behind. (8 MiB in four stalled the NVIDIA laptop in dense cutscenes,
        // where its driver runs well behind the render thread.)
        constexpr int kSegments = 8;
        GL &gl = m_context.gl;
        recreated = false;
        if (ring.buffer == 0 || bytes > ring.size / kSegments)
        {
            destroyStreamRing(ring);
            size_t size = initialSize;
            while (bytes > size / kSegments)
                size *= 2u;
            const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
            gl.glCreateBuffers(1, &ring.buffer);
            gl.glNamedBufferStorage(ring.buffer, static_cast<GLsizeiptr>(size), nullptr, flags);
            ring.base = static_cast<uint8_t *>(gl.glMapNamedBufferRange(ring.buffer, 0, static_cast<GLsizeiptr>(size), flags));
            if (ring.base == nullptr)
            {
                std::cerr << "[gs:gpu] persistent buffer mapping failed; streaming with glBufferSubData" << std::endl;
                gl.glDeleteBuffers(1, &ring.buffer);
                ring = StreamRing{};
                return SIZE_MAX;
            }
            ring.size = size;
            recreated = true;
        }
        const size_t segmentSize = ring.size / kSegments;
        size_t offset = (ring.head + align - 1u) / align * align;
        // Never straddle two segments: a segment's fence has to come after
        // every command that reads from it.
        if (offset / segmentSize != (offset + bytes - 1u) / segmentSize)
            offset = (offset / segmentSize + 1u) * segmentSize;
        if (offset + bytes > ring.size)
            offset = 0;
        const int segment = static_cast<int>(offset / segmentSize);
        if (segment != ring.segment)
        {
            // Every command reading the segment being left has been submitted.
            if (ring.fence[ring.segment] != nullptr)
                gl.glDeleteSync(ring.fence[ring.segment]);
            ring.fence[ring.segment] = gl.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            // The one being entered was last used a full lap ago.
            if (ring.fence[segment] != nullptr)
            {
                if (gl.glClientWaitSync(ring.fence[segment], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull) != GL_ALREADY_SIGNALED)
                    ++m_ringStalls;
                gl.glDeleteSync(ring.fence[segment]);
                ring.fence[segment] = nullptr;
            }
            ring.segment = segment;
        }
        ring.head = offset + bytes;
        return offset;
    }

    void Renderer::destroyStreamRing(StreamRing &ring)
    {
        GL &gl = m_context.gl;
        for (GLsync &fence : ring.fence)
        {
            if (fence != nullptr)
                gl.glDeleteSync(fence);
            fence = nullptr;
        }
        if (ring.buffer != 0)
        {
            gl.glUnmapNamedBuffer(ring.buffer);
            gl.glDeleteBuffers(1, &ring.buffer);
        }
        ring = StreamRing{};
    }

    void Renderer::destroyReadbackRing()
    {
        GL &gl = m_context.gl;
        for (ReadbackSlot &slot : m_readbackRing)
        {
            if (slot.fence != nullptr)
                gl.glDeleteSync(slot.fence);
            if (slot.buffer != 0)
            {
                if (slot.mapped != nullptr)
                    gl.glUnmapNamedBuffer(slot.buffer);
                gl.glDeleteBuffers(1, &slot.buffer);
            }
            slot = ReadbackSlot{};
        }
    }

    void Renderer::setScale(uint32_t scale)
    {
        m_requestedScale.store(std::clamp<uint32_t>(scale, 1u, 8u), std::memory_order_relaxed);
    }

    void Renderer::applyPendingScale()
    {
        const uint32_t scale = m_requestedScale.load(std::memory_order_relaxed);
        if (!m_gpu || scale == m_scale)
            return;
        // Targets carry everything VRAM lacks: put it there, then rebuild
        // them at the new size from VRAM as they are drawn again.
        flushBatch();
        m_syncReason = "rescale";
        resolveRange(0u, kLocalMemoryBytes);
        destroyTargets();
        m_scale = scale;
        std::cerr << "[gs:gpu] internal resolution x" << m_scale << std::endl;
    }

    void Renderer::destroyTargets()
    {
        GL &gl = m_context.gl;
        for (auto &entry : m_framebuffers)
            gl.glDeleteFramebuffers(1, &entry.second);
        m_framebuffers.clear();
        for (auto &target : m_targets)
        {
            gl.glDeleteFramebuffers(1, &target->loadFramebuffer);
            gl.glDeleteTextures(1, &target->texture);
            if (target->maskTexture != 0)
            {
                gl.glDeleteFramebuffers(1, &target->maskFramebuffer);
                gl.glDeleteTextures(1, &target->maskTexture);
            }
        }
        m_targets.clear();
        // m_overlayTarget points into m_targets, so it dangles the moment the
        // targets are destroyed. The interpolation samples it, so a stale
        // pointer there would be a read of freed memory.
        m_overlayTarget = nullptr;
        if (m_feedbackTexture != 0)
            gl.glDeleteTextures(1, &m_feedbackTexture);
        m_feedbackTexture = 0;
        if (m_zeroMask != 0)
            gl.glDeleteTextures(1, &m_zeroMask);
        m_zeroMask = 0;
        m_feedbackWidth = m_feedbackHeight = 0;
        m_drawColor = m_drawDepth = nullptr;
        destroyBlurTargets();
        for (int i = 0; i < kSynthRing; ++i)
        {
            if (m_synthTexture[i] != 0)
            {
                gl.glDeleteFramebuffers(1, &m_synthFramebuffer[i]);
                gl.glDeleteTextures(1, &m_synthTexture[i]);
                m_synthTexture[i] = m_synthFramebuffer[i] = 0;
            }
        }
        if (m_synthReadbackTexture != 0)
        {
            gl.glDeleteFramebuffers(1, &m_synthReadbackFramebuffer);
            gl.glDeleteTextures(1, &m_synthReadbackTexture);
            m_synthReadbackTexture = m_synthReadbackFramebuffer = 0;
        }
        if (m_prevComposedTexture != 0)
        {
            gl.glDeleteFramebuffers(1, &m_prevComposedFramebuffer);
            gl.glDeleteTextures(1, &m_prevComposedTexture);
            m_prevComposedTexture = m_prevComposedFramebuffer = 0;
        }
    }

    bool Renderer::clearFramebuffer(const GSContext &context, uint32_t rgba)
    {
        readbackVram();
        const bool cleared = m_cpu->ClearFramebuffer(context, rgba);
        uploadVram(0u, kLocalMemoryBytes);
        return cleared;
    }

    uint32_t Renderer::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
    {
        return m_cpu->ConsumeLocalToHostBytes(dst, maxBytes);
    }

    uint32_t Renderer::readVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
    {
        readbackVram();
        m_cpu->TextureFlush();
        return m_cpu->ReadVram(psm, base, bw, x, y);
    }

    void Renderer::writeVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
    {
        readbackVram();
        m_cpu->WriteVram(psm, base, bw, x, y, value);
        uploadVram(0u, kLocalMemoryBytes);
    }

    void Renderer::snapshotVram(std::vector<uint8_t> &out)
    {
        readbackVram();
        m_cpu->SnapshotVram(out);
    }

    GSTransferSnapshot Renderer::transferSnapshot()
    {
        if (!m_gpu || m_cpuTransfer)
            return m_cpu->GetTransferSnapshot();
        return m_transferState;
    }
}
