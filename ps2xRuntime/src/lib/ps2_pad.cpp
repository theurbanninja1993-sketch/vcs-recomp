#include <GLFW/glfw3.h>
#include "runtime/ps2_pad.h"
#include "runtime/ps2_platform.h"
#include "runtime/ps2_mods.h"
#include "runtime/ps2_prompt_art.h"

extern std::atomic<bool> g_ps2DumpRamRequest; // ps2_runtime.cpp
#include "runtime/ps2_host_settings.h"
#include "runtime/ps2_music_station.h"
#include "runtime/ps2_audio.h"
#include "ps2_host_backend.h"
#include <atomic>
#include <mutex>
#include <set>
#include <thread>

extern std::atomic<uint64_t> g_ps2PresentedFrameCounter;
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>
#if defined(__linux__)
// Reading a device's capabilities to derive a gamepad mapping (ps2BuildEvdevMapping).
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

// GLFW's gamepad mappings (from SDL's database, generated into mappings.h at
// build time) only carry the device revisions it knew about. A pad whose
// firmware reports a different revision matches nothing: it is detected, but
// every button and axis reads zero. GLFW can be handed extra mappings at
// runtime, which is how those are fixed without rebuilding.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

// GLFW's gamepad mappings (from SDL's database, generated into mappings.h at
// build time) only carry the device revisions it knew about. A pad whose
// firmware reports a different revision matches nothing: it is detected, but
// every button and axis reads zero. GLFW can be handed extra mappings at
// runtime, which is how those are fixed without rebuilding.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

}

namespace
{
    // Select+Start together opens the settings menu, so neither reaches the
    // game as part of the combo: a lone Select / Start is passed on once it
    // has been held 50 ms (a shorter tap is replayed for 100 ms on release).
    void filterMenuCombo(bool &select, bool &start)
    {
        struct Held
        {
            double since = -1.0;    // pressed at (not yet passed on)
            bool passed = false;    // passed on to the game
            double pulseUntil = -1.0;
        };
        static Held s_held[2];
        static bool s_combo = false;
        const double now = GetTime();
        bool *buttons[2] = {&select, &start};
        if (select && start)
            s_combo = true;
        if (s_combo)
        {
            if (!select && !start)
                s_combo = false;
            for (Held &held : s_held)
                held = {};
            select = start = false;
            return;
        }
        constexpr double kGrace = 0.05, kPulse = 0.1;
        for (int i = 0; i < 2; ++i)
        {
            Held &held = s_held[i];
            bool &down = *buttons[i];
            if (down)
            {
                if (held.since < 0.0)
                    held.since = now;
                held.passed = held.passed || now - held.since >= kGrace;
                down = held.passed;
            }
            else
            {
                if (held.since >= 0.0 && !held.passed)
                    held.pulseUntil = now + kPulse; // a quick tap
                held.since = -1.0;
                held.passed = false;
                down = now < held.pulseUntil;
            }
        }
    }
}

namespace
{
    std::mutex g_preferredPadMutex;
    std::string g_preferredPad;
}

void ps2SetPreferredGamepad(const std::string &name)
{
    std::lock_guard lock(g_preferredPadMutex);
    g_preferredPad = name;
}

// The live trigger positions, for the game-side driving hook (game_overrides.cpp).
std::atomic<float> g_ps2PadTriggerLeft{0.0f};
std::atomic<float> g_ps2PadTriggerRight{0.0f};

// Adds mappings GLFW does not ship, plus an optional user file. Must run with
// GLFW initialised - raylib does that in InitWindow - because
// glfwUpdateGamepadMappings only re-evaluates already-connected joysticks and
// returns GLFW_FALSE while GLFW is down. It is called again on every
// ps2ActiveGamepad() call until it takes, and explicitly right after
// InitWindow, because prompt setup runs earlier during boot.
void ps2ApplyGamepadMappings()
{
    static bool s_done = false;
    if (s_done)
        return;
    // Pads GLFW's database does not carry, or carries with the wrong layout.
    // One mapping per line: GLFW splits on newlines, so two entries sharing a
    // line are parsed as a single (wrong) mapping.
    //
    // Both Xbox pads below report a0/a1 as the left stick, a2/a3 as the right
    // stick, a4/a5 as the triggers and the d-pad as a hat (verified against the
    // devices). The database's nearest sibling for product 0x02ea puts the left
    // trigger on a2, which silently mis-maps the right stick and the triggers.
    static const char *kSupplement =
        // Xbox One S pad over USB reporting firmware revision 0x0517: the
        // database has 0x0408 / 0x0301 / 0x0000 for product 0x02ea on Linux.
        "030000005e040000ea02000017050000,Microsoft X-Box One S pad,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,"
        "dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b5,"
        "rightstick:b10,righttrigger:a4,rightx:a2,righty:a3,start:b7,x:b2,y:b3,platform:Linux,\n"
        // The same pad reporting no revision at all.
        "030000005e040000ea02000000000000,Microsoft X-Box One S pad,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,"
        "dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a5,leftx:a0,lefty:a1,rightshoulder:b5,"
        "rightstick:b10,righttrigger:a4,rightx:a2,righty:a3,start:b7,x:b2,y:b3,platform:Linux,\n"
        // Xbox Wireless Controller over Bluetooth (product 0x0b20, version
        // 0x0523), and the same device with a zeroed version field as some GLFW
        // builds report it. This driver reports extra buttons (BTN_C, BTN_Z,
        // BTN_TL2, BTN_TR2), and GLFW numbers raw buttons by ascending evdev
        // code, so every index after B is shifted: b3/b4 are X/Y, b6/b7 are
        // LB/RB, b10/b11 are Back/Start, b13/b14 are L3/R3. The right stick is
        // Z/RZ and the triggers are GAS (right) and BRAKE (left). Verified
        // against the device.
"050000005e040000200b000023050000,Xbox Wireless Controller,a:b0,b:b1,x:b3,y:b4,back:b10,start:b11,guide:b12,"
        "leftshoulder:b6,rightshoulder:b7,leftstick:b13,rightstick:b14,lefttrigger:a5,righttrigger:a4,leftx:a0,"
        "lefty:a1,rightx:a2,righty:a3,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,platform:Linux,\n"
        "050000005e040000200b000000000000,Xbox Wireless Controller,a:b0,b:b1,x:b3,y:b4,back:b10,start:b11,guide:b12,"
        "leftshoulder:b6,rightshoulder:b7,leftstick:b13,rightstick:b14,lefttrigger:a5,righttrigger:a4,leftx:a0,"
        "lefty:a1,rightx:a2,righty:a3,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,platform:Linux,\n";
    // A false return means GLFW is not up yet (prompt setup runs before
    // InitWindow), so do not latch - retry on the next call.
    if (!glfwUpdateGamepadMappings(kSupplement))
        return;
    s_done = true;
    std::fprintf(stderr, "[pad] extra gamepad mappings loaded\n");
    // <config>/ps2recomp/gamecontrollerdb.txt: SDL-style lines to drop in for
    // pads GLFW does not know (any entry whose guid matches a connected pad).
    std::error_code error;
    const std::filesystem::path userFile =
        std::filesystem::path(ps2HostSettingsPath()).parent_path() / "gamecontrollerdb.txt";
    if (std::filesystem::exists(userFile, error))
    {
        if (FILE *f = std::fopen(userFile.string().c_str(), "rb"))
        {
            std::string text;
            char buffer[4096];
            size_t read = 0;
            while ((read = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
                text.append(buffer, read);
            std::fclose(f);
            if (!text.empty())
                std::fprintf(stderr, "[pad] %s %s\n", userFile.string().c_str(),
                             glfwUpdateGamepadMappings(text.c_str()) ? "loaded" : "rejected");
        }
    }
}

// What the four joystick slots hold, refreshed at most a few times a second.
struct PadSnapshot
{
    bool present[4]{};
    bool mapped[4]{}; // GLFW recognises it as a gamepad
    std::string name[4];        // the raw device name (raylib's GetGamepadName)
    std::string gamepadName[4]; // GLFW's database name, when mapped (semantic:
                                // a DualShock 4's raw name is just "Wireless
                                // Controller", its database name is "PS4 Controller")
    std::string guid[4];        // bus/vendor/product/version, for the vendor check
};

// Probing the bus is not free: GLFW's gamepad queries drain the device's input
// queue and touch /dev/input, and the game reads the pad from the emulated game
// thread while raylib polls from the host thread. Refresh on a timer and answer
// from the snapshot in between - that keeps the per-call cost to a few loads and
// limits the cross-thread GLFW traffic to a handful of calls a second. The copy
// is returned by value: two threads read this, and the names must not be shared.
static PadSnapshot padSnapshot()
{
    static std::mutex s_mutex;
    static PadSnapshot s_snapshot;
    static std::chrono::steady_clock::time_point s_next{};
    std::lock_guard lock(s_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now >= s_next)
    {
        s_next = now + std::chrono::milliseconds(250);
        for (int i = 0; i < 4; ++i)
        {
            const bool present = IsGamepadAvailable(i);
            s_snapshot.present[i] = present;
            s_snapshot.mapped[i] = present && glfwJoystickIsGamepad(i) != 0;
            const char *name = present ? GetGamepadName(i) : nullptr;
            s_snapshot.name[i] = name != nullptr ? name : "";
            const char *gamepadName = present ? glfwGetGamepadName(i) : nullptr;
            s_snapshot.gamepadName[i] = gamepadName != nullptr ? gamepadName : "";
            const char *guid = present ? glfwGetJoystickGUID(i) : nullptr;
            s_snapshot.guid[i] = guid != nullptr ? guid : "";
        }
    }
    return s_snapshot;
}

// A device the game can actually read: present *and* recognised by GLFW's
// gamepad database. Keyboards and mice often expose phantom joystick nodes (the
// owner's Razer keyboard takes two slots) and those must never be auto-selected:
// they report no input at all, which looks exactly like a controller that is
// detected but dead.
bool ps2IsRealGamepad(int pad)
{
    return pad >= 0 && pad < 4 && padSnapshot().mapped[pad];
}

#if defined(__linux__)
// GLFW numbers a joystick's raw buttons and axes by walking the device's evdev
// codes in ascending order (see its openJoystickDevice), so the device's own
// capability bitmap - not its name - is what a mapping has to be written
// against. Two pads with the same buttons can report different codes (the
// Bluetooth Xbox pad reports BTN_C, BTN_Z, BTN_TL2 and BTN_TR2, which shifts
// every index after B), so deriving the mapping from the codes is the only way
// to be right without hard-coding each device.
static bool ps2BuildEvdevMapping(const char *guid, const std::string &name, std::string &out)
{
    static int s_reports = 0;
    const bool trace = ps2PadDebugEnabled() && s_reports < 8;
    auto fail = [&](const char *why) -> bool
    {
        if (trace)
        {
            ++s_reports;
            std::fprintf(stderr, "[pad] no mapping derived for '%s': %s\n", name.c_str(), why);
        }
        return false;
    };
    if (guid == nullptr || std::strlen(guid) != 32)
        return fail("no guid");
    unsigned bytes[16]{};
    for (int i = 0; i < 16; ++i)
        if (std::sscanf(guid + i * 2, "%2x", &bytes[i]) != 1)
            return false;
    const unsigned bus = bytes[0] | (bytes[1] << 8);
    const unsigned vendor = bytes[4] | (bytes[5] << 8);
    const unsigned product = bytes[8] | (bytes[9] << 8);
    const unsigned version = bytes[12] | (bytes[13] << 8);

    DIR *dir = opendir("/dev/input");
    if (dir == nullptr)
        return fail("cannot list /dev/input");
    std::vector<std::string> paths;
    for (dirent *entry = readdir(dir); entry != nullptr; entry = readdir(dir))
        if (std::strncmp(entry->d_name, "event", 5) == 0)
            paths.push_back(std::string("/dev/input/") + entry->d_name);
    closedir(dir);

    bool sawNonGamepad = false;
    for (const std::string &path : paths)
    {
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        struct input_id id{};
        unsigned char keyBits[(KEY_CNT + 7) / 8]{};
        unsigned char absBits[(ABS_CNT + 7) / 8]{};
        // EVIOCGID returns 0, but EVIOCGBIT returns the number of bytes copied
        // (96 for the key bitmap), so a "== 0" test rejects every device.
        const bool ok = ioctl(fd, EVIOCGID, &id) == 0 &&
                        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) >= 0 &&
                        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) >= 0;
        ::close(fd);
        if (!ok || id.bustype != bus || id.vendor != vendor || id.product != product || id.version != version)
            continue;

        const auto has = [](const unsigned char *bits, int code)
        { return (bits[code / 8] & (1 << (code % 8))) != 0; };
        // Only gamepads: sticks plus a face button. A keyboard or mouse posing as
        // a joystick has none of these and must never be mapped.
        if (!has(absBits, ABS_X) || !has(absBits, ABS_Y) ||
            (!has(keyBits, BTN_SOUTH) && !has(keyBits, BTN_GAMEPAD)))
        {
            // A keyboard or mouse with the same ids (a gaming keyboard's phantom
            // joystick node) - never map it. Another node may still be the pad.
            sawNonGamepad = true;
            continue;
        }

        const bool triggerAxes = has(absBits, ABS_GAS) || has(absBits, ABS_BRAKE);
        const bool rightStickZr = has(absBits, ABS_Z) || has(absBits, ABS_RZ);

        std::string map = std::string(guid) + "," + name + ",";
        int index = 0;
        for (int code = BTN_MISC; code < KEY_CNT; ++code)
        {
            if (!has(keyBits, code))
                continue;
            const char *element = nullptr;
            switch (code)
            {
                case BTN_SOUTH: element = "a"; break;
                case BTN_EAST: element = "b"; break;
                case BTN_NORTH: element = "x"; break;
                case BTN_WEST: element = "y"; break;
                case BTN_TL: element = "leftshoulder"; break;
                case BTN_TR: element = "rightshoulder"; break;
                // The triggers as buttons, only when there are no trigger axes.
                case BTN_TL2: element = triggerAxes ? nullptr : "lefttrigger"; break;
                case BTN_TR2: element = triggerAxes ? nullptr : "righttrigger"; break;
                case BTN_SELECT: element = "back"; break;
                case BTN_START: element = "start"; break;
                case BTN_MODE: element = "guide"; break;
                case BTN_THUMBL: element = "leftstick"; break;
                case BTN_THUMBR: element = "rightstick"; break;
                case BTN_DPAD_UP: element = "dpup"; break;
                case BTN_DPAD_DOWN: element = "dpdown"; break;
                case BTN_DPAD_LEFT: element = "dpleft"; break;
                case BTN_DPAD_RIGHT: element = "dpright"; break;
                default: break; // BTN_C, BTN_Z and friends have no SDL name
            }
            if (element != nullptr)
                map += std::string(element) + ":b" + std::to_string(index) + ",";
            ++index;
        }

        // The kernel's gamepad convention: X/Y the left stick, Z/RZ the right
        // stick, GAS the right trigger and BRAKE the left; pads that report the
        // right stick as RX/RY are handled too.
        int axis = 0;
        bool hat = false;
        for (int code = 0; code < ABS_CNT; ++code)
        {
            if (!has(absBits, code))
                continue;
            if (code >= ABS_HAT0X && code <= ABS_HAT3Y)
            {
                hat = true;
                ++code; // the Y half of the hat pair
                continue;
            }
            const char *element = nullptr;
            switch (code)
            {
                case ABS_X: element = "leftx"; break;
                case ABS_Y: element = "lefty"; break;
                case ABS_Z: element = "rightx"; break;
                case ABS_RZ: element = "righty"; break;
                case ABS_RX: element = rightStickZr ? nullptr : "rightx"; break;
                case ABS_RY: element = rightStickZr ? nullptr : "righty"; break;
                case ABS_GAS: element = "righttrigger"; break;
                case ABS_BRAKE: element = "lefttrigger"; break;
                default: break;
            }
            if (element != nullptr)
                map += std::string(element) + ":a" + std::to_string(axis) + ",";
            ++axis;
        }
        if (hat)
            map += "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,";
        map += "platform:Linux,";
        out = map;
        return true;
    }
    return fail(sawNonGamepad ? "the matching device is not a gamepad" : "no evdev node with a matching id");
}
#endif

// A pad GLFW has no mapping for is not a gamepad at all: it gets ignored, and
// the port silently reads whichever other device is connected. That is not an
// exotic case - the same controller over USB and over Bluetooth are two
// different GUIDs (bus, product and firmware revision all feed into it), and a
// firmware update changes the revision bytes. Derive a mapping from the device
// itself so it works whatever it reports.
static void ps2MapUnrecognisedPads()
{
#if defined(__linux__)
    // Host thread only. Deriving a mapping walks /dev/input and ioctls every
    // device, which is far too slow to run from the emulated game thread: it
    // stalled the game for a good fraction of a second, which showed up as
    // frame hitches and as the sound engine's audio arriving in bursts.
    if (!ps2IsHostThread())
        return;
    const PadSnapshot snapshot = padSnapshot();
    // A device GLFW has no mapping for is usually not a gamepad at all - a
    // keyboard or mouse exposing a phantom joystick node - and deriving one for
    // those means opening and ioctl'ing the whole input tree for nothing. Only
    // look at names that name a pad family, and only ever try a given GUID once.
    static std::mutex s_mutex;
    static std::set<std::string> s_attempted;
    std::string generated;
    {
        std::lock_guard lock(s_mutex);
        for (int i = 0; i < 4; ++i)
        {
            if (!snapshot.present[i] || snapshot.mapped[i] || snapshot.name[i].empty())
                continue;
            std::string lower = snapshot.name[i];
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            static const char *const kFamilies[] = {"xbox", "x-box", "dualsense", "dualshock",
                                                    "playstation", "ps4", "ps5", "switch",
                                                    "pro controller", "steam", "8bitdo", "gamepad"};
            bool family = false;
            for (const char *needle : kFamilies)
                family = family || lower.find(needle) != std::string::npos;
            if (!family)
                continue;
            const char *guid = glfwGetJoystickGUID(i);
            if (guid == nullptr || s_attempted.count(guid) != 0u)
                continue;
            s_attempted.insert(guid);
            std::string mapping;
            if (ps2BuildEvdevMapping(guid, snapshot.name[i], mapping))
                generated += mapping + "\n";
        }
    }
    if (generated.empty())
        return;
    if (glfwUpdateGamepadMappings(generated.c_str()))
        std::fprintf(stderr, "[pad] derived mapping(s) from the device:\n%s", generated.c_str());
#endif
}

int ps2ActiveGamepad()
{
    // Called on every pad read (several times a frame) and again from the host
    // frame loop, and the body below copies a snapshot holding twelve strings
    // and queries GLFW for up to four pads. The answer only changes when the
    // player actually touches something, so compute it once per presented frame
    // and let the rest of that frame's calls be an atomic load. Profiled at
    // ~4.5% of the game thread before this (mostly malloc/free from the
    // snapshot copy), which is a lot for a question with a one-frame answer.
    extern std::atomic<uint64_t> g_ps2PresentedFrameCounter;
    static std::atomic<uint64_t> s_cachedFrame{~0ull};
    static std::atomic<int> s_cachedValue{-1};
    const uint64_t frame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
    if (s_cachedFrame.load(std::memory_order_relaxed) == frame)
        return s_cachedValue.load(std::memory_order_relaxed);

    ps2ApplyGamepadMappings();
    const PadSnapshot snapshot = padSnapshot();
    int active = -1;
    {
        std::lock_guard lock(g_preferredPadMutex);
        if (!g_preferredPad.empty())
        {
            // The pinned name first among real gamepads, then among any joystick
            // (an unrecognised pad is honoured, but say so: it will not respond
            // until it has a mapping, and saying it every call would flood).
            for (int i = 0; i < 4 && active < 0; ++i)
                if (snapshot.mapped[i] && snapshot.name[i] == g_preferredPad)
                    active = i;
            if (active < 0)
            {
                for (int i = 0; i < 4 && active < 0; ++i)
                    if (snapshot.present[i] && snapshot.name[i] == g_preferredPad)
                        active = i;
                static std::string s_warned;
                if (active >= 0 && s_warned != g_preferredPad)
                {
                    s_warned = g_preferredPad;
                    std::fprintf(stderr,
                                 "[pad] '%s' is connected but GLFW has no mapping for it; "
                                 "it will not respond\n",
                                 g_preferredPad.c_str());
                }
            }
        }
    }
    if (active < 0)
    {
        // Automatic: the controller last used. Only real gamepads count, so a
        // keyboard or mouse can never win a slot.
        static std::atomic<int> s_auto{-1};
        // Sticks count as used when moved from where they rested when first
        // seen (some devices rest at -1).
        static float s_rest[4][4];
        static bool s_restKnown[4]{};
        auto used = [](int pad)
        {
            if (!s_restKnown[pad])
            {
                for (int axis = 0; axis < 4; ++axis)
                    s_rest[pad][axis] = GetGamepadAxisMovement(pad, axis);
                s_restKnown[pad] = true;
            }
            for (int button = GAMEPAD_BUTTON_LEFT_FACE_UP; button <= GAMEPAD_BUTTON_RIGHT_THUMB; ++button)
                if (IsGamepadButtonDown(pad, button))
                    return true;
            for (int axis = 0; axis < 4; ++axis)
                if (std::fabs(GetGamepadAxisMovement(pad, axis) - s_rest[pad][axis]) > 0.5f)
                    return true;
            return false;
        };
        int current = s_auto.load(std::memory_order_relaxed);
        if (current < 0 || !snapshot.mapped[current] || !used(current))
            for (int i = 0; i < 4; ++i)
                if (i != current && snapshot.mapped[i] && used(i))
                {
                    current = i;
                    break;
                }
        if (current >= 0 && !snapshot.mapped[current])
            current = -1;
        for (int i = 0; i < 4 && current < 0; ++i)
            if (snapshot.mapped[i])
                current = i;
        s_auto.store(current, std::memory_order_relaxed);
        active = current;
    }
    static std::atomic<int> s_logged{-2};
    if (s_logged.exchange(active, std::memory_order_relaxed) != active)
    {
        std::string all;
        for (int i = 0; i < 4; ++i)
            if (snapshot.present[i])
                all += " [" + std::to_string(i) + "] " + snapshot.name[i] +
                       (snapshot.mapped[i] ? "" : " (not a gamepad - ignored)");
        if (active >= 0)
            std::fprintf(stderr, "[pad] using controller %d: %s (connected:%s)\n", active,
                         snapshot.name[active].c_str(), all.c_str());
        else
            std::fprintf(stderr, "[pad] no controller (connected:%s)\n", all.empty() ? " none" : all.c_str());
        std::string line = "[pad] selection -> active=" + std::to_string(active);
        if (active >= 0)
            line += " '" + snapshot.name[active] + "'";
        line += " connected:" + all + "\n";
        ps2PadDebugLog(line.c_str());
    }
    s_cachedValue.store(active, std::memory_order_relaxed);
    s_cachedFrame.store(frame, std::memory_order_relaxed);
    return active;
}

// What a connected pad is, from its name: GLFW's own name when the pad is in
// its database, the name from our supplement otherwise ("Microsoft X-Box One
// S pad", hence the hyphenated spelling below).
// What a connected pad is. The raw device name is not enough: a DualShock 4
// reports itself as "Wireless Controller" with no PlayStation wording at all.
// Prefer GLFW's database name (the semantic one - "PS4 Controller") and fall
// back to the vendor id from the GUID.
static PS2InputDevice gamepadDeviceKind(int pad)
{
    if (pad < 0 || pad >= 4)
        return PS2InputDevice::Keyboard;
    const PadSnapshot snapshot = padSnapshot();
    const std::string &preferred = !snapshot.gamepadName[pad].empty() ? snapshot.gamepadName[pad] : snapshot.name[pad];
    std::string name = preferred;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    unsigned vendor = 0, product = 0;
    const std::string &guid = snapshot.guid[pad];
    if (guid.size() == 32)
    {
        const auto byte = [&guid](int i)
        {
            unsigned value = 0;
            std::sscanf(guid.c_str() + i * 2, "%2x", &value);
            return value;
        };
        vendor = byte(4) | (byte(5) << 8);
        product = byte(8) | (byte(9) << 8);
    }
    // Sony: 0x0ce6 is the DualSense, 0x0df2 the DualSense Edge; the rest are
    // DualShock 3/4.
    const bool sony = vendor == 0x054c;
    const bool sonyPs5 = sony && (product == 0x0ce6 || product == 0x0df2);
    if (name.find("dualsense") != std::string::npos || name.find("ps5") != std::string::npos || sonyPs5)
        return PS2InputDevice::DualSense;
    if (name.find("dualshock") != std::string::npos || name.find("playstation") != std::string::npos ||
        name.find("ps4") != std::string::npos || name.find("ps3") != std::string::npos || sony)
        return PS2InputDevice::DualShock;
    if (name.find("xbox") != std::string::npos || name.find("x-box") != std::string::npos)
        return PS2InputDevice::Xbox;
    if (name.find("steam") != std::string::npos)
        return PS2InputDevice::Steam;
    if (name.find("switch") != std::string::npos || name.find("pro controller") != std::string::npos)
        return PS2InputDevice::Switch;
    return PS2InputDevice::Generic; // e.g. 8BitDo etc.: Xbox-style ABXY layout
}

// The device the player last used is what the automatic prompts follow: the
// keyboard while typing, the pad as soon as it is touched. -1 until something
// is used (then the active pad, or the keyboard with no pad at all).
std::atomic<int> g_lastInputDevice{-1};

bool ps2PadDebugEnabled()
{
    static const bool s_enabled = std::getenv("PS2X_PAD_DEBUG") != nullptr;
    return s_enabled;
}

// Appends one line to padlog.txt in the working directory (like the F8 dumps)
// and in the config folder (~/.config/ps2recomp/padlog.txt), so a run can be
// shared without redirecting stderr, and echoes a short form to stderr.
void ps2PadDebugLog(const char *line)
{
    if (!ps2PadDebugEnabled())
        return;
    static FILE *s_file = std::fopen("padlog.txt", "a");
    static FILE *s_configFile = []() -> FILE *
    {
        const std::filesystem::path path =
            std::filesystem::path(ps2HostSettingsPath()).parent_path() / "padlog.txt";
        return std::fopen(path.string().c_str(), "a");
    }();
    for (FILE *sink : {s_file, s_configFile})
    {
        if (sink != nullptr)
        {
            std::fputs(line, sink);
            std::fflush(sink);
        }
    }
    std::fputs(line, stderr);
}

PS2InputDevice ps2InputDeviceKind()
{
    const int last = g_lastInputDevice.load(std::memory_order_relaxed);
    if (last >= 0)
        return static_cast<PS2InputDevice>(last);
    return gamepadDeviceKind(ps2ActiveGamepad());
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    // Auto prompts follow the last used device, not the first connected one:
    // any bound key / mouse button / the wheel means the keyboard, a pad
    // button press or a stick moved past half way means that pad.
    {
        const PS2HostSettings &settings = ps2HostSettings();
        const int pad = ps2ActiveGamepad();
        bool padUsed = false;
        if (pad >= 0)
        {
            for (int b = GAMEPAD_BUTTON_LEFT_FACE_UP; b <= GAMEPAD_BUTTON_LEFT_THUMB && !padUsed; ++b)
                padUsed = IsGamepadButtonPressed(pad, static_cast<GamepadButton>(b));
            for (int a = GAMEPAD_AXIS_LEFT_X; a <= GAMEPAD_AXIS_RIGHT_Y && !padUsed; ++a)
                padUsed = std::fabs(GetGamepadAxisMovement(pad, static_cast<GamepadAxis>(a))) > 0.5f;
        }
        bool keyboardUsed = false;
        int keyboardTrigger = -1; // action index*2+slot, or -2 for the mouse wheel
        for (int a = 0; a < ps2controls::kActionCount && !keyboardUsed; ++a)
        {
            for (int slot = 0; slot < 2 && !keyboardUsed; ++slot)
            {
                const int binding = settings.keys[a][slot];
                if (binding == ps2controls::kNone)
                    continue;
                const bool pressed = binding >= ps2controls::kMouseBase
                                         ? IsMouseButtonPressed(binding - ps2controls::kMouseBase)
                                         : IsKeyPressed(binding);
                if (pressed)
                {
                    keyboardUsed = true;
                    keyboardTrigger = a * 2 + slot;
                }
            }
        }
        if (!keyboardUsed && GetMouseWheelMove() != 0.0f)
        {
            keyboardUsed = true;
            keyboardTrigger = -2;
        }
        if (padUsed || keyboardUsed)
        {
            const int device = padUsed ? static_cast<int>(gamepadDeviceKind(pad))
                                       : static_cast<int>(PS2InputDevice::Keyboard);
            static const bool s_debugDevice = std::getenv("PS2X_PAD_DEBUG") != nullptr;
            static int s_lastDevice = -1;
            if (s_debugDevice && device != s_lastDevice)
            {
                if (padUsed)
                    std::fprintf(stderr, "[pad] input device -> %d (controller %d)\n", device, pad);
                else if (keyboardTrigger == -2)
                    std::fprintf(stderr, "[pad] input device -> keyboard (mouse wheel)\n");
                else
                    std::fprintf(stderr, "[pad] input device -> keyboard (action %d slot %d, binding %d)\n",
                                 keyboardTrigger / 2, keyboardTrigger % 2,
                                 settings.keys[keyboardTrigger / 2][keyboardTrigger % 2]);
                s_lastDevice = device;
            }
            g_lastInputDevice.store(device, std::memory_order_relaxed);
        }
    }

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;
    // Radio on/off: hold D-pad Down on the controller or "M" on the keyboard for
    // 0.75 s (the Mods settings toggle can disable it). Track skip: a single
    // press of D-pad Right / Left skips the music folder track (the keyboard
    // uses F10 for next). The D-pad is unmapped in-game, so neither ever
    // reaches the game.
    {
        const PS2HostSettings &settings = ps2HostSettings();
        static double s_offSince = 0.0;
        static bool s_offFired = false;
        static bool s_dpadPrev[2] = {};
        const int pad = ps2ActiveGamepad();
        const bool heldOff =
            settings.radioOffEnabled &&
            ((pad >= 0 && IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) || IsKeyDown(KEY_M));
        if (heldOff)
        {
            const double now = GetTime();
            if (s_offSince == 0.0)
                s_offSince = now;
            else if (!s_offFired && now - s_offSince >= 0.75)
            {
                s_offFired = true;
                const bool off = !ps2MusicStationRadioOff();
                ps2MusicStationSetRadioOff(off);
                // The game is re-tuned to its silent slot by vcsFrameUpdate once,
                // on the first frame of the off state (see game_overrides.cpp).
                std::fprintf(stderr, "[pad] radio %s\n", off ? "off" : "on");
            }
        }
        else
        {
            s_offSince = 0.0;
            s_offFired = false;
        }
        if (pad >= 0)
        {
            const bool dpadNext = IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT);
            const bool dpadPrev = IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_LEFT);
            if (dpadNext && !s_dpadPrev[0])
                ps2MusicStationNextTrack();
            if (dpadPrev && !s_dpadPrev[1])
                ps2MusicStationPrevTrack();
            s_dpadPrev[0] = dpadNext;
            s_dpadPrev[1] = dpadPrev;
        }
        else
        {
            s_dpadPrev[0] = s_dpadPrev[1] = false;
        }
    }

    // Controller buttons still held when the menu closes (Circle / Options
    // closing it) stay with the menu until released.
    static bool s_padHeldByMenu = false;
    if (g_ps2SettingsMenuOpen.load(std::memory_order_relaxed))
    {
        s_padHeldByMenu = true;
        return true; // the settings menu has the keyboard, mouse and controller
    }

    uint16_t btns = 0xFFFFu;
    // Analog pressure per PS2 button, indexed by its bit (Cross is 14, Square
    // 15, ...). Filled from the analog triggers, which stand in for the
    // pressure-sensitive face buttons the DualShock 2 had.
    uint8_t pressure[16]{};
    // Full trigger travel maps to this raw pressure. The game reads the byte,
    // multiplies it by 4 and clamps at 255 (FUN_002843d8 -> func_284370), so a
    // raw 0..255 saturates at a quarter of the travel and the throttle feels
    // like a switch. 64 puts the game's whole range across the whole travel.
    static const float s_pressureMax = []()
    {
        const char *env = std::getenv("PS2X_PAD_PRESSURE_MAX");
        const float value = env != nullptr ? std::strtof(env, nullptr) : 64.0f;
        return std::clamp(value, 8.0f, 255.0f);
    }();
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };
    auto press = [&clearBit](int ps2Button)
    {
        if (ps2Button >= 0 && ps2Button < ps2controls::kPs2ButtonCount)
            clearBit(static_cast<uint16_t>(1u << ps2Button));
    };

    const bool driving = m_vehicleProbe && m_rdram && m_vehicleProbe(m_rdram);
    const bool inMenu = m_menuProbe && m_menuRdram && m_menuProbe(m_menuRdram);
    static const bool s_debug = std::getenv("PS2X_PAD_DEBUG") != nullptr;
    static bool s_lastDriving = false, s_lastMenu = false;
    if (s_debug && (driving != s_lastDriving || inMenu != s_lastMenu))
    {
        s_lastDriving = driving;
        s_lastMenu = inMenu;
        std::fprintf(stderr, "[pad] driving=%d menu=%d\n", driving ? 1 : 0, inMenu ? 1 : 0);
    }
    const PS2HostSettings &settings = ps2HostSettings();
    // The game's pause menu / map uses its own fixed layout (classic PS2 by
    // default): gameplay remaps never change how the menus navigate, and the
    // menu layout never leaks into gameplay.
    const int context = inMenu ? ps2controls::Menus : (driving ? ps2controls::InVehicle : ps2controls::OnFoot);
    ps2controls::setCurrentContext(context);
    // Free aim (see PSPadBackend::setFreeAimProbe): the lock-on button (R1)
    // is withheld and the aim is reported instead.
    const bool freeAimAllowed = settings.freeAim && context == ps2controls::OnFoot && m_freeAimProbe != nullptr &&
                                m_freeAimRdram != nullptr && m_freeAimProbe(m_freeAimRdram);
    bool freeAimHeld = false;
    bool moving = false; // movement input this frame (movingInput())
    constexpr int kPs2R1 = 11;
    // The game's pause flag only becomes visible next frame, so the prompt
    // art would show the gameplay keys for one frame after pressing Start.
    // A fresh Start press predicts the toggle for the art only; the pad
    // mapping keeps the real context until the flag is seen. The prompt
    // layers are rebuilt here, early in the frame, so the new art lands
    // before this frame's draw.
    {
        const int pad = ps2ActiveGamepad();
        bool startPressed = pad >= 0 && IsGamepadButtonPressed(pad, GAMEPAD_BUTTON_MIDDLE_RIGHT);
        if (!startPressed)
        {
            auto pressedKey = [](int binding)
            { return binding != ps2controls::kNone && binding < ps2controls::kMouseBase && IsKeyPressed(binding); };
            startPressed = pressedKey(settings.keys[ps2controls::Pause][0]) ||
                           pressedKey(settings.keys[ps2controls::Pause][1]);
        }
        const int predicted = inMenu ? (driving ? ps2controls::InVehicle : ps2controls::OnFoot) : ps2controls::Menus;
        ps2PromptArtSetContext(startPressed ? predicted : -1);
    }
    ps2ModsRefreshButtonPromptsIfNeeded();

    // Gamepad: each host button maps to a PS2 button per context (the
    // Classic / Modern / Custom layout in the settings), sticks as they are.
    const int kGamepad = ps2ActiveGamepad();
    if (kGamepad >= 0)
    {
        static const int kRaylibButton[ps2controls::kHostButtonCount] = {
            GAMEPAD_BUTTON_RIGHT_FACE_DOWN, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, GAMEPAD_BUTTON_RIGHT_FACE_LEFT,
            GAMEPAD_BUTTON_RIGHT_FACE_UP, GAMEPAD_BUTTON_LEFT_TRIGGER_1, GAMEPAD_BUTTON_RIGHT_TRIGGER_1,
            GAMEPAD_BUTTON_LEFT_TRIGGER_2, GAMEPAD_BUTTON_RIGHT_TRIGGER_2, GAMEPAD_BUTTON_LEFT_THUMB,
            GAMEPAD_BUTTON_RIGHT_THUMB, GAMEPAD_BUTTON_MIDDLE_LEFT, GAMEPAD_BUTTON_MIDDLE_RIGHT,
            GAMEPAD_BUTTON_LEFT_FACE_UP, GAMEPAD_BUTTON_LEFT_FACE_DOWN, GAMEPAD_BUTTON_LEFT_FACE_LEFT,
            GAMEPAD_BUTTON_LEFT_FACE_RIGHT};
        bool hostDown[ps2controls::kHostButtonCount]{};
        // How far each host button is pulled, for the analog triggers. The
        // DualShock 2's face buttons were pressure-sensitive and the game reads
        // that pressure - in a vehicle the triggers are accelerate and brake -
        // so an all-or-nothing trigger makes driving either flat out or stopped.
        float hostPressure[ps2controls::kHostButtonCount]{};
        bool anyDown = false;
        for (int b = 0; b < ps2controls::kHostButtonCount; ++b)
        {
            bool down = IsGamepadButtonDown(kGamepad, kRaylibButton[b]);
            // Analog triggers (rest at -1) on pads without trigger buttons.
            if (b == ps2controls::PadLT || b == ps2controls::PadRT)
            {
                const int trigger = (b == ps2controls::PadLT) ? 0 : 1;
                const float axis = GetGamepadAxisMovement(
                    kGamepad, trigger == 0 ? GAMEPAD_AXIS_LEFT_TRIGGER : GAMEPAD_AXIS_RIGHT_TRIGGER);
                // raylib reports a trigger from -1 (released) to 1 (fully
                // pulled). The lowest value ever seen is taken as the rest
                // point, and the axis is only trusted as analog once it has
                // rested low: a pad whose mapping has no trigger axis at all
                // reports something else there, and must not become a throttle
                // that creeps on its own.
                static float s_triggerRest[2] = {0.0f, 0.0f};
                s_triggerRest[trigger] = std::min(s_triggerRest[trigger], axis);
                const float rest = s_triggerRest[trigger];
                hostPressure[b] = (rest <= -0.5f)
                                      ? std::clamp((axis - rest) / (1.0f - rest), 0.0f, 1.0f)
                                      : 0.0f;
                (trigger == 0 ? g_ps2PadTriggerLeft : g_ps2PadTriggerRight)
                    .store(hostPressure[b], std::memory_order_relaxed);
                // A small deadzone counts as pressed, so the full travel feeds
                // the pressure rather than only the upper half.
                down = down || hostPressure[b] > 0.1f;
            }
            hostDown[b] = down;
            anyDown = anyDown || down;
        }
        filterMenuCombo(hostDown[ps2controls::PadBack], hostDown[ps2controls::PadStart]);
        if (s_padHeldByMenu && !anyDown)
            s_padHeldByMenu = false;
        if (!s_padHeldByMenu)
            for (int b = 0; b < ps2controls::kHostButtonCount; ++b)
            {
                // Every button follows the context's map: the menu context keeps
                // the classic layout, so the D-pad (and everything else)
                // navigates the game's menus under every gameplay layout.
                // D-pad Down also holds the radio on/off toggle (above).
                const int ps2Button = settings.gamepadMap[context][b];
                // Pressure is recorded even below the press threshold: a light
                // pull should modulate, not do nothing.
                if (hostPressure[b] > 0.0f && ps2Button >= 0 && ps2Button < 16)
                {
                    const uint8_t value = static_cast<uint8_t>(std::lround(hostPressure[b] * s_pressureMax));
                    if (value > pressure[ps2Button])
                        pressure[ps2Button] = value;
                }
                if (!hostDown[b])
                    continue;
                if (ps2Button == kPs2R1 && freeAimAllowed &&
                    settings.gamepadLayout != ps2controls::GamepadLayout::Classic)
                {
                    freeAimHeld = true;
                    continue;
                }
                press(ps2Button);
            }

        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
        moving = moving || std::fabs(lx) > 0.25f || std::fabs(ly) > 0.25f;
    }

    // Keyboard + mouse (modern GTA PC style by default, rebindable), merged
    // with the gamepad: raylib reports any joystick device as "gamepad
    // available" (e.g. gaming mouse dongles exposing /dev/input/js*), so the
    // keyboard is always read.
    {
        namespace pc = ps2controls;
        const bool captured = m_mouseCaptured.load(std::memory_order_relaxed);
        auto bindingDown = [captured](int binding)
        {
            if (binding == pc::kNone)
                return false;
            if (binding >= pc::kMouseBase)
                return captured && IsMouseButtonDown(binding - pc::kMouseBase); // a click first captures the mouse
            return IsKeyDown(binding);
        };
        auto held = [&](int action)
        { return bindingDown(settings.keys[action][0]) || bindingDown(settings.keys[action][1]); };

        enum : int
        {
            Select = 0, L3, R3, Start, Up, Right, Down, Left, L2, R2, L1, R1, Triangle, Circle, Cross, Square
        };
        // Menus and common actions. The menu keys work in the game's menus
        // only (their own row of bindings), and the gameplay bindings do not
        // leak into the menus.
        const bool inMenu = context == ps2controls::Menus;
        if (inMenu)
        {
            if (held(pc::MenuUp))
                press(Up);
            if (held(pc::MenuDown))
                press(Down);
            if (held(pc::MenuLeft))
                press(Left);
            if (held(pc::MenuRight))
                press(Right);
            if (held(pc::MenuAccept))
                press(Cross);
            if (held(pc::MenuBack))
                press(Triangle);
            if (held(pc::MapTarget))
                press(Square); // the map's Target On/Off
        }
        if (held(pc::Pause))
            press(Start);
        // Cutscene skip: Cross, but only while a cutscene plays (when the
        // probe is wired), so it cannot fire during normal gameplay.
        const bool cutscene =
            m_cutsceneProbe == nullptr || (m_cutsceneRdram != nullptr && m_cutsceneProbe(m_cutsceneRdram));
        static bool s_lastCutscene = false;
        if (m_cutsceneProbe != nullptr && cutscene != s_lastCutscene)
        {
            s_lastCutscene = cutscene;
            std::fprintf(stderr, "[pad] cutscene=%d\n", cutscene ? 1 : 0);
        }
        if (held(pc::SkipCutscene) && cutscene)
            press(Cross);
        if (!inMenu)
        {
            if (held(pc::EnterVehicle))
                press(Triangle);
            if (held(pc::CameraMode))
                press(Select);
            if (held(pc::LookBehind))
                press(R3);
            if (held(pc::Fire))
                press(Circle);
        }

        // Left stick: WASD; in vehicles forward / back are the pedals. In the
        // game's menus the wheel zooms the map (R1 in, L1 out); navigation is
        // the menu keys above (arrows plus WASD).
        float moveX = 0.0f, moveY = 0.0f;
        if (inMenu)
        {
            int zoom = m_wheelSteps.exchange(0, std::memory_order_relaxed);
            if (zoom == 0)
            {
                // The wheel is also read by the mouse-look path when the mouse
                // is captured; either source gives at most one step per frame.
                const float wheel = GetMouseWheelMove();
                if (wheel != 0.0f)
                    zoom = wheel > 0.0f ? 1 : -1;
            }
            // Each notch holds the zoom button for a few frames; scrolling fast
            // queues several steps, so the hold (and the game's own repeat)
            // keeps up instead of one step per notch. The pad's L1/R1 buttons
            // are untouched.
            if (zoom != 0)
                m_menuZoomPulse = std::clamp(m_menuZoomPulse + zoom * 6, -48, 48);
            if (m_menuZoomPulse > 0)
                press(R1);
            else if (m_menuZoomPulse < 0)
                press(L1);
            m_menuZoomPulse += m_menuZoomPulse > 0 ? -1 : (m_menuZoomPulse < 0 ? 1 : 0);
        }
        else
        {
            if (held(pc::MoveLeft))
                moveX -= 1.0f;
            if (held(pc::MoveRight))
                moveX += 1.0f;
            if (driving)
            {
                if (held(pc::MoveForward))
                    press(Cross);
                if (held(pc::MoveBackward))
                    press(Square);
                if (held(pc::Handbrake))
                    press(R1);
                if (held(pc::Horn))
                    press(L1);
                // Bikes: the keyboard has no analog stick, so this pulls the
                // left stick back (hold accelerate for a wheelie) or forward
                // for a stoppie.
                if (held(pc::VehicleLeanBack))
                    moveY += 1.0f;
                if (held(pc::VehicleLeanForward))
                    moveY -= 1.0f;
            }
            else
            {
                if (held(pc::Sprint))
                    press(Cross);
                if (held(pc::Jump))
                    press(Square);
                if (held(pc::Interact))
                    press(L1);
                if (held(pc::Aim))
                {
                    if (freeAimAllowed)
                        freeAimHeld = true;
                    else
                        press(R1);
                }
                if (held(pc::MoveForward))
                    moveY -= 1.0f;
                if (held(pc::MoveBackward))
                    moveY += 1.0f;
            }
            moving = moving || moveX != 0.0f || moveY != 0.0f;
        }

        // Weapon cycling: mouse wheel / the Next weapon key, as short R2
        // (next) / L2 (previous) presses. In the game's menus the wheel is the
        // map zoom instead (above).
        if (!inMenu)
        {
            const int steps = m_wheelSteps.exchange(0, std::memory_order_relaxed);
            if (m_weaponPulse == 0 && steps != 0)
                m_weaponPulse = steps < 0 ? 4 : -4;
            auto pressedNow = [](int binding)
            {
                if (binding == pc::kNone || binding >= pc::kMouseBase)
                    return binding >= pc::kMouseBase && IsMouseButtonPressed(binding - pc::kMouseBase);
                return IsKeyPressed(binding);
            };
            if (m_weaponPulse == 0 && (pressedNow(settings.keys[pc::NextWeapon][0]) || pressedNow(settings.keys[pc::NextWeapon][1])))
                m_weaponPulse = 4;
            if (!driving && m_weaponPulse > 0)
                press(R2);
            if (!driving && m_weaponPulse < 0)
                press(L2);
            m_weaponPulse += m_weaponPulse > 0 ? -1 : (m_weaponPulse < 0 ? 1 : 0);
        }
    }

    // PS2X_FREE_AIM_TEST=<from>-<to>: hold the free aim between those
    // presented frames (headless tests; only where free aim applies).
    {
        static const std::pair<uint64_t, uint64_t> s_test = []()
        {
            unsigned long long from = 0, to = 0;
            if (const char *env = std::getenv("PS2X_FREE_AIM_TEST"))
                std::sscanf(env, "%llu-%llu", &from, &to);
            return std::make_pair(static_cast<uint64_t>(from), static_cast<uint64_t>(to));
        }();
        const uint64_t frame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        if (freeAimAllowed && frame >= s_test.first && frame < s_test.second)
            freeAimHeld = true;
    }
    m_freeAimHeld.store(freeAimHeld, std::memory_order_relaxed);
    m_movingInput.store(moving, std::memory_order_relaxed);
    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    // Analog pressure, by PS2 button bit, for readPadPortData to pick up.
    for (int bit = 0; bit < 16; ++bit)
        data[8 + bit] = pressure[bit];
    // PS2X_PAD_DEBUG=1: once a second, what a controller is doing - which pad
    // the port picked, whether the pinned name matched, the host buttons down
    // and the PS2 mask that resulted.
    if (ps2PadDebugEnabled())
    {
        static double s_next = 0.0;
        const double now = GetTime();
        if (now >= s_next)
        {
            s_next = now + 1.0;
            const int pad = ps2ActiveGamepad();
            std::string names;
            for (int i = 0; i < 4; ++i)
            {
                if (!IsGamepadAvailable(i))
                    continue;
                const char *nm = GetGamepadName(i);
                unsigned mask = 0;
                for (int b = GAMEPAD_BUTTON_LEFT_FACE_UP; b <= GAMEPAD_BUTTON_RIGHT_THUMB; ++b)
                    if (IsGamepadButtonDown(i, static_cast<GamepadButton>(b)))
                        mask |= 1u << b;
                char axes[64];
                std::snprintf(axes, sizeof(axes), "%d,%d,%d,%d", static_cast<int>(GetGamepadAxisMovement(i, 0) * 100.0f),
                              static_cast<int>(GetGamepadAxisMovement(i, 1) * 100.0f),
                              static_cast<int>(GetGamepadAxisMovement(i, 2) * 100.0f),
                              static_cast<int>(GetGamepadAxisMovement(i, 3) * 100.0f));
                names += " [" + std::to_string(i) + "]" + (nm ? nm : "?") +
                         "(mapped=" + (glfwJoystickIsGamepad(i) ? "1" : "0") + ",btn=" + std::to_string(mask) +
                         ",ax=" + axes + ")";
            }
            const char *name = pad >= 0 ? GetGamepadName(pad) : nullptr;
            std::string pressureText;
            for (int bit = 0; bit < 16; ++bit)
                if (pressure[bit] != 0)
                    pressureText += " " + std::to_string(bit) + ":" + std::to_string(pressure[bit]);
            char line[1024];
            std::snprintf(line, sizeof(line),
                          "[pad] active=%d '%s' pinned='%s' prompts='%s' connected:%s ps2mask=0x%04x axes=%u,%u,%u,%u pressure:%s\n",
                          pad, name ? name : "(none)", ps2HostSettings().controllerName.c_str(),
                          ps2CurrentPromptSet().c_str(), names.c_str(), static_cast<unsigned>(btns), data[6], data[7],
                          data[4], data[5], pressureText.empty() ? " none" : pressureText.c_str());
            ps2PadDebugLog(line);
        }
    }
    return true;
}

bool PSPadBackend::applyFakeMouse(float &yaw, float &pitch)
{
    static const char *s_env = std::getenv("PS2X_MOUSE_FAKE");
    if (!s_env)
        return false;
    static float s_yaw = 0.0f, s_pitch = 0.0f;
    static unsigned long long s_from = 0, s_to = ~0ull; // optional presented-frame window
    static const bool s_parsed = std::sscanf(s_env, "%f,%f,%llu,%llu", &s_yaw, &s_pitch, &s_from, &s_to) >= 1;
    const uint64_t frame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
    if (!s_parsed)
        return false;
    if (frame < s_from || frame >= s_to)
        return true; // captured, not moving
    yaw += s_yaw;
    pitch += s_pitch;
    return true;
}

void PSPadBackend::updateHostInput()
{
    ps2SetHostThread();
    ps2MapUnrecognisedPads();
    // Before the early returns below: the game's rumble must keep reaching the
    // controller while the settings menu is open or the mouse is released.
    ps2PadRumbleTick();
    // F7: a marker in the log (playtest notes: "it happened here").
    // F8: snapshot EE RAM to ram_f8_<frame>.bin (memory diffing while playing,
    // e.g. finding the radio station variable).
    if (IsKeyPressed(KEY_F8) || IsKeyPressed(KEY_F9))
        g_ps2DumpRamRequest.store(true, std::memory_order_relaxed);
    if (IsKeyPressed(KEY_F7))
    {
        std::fprintf(stderr, "[mark] frame=%llu\n", static_cast<unsigned long long>(g_ps2PresentedFrameCounter.load()));
        if (FILE *f = std::fopen("boolwrites.log", "a"))
        {
            std::fprintf(f, "[mark] frame=%llu\n", static_cast<unsigned long long>(g_ps2PresentedFrameCounter.load()));
            std::fclose(f);
        }
    }
    // Click in the window to capture the mouse for mouse look; Left Alt (or
    // losing focus) releases it.
    bool captured = m_mouseCaptured.load(std::memory_order_relaxed);
    // The key (or mouse button) bound to "Release the mouse" - Left Alt by
    // default - gives the cursor back; a left click captures it again.
    auto releaseMousePressed = []()
    {
        const auto &bindings = ps2HostSettings().keys[ps2controls::ReleaseMouse];
        for (int slot = 0; slot < 2; ++slot)
        {
            const int binding = bindings[slot];
            if (binding == ps2controls::kNone)
                continue;
            if (binding >= ps2controls::kMouseBase)
            {
                if (IsMouseButtonPressed(binding - ps2controls::kMouseBase))
                    return true;
            }
            else if (IsKeyPressed(binding))
                return true;
        }
        return false;
    };
    // Track the settings-menu open->close edge *before* the early return below,
    // so a close is noticed even though the menu-open branch returns.
    static bool s_settingsWasOpen = false;
    static bool s_wantCapture = false;
    const bool settingsOpenNow = g_ps2SettingsMenuOpen.load(std::memory_order_relaxed);
    const bool settingsJustClosed = s_settingsWasOpen && !settingsOpenNow;
    s_settingsWasOpen = settingsOpenNow;
    const bool mouseLookMode =
        ps2HostSettings().displayMode != PS2HostSettings::DisplayMode::Windowed;

    if (settingsOpenNow)
    {
        if (captured)
            EnableCursor();
        m_mouseCaptured.store(false, std::memory_order_relaxed);
        GetMouseDelta();
        return;
    }
    // Auto-capture in fullscreen/borderless: at launch and whenever the settings
    // menu closes. The intent is kept until the window is focused and the
    // capture actually happens, so a slow focus at launch is not lost. A click
    // still captures in any display mode.
    static bool s_didLaunchCapture = false;
    if (!s_didLaunchCapture)
    {
        s_didLaunchCapture = true;
        if (mouseLookMode)
            s_wantCapture = true;
    }
    if (settingsJustClosed)
        s_wantCapture = true;

    if (!captured && IsWindowFocused() &&
        (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || (s_wantCapture && mouseLookMode)))
    {
        DisableCursor();
        GetMouseDelta(); // swallow motion made while the cursor was free
        captured = true;
        s_wantCapture = false;
    }
    else if (captured && (releaseMousePressed() || !IsWindowFocused()))
    {
        EnableCursor();
        captured = false;
        s_wantCapture = false;
    }    m_mouseCaptured.store(captured, std::memory_order_relaxed);
    if (!captured)
        return;
    // raylib's DisableCursor() also re-centres the cursor on every call, which
    // zeroes the mouse delta. Re-assert the raw (hidden, uncentred) mode
    // directly instead, so the hide sticks without eating the movement.
    if (GLFWwindow *window = glfwGetCurrentContext())
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    // Direct mouse look (applied to the camera by a game hook, see
    // consumeMouseLook). Sensitivity / invert Y come from the settings (F2
    // menu, PS2X_MOUSE_SENS / PS2X_MOUSE_INVERT_Y); '-' / '=' adjust the
    // sensitivity while playing.
    PS2HostSettings &settings = ps2HostSettings();
    if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_EQUAL))
    {
        settings.mouseSensitivity = std::clamp(settings.mouseSensitivity * (IsKeyPressed(KEY_EQUAL) ? 1.25f : 0.8f), 0.05f, 20.0f);
        ps2HostSettingsSave();
        std::fprintf(stderr, "[pad] mouse sensitivity %.2f\n", settings.mouseSensitivity);
    }
    const float radiansPerPixel = 0.001f * settings.mouseSensitivity;
    const float pitchSign = settings.mouseInvertY ? -1.0f : 1.0f;
    const Vector2 delta = GetMouseDelta();
    if (delta.x != 0.0f || delta.y != 0.0f)
    {
        m_lookYaw.store(m_lookYaw.load(std::memory_order_relaxed) + delta.x * radiansPerPixel, std::memory_order_relaxed);
        m_lookPitch.store(m_lookPitch.load(std::memory_order_relaxed) - delta.y * radiansPerPixel * pitchSign,
                          std::memory_order_relaxed);
        static const bool s_debug = std::getenv("PS2X_PAD_DEBUG") != nullptr;
        if (s_debug)
            std::fprintf(stderr, "[pad] mouse d=(%.1f,%.1f)\n", delta.x, delta.y);
    }
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f)
        m_wheelSteps.fetch_add(wheel > 0.0f ? 1 : -1, std::memory_order_relaxed);
}
