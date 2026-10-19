#pragma once

#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <vector>

class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;
    virtual void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) = 0;

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;

    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;
    // A VBlank with nothing new to present: lets a backend whose presents
    // finish later (queued, or read back asynchronously) hand over the newest
    // finished frame the caller has not had yet. False: keep the current one.
    virtual bool PollPresentation(PresentationFrame &out)
    {
        (void)out;
        return false;
    }

    // Frame interpolation: warp the last composed frame forward by half the last
    // camera motion and return the texture to draw (0 if it cannot make one),
    // with its size. This is a *blocking* round trip where Present is not: the
    // caller has to have the texture before it draws. Backends that cannot do it
    // leave it alone.
    virtual uint32_t SynthesizeFrame(float phase, uint32_t &width, uint32_t &height)
    {
        (void)phase;
        (void)width;
        (void)height;
        return 0;
    }

    // The non-blocking form: ask for an interpolated frame for the moment it
    // will be displayed (steady_clock seconds), and fetch the newest finished
    // one. The host then never waits on a busy render thread - it shows the
    // latest result, repeating it for a frame at worst.
    virtual void RequestSynthesis(double displayTime) { (void)displayTime; }
    // The same frame as pixels, for a host that cannot be handed a texture - see
    // the note in gs_gpu_renderer.h. Empty (`false`) means the texture route is in
    // use, which is every backend that is not the GPU one and every platform where
    // the contexts share. A default here so no other backend needs to know.
    virtual bool LatestSynthesisPixels(std::vector<uint8_t> &pixels)
    {
        pixels.clear();
        return false;
    }
    virtual bool LatestSynthesis(uint32_t &texture, uint32_t &width, uint32_t &height, uint64_t &seq)
    {
        (void)texture;
        (void)width;
        (void)height;
        (void)seq;
        return false;
    }

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;

    // Restrict drawing to the 8-row strips with (y / 8) % count == index, for
    // banded multi-threaded rasterization. Transfers and clears are unaffected.
    virtual void SetRowBand(uint32_t index, uint32_t count) { (void)index; (void)count; }
    // Banded workers share VRAM: only one of them performs a host->local
    // upload's writes; the others call UploadImage with writes disabled so
    // their transfer bookkeeping still advances.
    virtual void SetUploadWrites(bool enabled) { (void)enabled; }
};
