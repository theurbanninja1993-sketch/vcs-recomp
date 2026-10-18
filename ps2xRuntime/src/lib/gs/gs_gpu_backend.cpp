#include <cstring>
#include "ThreadNaming.h"
#include <chrono>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include "runtime/gs/gs_gpu_backend.h"

#include "runtime/gs/gpu/gs_gpu_renderer.h"

#include <future>
#include <iostream>
#include <utility>

// The window's GL context (raylib's, via GLFW) so the renderer can join its
// share group and hand the composed display texture straight to the host.
#if defined(__linux__) && !defined(PS2X_IS_ANDROID)
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_GLX
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#elif defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WGL
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

#include "runtime/gs/gpu/gl_context.h"

namespace gsgpu
{
    // Called on the host (main) thread once the window exists and its context
    // is current. Safe to call again on a resize/recreate.
    void publishHostGlContext()
    {
#if defined(__linux__) && !defined(PS2X_IS_ANDROID)
        GLFWwindow *window = glfwGetCurrentContext();
        if (window == nullptr)
            return;
        g_hostSharedGlContext.store(reinterpret_cast<uintptr_t>(glfwGetGLXContext(window)), std::memory_order_relaxed);
        g_hostSharedGlDisplay.store(reinterpret_cast<uintptr_t>(glfwGetX11Display()), std::memory_order_relaxed);
#elif defined(_WIN32)
        GLFWwindow *window = glfwGetCurrentContext();
        if (window == nullptr)
            return;
        g_hostSharedGlContext.store(reinterpret_cast<uintptr_t>(glfwGetWGLContext(window)), std::memory_order_relaxed);
        // The window's DC, so the renderer can create its context on the same
        // DC as raylib's. WGL shares reliably only between contexts with
        // compatible pixel formats, and a private window picks its own.
        HWND hwnd = glfwGetWin32Window(window);
        g_hostSharedGlDisplay.store(reinterpret_cast<uintptr_t>(hwnd ? GetDC(hwnd) : nullptr), std::memory_order_relaxed);
#else
        (void)0;
#endif
    }
}

namespace
{
    // Bound on queued commands so the producer can't run unboundedly ahead.
    constexpr size_t kMaxQueuedCommands = 65536u;
}

GSGpuBackend::GSGpuBackend()
{
    m_thread = std::thread([this]()
                           { renderLoop(); });
}

GSGpuBackend::~GSGpuBackend()
{
    Flush();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_workAvailable.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

namespace
{
    constexpr size_t kBatchCommands = 128u;
}

void GSGpuBackend::enqueue(Command &&command, bool urgent)
{
    std::lock_guard<std::mutex> pendingLock(m_pendingMutex);
    m_pending.push_back(std::move(command));
    if (!urgent && m_pending.size() < kBatchCommands)
        return;
    flushPendingLocked();
}

void GSGpuBackend::flushPendingLocked()
{
    bool wasEmpty = false;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_space.wait(lock, [this]()
                     { return m_queue.size() < kMaxQueuedCommands; });
        wasEmpty = m_queue.empty();
        if (wasEmpty)
            m_queue.swap(m_pending);
        else
            for (Command &pending : m_pending)
                m_queue.push_back(std::move(pending));
        m_pending.clear();
    }
    // The render thread only sleeps on an empty queue.
    if (wasEmpty)
        m_workAvailable.notify_one();
}

void GSGpuBackend::runSync(const std::function<void()> &fn, const char *why) const
{
    // PS2X_SYNC_STATS=1: time callers spend waiting for the render thread.
    static const bool s_stats = std::getenv("PS2X_SYNC_STATS") != nullptr;
    struct Timer
    {
        const char *why;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~Timer()
        {
            static std::mutex s_mutex;
            static std::map<std::string, std::pair<uint64_t, double>> s_sites;
            static uint64_t s_calls = 0;
            std::lock_guard<std::mutex> lock(s_mutex);
            auto &site = s_sites[why];
            ++site.first;
            site.second += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (++s_calls % 5000u == 0u || s_calls == 1u) // the first one creates the file
            {
                // Written to sync.log next to the executable as well as stderr:
                // the Windows build has no console, so stderr alone means these
                // numbers are never actually seen there.
                static FILE *s_file = std::fopen("sync.log", "a");
                char line[1024];
                int written = std::snprintf(line, sizeof(line), "[sync:gs]");
                for (auto &[name, entry] : s_sites)
                {
                    if (written < 0 || static_cast<size_t>(written) >= sizeof(line))
                        break;
                    written += std::snprintf(line + written, sizeof(line) - static_cast<size_t>(written),
                                             " %s=%llu/%dms", name.c_str(),
                                             static_cast<unsigned long long>(entry.first),
                                             static_cast<int>(entry.second * 1000.0));
                }
                if (written > 0 && static_cast<size_t>(written) < sizeof(line) - 1)
                    line[written++] = '\n';
                line[written < 0 ? 0 : (static_cast<size_t>(written) < sizeof(line) ? written : sizeof(line) - 1)] = '\0';
                if (s_file != nullptr)
                {
                    std::fputs(line, s_file);
                    std::fflush(s_file);
                }
                std::fputs(line, stderr);
                s_sites.clear();
            }
        }
    };
    std::optional<Timer> timer;
    if (s_stats)
        timer.emplace(Timer{why});
    std::promise<void> done;
    Command command = Command::make(CommandType::Call);
    command.payload->call = [&]()
    {
        fn();
        done.set_value();
    };
    const_cast<GSGpuBackend *>(this)->enqueue(std::move(command), true);
    done.get_future().wait();
}

void GSGpuBackend::renderLoop()
{
    ThreadNaming::SetCurrentThreadName("GsGpu");
    std::vector<Command> batch;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workAvailable.wait(lock, [this]()
                                 { return m_stop || !m_queue.empty(); });
            if (m_queue.empty() && m_stop)
                break;
            batch.swap(m_queue);
        }
        if (batch.size() >= kMaxQueuedCommands)
            m_space.notify_all(); // a producer may be waiting for room
        for (Command &command : batch)
            execute(command);
        batch.clear();
    }
    m_renderer.reset(); // GL objects die with the context, on this thread
}

void GSGpuBackend::execute(Command &command)
{
    // Created on the first command rather than when the thread starts. The
    // Renderer's constructor creates its GL context, and that context can only
    // join the window's share group if the window exists and its context has
    // been published. This thread is started by the GS constructor, which runs
    // long before the window, so deferring it here means the first command -
    // the GS init, itself now ordered after the window - brings it up.
    if (!m_renderer)
        m_renderer = std::make_unique<gsgpu::Renderer>();
    switch (command.type)
    {
    case CommandType::State:
        m_currentState = command.payload->state;
        break;
    case CommandType::Submit:
    {
        GSPrimitiveBatch batch;
        batch.vertices = command.vertices;
        batch.vertexCount = command.vertexCount;
        batch.state = m_currentState;
        m_renderer->submit(batch);
        break;
    }
    case CommandType::LoadClut:
        m_renderer->loadClut(command.payload->tex0, command.payload->texclut);
        break;
    case CommandType::BeginTransfer:
        m_renderer->beginTransfer(command.payload->transfer);
        break;
    case CommandType::UploadImage:
        m_renderer->uploadImage(command.payload->data.data(), static_cast<uint32_t>(command.payload->data.size()));
        break;
    case CommandType::TextureFlush:
        m_renderer->textureFlush();
        break;
    case CommandType::Call:
        command.payload->call();
        break;
    }
}

void GSGpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    runSync([&]()
            { m_renderer->initialize(vram, vramSize); });
}

void GSGpuBackend::Reset()
{
    runSync([&]()
            { m_renderer->reset(); });
}

void GSGpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    if (!m_haveLastState || std::memcmp(&m_lastState, &batch.state, sizeof(GSDrawState)) != 0)
    {
        Command state = Command::make(CommandType::State);
        state.payload->state = batch.state;
        m_pending.push_back(std::move(state));
        m_lastState = batch.state;
        m_haveLastState = true;
    }
    Command &command = m_pending.emplace_back();
    command.type = CommandType::Submit;
    command.vertexCount = batch.vertexCount;
    command.vertices = batch.vertices;
    if (m_pending.size() >= kBatchCommands)
        flushPendingLocked();
}

void GSGpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    Command command = Command::make(CommandType::LoadClut);
    command.payload->tex0 = tex0;
    command.payload->texclut = texclut;
    enqueue(std::move(command));
}

void GSGpuBackend::BeginTransfer(const GSTransferCommand &transfer)
{
    Command command = Command::make(CommandType::BeginTransfer);
    command.payload->transfer = transfer;
    enqueue(std::move(command));
}

void GSGpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Command command = Command::make(CommandType::UploadImage);
    command.payload->data.assign(data, data + sizeBytes);
    enqueue(std::move(command));
}

void GSGpuBackend::Flush()
{
    Command command = Command::make(CommandType::Call);
    command.payload->call = []() {};
    enqueue(std::move(command), true);
}

void GSGpuBackend::TextureFlush()
{
    Command command;
    command.type = CommandType::TextureFlush;
    enqueue(std::move(command));
}

void GSGpuBackend::Sync(GSSyncReason reason)
{
    // FINISH/presentation are ordered in the stream; other reasons need the
    // render thread (and local memory) to be up to date.
    if (reason == GSSyncReason::Finish || reason == GSSyncReason::Presentation)
        return;
    runSync([&]()
            { m_renderer->syncLocalMemory(); }, "sync");
}

PresentationFrame GSGpuBackend::Present(const GSPresentationRequest &request)
{
    static const bool s_sync = std::getenv("PS2X_GS_GPU_SYNC_PRESENT") != nullptr;
    if (!s_sync)
    {
        // The guest never waits for presentation: queue it and hand back the
        // newest finished frame (at most one VBlank old).
        Command command = Command::make(CommandType::Call);
        command.payload->call = [this, request]()
        {
            PresentationFrame frame = m_renderer->present(request);
            std::lock_guard<std::mutex> lock(m_frameMutex);
            m_lastFrame = std::move(frame);
            ++m_lastFrameSeq;
        };
        enqueue(std::move(command), true);
        // Each finished frame is handed over once, by move: copying it here
        // (megabytes at a high internal scale) cost the game thread - which
        // calls this every VBlank - milliseconds per frame on a slow CPU.
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_lastHandedSeq == m_lastFrameSeq)
        {
            PresentationFrame none;
            none.unchanged = true;
            return none;
        }
        m_lastHandedSeq = m_lastFrameSeq;
        return std::move(m_lastFrame);
    }
    PresentationFrame frame;
    runSync([&]()
            { frame = m_renderer->present(request); }, "present");
    return frame;
}

bool GSGpuBackend::PollPresentation(PresentationFrame &out)
{
    static const bool s_sync = std::getenv("PS2X_GS_GPU_SYNC_PRESENT") != nullptr;
    if (s_sync)
        return false;
    // Collect a readback still in flight (the renderer hands each present the
    // copy made one present earlier, so the newest picture is waiting there
    // when the game repeats it), for the next VBlank to pick up.
    Command command = Command::make(CommandType::Call);
    command.payload->call = [this]()
    {
        PresentationFrame frame;
        if (!m_renderer->collectPendingReadback(frame))
            return;
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_lastFrame = std::move(frame);
        ++m_lastFrameSeq;
    };
    enqueue(std::move(command), true);
    // Hand over a frame finished since the caller last had one - the queued
    // present's own result included, which used to wait for the next new picture.
    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (m_lastHandedSeq == m_lastFrameSeq || !m_lastFrame)
        return false;
    m_lastHandedSeq = m_lastFrameSeq;
    out = std::move(m_lastFrame);
    return true;
}

uint32_t GSGpuBackend::SynthesizeFrame(float phase, uint32_t &width, uint32_t &height)
{
    // Blocking, unlike Present: the caller needs the texture before it draws.
    // Same shape as ReadVram - enqueue an urgent command and wait for the render
    // thread to run it. The locals are captured by reference and the wait
    // guarantees they outlive the lambda.
    uint32_t texture = 0, w = 0, h = 0;
    std::promise<void> done;
    Command command = Command::make(CommandType::Call);
    command.payload->call = [&]()
    {
        texture = m_renderer ? m_renderer->synthesizeHostFrame(phase, w, h) : 0u;
        done.set_value();
    };
    enqueue(std::move(command), true);
    done.get_future().wait();
    width = w;
    height = h;
    return texture;
}

void GSGpuBackend::RequestSynthesis(double displayTime)
{
    if (m_synthInFlight.exchange(true))
        return; // the previous request is still queued: it will serve
    Command command = Command::make(CommandType::Call);
    command.payload->call = [this, displayTime]()
    {
        uint32_t w = 0, h = 0;
        const uint32_t texture = m_renderer ? m_renderer->synthesizeHostFrameAt(displayTime, w, h) : 0u;
        {
            std::lock_guard<std::mutex> lock(m_synthResultMutex);
            m_synthResultTexture = texture;
            m_synthResultWidth = w;
            m_synthResultHeight = h;
            ++m_synthResultSeq;
            // Swapped, not copied: the renderer ends up holding the buffer this one
            // had, and fills it again next round.
            m_synthResultPixelsValid = false;
            if (m_renderer != nullptr)
            {
                uint32_t pw = 0, ph = 0;
                bool pvalid = false;
                std::vector<uint8_t> &pixels = m_renderer->synthPixels(pw, ph, pvalid);
                if (pvalid && pw == w && ph == h)
                {
                    m_synthResultPixels.swap(pixels);
                    m_synthResultPixelsValid = true;
                }
            }
        }
        m_synthInFlight.store(false);
    };
    enqueue(std::move(command), true);
}

bool GSGpuBackend::LatestSynthesisPixels(std::vector<uint8_t> &pixels)
{
    std::lock_guard<std::mutex> lock(m_synthResultMutex);
    if (!m_synthResultPixelsValid)
    {
        pixels.clear();
        return false;
    }
    pixels = m_synthResultPixels; // the host uploads it straight away
    return true;
}

bool GSGpuBackend::LatestSynthesis(uint32_t &texture, uint32_t &width, uint32_t &height, uint64_t &seq)
{
    std::lock_guard<std::mutex> lock(m_synthResultMutex);
    texture = m_synthResultTexture;
    width = m_synthResultWidth;
    height = m_synthResultHeight;
    seq = m_synthResultSeq;
    return m_synthResultSeq != 0;
}

bool GSGpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    bool cleared = false;
    runSync([&]()
            { cleared = m_renderer->clearFramebuffer(context, rgba); }, "clear");
    return cleared;
}

uint32_t GSGpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    uint32_t count = 0;
    runSync([&]()
            { count = m_renderer->consumeLocalToHostBytes(dst, maxBytes); }, "localToHost");
    return count;
}

uint32_t GSGpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    uint32_t value = 0;
    runSync([&]()
            { value = m_renderer->readVram(psm, base, bw, x, y); }, "readVram");
    return value;
}

void GSGpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    runSync([&]()
            { m_renderer->writeVram(psm, base, bw, x, y, value); }, "writeVram");
}

void GSGpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    runSync([&]()
            { m_renderer->snapshotVram(out); }, "snapshot");
}

GSTransferSnapshot GSGpuBackend::GetTransferSnapshot() const
{
    GSTransferSnapshot snapshot{};
    runSync([&]()
            { snapshot = m_renderer->transferSnapshot(); }, "transferSnapshot");
    return snapshot;
}
