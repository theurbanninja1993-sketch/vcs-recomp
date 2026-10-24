#include <cmath>
#include "runtime/ps2_host_settings.h"

#include "runtime/ps2_platform.h"

#include "raylib.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_gpu_backend.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

std::atomic<bool> g_ps2SettingsMenuOpen{false};
std::atomic<bool> g_ps2LaunchMenuPending{false};

namespace ps2controls
{
    const char *ps2ButtonName(int button)
    {
        static const char *kNames[kPs2ButtonCount] = {"Select", "L3", "R3", "Start", "D-pad Up", "D-pad Right",
                                                      "D-pad Down", "D-pad Left", "L2", "R2", "L1", "R1",
                                                      "Triangle", "Circle", "Cross", "Square"};
        return button >= 0 && button < kPs2ButtonCount ? kNames[button] : "(none)";
    }

    const char *hostButtonName(int button)
    {
        static const char *kNames[kHostButtonCount] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Left stick click",
                                                       "Right stick click", "Back / View", "Start / Menu",
                                                       "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right"};
        return button >= 0 && button < kHostButtonCount ? kNames[button] : "?";
    }

    namespace
    {
        std::atomic<int> g_currentContext{OnFoot};
    }

    void setCurrentContext(int context)
    {
        g_currentContext.store(context, std::memory_order_relaxed);
    }

    int currentContext()
    {
        return g_currentContext.load(std::memory_order_relaxed);
    }

    int hostButtonForPs2(int ps2Button, int context)
    {
        if (ps2Button < 0 || ps2Button >= kPs2ButtonCount)
            return kNone;
        if (context < 0 || context >= kContextCount)
            context = OnFoot;
        int classic[kContextCount][kHostButtonCount];
        presetGamepad(GamepadLayout::Classic, classic);
        const PS2HostSettings &settings = ps2HostSettings();
        int first = kNone, remapped = kNone;
        for (int b = 0; b < kHostButtonCount; ++b)
        {
            if (settings.gamepadMap[context][b] != ps2Button)
                continue;
            if (first == kNone)
                first = b;
            // A button that only serves this PS2 input in a remapped layout is
            // the more honest answer ("Modern": RT fires, not B).
            if (remapped == kNone && settings.gamepadMap[context][b] != classic[context][b])
                remapped = b;
        }
        return remapped != kNone ? remapped : first;
    }

    namespace
    {
        enum Ps2 : int
        {
            Select = 0, L3, R3, Start, Up, Right, Down, Left, L2, R2, L1, R1, Triangle, Circle, Cross, Square
        };
    }

    void presetGamepad(GamepadLayout layout, int map[kContextCount][kHostButtonCount])
    {
        // Classic: the PS2 pad, button for button.
        static const int kClassic[kHostButtonCount] = {Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3,
                                                       Select, Start, Up, Down, Left, Right};
        for (int context = 0; context < kContextCount; ++context)
            for (int b = 0; b < kHostButtonCount; ++b)
                map[context][b] = kClassic[b];
        if (layout != GamepadLayout::Modern)
            return;
        // Modern (GTA V style). On foot: RT fire, LT aim / lock on, bumpers
        // cycle weapons, B is the action button.
        int *foot = map[OnFoot];
        foot[PadRT] = Circle;
        foot[PadLT] = R1;
        foot[PadLB] = L2;
        foot[PadRB] = R2;
        foot[PadB] = L1;
        // In vehicles: RT accelerate, LT brake / reverse, RB handbrake, LB horn.
        int *car = map[InVehicle];
        car[PadRT] = Cross;
        car[PadLT] = Square;
        car[PadRB] = R1;
        car[PadLB] = L1;
        // Menus keep the classic map in every layout: menu navigation must not
        // depend on the gameplay bindings (and vice versa). One modern
        // convention on top: B is the back button. The game's own menus read
        // Triangle as back, so B sends Triangle; X and Y keep the base game's
        // Square (target) and Triangle (legend) roles on the map.
        int *menus = map[Menus];
        menus[PadB] = Triangle;
    }

    const char *actionName(int action)
    {
        static const char *kNames[kActionCount] = {
            "Move forward / accelerate", "Move back / brake", "Move / steer left", "Move / steer right", "Sprint",
            "Jump", "Fire / attack", "Aim / lock on", "Action", "Next weapon", "Enter / exit vehicle", "Handbrake",
            "Horn", "Camera mode", "Look behind", "Pause", "Menu: accept", "Menu: back", "Menu: up", "Menu: down",
            "Menu: left", "Menu: right", "Map: target", "Skip cutscene",
            "Vehicle: lean back (wheelie)", "Vehicle: lean forward (stoppie)", "Release the mouse (mouse look)"};
        return action >= 0 && action < kActionCount ? kNames[action] : "?";
    }

    void defaultKeys(int keys[kActionCount][2])
    {
        for (int a = 0; a < kActionCount; ++a)
            keys[a][0] = keys[a][1] = kNone;
        auto set = [&](int action, int first, int second = kNone)
        {
            keys[action][0] = first;
            keys[action][1] = second;
        };
        set(MoveForward, KEY_W);
        set(MoveBackward, KEY_S);
        set(MoveLeft, KEY_A);
        set(MoveRight, KEY_D);
        set(Sprint, KEY_LEFT_SHIFT);
        set(Jump, KEY_SPACE);
        set(Fire, kMouseBase + MOUSE_BUTTON_LEFT);
        set(Aim, kMouseBase + MOUSE_BUTTON_RIGHT);
        set(Interact, KEY_Q);
        set(NextWeapon, KEY_TAB);
        set(EnterVehicle, KEY_E, KEY_F);
        set(Handbrake, KEY_SPACE);
        set(Horn, KEY_H);
        set(CameraMode, KEY_V);
        set(LookBehind, KEY_C);
        // In a vehicle the keyboard has no analog stick: pull the left stick
        // back with Shift (hold accelerate for a wheelie).
        set(VehicleLeanBack, KEY_LEFT_SHIFT);
        set(VehicleLeanForward, KEY_LEFT_CONTROL);
        // Release the mouse from the window (click to capture it again).
        set(ReleaseMouse, KEY_LEFT_ALT);
        set(Pause, KEY_ESCAPE, KEY_P);
        set(MenuAccept, KEY_ENTER, KEY_KP_ENTER);
        set(MenuBack, KEY_BACKSPACE);
        // The menu keys: the arrows plus WASD (the menu context ignores the
        // gameplay bindings, so the movement keys need their own menu row).
        set(MenuUp, KEY_UP, KEY_W);
        set(MenuDown, KEY_DOWN, KEY_S);
        set(MenuLeft, KEY_LEFT, KEY_A);
        set(MenuRight, KEY_RIGHT, KEY_D);
        set(MapTarget, KEY_SPACE); // Square on the map; Backspace is Triangle (back / legend)
        set(SkipCutscene, KEY_ENTER, KEY_KP_ENTER); // Cross, like every GTA cutscene skip
    }

    std::string bindingName(int binding)
    {
        if (binding == kNone)
            return "-";
        if (binding >= kMouseBase)
        {
            static const char *kMouse[] = {"Mouse Left", "Mouse Right", "Mouse Middle", "Mouse 4", "Mouse 5",
                                           "Mouse 6", "Mouse 7"};
            const int button = binding - kMouseBase;
            return button >= 0 && button < 7 ? kMouse[button] : "Mouse ?";
        }
        if ((binding >= KEY_A && binding <= KEY_Z) || (binding >= KEY_ZERO && binding <= KEY_NINE))
            return std::string(1, static_cast<char>(binding));
        if (binding >= KEY_F1 && binding <= KEY_F12)
            return "F" + std::to_string(binding - KEY_F1 + 1);
        if (binding >= KEY_KP_0 && binding <= KEY_KP_9)
            return "Num " + std::to_string(binding - KEY_KP_0);
        switch (binding)
        {
        case KEY_SPACE: return "Space";
        case KEY_ESCAPE: return "Esc";
        case KEY_ENTER: return "Enter";
        case KEY_KP_ENTER: return "Num Enter";
        case KEY_TAB: return "Tab";
        case KEY_BACKSPACE: return "Backspace";
        case KEY_INSERT: return "Insert";
        case KEY_DELETE: return "Delete";
        case KEY_HOME: return "Home";
        case KEY_END: return "End";
        case KEY_PAGE_UP: return "Page Up";
        case KEY_PAGE_DOWN: return "Page Down";
        case KEY_UP: return "Up";
        case KEY_DOWN: return "Down";
        case KEY_LEFT: return "Left";
        case KEY_RIGHT: return "Right";
        case KEY_LEFT_SHIFT: return "Left Shift";
        case KEY_RIGHT_SHIFT: return "Right Shift";
        case KEY_LEFT_CONTROL: return "Left Ctrl";
        case KEY_RIGHT_CONTROL: return "Right Ctrl";
        case KEY_LEFT_ALT: return "Left Alt";
        case KEY_RIGHT_ALT: return "Right Alt";
        case KEY_CAPS_LOCK: return "Caps Lock";
        case KEY_GRAVE: return "`";
        case KEY_MINUS: return "-";
        case KEY_EQUAL: return "=";
        case KEY_LEFT_BRACKET: return "[";
        case KEY_RIGHT_BRACKET: return "]";
        case KEY_BACKSLASH: return "\\";
        case KEY_SEMICOLON: return ";";
        case KEY_APOSTROPHE: return "'";
        case KEY_COMMA: return ",";
        case KEY_PERIOD: return ".";
        case KEY_SLASH: return "/";
        default: return "Key " + std::to_string(binding);
        }
    }

    std::string promptBindingName(int binding)
    {
        if (binding == kNone)
            return {};
        if (binding >= kMouseBase)
        {
            static const char *kMouse[] = {"LMB", "RMB", "MMB", "M4", "M5", "M6", "M7"};
            const int button = binding - kMouseBase;
            return button >= 0 && button < 7 ? kMouse[button] : std::string("MOUSE");
        }
        if ((binding >= KEY_A && binding <= KEY_Z) || (binding >= KEY_ZERO && binding <= KEY_NINE))
            return std::string(1, static_cast<char>(binding));
        if (binding >= KEY_F1 && binding <= KEY_F12)
            return "F" + std::to_string(binding - KEY_F1 + 1);
        if (binding >= KEY_KP_0 && binding <= KEY_KP_9)
            return "NUM " + std::to_string(binding - KEY_KP_0);
        switch (binding)
        {
        case KEY_SPACE: return "SPACE";
        case KEY_ESCAPE: return "ESC";
        case KEY_ENTER:
        case KEY_KP_ENTER: return "ENTER";
        case KEY_TAB: return "TAB";
        case KEY_BACKSPACE: return "BKSP";
        case KEY_LEFT_SHIFT:
        case KEY_RIGHT_SHIFT: return "SHIFT";
        case KEY_LEFT_CONTROL:
        case KEY_RIGHT_CONTROL: return "CTRL";
        case KEY_LEFT_ALT:
        case KEY_RIGHT_ALT: return "ALT";
        case KEY_UP: return "UP";
        case KEY_DOWN: return "DOWN";
        case KEY_LEFT: return "LEFT";
        case KEY_RIGHT: return "RIGHT";
        case KEY_MINUS: return "-";
        case KEY_EQUAL: return "=";
        case KEY_LEFT_BRACKET: return "[";
        case KEY_RIGHT_BRACKET: return "]";
        case KEY_BACKSLASH: return "\\";
        case KEY_SEMICOLON: return ";";
        case KEY_APOSTROPHE: return "'";
        case KEY_COMMA: return ",";
        case KEY_PERIOD: return ".";
        case KEY_SLASH: return "/";
        default: return "KEY";
        }
    }
}

PS2HostSettings::PS2HostSettings()
{
    ps2controls::presetGamepad(gamepadLayout, gamepadMap);
    ps2controls::defaultKeys(keys);
}

namespace
{
    PS2HostSettings g_settings;
    bool g_windowApplyPending = false;
    bool g_firstLaunch = false;

    int clampMode(int value) { return std::clamp(value, 0, 2); }
}

PS2HostSettings &ps2HostSettings()
{
    return g_settings;
}

std::string ps2HostSettingsPath()
{
    // ps2ConfigDirectory() is the user config folder, or the build's own folder
    // in portable mode (see ps2_platform.h).
    return (ps2ConfigDirectory() / "settings.ini").string();
}

void ps2HostSettingsLoad()
{
    PS2HostSettings &s = g_settings;
    std::ifstream in(ps2HostSettingsPath());
    g_firstLaunch = !in.is_open();
    std::string line;
    while (std::getline(in, line))
    {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || line[0] == ';' || eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        const char *v = value.c_str();
        if (key == "display_mode")
            s.displayMode = static_cast<PS2HostSettings::DisplayMode>(clampMode(std::atoi(v)));
        else if (key == "window_width")
            s.windowWidth = std::clamp(std::atoi(v), 320, 16384);
        else if (key == "window_height")
            s.windowHeight = std::clamp(std::atoi(v), 240, 16384);
        else if (key == "aspect")
            s.aspect = static_cast<PS2HostSettings::Aspect>(clampMode(std::atoi(v)));
        else if (key == "smooth_filtering")
            s.smoothFiltering = std::atoi(v) != 0;
        else if (key == "mouse_sensitivity")
            s.mouseSensitivity = std::clamp(std::strtof(v, nullptr), 0.05f, 20.0f);
        else if (key == "internal_scale")
            s.internalScale = std::clamp(std::atoi(v), 0, 4); // 0 = Auto
        else if (key == "flicker_filter")
            s.flickerFilter = std::atoi(v) != 0;
        else if (key == "vsync")
            s.vsync = std::atoi(v) != 0;
        else if (key == "frame_rate")
        {
            const int hz = std::atoi(v);
            s.frameRate = hz >= 240 ? PS2HostSettings::FrameRate::Hz240
                                    : (hz >= 180 ? PS2HostSettings::FrameRate::Hz180
                                                 : (hz >= 120 ? PS2HostSettings::FrameRate::Hz120
                                                              : PS2HostSettings::FrameRate::Native));
        }
        else if (key == "motion_blur")
            // Off, or PS2 for anything else: Modern is parked (env-only), and a
            // settings file from when it was in the menu could say 2, which must
            // not become an out-of-range value for a two-item combo.
            s.motionBlur = std::atoi(v) == 1 ? PS2HostSettings::MotionBlur::Off : PS2HostSettings::MotionBlur::PS2;
        else if (key == "motion_blur_strength")
            s.motionBlurStrength = std::clamp(std::strtof(v, nullptr), 0.0f, 1.5f);
        else if (key == "field_of_view")
            (void)v; // field_of_view: removed (the game has its own FOV options)
        else if (key == "draw_distance")
            s.drawDistance = std::clamp(std::strtof(v, nullptr), 0.5f, 4.0f);
        else if (key == "mouse_turns_player")
            s.mouseTurnsPlayer = std::atoi(v) != 0;
        else if (key == "free_aim")
            s.freeAim = std::atoi(v) != 0;
        else if (key == "mouse_invert_y")
            s.mouseInvertY = std::atoi(v) != 0;
        else if (key == "music_station")
            s.musicStation = std::atoi(v) != 0;
        else if (key == "music_station_volume")
            s.musicStationVolume = std::clamp(std::strtof(v, nullptr), 0.0f, 1.0f);
        else if (key == "music_shuffle")
            s.musicShuffle = std::atoi(v) != 0;
        else if (key == "radio_off_enabled")
            s.radioOffEnabled = std::atoi(v) != 0;
        else if (key == "controller")
            s.controllerName = value;
        else if (key == "pnach_cheats")
            s.pnachCheats = std::atoi(v) != 0;
        else if (key == "button_prompts")
            s.buttonPrompts = static_cast<PS2HostSettings::ButtonPrompts>(std::clamp(std::atoi(v), 0, 7));
        else if (key == "mods_disabled")
        {
            s.disabledMods.clear();
            size_t start = 0;
            while (start <= value.size())
            {
                const size_t bar = value.find('|', start);
                const std::string name = value.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                if (!name.empty())
                    s.disabledMods.push_back(name);
                if (bar == std::string::npos)
                    break;
                start = bar + 1;
            }
        }
        else if (key == "show_at_launch")
            s.showAtLaunch = std::atoi(v) != 0;
        else if (key == "show_fps")
            s.showFps = std::atoi(v) != 0;
        else if (key == "gamepad_layout")
        {
            s.gamepadLayout = static_cast<ps2controls::GamepadLayout>(clampMode(std::atoi(v)));
            if (s.gamepadLayout != ps2controls::GamepadLayout::Custom)
                ps2controls::presetGamepad(s.gamepadLayout, s.gamepadMap);
        }
        else if (key.rfind("pad_", 0) == 0 && s.gamepadLayout == ps2controls::GamepadLayout::Custom)
        {
            // pad_<context>_<host button>=<ps2 button>
            int context = 0, button = 0;
            if (std::sscanf(key.c_str(), "pad_%d_%d", &context, &button) == 2 && context >= 0 &&
                context < ps2controls::kContextCount && button >= 0 && button < ps2controls::kHostButtonCount)
                s.gamepadMap[context][button] = std::clamp(std::atoi(v), ps2controls::kNone, ps2controls::kPs2ButtonCount - 1);
        }
        else if (key.rfind("key_", 0) == 0)
        {
            // key_<action>=<binding>,<binding>
            int action = 0, first = ps2controls::kNone, second = ps2controls::kNone;
            if (std::sscanf(key.c_str(), "key_%d", &action) == 1 && action >= 0 && action < ps2controls::kActionCount &&
                std::sscanf(v, "%d,%d", &first, &second) >= 1)
            {
                s.keys[action][0] = first;
                s.keys[action][1] = second;
            }
        }
    }
    if (std::getenv("PS2X_MODERN_DRIVING") && s.gamepadLayout == ps2controls::GamepadLayout::Classic)
    {
        s.gamepadLayout = ps2controls::GamepadLayout::Modern; // the old switch for trigger driving
        ps2controls::presetGamepad(s.gamepadLayout, s.gamepadMap);
    }
    if (const char *env = std::getenv("PS2X_MOUSE_SENS"))
        s.mouseSensitivity = std::clamp(std::strtof(env, nullptr), 0.05f, 20.0f);
    if (std::getenv("PS2X_MOUSE_INVERT_Y"))
        s.mouseInvertY = true;
    // The launcher's own mode: a fullscreen launcher starts the game fullscreen and a
    // windowed one starts it in a window. Either fullscreen kind already chosen in F2
    // is kept; otherwise fullscreen means borderless.
    if (const char *env = std::getenv("PS2X_DISPLAY_MODE"))
    {
        const std::string mode = env;
        if (mode == "windowed")
            s.displayMode = PS2HostSettings::DisplayMode::Windowed;
        else if (mode == "fullscreen" && s.displayMode == PS2HostSettings::DisplayMode::Windowed)
            s.displayMode = PS2HostSettings::DisplayMode::Borderless;
    }
    g_windowApplyPending = true;
    ps2HostSettingsApplyRenderer();
}

// Auto: the smallest whole multiple of the PS2's 448 lines that roughly covers
// the window (90% of its height), 1x-4x. 720p -> 2x, 1080p -> 3x (rendered at
// 1344 lines and scaled down, sharper than a fractional 2.25x would be), 1440p
// -> 3x, 4K -> 4x. The renderer only works in whole multiples.
int ps2HostSettingsEffectiveScale()
{
    if (g_settings.internalScale >= 1)
        return g_settings.internalScale;
    const int height = IsWindowReady() ? GetScreenHeight() : 1080;
    const int wanted = static_cast<int>(std::ceil(0.9 * static_cast<double>(std::max(height, 1)) / 448.0));
    return std::clamp(wanted, 1, 4);
}

void ps2HostSettingsApplyRenderer()
{
    // PS2X_GS_SCALE (tests) wins over the saved value at startup.
    static const char *s_envScale = std::getenv("PS2X_GS_SCALE");
    static bool s_first = true;
    if (!(s_first && s_envScale))
        g_gsGpuScaleRequest.store(static_cast<uint32_t>(ps2HostSettingsEffectiveScale()), std::memory_order_relaxed);
    s_first = false;
    g_gsFlickerFilter.store(g_settings.flickerFilter, std::memory_order_relaxed);
    // PS2X_FRAME_RATE overrides it from the environment, like the other settings
    // that are worth A/B-ing from a launcher.
    static const char *s_envRate = std::getenv("PS2X_FRAME_RATE");
    // Interpolation is parked: a saved frame_rate above 60 is ignored; only
    // PS2X_FRAME_RATE (testing) can turn it on.
    const uint32_t rate = s_envRate != nullptr ? static_cast<uint32_t>(std::max(60, std::atoi(s_envRate))) : 60u;
    g_gsFrameRate.store(rate, std::memory_order_relaxed);
    // Both Off and Modern drop the game's own blur accumulation: Off leaves the
    // picture sharp, Modern replaces the smear with its own. Keeping the game's
    // blur as well would blend two different ghosts.
    //
    // Modern is parked: it is not in the settings menu, and only
    // PS2X_MODERN_BLUR=1 turns it on, because it smears the HUD on frames where
    // the game draws the HUD after the copy that carries the overlay mask to the
    // display (see the note in the renderer). Frame interpolation needs the same
    // mask, so the fix belongs with that work.
    static const char *s_envModern = std::getenv("PS2X_MODERN_BLUR");
    static const char *s_envStrength = std::getenv("PS2X_MODERN_BLUR_STRENGTH");
    const bool modern = s_envModern != nullptr && std::atoi(s_envModern) != 0;
    const float strength = s_envStrength != nullptr ? std::strtof(s_envStrength, nullptr) : g_settings.motionBlurStrength;
    g_gsMotionBlurOff.store(modern || g_settings.motionBlur == PS2HostSettings::MotionBlur::Off,
                            std::memory_order_relaxed);
    g_gsModernBlur.store(modern, std::memory_order_relaxed);
    g_gsModernBlurStrength.store(modern ? strength : 0.0f, std::memory_order_relaxed);
}

bool ps2HostSettingsFirstLaunch()
{
    return g_firstLaunch;
}

std::string ps2HostSettingsGpuInfo()
{
    return g_gsGpuInfo;
}

bool ps2HostSettingsGpuRenderer()
{
    // The renderer actually running. The GPU backend reports its own state and
    // falls back to a CPU path when it cannot create a GL 4.5 context; this
    // used to report whether PS2X_GS_GPU was set, which greyed out the internal
    // resolution control on every launch once the GPU path became the default.
    return g_gsGpuActive.load(std::memory_order_relaxed);
}

void ps2HostSettingsSave()
{
    const PS2HostSettings &s = g_settings;
    const std::string path = ps2HostSettingsPath();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out)
    {
        std::fprintf(stderr, "[settings] cannot write %s\n", path.c_str());
        return;
    }
    out << "# PS2Recomp host settings (also editable in game: F2)\n"
        << "show_at_launch=" << (s.showAtLaunch ? 1 : 0) << "\n"
        << "show_fps=" << (s.showFps ? 1 : 0) << "\n"
        << "pnach_cheats=" << (s.pnachCheats ? 1 : 0) << "\n"
        << "button_prompts=" << static_cast<int>(s.buttonPrompts) << "\n";
    out << "mods_disabled=";
    for (size_t i = 0; i < s.disabledMods.size(); ++i)
        out << (i ? "|" : "") << s.disabledMods[i];
    out << "\n"
        << "display_mode=" << static_cast<int>(s.displayMode) << "\n"
        << "window_width=" << s.windowWidth << "\n"
        << "window_height=" << s.windowHeight << "\n"
        << "aspect=" << static_cast<int>(s.aspect) << "\n"
        << "smooth_filtering=" << (s.smoothFiltering ? 1 : 0) << "\n"
        << "internal_scale=" << s.internalScale << "\n"
        << "vsync=" << (s.vsync ? 1 : 0) << "\n"
        << "pnach_cheats=" << (s.pnachCheats ? 1 : 0) << "\n"
        << "flicker_filter=" << (s.flickerFilter ? 1 : 0) << "\n"
        << "vsync=" << (s.vsync ? 1 : 0) << "\n"
        << "frame_rate=" << static_cast<int>(s.frameRate) << "\n"
        << "motion_blur=" << static_cast<int>(s.motionBlur) << "\n"
        << "motion_blur_strength=" << s.motionBlurStrength << "\n"

        << "draw_distance=" << s.drawDistance << "\n"
        << "mouse_sensitivity=" << s.mouseSensitivity << "\n"
        << "mouse_invert_y=" << (s.mouseInvertY ? 1 : 0) << "\n"
        << "mouse_turns_player=" << (s.mouseTurnsPlayer ? 1 : 0) << "\n"
        << "free_aim=" << (s.freeAim ? 1 : 0) << "\n"
        << "controller=" << s.controllerName << "\n"
        << "music_station=" << (s.musicStation ? 1 : 0) << "\n"
        << "music_station_volume=" << s.musicStationVolume << "\n"
        << "music_shuffle=" << (s.musicShuffle ? 1 : 0) << "\n"
        << "radio_off_enabled=" << (s.radioOffEnabled ? 1 : 0) << "\n"
        << "gamepad_layout=" << static_cast<int>(s.gamepadLayout) << "\n";
    out << "mods_disabled=";
    for (size_t i = 0; i < s.disabledMods.size(); ++i)
        out << (i ? "|" : "") << s.disabledMods[i];
    out << "\n";
    // Load sees the layout before the per-button lines.
    if (s.gamepadLayout == ps2controls::GamepadLayout::Custom)
        for (int context = 0; context < ps2controls::kContextCount; ++context)
            for (int button = 0; button < ps2controls::kHostButtonCount; ++button)
                out << "pad_" << context << "_" << button << "=" << s.gamepadMap[context][button] << "\n";
    for (int action = 0; action < ps2controls::kActionCount; ++action)
        out << "key_" << action << "=" << s.keys[action][0] << "," << s.keys[action][1] << "  # "
            << ps2controls::actionName(action) << "\n";
}

void ps2HostSettingsRequestWindowApply()
{
    g_windowApplyPending = true;
}

void ps2HostSettingsApplyWindow()
{
    PS2HostSettings &s = g_settings;
    // Auto internal resolution follows the window: re-evaluate when it
    // changes size (a scale change rebuilds the render targets, so only then).
    if (s.internalScale == 0)
    {
        static int s_lastAuto = 0;
        const int scale = ps2HostSettingsEffectiveScale();
        if (scale != s_lastAuto)
        {
            s_lastAuto = scale;
            static const char *s_envScale = std::getenv("PS2X_GS_SCALE");
            if (s_envScale == nullptr)
                g_gsGpuScaleRequest.store(static_cast<uint32_t>(scale), std::memory_order_relaxed);
        }
    }
    // Applied every call (the host calls this each frame) so the toggle takes
    // effect at once; the static keeps the GL call from being re-issued.
    static int s_appliedVsync = -1;
    if (s_appliedVsync != (s.vsync ? 1 : 0))
    {
        s_appliedVsync = s.vsync ? 1 : 0;
        if (s.vsync)
            SetWindowState(FLAG_VSYNC_HINT);
        else
            ClearWindowState(FLAG_VSYNC_HINT);
    }
    // Borderless at startup on Windows can come out misplaced: the first
    // apply runs before the window has settled on its monitor and DPI scale,
    // and the resize Windows makes right after leaves the borderless window
    // offset (toggling it off and on fixed it). So for the first seconds,
    // check that the borderless window covers its monitor and, if not,
    // re-apply - at most twice, logged either way.
    {
        static int s_frames = 0, s_retries = 0;
        static bool s_reported = false;
        ++s_frames;
        if (s.displayMode == PS2HostSettings::DisplayMode::Borderless && IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) &&
            s_frames >= 20 && s_frames <= 600 && s_frames % 20 == 0 && !g_windowApplyPending)
        {
            const int monitor = GetCurrentMonitor();
            const Vector2 monitorPos = GetMonitorPosition(monitor);
            const Vector2 windowPos = GetWindowPosition();
            const int mw = GetMonitorWidth(monitor), mh = GetMonitorHeight(monitor);
            const int ww = GetScreenWidth(), wh = GetScreenHeight();
            const bool covers = std::abs(windowPos.x - monitorPos.x) < 2.0f && std::abs(windowPos.y - monitorPos.y) < 2.0f &&
                                std::abs(ww - mw) < 2 && std::abs(wh - mh) < 2;
            if (!covers && s_retries < 2)
            {
                ++s_retries;
                std::fprintf(stderr,
                             "[window] borderless misplaced (window %dx%d at %.0f,%.0f, monitor %d %dx%d at %.0f,%.0f): "
                             "re-applying (%d)\n",
                             ww, wh, windowPos.x, windowPos.y, monitor, mw, mh, monitorPos.x, monitorPos.y, s_retries);
                ToggleBorderlessWindowed();
                ToggleBorderlessWindowed();
            }
            else if (!s_reported && (covers || s_retries >= 2))
            {
                s_reported = true;
                std::fprintf(stderr, "[window] borderless %s (window %dx%d at %.0f,%.0f, monitor %d %dx%d at %.0f,%.0f)\n",
                             covers ? "ok" : "still misplaced after re-applying", ww, wh, windowPos.x, windowPos.y, monitor,
                             mw, mh, monitorPos.x, monitorPos.y);
            }
        }
    }
    if (!g_windowApplyPending)
    {
        // Remember a window resized by hand.
        if (s.displayMode == PS2HostSettings::DisplayMode::Windowed && !IsWindowFullscreen() &&
            !IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) && IsWindowResized())
        {
            s.windowWidth = GetScreenWidth();
            s.windowHeight = GetScreenHeight();
        }
        return;
    }
    g_windowApplyPending = false;

    // Leave the current mode first, then enter the requested one.
    if (IsWindowFullscreen() && s.displayMode != PS2HostSettings::DisplayMode::Fullscreen)
        ToggleFullscreen();
    if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) && s.displayMode != PS2HostSettings::DisplayMode::Borderless)
        ToggleBorderlessWindowed();

    switch (s.displayMode)
    {
    case PS2HostSettings::DisplayMode::Windowed:
    {
        const int monitor = GetCurrentMonitor();
        const int width = std::min(s.windowWidth, std::max(320, GetMonitorWidth(monitor)));
        const int height = std::min(s.windowHeight, std::max(240, GetMonitorHeight(monitor)));
        SetWindowSize(width, height);
        const Vector2 monitorPos = GetMonitorPosition(monitor);
        SetWindowPosition(static_cast<int>(monitorPos.x) + (GetMonitorWidth(monitor) - width) / 2,
                          static_cast<int>(monitorPos.y) + (GetMonitorHeight(monitor) - height) / 2);
        break;
    }
    case PS2HostSettings::DisplayMode::Borderless:
        if (!IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
            ToggleBorderlessWindowed();
        break;
    case PS2HostSettings::DisplayMode::Fullscreen:
        if (!IsWindowFullscreen())
        {
            const int monitor = GetCurrentMonitor();
            SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor));
            ToggleFullscreen();
        }
        break;
    }
}

bool ps2HostLaunchedFromLauncher()
{
    // Read once: the environment cannot change under us, and this is asked every
    // frame the settings menu draws.
    static const bool launched = []
    {
        const char *value = std::getenv("PS2X_LAUNCHED_FROM_LAUNCHER");
        return value != nullptr && *value != '\0' && std::string(value) != "0";
    }();
    return launched;
}
