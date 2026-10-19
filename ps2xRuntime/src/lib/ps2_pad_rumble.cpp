// Controller rumble.
//
// The game drives the two DualShock motors through scePadSetActDirect (see the
// stub in Kernel/Stubs/Pad.cpp), but nothing in the port's input stack can pass
// that on by itself: raylib's GLFW backend stubs SetGamepadVibration out ("not
// available on target platform") and GLFW 3.4 exposes no rumble at all. Each
// platform therefore talks to its own device layer - the kernel's evdev force
// feedback on Linux (the same /dev/input tree the pad mapping derivation
// already walks), XInput on Windows.
//
// This lives in its own translation unit so <windows.h> and raylib.h - which
// disagree about Rectangle, DrawText, CloseWindow and friends - never meet.
#include "runtime/ps2_pad.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// The joystick's GLFW GUID, which is what identifies the device on Linux. GLFW
// has no conflicting declarations with either platform header.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace
{
    // Low byte = small (right/weak) motor, high byte = large (left/strong).
    std::atomic<uint32_t> g_padMotors{0};

    bool rumbleEnabled()
    {
        static const bool s_enabled = []()
        {
            const char *env = std::getenv("PS2X_PAD_RUMBLE");
            return env == nullptr || std::atoi(env) != 0;
        }();
        return s_enabled;
    }
}

void ps2PadRumbleSetMotors(uint8_t small, uint8_t large)
{
    const uint32_t packed = (static_cast<uint32_t>(large) << 8) | small;
    const uint32_t previous = g_padMotors.exchange(packed, std::memory_order_relaxed);
    if (previous != packed && ps2PadDebugEnabled())
    {
        char line[64];
        std::snprintf(line, sizeof line, "[rumble] small=%u large=%u\n", small, large);
        ps2PadDebugLog(line);
    }
}

#if defined(__linux__)
namespace
{
    int g_ffFd = -1;
    int g_ffEffect = -1;

    // bus / vendor / product / version out of a GLFW joystick GUID: the fields
    // ps2BuildEvdevMapping matches on, and what tells two pads apart.
    bool rumbleParseGuid(const char *guid, unsigned &bus, unsigned &vendor, unsigned &product, unsigned &version)
    {
        if (guid == nullptr || std::strlen(guid) != 32)
            return false;
        unsigned bytes[16]{};
        for (int i = 0; i < 16; ++i)
            if (std::sscanf(guid + i * 2, "%2x", &bytes[i]) != 1)
                return false;
        bus = bytes[0] | (bytes[1] << 8);
        vendor = bytes[4] | (bytes[5] << 8);
        product = bytes[8] | (bytes[9] << 8);
        version = bytes[12] | (bytes[13] << 8);
        return true;
    }

    // Open the pad's event node read-write, but only when it really does force
    // feedback: a DualSense exposes four event nodes and only the first has it.
    int rumbleOpenNode(const char *guid)
    {
        unsigned bus, vendor, product, version;
        if (!rumbleParseGuid(guid, bus, vendor, product, version))
            return -1;
        DIR *dir = opendir("/dev/input");
        if (dir == nullptr)
            return -1;
        std::vector<std::string> paths;
        for (dirent *entry = readdir(dir); entry != nullptr; entry = readdir(dir))
            if (std::strncmp(entry->d_name, "event", 5) == 0)
                paths.push_back(std::string("/dev/input/") + entry->d_name);
        closedir(dir);
        for (const std::string &path : paths)
        {
            const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                continue;
            struct input_id id{};
            unsigned char ffBits[(FF_CNT + 7) / 8]{};
            const bool ok = ioctl(fd, EVIOCGID, &id) == 0 &&
                            ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffBits)), ffBits) >= 0;
            const bool hasRumble = ok && (ffBits[FF_RUMBLE / 8] & (1 << (FF_RUMBLE % 8))) != 0;
            if (!hasRumble || id.bustype != bus || id.vendor != vendor || id.product != product || id.version != version)
            {
                ::close(fd);
                continue;
            }
            return fd;
        }
        return -1;
    }

    void rumbleStopEffect()
    {
        if (g_ffFd < 0 || g_ffEffect < 0)
            return;
        struct input_event ev{};
        ev.type = EV_FF;
        ev.code = static_cast<uint16_t>(g_ffEffect);
        ev.value = 0;
        ::write(g_ffFd, &ev, sizeof ev);
    }

    void rumbleApply(uint8_t small, uint8_t large)
    {
        if (g_ffFd < 0)
            return;
        if (small == 0 && large == 0)
        {
            rumbleStopEffect();
            return;
        }
        // One effect, kept and updated in place: length 0 plays until stopped,
        // so the game's "set the motors and leave them" state is exactly what
        // the pad feels, and a later change only re-uploads the magnitudes.
        struct ff_effect effect{};
        effect.type = FF_RUMBLE;
        effect.id = static_cast<int16_t>(g_ffEffect); // -1 allocates, else updates
        effect.u.rumble.strong_magnitude = static_cast<uint16_t>(large) * 257u; // left
        effect.u.rumble.weak_magnitude = static_cast<uint16_t>(small) * 257u;   // right
        effect.replay.length = 0;
        effect.replay.delay = 0;
        const int rc = ioctl(g_ffFd, EVIOCSFF, &effect);
        if (ps2PadDebugEnabled())
        {
            char line[128];
            std::snprintf(line, sizeof line, "[rumble] apply small=%u large=%u effect=%d rc=%d\n",
                          small, large, rc < 0 ? -1 : effect.id, rc);
            ps2PadDebugLog(line);
        }
        if (rc < 0)
            return;
        g_ffEffect = effect.id;
        struct input_event ev{};
        ev.type = EV_FF;
        ev.code = static_cast<uint16_t>(g_ffEffect);
        ev.value = 1;
        ::write(g_ffFd, &ev, sizeof ev);
    }

    void rumbleRebind(int pad)
    {
        rumbleStopEffect();
        if (g_ffFd >= 0)
        {
            ::close(g_ffFd);
            g_ffFd = -1;
        }
        g_ffEffect = -1;
        const char *guid = (pad >= 0) ? glfwGetJoystickGUID(pad) : nullptr;
        if (guid != nullptr)
            g_ffFd = rumbleOpenNode(guid);
        if (ps2PadDebugEnabled())
        {
            char line[128];
            std::snprintf(line, sizeof line, "[rumble] rebind pad=%d guid=%s fd=%d\n", pad,
                          guid ? guid : "(none)", g_ffFd);
            ps2PadDebugLog(line);
        }
    }
}
#elif defined(_WIN32)
namespace
{
    // Windows answers Xbox-family pads through XInput, which is present on
    // every supported Windows. Loaded by hand so the build needs no import
    // library, and applied to every slot: only one pad is normally connected.
    //
    // A DualSense / DualShock 4 / Switch pad on Windows is not an XInput device
    // and will not rumble here; those need HID output reports, which this does
    // not yet do.
    using XInputSetStateFn = unsigned long (*)(unsigned long, void *);
    XInputSetStateFn g_xinputSetState = nullptr;
    bool g_xinputLoaded = false;

    void rumbleRebind(int)
    {
        if (g_xinputLoaded)
            return;
        g_xinputLoaded = true;
        for (const char *name : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
            if (HMODULE lib = LoadLibraryA(name))
                if ((g_xinputSetState = reinterpret_cast<XInputSetStateFn>(
                         GetProcAddress(lib, "XInputSetState"))) != nullptr)
                    return;
    }

    void rumbleApply(uint8_t small, uint8_t large)
    {
        if (g_xinputSetState == nullptr)
            return;
        struct XVibration
        {
            uint16_t left;
            uint16_t right;
        } vibration{static_cast<uint16_t>(large * 257), static_cast<uint16_t>(small * 257)};
        for (unsigned slot = 0; slot < 4; ++slot)
            g_xinputSetState(slot, &vibration);
    }
}
#else
namespace
{
    void rumbleRebind(int) {}
    void rumbleApply(uint8_t, uint8_t) {}
}
#endif

void ps2PadRumbleTick()
{
    if (!rumbleEnabled())
        return;
    const uint32_t motors = g_padMotors.load(std::memory_order_relaxed);
    const int pad = ps2ActiveGamepad();

    // Rebind only when the active controller changes: finding the device walks
    // /dev/input, which is far too slow to do every frame.
    static int s_boundPad = -2;
    static uint32_t s_applied = ~0u;
    if (pad != s_boundPad)
    {
        s_boundPad = pad;
        rumbleRebind(pad);
        s_applied = ~0u; // push the current state to the new device
    }
    if (motors == s_applied)
        return;
    s_applied = motors;
    rumbleApply(static_cast<uint8_t>(motors & 0xFFu), static_cast<uint8_t>((motors >> 8) & 0xFFu));
}
