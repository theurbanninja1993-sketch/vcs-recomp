#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_texture_page_cache.h"

#include <array>
#include <mutex>
#include <vector>

class GSCpuBackend final : public GSRasterBackend
{
public:
    GSCpuBackend();

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

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    // CLUT buffer (entry i: low half at i, high half at i + 256), for palette hashing.
    const std::array<uint16_t, 512> &clutTable() const { return m_clut; }
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;
    void SetRowBand(uint32_t index, uint32_t count) override;
    void SetUploadWrites(bool enabled) override { m_uploadWrites = enabled; }

    // Helpers for the GPU renderer (each takes the backend mutex once).
    // Raw pixel values in the PSM's native width (no format conversion).
    void ReadVramRect(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t *out) const;
    void WriteVramRect(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       const uint32_t *in);
    // Decodes the draw state's texture (TEX0, current CLUT, TEXA) to RGBA8
    // (R in the low byte), point-sampled, width x height texels from (0,0).
    void DecodeTexture(const GSDrawState &state, uint32_t width, uint32_t height, uint32_t *out);
    // Changes whenever the CLUT buffer contents may have changed.
    [[nodiscard]] uint64_t ClutVersion() const { return m_clutVersion; }

private:
    void ResetUnlocked();
    void LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    uint32_t ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    void DrawPrimitive(const GSPrimitiveBatch &batch);
    void DrawSprite(const GSPrimitiveBatch &batch);
    void DrawTriangle(const GSPrimitiveBatch &batch);
    void DrawLine(const GSPrimitiveBatch &batch);
    // depthPassed: the caller already ran this pixel's Z test against the
    // current Z buffer (DrawTriangle's early test), so it is not re-read.
    void WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog,
                    bool depthPassed = false);
    uint32_t SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v);
    uint32_t LookupCLUT(const GSDrawState &state, uint8_t index, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm);

    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY) const;

    using WriteVramFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using ReadVramFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};
    std::array<uint16_t, 512> m_clut{};
    std::array<uint32_t, 2> m_clutCbp{};
    GSMem::TexturePageCache m_texturePageCache;
    uint32_t m_bandIndex = 0;
    uint32_t m_bandCount = 1;
    bool m_uploadWrites = true;
    uint64_t m_clutVersion = 1;
    uint32_t TexelToRgba(const GSDrawState &state, uint32_t raw);
    // Row-band ownership per 8-row strip, precomputed by SetRowBand: ownsRow()
    // runs per pixel and the modulo by a runtime band count was a division.
    static constexpr uint32_t kBandTableStrips = 1024u;
    std::array<uint8_t, kBandTableStrips> m_ownsStrip{};
    bool ownsRow(int y) const
    {
        if (m_bandCount <= 1u)
            return true;
        const uint32_t strip = static_cast<uint32_t>(y >> 3);
        if (strip < kBandTableStrips)
            return m_ownsStrip[strip] != 0u;
        return (strip % m_bandCount) == m_bandIndex;
    }

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;
};
