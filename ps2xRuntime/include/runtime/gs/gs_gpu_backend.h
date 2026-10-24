#pragma once

#include "runtime/gs/gs_backend.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gsgpu
{
    class Renderer;
}

// GS raster backend that renders with the GPU (see docs/gpu-renderer.md).
// Every call is executed in order on a dedicated render thread that owns the
// GL context; calls returning data wait for the render thread.
// Internal resolution the GPU renderer should use (render targets this many
// times the GS size; 0 = no request). Set from the host settings; applied
// between frames. PS2X_GS_SCALE sets the starting value.
extern std::atomic<uint32_t> g_gsGpuScaleRequest;
// Settings > Display > Motion blur. When true the renderer skips the draws
// that read the game's blur accumulation buffers, so the picture stays sharp.
// Both "Off" and "Modern" set it: Modern replaces the game's smear with its own.
extern std::atomic<bool> g_gsMotionBlurOff;
extern std::atomic<uint32_t> g_gsMotionBlurBase[2]; // per-game blur buffers (game override)
extern std::atomic<bool> g_gsMotionBlurFrontTrail;
// Settings > Display > Frame rate. 60 shows only the game's own frames; above
// that the host presents faster and the frames in between are synthesized.
extern std::atomic<uint32_t> g_gsFrameRate;
// True while the host is presenting faster than the game and synthesizing the
// frames in between. The overlay mask is needed then as much as for the blur: a
// synthesized frame must not move the HUD.
extern std::atomic<bool> g_gsFrameInterp;
// "Modern": blur the composed frame along the camera's motion, leaving the
// screen-space overlays (the HUD, the minimap) sharp. Strength is how far the
// smear reaches, in frames of camera motion.
extern std::atomic<bool> g_gsModernBlur;
extern std::atomic<float> g_gsModernBlurStrength;

namespace gsgpu
{
    // Publishes the host window's native GL context (see gl_context.h); call
    // from the main thread once the window exists.
    void publishHostGlContext();
}
// Host window size in pixels, published by the main thread (0 = unknown).
// The upscaled display readback is capped to it.
extern std::atomic<uint32_t> g_gsGpuPresentWidth;
extern std::atomic<uint32_t> g_gsGpuPresentHeight;

// True once the GPU renderer is actually rasterizing. The GPU backend falls
// back to its own CPU path when the GL 4.5 context cannot be created, and that
// fallback has neither texture replacement nor the internal-resolution scale,
// so the settings UI must not claim the features are available.
extern std::atomic<bool> g_gsGpuActive;

// The GL version and renderer the GPU path got ("OpenGL 4.6 (Core Profile) Mesa
// ..."), empty until it starts. Written once at start-up; the settings UI shows
// it, which is the only way to see it on Windows (no console there).
extern char g_gsGpuInfo[160];

class GSGpuBackend final : public GSRasterBackend
{
public:
    GSGpuBackend();
    ~GSGpuBackend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;
    bool PollPresentation(PresentationFrame &out) override;

    // Blocking round trip to the render thread (see GSRasterBackend).
    uint32_t SynthesizeFrame(float phase, uint32_t &width, uint32_t &height) override;
    void RequestSynthesis(double displayTime) override;
    bool LatestSynthesis(uint32_t &texture, uint32_t &width, uint32_t &height, uint64_t &seq) override;
    bool LatestSynthesisPixels(std::vector<uint8_t> &pixels) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

private:
    // RequestSynthesis / LatestSynthesis: one request in flight at a time.
    std::atomic<bool> m_synthInFlight{false};
    std::mutex m_synthResultMutex;
    uint32_t m_synthResultTexture = 0, m_synthResultWidth = 0, m_synthResultHeight = 0;
    std::vector<uint8_t> m_synthResultPixels; // non-empty when the host gets pixels, not a texture
    bool m_synthResultPixelsValid = false;
    uint64_t m_synthResultSeq = 0;
    enum class CommandType : uint8_t
    {
        State, // draw state for the following Submits
        Submit,
        LoadClut,
        BeginTransfer,
        UploadImage,
        TextureFlush,
        Call,
    };

    // Primitives carry only their vertices (the draw state, which rarely
    // changes, is a separate State command); rarer commands use a payload.
    struct Payload
    {
        GSDrawState state{};
        GSTex0Reg tex0{};
        GSTexClutReg texclut{};
        GSTransferCommand transfer{};
        std::vector<uint8_t> data;
        std::function<void()> call;
    };
    struct Command
    {
        CommandType type = CommandType::Call;
        uint8_t vertexCount = 0;
        std::array<GSVertex, 3> vertices{};
        std::unique_ptr<Payload> payload;

        static Command make(CommandType type)
        {
            Command command;
            command.type = type;
            command.payload = std::make_unique<Payload>();
            return command;
        }
    };
    // Producer side (under m_pendingMutex): state sent with the last State.
    GSDrawState m_lastState{};
    bool m_haveLastState = false;
    // Render thread: state of the next Submit.
    GSDrawState m_currentState{};

    // Commands collect in a producer-side batch that moves to the render
    // queue every kBatchCommands, or at once when `urgent` (anything that
    // waits for or must promptly reach the render thread).
    void enqueue(Command &&command, bool urgent = false);
    void flushPendingLocked(); // m_pendingMutex held
    // Runs `fn` on the render thread after everything queued before it.
    void runSync(const std::function<void()> &fn, const char *why = "other") const;
    void renderLoop();
    void execute(Command &command);

    std::unique_ptr<gsgpu::Renderer> m_renderer; // render thread only

    // Last frame the render thread presented (Present returns it without
    // waiting; PS2X_GS_GPU_SYNC_PRESENT=1 presents synchronously).
    std::mutex m_frameMutex;
    PresentationFrame m_lastFrame;
    // Bumped whenever m_lastFrame is replaced; m_lastHandedSeq is the one the
    // caller was last given, so PollPresentation knows if it has a newer one.
    uint64_t m_lastFrameSeq = 0, m_lastHandedSeq = 0;

    mutable std::mutex m_mutex;
    mutable std::condition_variable m_workAvailable;
    mutable std::condition_variable m_space;
    mutable std::vector<Command> m_queue;
    std::mutex m_pendingMutex;
    std::vector<Command> m_pending;
    bool m_stop = false;
    std::thread m_thread;
};
