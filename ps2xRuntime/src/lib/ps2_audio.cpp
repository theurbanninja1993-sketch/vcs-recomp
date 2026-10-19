#include <atomic>
#include "ThreadNaming.h"
#include "runtime/ps2_audio.h"
#include "runtime/ps2_memory.h"
#include "ps2_host_backend.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
    std::vector<uint8_t> buildWavFromPcm(const int16_t *pcm, size_t sampleCount, uint32_t sampleRate)
    {
        const uint32_t dataSize = static_cast<uint32_t>(sampleCount * 2);
        const uint32_t fileSize = 36 + dataSize;
        std::vector<uint8_t> wav(8 + fileSize);

        uint8_t *p = wav.data();
        p[0] = 'R';
        p[1] = 'I';
        p[2] = 'F';
        p[3] = 'F';
        p[4] = static_cast<uint8_t>(fileSize);
        p[5] = static_cast<uint8_t>(fileSize >> 8);
        p[6] = static_cast<uint8_t>(fileSize >> 16);
        p[7] = static_cast<uint8_t>(fileSize >> 24);
        p[8] = 'W';
        p[9] = 'A';
        p[10] = 'V';
        p[11] = 'E';
        p[12] = 'f';
        p[13] = 'm';
        p[14] = 't';
        p[15] = ' ';
        p[16] = 16;
        p[17] = 0;
        p[18] = 0;
        p[19] = 0;
        p[20] = 1;
        p[21] = 0;
        p[22] = 1;
        p[23] = 0;
        p[24] = static_cast<uint8_t>(sampleRate);
        p[25] = static_cast<uint8_t>(sampleRate >> 8);
        p[26] = static_cast<uint8_t>(sampleRate >> 16);
        p[27] = static_cast<uint8_t>(sampleRate >> 24);
        const uint32_t byteRate = sampleRate * 2;
        p[28] = static_cast<uint8_t>(byteRate);
        p[29] = static_cast<uint8_t>(byteRate >> 8);
        p[30] = static_cast<uint8_t>(byteRate >> 16);
        p[31] = static_cast<uint8_t>(byteRate >> 24);
        p[32] = 2;
        p[33] = 0;
        p[34] = 16;
        p[35] = 0;
        p[36] = 'd';
        p[37] = 'a';
        p[38] = 't';
        p[39] = 'a';
        p[40] = static_cast<uint8_t>(dataSize);
        p[41] = static_cast<uint8_t>(dataSize >> 8);
        p[42] = static_cast<uint8_t>(dataSize >> 16);
        p[43] = static_cast<uint8_t>(dataSize >> 24);
        std::memcpy(p + 44, pcm, dataSize);
        return wav;
    }
}

namespace ps2_vag
{
    bool decode(const uint8_t *data, uint32_t sizeBytes,
                std::vector<int16_t> &outPcm, uint32_t &outSampleRate);
}

struct PS2AudioBackend::Impl
{
    struct TrackedSound
    {
        Sound snd;
        uint32_t sampleKey;
    };
    std::vector<TrackedSound> activeSounds;
    AudioStream stream{};
    bool streamLoaded = false;
};

// Frames the audio device asked for and the ring could not supply. They are
// filled with silence, so a choppy stream is otherwise invisible: the dropped
// counter only sees the ring overflowing, never running dry.
std::atomic<uint64_t> g_ps2AudioFramesUnderrun{0};
// Ring fill level at the last callback (48 kHz frames; 48000 is one second).
std::atomic<uint32_t> g_ps2AudioRingDepth{0};

namespace
{
    // SPU2 output ring, filled by the IOP (emulation thread) and drained by the
    // audio device callback. raylib callbacks carry no user pointer, hence the
    // single global stream. Ported from the VCS port's validated audio path:
    // lock-free (a mutex here was a dropout hazard - the callback could block
    // behind a submit and miss its deadline, heard as a dropout) with
    // free-running counters so producer and consumer never touch each other's
    // fields.
    struct StreamRing
    {
        // Power of two, so indexing is a mask.
        static constexpr size_t kFrames = 32768u; // ~0.68 s at 48 kHz
        static constexpr size_t kMask = kFrames - 1u;
        std::vector<int16_t> samples = std::vector<int16_t>(kFrames * 2u, 0);
        std::atomic<size_t> writeFrame{0}; // published by the producer
        std::atomic<size_t> readFrame{0};  // published by the consumer
    };

    StreamRing g_streamRing;

    // The emulator produces audio per emulated frame, so a game running a few
    // percent off real time feeds the ring slightly slower than the device
    // drains it: the ring runs dry and refills in bursts, and the silence
    // between bursts is heard as crackling. Reading the ring at a slightly
    // adjusted rate absorbs that: a bounded, inaudible drift correction that
    // holds the fill level near a target. With no deficit the ratio is exactly
    // 1 and the samples pass through untouched.
    constexpr double kDriftLimit = 0.02; // never stretch or squeeze more than 2%
    void streamCallback(void *bufferData, unsigned int frames)
    {
        // This runs on the audio device's thread, which is not ours; name it
        // once so the per-thread CPU report can tell it apart from the rest.
        static const bool s_named = []()
        {
            ThreadNaming::SetCurrentThreadName("AudioCallback");
            return true;
        }();
        (void)s_named;
        static double s_readPos = 0.0; // fractional position, device thread only
        int16_t *out = static_cast<int16_t *>(bufferData);
        const size_t write = g_streamRing.writeFrame.load(std::memory_order_acquire);
        const size_t read = g_streamRing.readFrame.load(std::memory_order_relaxed);
        const size_t available = write - read;

        // How much to hold back before the next gap. The emulated sound engine
        // does not submit smoothly: it delivers ~0.2 s of audio at once, then a
        // gap, so a shallow ring runs dry between bursts and the silence is
        // heard as crackling. Start shallow (low latency) and grow on every dry
        // callback, decaying back when it has been steady - a fast machine keeps
        // low latency, a jittery one buffers enough to bridge its bursts.
        constexpr size_t kTargetStep = StreamRing::kFrames / 64u; // ~10.7 ms
        static size_t s_target = StreamRing::kFrames / 8u;        // ~85 ms
        static int s_cleanCallbacks = 0;
        const double error = static_cast<double>(available) - static_cast<double>(s_target);
        const double ratio = std::clamp(1.0 + error / static_cast<double>(s_target) * kDriftLimit,
                                        1.0 - kDriftLimit, 1.0 + kDriftLimit);

        size_t produced = 0;
        for (; produced < frames; ++produced)
        {
            const size_t i0 = static_cast<size_t>(s_readPos);
            if (i0 + 1u >= available)
                break; // nothing left to interpolate between
            const double frac = s_readPos - static_cast<double>(i0);
            const size_t f0 = (read + i0) & StreamRing::kMask;
            const size_t f1 = (read + i0 + 1u) & StreamRing::kMask;
            out[produced * 2u] = static_cast<int16_t>(g_streamRing.samples[f0 * 2u] * (1.0 - frac) +
                                                      g_streamRing.samples[f1 * 2u] * frac);
            out[produced * 2u + 1u] = static_cast<int16_t>(g_streamRing.samples[f0 * 2u + 1u] * (1.0 - frac) +
                                                           g_streamRing.samples[f1 * 2u + 1u] * frac);
            s_readPos += ratio;
        }
        if (produced < frames)
        {
            g_ps2AudioFramesUnderrun.fetch_add(frames - produced, std::memory_order_relaxed);
            s_target = std::min(StreamRing::kFrames / 2u, s_target + kTargetStep);
            s_cleanCallbacks = 0;
        }
        else if (++s_cleanCallbacks > 200) // ~a few seconds without a dry callback
        {
            s_cleanCallbacks = 0;
            s_target = std::max(StreamRing::kFrames / 8u, s_target - kTargetStep);
        }
        std::memset(out + produced * 2u, 0, (frames - produced) * 2u * sizeof(int16_t));

        const size_t consumed = static_cast<size_t>(s_readPos);
        s_readPos -= static_cast<double>(consumed);
        g_streamRing.readFrame.store(read + consumed, std::memory_order_release);
        g_ps2AudioRingDepth.store(static_cast<uint32_t>(write - (read + consumed)), std::memory_order_relaxed);
    }
}

PS2AudioBackend::PS2AudioBackend() : m_impl(std::make_unique<Impl>())
{
}

PS2AudioBackend::~PS2AudioBackend()
{
    if (m_impl)
        stopAll();
}

void PS2AudioBackend::setAudioReady(bool ready)
{
    if (ready == m_audioReady)
        return;
    if (ready)
    {
        m_impl->stream = LoadAudioStream(48000, 16, 2);
        SetAudioStreamCallback(m_impl->stream, &streamCallback);
        PlayAudioStream(m_impl->stream);
        m_impl->streamLoaded = true;
    }
    else if (m_impl->streamLoaded)
    {
        StopAudioStream(m_impl->stream);
        UnloadAudioStream(m_impl->stream);
        m_impl->streamLoaded = false;
    }
    m_audioReady = ready;
}

std::atomic<uint64_t> g_ps2AudioFramesSubmitted{0}, g_ps2AudioFramesDropped{0};

void PS2AudioBackend::submitStream(const int16_t *stereoFrames, uint32_t frameCount)
{
    if (!stereoFrames || frameCount == 0u)
        return;
    // PS2X_AUDIO_DUMP=<file>: append the raw output (48 kHz stereo s16le).
    static FILE *s_dump = []() -> FILE *
    {
        const char *path = std::getenv("PS2X_AUDIO_DUMP");
        return path ? std::fopen(path, "wb") : nullptr;
    }();
    if (s_dump)
    {
        std::fwrite(stereoFrames, sizeof(int16_t) * 2u, frameCount, s_dump);
        std::fflush(s_dump);
    }
    g_ps2AudioFramesSubmitted.fetch_add(frameCount, std::memory_order_relaxed);
    size_t write = g_streamRing.writeFrame.load(std::memory_order_relaxed);
    const size_t read = g_streamRing.readFrame.load(std::memory_order_acquire);
    size_t used = write - read;
    for (uint32_t i = 0; i < frameCount; ++i)
    {
        if (used >= StreamRing::kFrames)
        {
            // Full: drop this frame rather than stall the emulator. The newest
            // one, not the oldest - the read pointer belongs to the consumer.
            g_ps2AudioFramesDropped.fetch_add(1u, std::memory_order_relaxed);
            continue;
        }
        const size_t frame = write & StreamRing::kMask;
        g_streamRing.samples[frame * 2u] = stereoFrames[i * 2u];
        g_streamRing.samples[frame * 2u + 1u] = stereoFrames[i * 2u + 1u];
        ++write;
        ++used;
    }
    g_streamRing.writeFrame.store(write, std::memory_order_release);
}

void PS2AudioBackend::onVagTransfer(const uint8_t *rdram, uint32_t srcAddr, uint32_t sizeBytes)
{
    if (!rdram || sizeBytes < 48)
        return;

    const uint32_t physAddr = srcAddr & PS2_RAM_MASK;
    if (physAddr + sizeBytes > PS2_RAM_SIZE)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(rdram + physAddr, sizeBytes, pcm, sampleRate))
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = std::move(sample);
    m_mostRecentSampleKey = physAddr;
}

void PS2AudioBackend::onVagTransferFromBuffer(const uint8_t *data, uint32_t sizeBytes, uint32_t keyAddr)
{
    if (!data || sizeBytes < 48)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(data, sizeBytes, pcm, sampleRate))
        return;

    const uint32_t physAddr = keyAddr & PS2_RAM_MASK;
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = sample;
    m_mostRecentSampleKey = physAddr;
    m_loadOrderSamples.push_back(std::move(sample));
    m_loadOrderSampleKeys.push_back(physAddr);
    constexpr size_t kMaxLoadOrderSamples = 32;
    if (m_loadOrderSamples.size() > kMaxLoadOrderSamples)
    {
        m_loadOrderSamples.erase(m_loadOrderSamples.begin());
        m_loadOrderSampleKeys.erase(m_loadOrderSampleKeys.begin());
    }
}

namespace
{
    constexpr uint32_t LIBSD_CMD_SET_VOICE = 0x8010u;
}

void PS2AudioBackend::onSoundCommand(uint32_t sid, uint32_t rpcNum,
                                     const uint8_t *sendBuf, uint32_t sendSize,
                                     uint8_t *recvBuf, uint32_t recvSize)
{
    if (sid != 0x80000701u)
        return;

    if ((rpcNum == LIBSD_CMD_SET_VOICE || (rpcNum & 0xFF00u) == 0x8100u) &&
        sendBuf && sendSize >= 20)
    {
        uint32_t sampleAddr = 0;
        uint32_t voiceIndex = 0xFFFFFFFFu;
        for (int vo = 4; vo >= 0 && voiceIndex == 0xFFFFFFFFu; vo -= 4)
        {
            if (vo < static_cast<int>(sendSize))
            {
                uint32_t v = 0;
                std::memcpy(&v, sendBuf + vo, sizeof(v));
                if (v < 24u)
                    voiceIndex = v;
            }
        }

        constexpr uint32_t kMinPlausibleAddr = 0x1000u;
        for (int off = 12; off <= 24 && sampleAddr == 0; off += 4)
        {
            if (sendSize >= static_cast<uint32_t>(off + 4))
            {
                uint32_t cand = 0;
                std::memcpy(&cand, sendBuf + off, sizeof(cand));
                if (cand >= kMinPlausibleAddr && (cand <= PS2_RAM_MASK || (cand & ~PS2_RAM_MASK) == 0))
                    sampleAddr = cand;
            }
        }
        if (sampleAddr == 0)
            sampleAddr = m_mostRecentSampleKey;

        float pitch = 1.0f;
        if (sendSize >= 12)
        {
            uint16_t pitchHalf = 0;
            std::memcpy(&pitchHalf, sendBuf + 8, sizeof(pitchHalf));
            if (pitchHalf != 0)
                pitch = 4096.0f / static_cast<float>(pitchHalf);
        }
        play(sampleAddr, pitch, 1.0f, voiceIndex);
    }
}

void PS2AudioBackend::play(uint32_t sampleAddr, float pitch, float volume, uint32_t voiceIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample *sampleToPlay = nullptr;
    uint32_t sampleKey = 0;

    auto it = m_sampleBank.find(sampleAddr & PS2_RAM_MASK);
    if (it != m_sampleBank.end())
    {
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    else if (voiceIndex != 0xFFFFFFFFu &&
             voiceIndex < m_loadOrderSamples.size() &&
             voiceIndex < m_loadOrderSampleKeys.size())
    {
        sampleToPlay = &m_loadOrderSamples[voiceIndex];
        sampleKey = m_loadOrderSampleKeys[voiceIndex];
    }
    else
    {
        it = m_sampleBank.find(m_mostRecentSampleKey);
        if (it == m_sampleBank.end())
            return;
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    if (!sampleToPlay || sampleToPlay->pcm.empty())
        return;

    const bool isBgm = (sampleToPlay->pcm.size() > static_cast<size_t>(sampleToPlay->sampleRate * 5));
    playDecodedSample(sampleKey, *sampleToPlay, pitch, volume, isBgm);
}

void PS2AudioBackend::pruneFinishedSounds()
{
#if defined(PLATFORM_VITA)
    return;
#else
    auto &sounds = m_impl->activeSounds;
    auto it = sounds.begin();
    while (it != sounds.end())
    {
        if (!IsSoundPlaying(it->snd))
        {
            UnloadSound(it->snd);
            it = sounds.erase(it);
        }
        else
        {
            ++it;
        }
    }
#endif
}

void PS2AudioBackend::playDecodedSample(uint32_t sampleKey, DecodedSample &sample, float pitch, float volume,
                                        bool isBgm)
{
#if defined(PLATFORM_VITA)
    (void)sampleKey;
    (void)sample;
    (void)pitch;
    (void)volume;
    (void)isBgm;
    return;
#else
    if (!m_audioReady || sample.pcm.empty())
        return;

    pruneFinishedSounds();

    for (const auto &t : m_impl->activeSounds)
    {
        if (t.sampleKey == sampleKey && IsSoundPlaying(t.snd))
            return;
    }

    auto &sounds = m_impl->activeSounds;
    if (isBgm)
    {
        for (auto it = sounds.begin(); it != sounds.end();)
        {
            if (IsSoundPlaying(it->snd))
            {
                StopSound(it->snd);
                UnloadSound(it->snd);
                it = sounds.erase(it);
            }
            else
                ++it;
        }
    }

    constexpr int kMaxConcurrentSounds = 4;
    while (static_cast<int>(sounds.size()) >= kMaxConcurrentSounds)
    {
        StopSound(sounds.front().snd);
        UnloadSound(sounds.front().snd);
        sounds.erase(sounds.begin());
    }

    std::vector<uint8_t> wav = buildWavFromPcm(sample.pcm.data(), sample.pcm.size(), sample.sampleRate);
    Wave wave = LoadWaveFromMemory(".wav", wav.data(), static_cast<int>(wav.size()));
    if (wave.frameCount <= 0)
        return;
    Sound snd = LoadSoundFromWave(wave);
    UnloadWave(wave);
    SetSoundPitch(snd, pitch);
    SetSoundVolume(snd, volume);
    m_impl->activeSounds.push_back({snd, sampleKey});
    PlaySound(snd);
#endif
}

void PS2AudioBackend::stop(uint32_t voiceId)
{
    (void)voiceId;
}

void PS2AudioBackend::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
#if defined(PLATFORM_VITA)
    return;
#else
    for (auto &t : m_impl->activeSounds)
    {
        StopSound(t.snd);
        UnloadSound(t.snd);
    }
    m_impl->activeSounds.clear();
#endif
}
