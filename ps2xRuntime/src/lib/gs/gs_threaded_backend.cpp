#include "runtime/gs/gs_threaded_backend.h"
#include "ThreadNaming.h"

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace
{
    constexpr uint32_t kLocalMemoryBytes = 4u * 1024u * 1024u;
    constexpr uint32_t kPageBytes = 8192u;
    // How far (in commands) the producer may run ahead of the slowest worker.
    constexpr uint64_t kMaxCommandsAhead = 32768u;

    struct PageShape
    {
        uint32_t width;
        uint32_t height;
    };

    // GS page dimensions per pixel storage format (each page is 8 KiB).
    PageShape pageShapeForPsm(uint8_t psm)
    {
        switch (psm)
        {
        case 0x02: // CT16
        case 0x0A: // CT16S
        case 0x32: // Z16
        case 0x3A: // Z16S
            return {64u, 64u};
        case 0x13: // T8
            return {128u, 64u};
        case 0x14: // T4
            return {128u, 128u};
        default: // 32-bit layouts, incl. T8H/T4HL/T4HH stored in 32-bit pages
            return {64u, 32u};
        }
    }

    // Conservative byte range covered by a buffer of `width` x `height` pixels
    // starting at `baseBytes` with a row stride of `bufferWidth64` * 64 pixels.
    void bufferRange(uint32_t baseBytes, uint32_t bufferWidth64, uint32_t height, uint8_t psm,
                     uint32_t &begin, uint32_t &end)
    {
        const PageShape shape = pageShapeForPsm(psm);
        const uint32_t strideWidth = std::max<uint32_t>(bufferWidth64, 1u) * 64u;
        const uint32_t pagesWide = (strideWidth + shape.width - 1u) / shape.width;
        const uint32_t pagesHigh = (std::max<uint32_t>(height, 1u) + shape.height - 1u) / shape.height;
        begin = baseBytes & ~(kPageBytes - 1u);
        // One extra page covers a base that isn't page aligned (frame/Z bases
        // always are; a spurious extra page made adjacent buffers "overlap").
        const uint32_t misalignPage = (baseBytes & (kPageBytes - 1u)) != 0u ? kPageBytes : 0u;
        const uint64_t bytes = static_cast<uint64_t>(pagesWide) * pagesHigh * kPageBytes + misalignPage;
        end = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(begin) + bytes, kLocalMemoryBytes));
    }

    // Bytes a textured draw can read: the texel rows its coordinates span
    // (plus a filtering margin) instead of the whole 2^TH-high texture.
    void textureReadRange(const GSPrimitiveBatch &batch, uint32_t &begin, uint32_t &end)
    {
        const auto &tex0 = batch.state.context.tex0;
        const uint32_t texHeight = 1u << std::min<uint32_t>(tex0.th, 11u);
        bufferRange(tex0.tbp0 * 256u, tex0.tbw, texHeight, tex0.psm, begin, end);

        static const bool s_fullRange = std::getenv("PS2X_GS_FULL_TEXRANGE") != nullptr;
        const uint32_t count = std::min<uint32_t>(batch.vertexCount, 3u);
        if (s_fullRange || count == 0u || ((tex0.tbp0 * 256u) & (kPageBytes - 1u)) != 0u)
            return;
        // CLAMP.WMT (bits 2-3): REGION_REPEAT (3) can remap rows anywhere.
        if (((batch.state.context.clamp >> 2) & 3u) == 3u)
            return;
        float vMin = 1.0e30f, vMax = -1.0e30f;
        for (uint32_t i = 0; i < count; ++i)
        {
            const GSVertex &vertex = batch.vertices[i];
            float v = 0.0f;
            if (batch.state.prim.fst)
                v = static_cast<float>(vertex.v) / 16.0f;
            else
            {
                if (!(std::fabs(vertex.q) > 1.0e-6f))
                    return; // degenerate perspective: keep the full texture
                v = vertex.t / vertex.q * static_cast<float>(texHeight);
            }
            if (!std::isfinite(v))
                return;
            vMin = std::min(vMin, v);
            vMax = std::max(vMax, v);
        }
        constexpr float kMargin = 2.0f; // bilinear neighbours + rounding
        if (vMin - kMargin < 0.0f || vMax + kMargin >= static_cast<float>(texHeight))
            return; // touches an edge: wrapping/clamping may reach any row
        const PageShape shape = pageShapeForPsm(tex0.psm);
        const uint32_t strideWidth = std::max<uint32_t>(tex0.tbw, 1u) * 64u;
        const uint32_t pagesWide = (strideWidth + shape.width - 1u) / shape.width;
        const uint32_t rowBegin = static_cast<uint32_t>(vMin - kMargin);
        const uint32_t rowEnd = static_cast<uint32_t>(vMax + kMargin) + 1u;
        const uint32_t pageRowBegin = rowBegin / shape.height;
        const uint32_t pageRowEnd = (rowEnd + shape.height - 1u) / shape.height;
        const uint64_t base = tex0.tbp0 * 256u;
        const uint64_t narrowBegin = base + static_cast<uint64_t>(pageRowBegin) * pagesWide * kPageBytes;
        const uint64_t narrowEnd = base + static_cast<uint64_t>(pageRowEnd) * pagesWide * kPageBytes;
        if (narrowBegin >= narrowEnd)
            return;
        begin = static_cast<uint32_t>(std::min<uint64_t>(narrowBegin, kLocalMemoryBytes));
        end = static_cast<uint32_t>(std::min<uint64_t>(narrowEnd, kLocalMemoryBytes));
    }
}

GSThreadedBackend::GSThreadedBackend(BackendFactory factory, uint32_t workerCount)
{
    m_statsEnabled = std::getenv("PS2X_GS_STATS") != nullptr;
    workerCount = std::max<uint32_t>(workerCount, 1u);
    for (uint32_t index = 0; index < workerCount; ++index)
    {
        auto worker = std::make_unique<Worker>();
        worker->backend = factory();
        worker->backend->SetRowBand(index, workerCount);
        m_workers.push_back(std::move(worker));
    }
    for (uint32_t index = 0; index < workerCount; ++index)
    {
        m_workers[index]->thread = std::thread([this, index]()
                                               {
                                                   ThreadNaming::SetCurrentThreadName(("GsRaster" + std::to_string(index)).c_str());
                                                   workerLoop(index);
                                               });
    }
}

GSThreadedBackend::~GSThreadedBackend()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_workAvailable.notify_all();
    for (auto &worker : m_workers)
    {
        if (worker->thread.joinable())
            worker->thread.join();
    }
}

uint64_t GSThreadedBackend::slowestPosition() const
{
    uint64_t slowest = UINT64_MAX;
    for (const auto &worker : m_workers)
        slowest = std::min(slowest, worker->position.load(std::memory_order_acquire));
    return slowest;
}

GSThreadedBackend::Command &GSThreadedBackend::commandAt(uint64_t sequence)
{
    // Caller holds m_mutex.
    const uint64_t offset = sequence - m_firstChunkSequence;
    return m_chunks[static_cast<size_t>(offset / kChunkSize)]->commands[offset % kChunkSize];
}

void GSThreadedBackend::enqueue(Command &&command)
{
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_statsEnabled)
            ++m_statCounts[static_cast<uint32_t>(command.type)];
        if (m_published - slowestPosition() >= kMaxCommandsAhead)
        {
            ++m_statBackpressureWaits;
            const auto waitStart = std::chrono::steady_clock::now();
            m_progressWaiters.fetch_add(1u);
            m_progress.wait(lock, [this]()
                            { return m_published - slowestPosition() < kMaxCommandsAhead; });
            m_progressWaiters.fetch_sub(1u);
            if (m_statsEnabled)
                m_statBackpressureUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - waitStart).count();
        }

        // Drop chunks every worker has finished with.
        const uint64_t slowest = slowestPosition();
        while (!m_chunks.empty() && m_firstChunkSequence + kChunkSize <= slowest)
        {
            m_chunks.pop_front();
            m_firstChunkSequence += kChunkSize;
        }
        if (m_chunks.empty())
            m_firstChunkSequence = m_published - (m_published % kChunkSize);

        const uint64_t offset = m_published - m_firstChunkSequence;
        if (offset / kChunkSize >= m_chunks.size())
            m_chunks.push_back(std::make_unique<Chunk>());
        commandAt(m_published) = std::move(command);
        ++m_published;
        if (m_statsEnabled)
            reportStats();
        if (m_sleepingWorkers == 0u)
            return;
    }
    m_workAvailable.notify_all();
}

void GSThreadedBackend::reportStats()
{
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                                   std::chrono::steady_clock::now().time_since_epoch())
                                                   .count());
    if (now < m_statLastReport + 10u)
        return;
    m_statLastReport = now;
    static const char *kNames[] = {"submit", "clut", "xfer", "upload", "texflush", "present", "barrier", "?"};
    std::cerr << "[gs:stats]";
    for (uint32_t i = 0; i < 7u; ++i)
        std::cerr << " " << kNames[i] << "=" << m_statCounts[i];
    std::cerr << " hazard=" << m_statHazardBarriers << " (tex=" << m_statHazardCause[0] << " clut=" << m_statHazardCause[1]
              << " upload=" << m_statHazardCause[2] << " merges=" << m_statRangeMerges << ")"
              << " backpressure=" << m_statBackpressureWaits << " (" << static_cast<uint64_t>(m_statBackpressureUs / 1000.0)
              << " ms) drains=" << m_statDrains << " (" << static_cast<uint64_t>(m_statDrainUs / 1000.0) << " ms)" << std::endl;
}

void GSThreadedBackend::drain() const
{
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto waitStart = std::chrono::steady_clock::now();
    m_progressWaiters.fetch_add(1u);
    m_progress.wait(lock, [this]()
                    { return slowestPosition() >= m_published; });
    m_progressWaiters.fetch_sub(1u);
    if (m_statsEnabled)
    {
        ++m_statDrains;
        m_statDrainUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - waitStart).count();
    }
}

void GSThreadedBackend::runExclusive(Command &command)
{
    GSRasterBackend &backend = *m_workers[0]->backend;
    switch (command.type)
    {
    case CommandType::BeginTransfer:
        backend.BeginTransfer(command.transfer);
        break;
    case CommandType::Present:
    {
        backend.Sync(GSSyncReason::Presentation);
        PresentationFrame frame = backend.Present(command.presentRequest);
        std::lock_guard<std::mutex> lock(m_presentMutex);
        m_lastPresented = std::move(frame);
        break;
    }
    default:
        break;
    }
}


namespace
{
    inline void cpuRelax()
    {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }
}

// Thread id of rasterizer worker 0, for PS2X_PC_SAMPLE_GS profiling.
std::atomic<int> g_ps2GsWorkerTid{0};

void GSThreadedBackend::workerLoop(uint32_t index)
{
#if defined(__linux__)
    if (index == 0u)
        g_ps2GsWorkerTid.store(static_cast<int>(syscall(SYS_gettid))); // PS2X_PC_SAMPLE_GS
#endif
    Worker &self = *m_workers[index];
    GSRasterBackend &backend = *self.backend;
    uint64_t knownSlowest = 0; // see waitForFence
    const uint32_t workerCount = static_cast<uint32_t>(m_workers.size());
    uint64_t position = 0;
    for (;;)
    {
        uint64_t available = 0;
        Chunk *chunk = nullptr;
        uint64_t chunkBase = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_stop && m_published <= position)
            {
                ++m_sleepingWorkers;
                m_workAvailable.wait(lock, [&]()
                                     { return m_stop || m_published > position; });
                --m_sleepingWorkers;
            }
            if (m_published <= position)
                return;
            available = m_published;
        }

        while (position < available)
        {
            if (chunk == nullptr || position >= chunkBase + kChunkSize)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const uint64_t offset = position - m_firstChunkSequence;
                chunkBase = position - (offset % kChunkSize);
                chunk = m_chunks[static_cast<size_t>(offset / kChunkSize)].get();
            }
            Command &command = chunk->commands[position - chunkBase];

            switch (command.type)
            {
            case CommandType::Submit:
                if (command.fence != 0u)
                    waitForFence(command.fence, knownSlowest);
                backend.Submit(command.batch);
                break;
            case CommandType::LoadClut:
                if (command.fence != 0u)
                    waitForFence(command.fence, knownSlowest);
                backend.LoadClut(command.tex0, command.texclut);
                break;
            case CommandType::TextureFlush:
                backend.TextureFlush();
                break;
            case CommandType::UploadImage:
            {
                // The first worker to arrive writes the data; the others wait for
                // it and only advance their transfer state. Writing early is safe:
                // BeginTransfer inserts a barrier if earlier commands still read
                // or draw this memory.
                std::atomic<uint8_t> &state = chunk->uploadState[position - chunkBase];
                uint8_t expected = 0u;
                if (state.compare_exchange_strong(expected, 1u, std::memory_order_acq_rel))
                {
                    if (command.fence != 0u)
                        waitForFence(command.fence, knownSlowest);
                    backend.UploadImage(command.data.data(), static_cast<uint32_t>(command.data.size()));
                    state.store(2u, std::memory_order_release);
                }
                else
                {
                    while (state.load(std::memory_order_acquire) != 2u)
                        cpuRelax();
                    backend.SetUploadWrites(false);
                    backend.UploadImage(command.data.data(), static_cast<uint32_t>(command.data.size()));
                    backend.SetUploadWrites(true);
                }
                backend.TextureFlush();
                break;
            }
            case CommandType::BeginTransfer:
                if (command.transfer.direction == 0u)
                {
                    backend.BeginTransfer(command.transfer);
                    break;
                }
                [[fallthrough]];
            case CommandType::Present:
            case CommandType::Barrier:
            {
                auto waitUntil = [this](auto &&ready)
                {
                    // ~0.1-0.3 ms of spinning covers the common short wait.
                    for (uint32_t spin = 0; spin < 4096u; ++spin)
                    {
                        if (ready())
                            return;
                        cpuRelax();
                    }
                    std::unique_lock<std::mutex> lock(m_barrierMutex);
                    m_barrierCv.wait(lock, ready);
                };
                auto wakeAll = [this]()
                {
                    {
                        std::lock_guard<std::mutex> lock(m_barrierMutex);
                    }
                    m_barrierCv.notify_all();
                };
                const uint64_t generation = m_barrierGeneration.load(std::memory_order_acquire);
                const uint32_t arrived = m_barrierArrived.fetch_add(1u, std::memory_order_acq_rel) + 1u;
                if (index == 0u)
                {
                    waitUntil([&]()
                              { return m_barrierArrived.load(std::memory_order_acquire) == workerCount; });
                    runExclusive(command);
                    m_barrierArrived.store(0u, std::memory_order_relaxed);
                    m_barrierGeneration.fetch_add(1u, std::memory_order_acq_rel);
                    wakeAll();
                }
                else
                {
                    if (arrived == workerCount)
                        wakeAll();
                    waitUntil([&]()
                              { return m_barrierGeneration.load(std::memory_order_acquire) != generation; });
                }
                // Local memory may have changed under every worker's texture cache.
                if (command.type == CommandType::BeginTransfer)
                    backend.TextureFlush();
                break;
            }
            }

            ++position;
            self.position.store(position);
            if ((position & 255u) == 0u && m_progressWaiters.load() != 0u)
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                }
                m_progress.notify_all();
            }
        }
        if (m_progressWaiters.load() != 0u)
        {
            {
                // Pair with waiters in enqueue()/drain().
                std::lock_guard<std::mutex> lock(m_mutex);
            }
            m_progress.notify_all();
        }
    }
}

bool GSThreadedBackend::overlaps(const std::vector<Range> &ranges, uint32_t beginByte, uint32_t endByte)
{
    for (const Range &range : ranges)
    {
        if (beginByte < range.end && range.begin < endByte)
            return true;
    }
    return false;
}

uint64_t GSThreadedBackend::overlapFence(const std::vector<Range> &ranges, uint32_t beginByte, uint32_t endByte)
{
    uint64_t fence = 0;
    for (const Range &range : ranges)
    {
        if (beginByte < range.end && range.begin < endByte)
            fence = std::max(fence, range.seq + 1u);
    }
    return fence;
}

void GSThreadedBackend::waitForFence(uint64_t fence, uint64_t &knownSlowest) const
{
    if (fence <= knownSlowest)
        return;
    for (uint32_t spin = 0;; ++spin)
    {
        knownSlowest = slowestPosition();
        if (knownSlowest >= fence)
            return;
        if (spin < 4096u)
            cpuRelax();
        else
            std::this_thread::yield();
    }
}

void GSThreadedBackend::addRange(std::vector<Range> &ranges, uint32_t beginByte, uint32_t endByte, uint64_t seq)
{
    for (Range &range : ranges)
    {
        if (range.begin <= beginByte && endByte <= range.end)
        {
            range.seq = std::max(range.seq, seq);
            return;
        }
    }
    if (ranges.size() >= 16u)
    {
        // Ranges every worker has moved past can no longer conflict.
        const uint64_t slowest = slowestPosition();
        ranges.erase(std::remove_if(ranges.begin(), ranges.end(), [slowest](const Range &range)
                                    { return range.seq + 1u <= slowest; }),
                     ranges.end());
    }
    if (ranges.size() >= 16u)
    {
        ++m_statRangeMerges;
        // Too many distinct ranges: keep one conservative span.
        Range merged{beginByte, endByte, seq};
        for (const Range &range : ranges)
        {
            merged.begin = std::min(merged.begin, range.begin);
            merged.end = std::max(merged.end, range.end);
            merged.seq = std::max(merged.seq, range.seq);
        }
        ranges.assign(1u, merged);
        return;
    }
    ranges.push_back(Range{beginByte, endByte, seq});
}

void GSThreadedBackend::logHazard(const char *cause, uint32_t beginByte, uint32_t endByte)
{
    if (!m_statsEnabled || m_statHazardSamples >= 40u || (m_statHazardBarriers % 5000u) != 0u)
        return;
    ++m_statHazardSamples;
    std::cerr << std::hex << "[gs:hazard] " << cause << " read=[0x" << beginByte << ",0x" << endByte << ") dirty=";
    for (const Range &range : m_dirtyTargets)
        std::cerr << "[0x" << range.begin << ",0x" << range.end << ")";
    std::cerr << std::dec << std::endl;
}

void GSThreadedBackend::enqueueBarrier()
{
    if (m_statsEnabled)
        ++m_statHazardBarriers;
    Command barrier;
    barrier.type = CommandType::Barrier;
    enqueue(std::move(barrier));
    m_dirtyTargets.clear();
    m_readSources.clear();
}

void GSThreadedBackend::noteDrawTargets(const GSDrawState &state, uint64_t seq)
{
    const auto &ctx = state.context;
    const uint32_t height = static_cast<uint32_t>(ctx.scissor.y1) + 1u;
    uint32_t begin = 0;
    uint32_t end = 0;
    bufferRange(ctx.frame.fbp * kPageBytes, ctx.frame.fbw, height, ctx.frame.psm, begin, end);
    addRange(m_dirtyTargets, begin, end, seq);
    const bool zWrites = !ctx.zbuf.zmask && ((ctx.test >> 16) & 1u) != 0u;
    if (zWrites)
    {
        bufferRange(ctx.zbuf.zbp * kPageBytes, ctx.frame.fbw, height, ctx.zbuf.psm, begin, end);
        addRange(m_dirtyTargets, begin, end, seq);
    }
}

void GSThreadedBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    drain();
    for (auto &worker : m_workers)
        worker->backend->Initialize(vram, vramSize);
}

void GSThreadedBackend::Reset()
{
    drain();
    for (auto &worker : m_workers)
        worker->backend->Reset();
    m_dirtyTargets.clear();
    m_readSources.clear();
}

void GSThreadedBackend::Submit(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const uint64_t seq = m_published; // this draw's sequence number (single producer)
    uint64_t fence = 0;
    if (state.prim.tme)
    {
        uint32_t begin = 0;
        uint32_t end = 0;
        textureReadRange(batch, begin, end);
        fence = overlapFence(m_dirtyTargets, begin, end);
        if (fence != 0u)
        {
            ++m_statHazardCause[0];
            logHazard("texture", begin, end);
        }
        addRange(m_readSources, begin, end, seq);
    }

    Command command;
    command.type = CommandType::Submit;
    command.batch = batch;
    command.fence = fence;
    enqueue(std::move(command));
    noteDrawTargets(state, seq);
}

void GSThreadedBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const uint32_t begin = tex0.cbp * 256u;
    const uint64_t seq = m_published;
    const uint64_t fence = overlapFence(m_dirtyTargets, begin, begin + 1024u);
    if (fence != 0u)
    {
        ++m_statHazardCause[1];
        logHazard("clut", begin, begin + 1024u);
    }
    addRange(m_readSources, begin, begin + 1024u, seq);
    Command command;
    command.type = CommandType::LoadClut;
    command.tex0 = tex0;
    command.texclut = texclut;
    command.fence = fence;
    enqueue(std::move(command));
}

void GSThreadedBackend::BeginTransfer(const GSTransferCommand &command)
{
    if (command.direction == 0u)
    {
        // Host->local: every worker performs the upload in stream order. It
        // only has to wait for the others if a worker may still read or draw
        // that memory for earlier commands.
        const auto &buf = command.bitbltbuf;
        uint32_t begin = 0;
        uint32_t end = 0;
        bufferRange(buf.dbp * 256u, buf.dbw, static_cast<uint32_t>(command.trxpos.dsay) + command.trxreg.rrh,
                    buf.dpsm, begin, end);
        // The claiming worker writes the upload early; it must wait until no
        // worker can still draw to or read this memory for earlier commands.
        m_uploadFence = std::max(overlapFence(m_dirtyTargets, begin, end), overlapFence(m_readSources, begin, end));
        if (m_uploadFence != 0u)
        {
            ++m_statHazardCause[2];
            logHazard("upload", begin, end);
        }
    }
    Command queued;
    queued.type = CommandType::BeginTransfer;
    queued.transfer = command;
    enqueue(std::move(queued));
    if (command.direction != 0u)
    {
        m_uploadFence = 0;
        m_dirtyTargets.clear();
        m_readSources.clear();
    }
}

void GSThreadedBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Command command;
    command.type = CommandType::UploadImage;
    command.fence = m_uploadFence;
    command.data.assign(data, data + sizeBytes);
    enqueue(std::move(command));
}

void GSThreadedBackend::Flush()
{
    // Commands are already handed to the workers as they arrive.
}

void GSThreadedBackend::TextureFlush()
{
    // Each worker drops its own texture page cache in stream order; draws that
    // read freshly rendered memory are already fenced by the hazard tracking.
    Command command;
    command.type = CommandType::TextureFlush;
    enqueue(std::move(command));
}

void GSThreadedBackend::Sync(GSSyncReason reason)
{
    // FINISH only raises the CSR flag (done by the frontend), and presentation
    // is captured in order by the workers (see Present); waiting for the
    // rasterizer in either case would serialize the EE with the GS.
    if (reason == GSSyncReason::Finish || reason == GSSyncReason::Presentation)
        return;
    drain();
    m_workers[0]->backend->Sync(reason);
}

PresentationFrame GSThreadedBackend::Present(const GSPresentationRequest &request)
{
    Command command;
    command.type = CommandType::Present;
    command.presentRequest = request;
    enqueue(std::move(command));
    m_dirtyTargets.clear();
    m_readSources.clear();
    std::lock_guard<std::mutex> lock(m_presentMutex);
    return m_lastPresented;
}

bool GSThreadedBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    drain();
    GSRasterBackend &backend = *m_workers[0]->backend;
    backend.SetRowBand(0u, 1u);
    const bool cleared = backend.ClearFramebuffer(context, rgba);
    backend.SetRowBand(0u, static_cast<uint32_t>(m_workers.size()));
    for (auto &worker : m_workers)
        worker->backend->TextureFlush();
    return cleared;
}

uint32_t GSThreadedBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    drain();
    return m_workers[0]->backend->ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSThreadedBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    drain();
    return m_workers[0]->backend->ReadVram(psm, base, bw, x, y);
}

void GSThreadedBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    drain();
    m_workers[0]->backend->WriteVram(psm, base, bw, x, y, value);
    for (auto &worker : m_workers)
        worker->backend->TextureFlush();
}

void GSThreadedBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    drain();
    m_workers[0]->backend->SnapshotVram(out);
}

GSTransferSnapshot GSThreadedBackend::GetTransferSnapshot() const
{
    // Debug bookkeeping only; reading worker 0's (mutex-guarded) transfer
    // state without draining keeps uploads asynchronous.
    return m_workers[0]->backend->GetTransferSnapshot();
}
