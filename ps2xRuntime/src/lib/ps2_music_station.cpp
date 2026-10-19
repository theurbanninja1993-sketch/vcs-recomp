#include "runtime/ps2_music_station.h"
#include "runtime/ps2_host_settings.h"
#include "runtime/ps2_mods.h"

#include "raylib.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <atomic>
#include <iostream>
#include <random>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    Ps2MusicStationQuery g_query = nullptr;
    std::vector<std::string> g_tracks;
    bool g_scanned = false;
    size_t g_index = 0;
    Music g_music{};
    bool g_loaded = false;
    bool g_playing = false;
    // The runtime's own radio on/off switch (D-pad Down held): set by the pad
    // backend, read by the music update, the game query and the overlay.
    std::atomic<bool> g_radioOff{false};
    bool g_skip = false;
    bool g_prev = false;
    std::string g_title;
    double g_startedAt = -100.0;
    bool g_active = false;
    double g_activeSince = -100.0;

    bool isAudioFile(const fs::path &path)
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".mp3" || ext == ".ogg" || ext == ".flac" || ext == ".wav";
    }

    void unload()
    {
        if (g_loaded)
        {
            StopMusicStream(g_music);
            UnloadMusicStream(g_music);
        }
        g_loaded = false;
        g_playing = false;
    }

    void scan()
    {
        unload();
        g_tracks.clear();
        std::error_code ec;
        const fs::path dir = ps2MusicStationDirectory();
        fs::create_directories(dir, ec);
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
                break;
            if (it->is_regular_file(ec) && isAudioFile(it->path()))
                g_tracks.push_back(it->path().string());
        }
        std::sort(g_tracks.begin(), g_tracks.end());
        if (ps2HostSettings().musicShuffle)
            std::shuffle(g_tracks.begin(), g_tracks.end(), std::mt19937(std::random_device{}()));
        g_index = 0;
        g_scanned = true;
        std::cerr << "[music] " << g_tracks.size() << " tracks in " << dir.string() << std::endl;
    }

    // Loads the track at g_index (skipping files raylib can't decode).
    bool loadCurrent()
    {
        unload();
        for (size_t tries = 0; tries < g_tracks.size(); ++tries)
        {
            const std::string &path = g_tracks[g_index % g_tracks.size()];
            g_music = LoadMusicStream(path.c_str());
            if (IsMusicValid(g_music))
            {
                g_music.looping = false;
                g_loaded = true;
                g_title = fs::path(path).stem().string();
                return true;
            }
            std::cerr << "[music] can't play " << path << std::endl;
            g_index = (g_index + 1) % g_tracks.size();
        }
        return false;
    }
}

void ps2MusicStationSetQuery(Ps2MusicStationQuery query)
{
    g_query = query;
}

bool ps2MusicStationSupported()
{
    return g_query != nullptr;
}

const char *ps2MusicStationName()
{
    // The name the game itself uses for this slot (GXT key FEA_CSS,
    // "CUSTOM SOUNDTRACKS:"), left over from the PC/PSP custom-tracks feature.
    return "Custom Soundtracks";
}

std::string ps2MusicStationDirectory()
{
    if (const char *env = std::getenv("PS2X_MUSIC_DIR"); env && *env)
        return env;
    return (fs::path(ps2ExecutableDirectoryPath()) / "music").string();
}

size_t ps2MusicStationTrackCount()
{
    return g_tracks.size();
}

std::string ps2MusicStationNowPlaying(double &startedAt)
{
    startedAt = g_startedAt;
    return g_title;
}

bool ps2MusicStationActive()
{
    return g_active;
}

double ps2MusicStationActiveSince()
{
    return g_activeSince;
}

std::string ps2MusicStationLogoPath()
{
    if (const char *env = std::getenv("PS2X_CUSTOM_LOGO"); env && *env)
        return env;
    return (fs::path(ps2ExecutableDirectoryPath()) / "custom_logo.png").string();
}

void ps2MusicStationNextTrack()
{
    g_skip = true;
}

void ps2MusicStationPrevTrack()
{
    g_prev = true;
}

void ps2MusicStationRescan()
{
    g_scanned = false;
}

void ps2MusicStationSetRadioOff(bool off)
{
    g_radioOff.store(off, std::memory_order_relaxed);
}

bool ps2MusicStationRadioOff()
{
    return g_radioOff.load(std::memory_order_relaxed);
}

void ps2MusicStationUpdate(const uint8_t *rdram)
{
    const PS2HostSettings &settings = ps2HostSettings();
    if (!g_query || !IsAudioDeviceReady())
        return;
    if (!g_scanned)
        scan();
    const bool active = settings.musicStation && !g_tracks.empty() && rdram && g_query(rdram) &&
                        !g_radioOff.load(std::memory_order_relaxed) &&
                        !g_ps2SettingsMenuOpen.load(std::memory_order_relaxed);
    if (active != g_active)
    {
        g_active = active;
        if (active)
            g_activeSince = GetTime();
    }
    if (!active)
    {
        if (g_playing)
        {
            PauseMusicStream(g_music); // resumes where it left off
            g_playing = false;
            std::cerr << "[music] paused" << std::endl;
        }
        g_skip = false;
        g_prev = false;
        return;
    }
    const bool ended = g_loaded && g_playing && !IsMusicStreamPlaying(g_music);
    if (!g_loaded || ended || g_skip || g_prev)
    {
        if (g_loaded)
        {
            // Next / previous track (D-pad Right / Left on the controller,
            // F10 on the keyboard for next).
            if (g_prev)
                g_index = (g_index + g_tracks.size() - 1) % g_tracks.size();
            else
                g_index = (g_index + 1) % g_tracks.size();
        }
        g_skip = false;
        g_prev = false;
        if (!loadCurrent())
            return;
        PlayMusicStream(g_music);
        g_playing = true;
        g_startedAt = GetTime();
        std::cerr << "[music] playing " << g_title << std::endl;
    }
    else if (!g_playing)
    {
        ResumeMusicStream(g_music);
        g_playing = true;
        g_startedAt = GetTime(); // show the title again when tuning back in
        std::cerr << "[music] resumed" << std::endl;
    }
    SetMusicVolume(g_music, std::clamp(settings.musicStationVolume, 0.0f, 1.0f));
    UpdateMusicStream(g_music);
}
