#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gpu/gl_context.h"
#include "runtime/gs/gs_texture_replacements.h"
#include "runtime/ps2_camera.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class GSCpuBackend;

// GL state calls the state cache skipped (defined in gs_gpu_renderer.cpp).
extern std::atomic<uint64_t> g_gsGpuStateSkips;

namespace gsgpu
{
    // GS rendering state that lives on the GPU backend's render thread.
    //
    // Local memory: a 4 MiB GPU buffer ("VRAM") in the GS's swizzled layout
    // is the reference. Transfers, CLUT loads and texture sampling operate on
    // it directly in shaders. Frame and Z buffers drawn by the GPU live in GL
    // textures ("targets"); each tracks rows it drew that VRAM lacks (resolved
    // into VRAM on demand by a compute pass) and rows VRAM changed since it
    // was filled (reloaded before it is drawn). The CPU copy of local memory
    // (shared with an inner GSCpuBackend, which presents frames) is refreshed
    // from VRAM only when the CPU needs it.
    class Renderer
    {
    public:
        Renderer();
        ~Renderer();

        void initialize(uint8_t *vram, uint32_t vramSize);
        void reset();

        void submit(const GSPrimitiveBatch &batch);
        void loadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
        void beginTransfer(const GSTransferCommand &command);
        void uploadImage(const uint8_t *data, uint32_t sizeBytes);
        void textureFlush();

        // Brings CPU local memory up to date with the GPU.
        void syncLocalMemory();
        PresentationFrame present(const GSPresentationRequest &request);
        // The newest asynchronous presentation readback not handed over yet, if
        // any (see m_readbackRing). Render thread.
        bool collectPendingReadback(PresentationFrame &frame);
        // Frame interpolation: warp the last composed frame forward by half the
        // last camera motion and write it into its own texture, leaving the
        // overlays alone. Returns the texture (0 if it cannot make one). Called
        // from the render thread, through the backend.
        GLuint synthesizeHostFrame(float phase, uint32_t &outWidth, uint32_t &outHeight);
        // The same, with the phase taken for displayTime (steady_clock seconds).
        GLuint synthesizeHostFrameAt(double displayTime, uint32_t &outWidth, uint32_t &outHeight)
        {
            m_synthDisplayTime = displayTime;
            const GLuint texture = synthesizeHostFrame(-1.0f, outWidth, outHeight);
            m_synthDisplayTime = 0.0;
            return texture;
        }
        // The synthesized frame as pixels, for a host that cannot see our textures.
        // `valid` is false whenever the texture route was used instead.
        std::vector<uint8_t> &synthPixels(uint32_t &outWidth, uint32_t &outHeight, bool &valid)
        {
            outWidth = m_synthPixelsWidth;
            outHeight = m_synthPixelsHeight;
            valid = m_synthPixelsValid;
            return m_synthPixels;
        }
        bool clearFramebuffer(const GSContext &context, uint32_t rgba);
        uint32_t consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes);

        uint32_t readVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
        void writeVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
        void snapshotVram(std::vector<uint8_t> &out);
        GSTransferSnapshot transferSnapshot();

        [[nodiscard]] bool gpuAvailable() const { return m_gpu; }

        // Internal resolution: render targets are `scale` times the GS size
        // in each direction (1 = native). Changing it rebuilds the targets
        // from local memory.
        void setScale(uint32_t scale);

    private:
        struct Target
        {
            bool depth = false;
            uint32_t block = 0; // base address in 256-byte blocks
            uint32_t bw = 0;    // buffer width in 64-pixel units
            uint8_t psm = 0;
            uint32_t width = 0, height = 0;
            // Rows [0, usedRows) have been drawn or are about to be; the texture
            // only mirrors memory there, and [beginByte, endByte) covers them.
            uint32_t usedRows = 0;
            uint32_t beginByte = 0, endByte = 0;
            GLuint texture = 0;
            GLuint loadFramebuffer = 0; // the texture alone, for load passes
            // Screen-space overlay mask (the HUD, the minimap): 1 where a
            // primitive with no perspective (q == 1) was drawn. The compose reads
            // it so the motion blur and interpolation can leave those pixels
            // alone - they do not move with the camera. Lazily created.
            GLuint maskTexture = 0;
            GLuint maskFramebuffer = 0;
            uint64_t maskFrame = ~0ull; // frame the mask was last cleared for
            // Rows the GPU drew that VRAM lacks, and rows whose texture
            // contents are stale (VRAM changed). A target never has both.
            uint32_t dirtyY0 = ~0u, dirtyY1 = 0;
            uint32_t staleY0 = ~0u, staleY1 = 0;
            uint64_t drawSeq = 0; // last draw into it, to order overlapping resolves

            [[nodiscard]] bool gpuDirty() const { return dirtyY0 <= dirtyY1; }
            [[nodiscard]] bool stale() const { return staleY0 <= staleY1; }
        };

        struct Vertex
        {
            float x, y;
            uint32_t z;
            uint32_t rgba;
            float s, t, q;
            float fog;
        };

        // Everything that selects GL state / uniforms for a draw.
        struct DrawKey
        {
            uint32_t frameBlock, frameBw, framePsm, fbmsk;
            uint32_t zBlock, zPsm, zmask, zte;
            int32_t scissor[4];
            uint64_t test, alpha, clamp, tex1, fba;
            uint32_t tme, fge, abe, pabe;
            uint32_t tbp0, tbw, tpsm, tw, th, tcc, tfx, cbp, cpsm, csm, csa;
            uint64_t clutVersion;
            uint32_t ta0, ta1, aem;
            uint32_t fogColor;
            uint32_t linear;
            bool operator==(const DrawKey &other) const;
        };

        // Uniform values, cached per program and indexed by location so the
        // lookup is a vector index rather than a map probe on the hot path.
        // Uniforms are per-program (glProgramUniform*), so a cached value is
        // only invalidated by another glProgramUniform on the same program -
        // nothing outside the renderer can touch these.
        enum class UniformKind : uint8_t { Int, Uint, Float };
        struct UniformSlot
        {
            uint8_t valid = 0;
            uint8_t count = 0;
            UniformKind kind = UniformKind::Int;
            uint32_t bits[4] = {};
        };
        struct Program
        {
            GLuint id = 0;
            std::unordered_map<const char *, GLint> locations;
            std::vector<UniformSlot> uniforms;
        };
        // Sets a uniform only if it differs. Bit comparison, so floats compare
        // exactly (the same value is stored bit-identically).
        void setUniformBits(Program &program, GLint location, UniformKind kind, uint32_t count, const uint32_t *bits);
        void setUniform1i(Program &p, GLint l, int32_t v) { const uint32_t b[1] = {static_cast<uint32_t>(v)}; setUniformBits(p, l, UniformKind::Int, 1, b); }
        void setUniform2i(Program &p, GLint l, int32_t a, int32_t c) { const uint32_t b[2] = {static_cast<uint32_t>(a), static_cast<uint32_t>(c)}; setUniformBits(p, l, UniformKind::Int, 2, b); }
        void setUniform4i(Program &p, GLint l, int32_t a, int32_t c, int32_t d, int32_t e) { const uint32_t b[4] = {static_cast<uint32_t>(a), static_cast<uint32_t>(c), static_cast<uint32_t>(d), static_cast<uint32_t>(e)}; setUniformBits(p, l, UniformKind::Int, 4, b); }
        void setUniform1ui(Program &p, GLint l, uint32_t a) { const uint32_t b[1] = {a}; setUniformBits(p, l, UniformKind::Uint, 1, b); }
        void setUniform2ui(Program &p, GLint l, uint32_t a, uint32_t c) { const uint32_t b[2] = {a, c}; setUniformBits(p, l, UniformKind::Uint, 2, b); }
        void setUniform3ui(Program &p, GLint l, uint32_t a, uint32_t c, uint32_t d) { const uint32_t b[3] = {a, c, d}; setUniformBits(p, l, UniformKind::Uint, 3, b); }
        void setUniform4ui(Program &p, GLint l, uint32_t a, uint32_t c, uint32_t d, uint32_t e) { const uint32_t b[4] = {a, c, d, e}; setUniformBits(p, l, UniformKind::Uint, 4, b); }
        void setUniform1f(Program &p, GLint l, float v) { uint32_t b[1]; std::memcpy(b, &v, 4); setUniformBits(p, l, UniformKind::Float, 1, b); }
        void setUniform2f(Program &p, GLint l, float a, float c) { uint32_t b[2]; std::memcpy(b, &a, 4); std::memcpy(b + 1, &c, 4); setUniformBits(p, l, UniformKind::Float, 2, b); }
        void setUniform3f(Program &p, GLint l, float a, float c, float d) { uint32_t b[3]; std::memcpy(b, &a, 4); std::memcpy(b + 1, &c, 4); std::memcpy(b + 2, &d, 4); setUniformBits(p, l, UniformKind::Float, 3, b); }
        void setUniform4f(Program &p, GLint l, float a, float c, float d, float e) { uint32_t b[4]; std::memcpy(b, &a, 4); std::memcpy(b + 1, &c, 4); std::memcpy(b + 2, &d, 4); std::memcpy(b + 3, &e, 4); setUniformBits(p, l, UniformKind::Float, 4, b); }

        void initGpuObjects();
        // Clears every program's cached uniform values. initGpuObjects() only
        // reassigns .id, so a recreated context would otherwise keep values
        // cached against the old program objects and skip setting them.
        void clearUniformCaches()
        {
            for (Program *p : {&m_draw, &m_loadColor, &m_loadDepth, &m_resolve, &m_hostToLocal, &m_localToLocal,
                               &m_clutLoad, &m_compose, &m_mask, &m_maskCopy, &m_blur, &m_synth})
                p->uniforms.clear();
        }
        void flushBatch();
        // Creates the target's overlay mask if needed and clears it once per
        // frame; returns its framebuffer (0 if the target cannot have one).
        GLuint ensureTargetMask(GL &gl, Target &target);
        // Draws the pending batch into the target's overlay mask.
        void drawOverlayMask(GL &gl, Target &target);
        // Draws the pending batch into the destination's overlay mask, sampling
        // the source's mask: a screen-space copy of one target into another
        // carries the overlays with it.
        void drawMaskCopy(GL &gl, Target &dst, Target &src);
        void applyDrawState(const DrawKey &key, const GSDrawState &state);
        GLint uniform(Program &program, const char *name);

        Target &colorTarget(const GSFrameReg &frame);
        Target &depthTarget(const GSZbufReg &zbuf, uint32_t bw);
        Target &findOrCreateTarget(bool depth, uint32_t block, uint32_t bw, uint8_t psm);
        GLuint framebufferFor(const Target &color, const Target *depth);
        void loadRows(Target &target, uint32_t y0, uint32_t rows); // VRAM -> texture
        void resolveTarget(Target &target);                        // texture -> VRAM (dirty rows)
        // Before VRAM [begin, end) is read: resolve targets drawn there.
        void resolveRange(uint32_t beginByte, uint32_t endByte);
        // After VRAM [begin, end) changed: targets there are stale.
        void vramWroteRange(uint32_t beginByte, uint32_t endByte);
        // Before the GPU draws rows [drawY0, drawY1] of `target`: refresh
        // it, and retire other targets aliasing that memory.
        void prepareTargetForDraw(Target &target, int32_t drawY0, int32_t drawY1);
        // Mirror more rows of `target` (fetched from VRAM).
        void growTarget(Target &target, uint32_t needRows);
        // Memory bytes holding rows [y0, y1] of `target` (whole page rows).
        void rowsToBytes(const Target &target, uint32_t y0, uint32_t y1, uint32_t &begin, uint32_t &end) const;
        // Rows of `target` (within its used rows) stored in [begin, end).
        bool bytesToRows(const Target &target, uint32_t begin, uint32_t end, uint32_t &y0, uint32_t &y1) const;
        void markStaleRows(Target &target, uint32_t beginByte, uint32_t endByte);
        // Reloads the stale rows within [needY0, needY1] (all of them by
        // default); the rest stay stale until something needs them.
        void loadStaleRows(Target &target, uint32_t needY0 = 0u, uint32_t needY1 = ~0u);
        // Colour target a draw can sample directly (instead of local memory)
        // for texture TBP0/TBW/PSM covering `rows`; brought up to date.
        Target *textureSourceTarget(uint32_t tbp0, uint32_t tbw, uint32_t tpsm, uint32_t rows, uint32_t needY0,
                                    uint32_t needY1);
        // Scaled copy of `target` for draws sampling the target they render to.
        GLuint feedbackCopy(const Target &target);
        // High-resolution version of the CPU-composed presentation frame.
        void upscalePresentation(const GSPresentationRequest &request, PresentationFrame &frame);
        void applyPendingScale();
        void destroyTargets();

        // Texture replacement (PCSX2-format packs, gs_texture_replacements.h):
        // texture memory is hashed from the CPU copy of local memory, kept in
        // step with uploads while a pack is loaded; replacement images load
        // on a worker thread and are sampled in place of the GS texture.
        struct Replacement
        {
            GLuint texture = 0;
            bool requested = false;
            uint64_t stamp = 0; // the file's stamp the texture was loaded from
        };
        struct HashedTexture
        {
            uint64_t generation = 0; // 1 + sum of its pages' write generations when hashed
            uint64_t hash = 0;
            // The replacement this texture resolves to for this generation, and
            // whether that is settled. The same texture is drawn thousands of
            // times, and re-finding it in both replacement maps on every draw was
            // the pack's per-draw cost. A texture whose load has only been
            // *requested* is deliberately left unresolved, so its id is picked up
            // when the worker delivers it.
            GLuint replacement = 0;
            bool resolved = false;
        };
        void updateReplacements(); // between frames: a new set of packs
        bool replacementsActive() const { return m_replIndex && !m_replIndex->files.empty(); }
        GLuint replacementTexture(const GSDrawState &state);
        // PS2X_TEXREPL_DUMP=<dir>: write every texture the matcher sees as a
        // PNG named by its replacement key (<tex0Hash>-<clutHash>-<bits>.png),
        // the same naming PCSX2 uses. For authoring packs (e.g. button-prompt
        // sets); the RGBA expansion is best effort for identification.
        void dumpReplacementTexture(const GSDrawState &state, const gsrepl::Key &key);
        uint64_t hashTextureMemory(const GSDrawState &state);
        uint64_t hashClut(const GSDrawState &state);
        // CPU-copy bookkeeping for [begin, end): a write happened; `cpuStale`:
        // only VRAM holds it (GPU writes) until the next readback.
        void markPagesWritten(uint32_t beginByte, uint32_t endByte, bool cpuStale);
        void replacementWorker();
        void uploadLoadedReplacements();
        void stopReplacementWorker();

        // CPU copy of local memory <-> VRAM.
        void readbackVram();
        void uploadVram(uint32_t beginByte, uint32_t endByte);
        void dispatchHostToLocal(const uint8_t *data, uint32_t pixels, uint32_t streamBytes);
        void dispatchLocalToLocal(const GSTransferCommand &command);

        GLContext m_context;
        bool m_gpu = false;
        std::unique_ptr<GSCpuBackend> m_cpu; // presentation, local->host transfers
        uint8_t *m_vram = nullptr;

        std::shared_ptr<const gsrepl::Index> m_replIndex;
        uint64_t m_replVersion = 0;
        std::unordered_map<gsrepl::Key, Replacement, gsrepl::KeyHash> m_replacements;
        std::unordered_map<uint64_t, HashedTexture> m_textureHashes;
        std::unordered_set<gsrepl::Key, gsrepl::KeyHash> m_dumpedTextures; // PS2X_TEXREPL_DUMP
        std::vector<uint32_t> m_pageGeneration = std::vector<uint32_t>(512, 0u); // 8 KiB pages of local memory
        std::vector<uint8_t> m_pageCpuStale = std::vector<uint8_t>(512, 0u);
        uint64_t m_clutHashKey = ~0ull, m_clutHash = 0;
        std::thread m_replThread;
        std::mutex m_replMutex;
        std::condition_variable m_replCv;
        bool m_replStop = false;
        std::deque<std::pair<gsrepl::Key, std::string>> m_replRequests;
        std::deque<std::pair<gsrepl::Key, std::unique_ptr<gsrepl::Image>>> m_replResults;
        std::atomic<uint32_t> m_replResultCount{0};
        // Loaded replacements waiting for their GPU upload (render thread only),
        // and the bytes uploaded this frame (a burst is spread over frames).
        std::deque<std::pair<gsrepl::Key, std::unique_ptr<gsrepl::Image>>> m_replPendingUploads;
        size_t m_replUploadedThisFrame = 0;
        uint64_t m_statReplDraws = 0, m_statReplHits = 0, m_statReplMisses = 0, m_statReplLoaded = 0, m_statReplHashes = 0;
        // PS2X_TEXREPL_STATS: prints the accumulated replacement-related
        // render-thread counters (called at the interval and at teardown).
        void emitTexreplStats();

        uint32_t m_scale = 1;
        std::atomic<uint32_t> m_requestedScale{1};
        GLuint m_feedbackTexture = 0;
        uint32_t m_feedbackWidth = 0, m_feedbackHeight = 0;
        std::vector<std::unique_ptr<Target>> m_targets;
        std::unordered_map<uint64_t, GLuint> m_framebuffers; // (color tex, depth tex) -> FBO

        GLuint m_vramBuffer = 0, m_swizzleBuffer = 0, m_clutBuffer = 0, m_stagingBuffer = 0;
        size_t m_stagingCapacity = 0;
        Program m_draw, m_loadColor, m_loadDepth, m_resolve, m_hostToLocal, m_localToLocal, m_clutLoad, m_compose;
        // Writes 1 wherever a screen-space overlay primitive lands (see
        // drawOverlayMask); reuses the draw vertex shader so the geometry lands
        // in the same place.
        Program m_mask;
        // Carries a target's overlay mask along when a screen-space pass copies
        // that target into another (the game's copies of the finished frame), so
        // the mask reaches the display the compose reads.
        Program m_maskCopy;
        // The widest target an overlay has been drawn into: the scene framebuffer,
        // where the HUD lives. The compose reads the mask from here rather than
        // from the display, because the copies that carry the mask to the display
        // happen on only some frames - a mask read from the display is missing on
        // the others and the HUD blurs outright. The two rects are the same size
        // and start within a couple of rows of each other, which for a protective
        // region is close enough.
        Target *m_overlayTarget = nullptr;
        // Where the composed rect starts in the overlay mask's texture, in scaled
        // pixels (the compose's u_rect1 origin). The synth must fetch the mask at
        // exactly the same texels the compose does.
        int32_t m_overlayMaskOriginX = 0, m_overlayMaskOriginY = 0;
        // How many times the outgoing picture has been copied into the
        // interpolation source, and the id it holds - a source that never
        // updates is a frozen picture.
        uint64_t m_prevComposedCopies = 0;
        GLuint m_zeroMask = 0; // 1x1 R8 zero, for a source that has no mask yet
        bool m_maskEnabled = false;
        // The target a draw samples as its texture, if it is one (set by
        // applyDrawState): a draw that reads a target is a copy of the frame.
        Target *m_textureSourceTarget = nullptr;
        // The size the game declares for that texture. It is not the target's
        // dimensions: the port's target texture can be taller (it is clamped to
        // what local memory allows), while the game's view of it is this.
        uint32_t m_textureSourceWidth = 0, m_textureSourceHeight = 0;
        GLuint m_presentTexture = 0, m_presentFramebuffer = 0; // upscaled display output
        uint32_t m_presentWidth = 0, m_presentHeight = 0;
        // Motion blur ("Modern"). The compose draws into m_composeFramebuffer, the
        // blur pass reads that and writes the display target. Grow-only, like the
        // present pair. No history: the smear is spatial, along the camera's own
        // motion, so there is nothing to seed and nothing that can accumulate.
        Program m_blur;
        GLuint m_composeTexture = 0, m_composeFramebuffer = 0;
        uint32_t m_composeWidth = 0, m_composeHeight = 0;
        Ps2CameraState m_blurPrevCamera; // the camera of the last composed frame
        // Frame interpolation: the camera of the last two composed frames, the
        // texture the last compose produced, and the texture a synthesized frame
        // is written into. The camera is tracked every compose, not just when the
        // blur is on, because the interpolation deltas need it too.
        Ps2CameraState m_interpPrevCamera, m_interpCamera;
        Ps2CameraState m_interpPrevPrevCamera; // one more back, for PS2X_INTERP_ALIGN
        void interpAlignCheck(); // PS2X_INTERP_ALIGN diagnostic
        bool m_interpHavePrev = false;
        // Game frames, not VBlanks: VCS draws a new view only every 2-4 VBlanks
        // while the renderer composes every VBlank, so the pair shifts only when
        // the picture's camera changes. The shift time and the measured interval
        // between game frames give the phase (render-thread clock).
        bool m_interpShifted = false;    // this compose started a new game frame
        Ps2CameraState m_requestCamera;  // the camera that came with this compose's request
        uint64_t m_interpContentSeq = ~0ull; // drawSeq of the composed source last time
        bool m_interpPending = false;    // picture changed, its camera not seen yet
        int m_interpPendingComposes = 0;
        bool m_interpPairedNow = false;  // a pair completed this compose (diagnostics)
        // Pairs pictures with cameras by picture change (see the definition).
        void interpPair(uint64_t contentSeq);
        double m_interpPairTime = 0.0;   // when the current pair was set (seconds)
        double m_interpInterval = 1.0 / 30.0; // running average between game frames
        GLuint m_lastComposedTexture = 0;
        uint32_t m_lastComposedWidth = 0, m_lastComposedHeight = 0;
        // The picture composed before the newest one: the source the
        // interpolation warps between the previous and current pictures.
        GLuint m_prevComposedTexture = 0, m_prevComposedFramebuffer = 0;
        uint32_t m_prevComposedWidth = 0, m_prevComposedHeight = 0;
        Program m_synth;
        // The overlay mask of each composed picture, snapshotted right after the
        // compose with its own coordinates. The scene's live mask is cleared and
        // rebuilt as the game draws the next frame, so read later (when the host
        // asks for an interpolated frame) it is usually empty - the HUD then got
        // warped with the world. [m_interpMaskCur] is the newest picture's,
        // the other the previous picture's.
        Program m_maskSnap;
        GLuint m_interpMask[2] = {0u, 0u}, m_interpMaskFramebuffer[2] = {0u, 0u};
        uint32_t m_interpMaskWidth = 0, m_interpMaskHeight = 0;
        int m_interpMaskCur = 0;
        // A ring, not a pair. The host *queues* its draws, so several synthesized
        // frames can be in flight at once: at 240 there are three per picture, and
        // a two-deep ring lets the renderer rewrite a texture before the host's
        // draw of it has even executed - the draw then shows a later frame, or a
        // partly rewritten one.
        static constexpr int kSynthRing = 6;
        // A synthesized frame comes back as pixels when the host's context cannot
        // be shared - on Windows the driver refuses, so handing over a texture would
        // be handing over a name the host's context cannot see. Same route the
        // composed frames already take. Sized drawWidth*drawHeight*4.
        std::vector<uint8_t> m_synthPixels;
        uint32_t m_synthPixelsWidth = 0, m_synthPixelsHeight = 0;
        bool m_synthPixelsValid = false;
        GLuint m_synthTexture[kSynthRing] = {}, m_synthFramebuffer[kSynthRing] = {};
        uint32_t m_synthWidth = 0, m_synthHeight = 0;
        double m_synthDisplayTime = 0.0; // > 0: the phase is for this moment
        int m_synthIndex = 0;
        // Downscaled copy of the synthesized frame at the readback size, so the
        // host draws it exactly as it draws the composed picture.
        GLuint m_synthReadbackTexture = 0, m_synthReadbackFramebuffer = 0;
        uint32_t m_synthReadbackWidth = 0, m_synthReadbackHeight = 0;
        // Reads the published camera into m_interpCamera, tracking the previous
        // one. Called once per compose, whatever else is enabled.
        void trackCamera(const GSPresentationRequest &request);
        void ensureBlurTargets(GL &gl, uint32_t width, uint32_t height);
        // Smears the frame the compose just drew along the camera's motion,
        // scaling the smear per pixel by the overlay mask in its alpha, and
        // writes the result into destinationFramebuffer.
        void blurCompose(GL &gl, uint32_t width, uint32_t height, GLuint destinationFramebuffer);
        void destroyBlurTargets();
        // PS2X_GS_TRACE_DRAWS=<presentIndex>: the presented-frame counter and a
        // per-frame draw counter, for logging one frame's draws in order.
        uint64_t m_presentIndex = 0;
        uint64_t m_frameDrawIndex = 0;
        // Downscale target for the presentation readback (see upscalePresentation).
        GLuint m_readbackTexture = 0, m_readbackFramebuffer = 0;
        uint32_t m_readbackWidth = 0, m_readbackHeight = 0;
        // Asynchronous presentation readback: each present copies into the next
        // pack buffer of this ring and fences it, and hands over the copy made
        // one present earlier, which the GPU has normally finished by then. A
        // synchronous read made the renderer wait for the whole frame on the GPU
        // before it could submit the next one (see upscalePresentation).
        struct ReadbackSlot
        {
            GLuint buffer = 0;
            size_t capacity = 0;
            GLsync fence = nullptr;
            uint32_t width = 0, height = 0;
            uint32_t displayFbp = 0;
            const uint8_t *mapped = nullptr; // persistent client-side mapping, when available
            uint64_t serial = 0;             // issue order, to find the newest finished copy
        };
        ReadbackSlot m_readbackRing[4];
        uint32_t m_readbackNext = 0;
        uint64_t m_readbackSerial = 0;
        // The newest copy the GPU has finished, without waiting; older pending
        // ones are dropped. nullptr: none is ready yet.
        ReadbackSlot *newestFinishedReadback();
        void destroyReadbackRing();
        // PS2X_GS_GPU_TIMERS=1: where the GPU's time goes. Each draw / dispatch /
        // copy is bracketed by a GL_TIME_ELAPSED query (gpuMark ... gpuEnd), and
        // every 300 presents the milliseconds per present each kind took on the
        // GPU are printed. Diagnostic only.
        enum class GpuWork : uint8_t
        {
            Draw,
            Load,
            Resolve,
            Feedback,
            Transfer,
            Present,
            Idle,
            Count
        };
        void gpuMark(GpuWork work);
        void gpuEnd();
        bool m_gpuQueryOpen = false;
        void gpuTimersFrameEnd();
        bool m_gpuTimers = false;
        GpuWork m_gpuWork = GpuWork::Idle;
        struct GpuMark
        {
            GLuint query;
            GpuWork work;
        };
        std::vector<GpuMark> m_gpuMarks;      // issued, oldest first
        std::vector<GLuint> m_gpuFreeQueries;
        double m_gpuWorkNs[static_cast<int>(GpuWork::Count)] = {};
        uint32_t m_gpuTimerFrames = 0;
        void takeReadback(ReadbackSlot &slot, PresentationFrame &frame);
        // Double-buffered composed display handed to the host as a texture when
        // the renderer shares the window's GL context (no readback, no upload).
        GLuint m_presentShared[2] = {0u, 0u};
        GLuint m_presentSharedFramebuffer[2] = {0u, 0u};
        uint32_t m_presentSharedWidth = 0, m_presentSharedHeight = 0;
        GLint m_presentSharedFilter = 0; // GL_LINEAR / GL_NEAREST, follows the setting
        int m_presentSharedIndex = 0;
        GLuint m_vao = 0, m_emptyVao = 0;
        GLuint m_vbo = 0;
        size_t m_vboCapacity = 0;
        // Streaming without the driver's help: a persistently mapped buffer
        // written ahead of the GPU in eight fenced segments. Rewriting one buffer
        // with glBufferSubData for every draw made the driver stage a hidden
        // copy each time - on NVIDIA most of its per-draw CPU cost, which bound
        // dense cutscenes on a laptop. PS2X_GS_STREAM_RING=0: the old path.
        struct StreamRing
        {
            GLuint buffer = 0;
            uint8_t *base = nullptr;
            size_t size = 0, head = 0;
            int segment = 0;
            GLsync fence[8] = {};
        };
        StreamRing m_vertexRing, m_stagingRing;
        // Offset of `bytes` in the ring (aligned, within one segment), or
        // SIZE_MAX if it cannot be mapped. `recreated`: the buffer is new.
        size_t ringAlloc(StreamRing &ring, size_t bytes, size_t align, size_t initialSize, bool &recreated);
        void destroyStreamRing(StreamRing &ring);
        bool m_streamRing = true;
        GLint m_ssboAlign = 256;
        uint32_t m_ringStalls = 0; // fence waits that had to wait (PS2X_GS_GPU_TIMERS)
        GLint m_firstVertex = 0; // of the batch in the vertex buffer (mask draws reuse it)
        // Replacement texture filtering/wrap as sampler objects (index: linear*4
        // + clampS*2 + clampT) rather than texture parameters changed per draw.
        GLuint m_replSamplers[8] = {};
        std::vector<Vertex> m_vertices;
        bool m_haveKey = false;
        bool m_inFlush = false; // applyDrawState may resolve memory mid-flush
        // resolveTarget issues the two glMemoryBarrier calls a resolve needs,
        // but only once per *group*: consecutive resolves in one call site are
        // ordered by a single pair. Two barriers per resolve cost ~6 driver
        // points at 250-350 draws/frame (owner-measured, dropping both gains
        // 2.1 fps), and the spec only needs one pair around the writes.
        bool m_resolveGroupOpen = false;
        // A resolve's read of a target texture only needs ordering if something
        // drew into that framebuffer since the last barrier. Every resolve site
        // resolves ~1 target, so per-site grouping coalesces nothing (measured);
        // this is what actually removes barriers - a draw followed by several
        // resolves costs one barrier, not one each.
        bool m_framebufferWriteSinceBarrier = true;

        // GL state cache for the four calls every pass re-issues whether or not
        // they changed. Consecutive passes overwhelmingly use the same program,
        // framebuffer, viewport and VAO (all 8,349 resolves share one program;
        // consecutive draws share a target), so this skips the redundant ones.
        // Driver-independent by construction: fewer calls is fewer calls, which
        // is why the skip count is the number to measure locally.
        //
        // Sentinels rather than a valid flag: no GL name is 0xFFFFFFFF and no
        // viewport component is INT32_MIN, so the first call always applies.
        struct GlStateCache
        {
            uint32_t program = ~0u, framebuffer = ~0u, vao = ~0u;
            int32_t viewport[4] = {INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN};
            int32_t scissor[4] = {INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN};
            int32_t scissorTest = -1, depthTest = -1, blend = -1;
            int32_t depthFunc = -1, depthMask = -1;
            int32_t colorMask[4] = {-1, -1, -1, -1};
        };
        GlStateCache m_glState;
        uint64_t m_glStateSkips = 0; // GL state calls the cache avoided
        void invalidateGlState() { m_glState = GlStateCache{}; }
        void useProgram(GLuint program)
        {
            if (m_glState.program == program) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.program = program;
            m_context.gl.glUseProgram(program);
        }
        void bindFramebuffer(GLuint framebuffer)
        {
            if (m_glState.framebuffer == framebuffer) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.framebuffer = framebuffer;
            m_context.gl.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        }
        void bindVao(GLuint vao)
        {
            if (m_glState.vao == vao) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.vao = vao;
            m_context.gl.glBindVertexArray(vao);
        }
        void setScissorTest(bool on)
        {
            const int32_t v = on ? 1 : 0;
            if (m_glState.scissorTest == v) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.scissorTest = v;
            if (on) m_context.gl.glEnable(GL_SCISSOR_TEST); else m_context.gl.glDisable(GL_SCISSOR_TEST);
        }
        void setScissor(int32_t x, int32_t y, int32_t w, int32_t h)
        {
            if (m_glState.scissor[0] == x && m_glState.scissor[1] == y && m_glState.scissor[2] == w &&
                m_glState.scissor[3] == h)
            { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.scissor[0] = x; m_glState.scissor[1] = y; m_glState.scissor[2] = w; m_glState.scissor[3] = h;
            m_context.gl.glScissor(x, y, w, h);
        }
        void setDepthTest(bool on)
        {
            const int32_t v = on ? 1 : 0;
            if (m_glState.depthTest == v) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.depthTest = v;
            if (on) m_context.gl.glEnable(GL_DEPTH_TEST); else m_context.gl.glDisable(GL_DEPTH_TEST);
        }
        void setBlend(bool on)
        {
            const int32_t v = on ? 1 : 0;
            if (m_glState.blend == v) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.blend = v;
            if (on) m_context.gl.glEnable(GL_BLEND); else m_context.gl.glDisable(GL_BLEND);
        }
        void setDepthFunc(int32_t func)
        {
            if (m_glState.depthFunc == func) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.depthFunc = func;
            m_context.gl.glDepthFunc(static_cast<GLenum>(func));
        }
        void setDepthMask(int32_t mask)
        {
            if (m_glState.depthMask == mask) { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.depthMask = mask;
            m_context.gl.glDepthMask(static_cast<GLboolean>(mask));
        }
        void setColorMask(int32_t r, int32_t g, int32_t b, int32_t a)
        {
            if (m_glState.colorMask[0] == r && m_glState.colorMask[1] == g && m_glState.colorMask[2] == b &&
                m_glState.colorMask[3] == a)
            { ++m_glStateSkips; ++g_gsGpuStateSkips; return; }
            m_glState.colorMask[0] = r; m_glState.colorMask[1] = g; m_glState.colorMask[2] = b; m_glState.colorMask[3] = a;
            m_context.gl.glColorMask(static_cast<GLboolean>(r), static_cast<GLboolean>(g), static_cast<GLboolean>(b),
                                     static_cast<GLboolean>(a));
        }
        void setViewport(int32_t x, int32_t y, int32_t w, int32_t h)
        {
            if (m_glState.viewport[0] == x && m_glState.viewport[1] == y && m_glState.viewport[2] == w &&
                m_glState.viewport[3] == h)
            {
                ++m_glStateSkips;
                ++g_gsGpuStateSkips;
                return;
            }
            m_glState.viewport[0] = x; m_glState.viewport[1] = y; m_glState.viewport[2] = w; m_glState.viewport[3] = h;
            m_context.gl.glViewport(x, y, w, h);
        }
        // A resolve's VRAM write only needs ordering before the next VRAM read.
        // Every reader funnels through loadRows() and readbackVram(), so the
        // barrier is issued there - one per read instead of one per resolve.
        bool m_vramWriteSinceBarrier = false;
        void vramBarrierBeforeRead();
        void resolveBarrierBegin();
        void resolveGroupEnd();
        DrawKey m_key{};
        GSDrawState m_keyState{};
        Target *m_drawColor = nullptr;
        Target *m_drawDepth = nullptr;
        int32_t m_drawY0 = 0, m_drawY1 = 0;
        // Vertex bounds of the pending batch: rows for the target bookkeeping,
        // x too so a screen-sized draw can be told from a small one.
        int32_t m_batchY0 = 0, m_batchY1 = 0;
        int32_t m_batchX0 = 0, m_batchX1 = 0;
        uint64_t m_drawSeq = 0;

        // CLUT: CBP0/CBP1 registers for the CLD compare modes, and a version
        // bumped per load so batches don't straddle one.
        uint32_t m_clutCbp[2] = {0, 0};
        uint64_t m_clutVersion = 0;

        // Current host->local transfer.
        GSTransferCommand m_transfer{};
        GSTransferSnapshot m_transferState{};
        bool m_cpuTransfer = false; // local->host: served by m_cpu

        uint64_t m_statBlurSkipped = 0; // motion-blur draws dropped
        uint64_t m_statDraws = 0, m_statResolves = 0, m_statLoads = 0, m_statReadbacks = 0, m_statClutLoads = 0,
                 m_statTransfers = 0;
        const char *m_syncReason = "other";
        std::unordered_map<std::string, uint64_t> m_statResolveReasons;
        double m_timeReadback = 0, m_timeDraw = 0, m_timePresent = 0;
    };
}
