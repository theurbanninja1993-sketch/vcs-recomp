#ifndef PS2_HOST_SETTINGS_H
#define PS2_HOST_SETTINGS_H

#include <atomic>
#include <string>
#include <vector>

// Controls. PS2 buttons are indices into the pad's button word (bit i,
// active low): 0 Select, 1 L3, 2 R3, 3 Start, 4 Up, 5 Right, 6 Down, 7 Left,
// 8 L2, 9 R2, 10 L1, 11 R1, 12 Triangle, 13 Circle, 14 Cross, 15 Square.
namespace ps2controls
{
    constexpr int kPs2ButtonCount = 16;
    constexpr int kNone = -1;
    const char *ps2ButtonName(int button);

    // Host gamepad buttons (Xbox naming).
    enum HostButton : int
    {
        PadA, PadB, PadX, PadY, PadLB, PadRB, PadLT, PadRT, PadLS, PadRS, PadBack, PadStart,
        PadUp, PadDown, PadLeft, PadRight, kHostButtonCount
    };
    const char *hostButtonName(int button);

    enum Context : int
    {
        OnFoot = 0,
        InVehicle = 1,
        Menus = 2, // the game's menus (and the title / loading screens): its own map
        kContextCount
    };

    enum class GamepadLayout : int
    {
        Classic = 0, // the PS2 controls
        Modern = 1,  // GTA V style: triggers fire/aim on foot, gas/brake in vehicles
        Custom = 2,
    };
    // Fills map[context][hostButton] = PS2 button for a preset layout.
    void presetGamepad(GamepadLayout layout, int map[kContextCount][kHostButtonCount]);

    // Keyboard / mouse actions; each has two bindings (raylib key codes, or
    // kMouseBase + mouse button).
    constexpr int kMouseBase = 1000;
    enum ActionId : int
    {
        MoveForward, MoveBackward, MoveLeft, MoveRight, Sprint, Jump, Fire, Aim, Interact, NextWeapon,
        EnterVehicle, Handbrake, Horn, CameraMode, LookBehind, Pause, MenuAccept, MenuBack, MenuUp,
        MenuDown, MenuLeft, MenuRight, MapTarget, SkipCutscene, kActionCount
    };
    const char *actionName(int action);
    void defaultKeys(int keys[kActionCount][2]);
    // Display name of a binding ("W", "Mouse Left", "-" for none).
    std::string bindingName(int binding);
}

// Player-facing host settings (display, mouse), edited in the in-game
// settings menu (F2) and kept in <config>/ps2recomp/settings.ini.
// Owned by the window thread; the game thread only reads menuOpen.
struct PS2HostSettings
{
    enum class DisplayMode : int
    {
        Windowed = 0,
        Borderless = 1,  // borderless window covering the monitor
        Fullscreen = 2,  // exclusive fullscreen at the desktop resolution
    };
    enum class Aspect : int
    {
        Standard = 0,   // 4:3, as on a PS2 TV
        Widescreen = 1, // 16:9: pair with the game's own widescreen option
        Stretch = 2,    // fill the window
    };

    DisplayMode displayMode = DisplayMode::Windowed;
    int windowWidth = 960;
    int windowHeight = 720;
    Aspect aspect = Aspect::Standard;
    bool smoothFiltering = true; // bilinear scaling of the picture (off: sharp pixels)
    int internalScale = 1;       // GPU renderer resolution multiplier (1 = native 640x448; 0 = Auto, from the window height)
    bool vsync = true;           // swap synchronised to the display (tear-free)
    bool pnachCheats = true;     // apply .pnach memory patches from enabled mods
    std::vector<std::string> disabledMods; // mod folder names switched off
    bool flickerFilter = false;  // the game's interlace flicker filter (a vertical blur)
    // The game's motion trail: every frame it copies the picture to a 512x512
    // buffer (TEX0 12288) and blends it back over the display, so moving things
    // smear. Off skips those draws (sharp picture, less work).
    bool motionBlurOff = false;
    float drawDistance = 1.0f; // multiplier on view / detail distances (1 = original)
    // Custom Soundtracks: the music folder plays on the game's silent radio slot
    // (past the ten real stations) while in a vehicle.
    bool musicStation = true;
    bool musicShuffle = false;
    float musicStationVolume = 0.8f;
    bool radioOffEnabled = true; // hold D-pad Down / M toggles "Radio Off"
    // Modern free aim: on foot with a gun, the aim button (right mouse; the
    // aim trigger on the Modern/Custom pad layouts) aims where the camera looks
    // (crosshair) instead of locking on. The Classic pad layout keeps lock-on.
    bool freeAim = true;
    bool mouseTurnsPlayer = true; // on foot, mouse turns also turn a moving player (no skid stops)
    std::string controllerName; // controller to use when several are connected ("" = automatic)
    float mouseSensitivity = 1.0f;
    bool mouseInvertY = false;

    bool showAtLaunch = false; // open the settings before the game starts, every launch (always on the first)
    bool showFps = false;      // draw the frame rate over the picture (Display options)

    ps2controls::GamepadLayout gamepadLayout = ps2controls::GamepadLayout::Classic;
    int gamepadMap[ps2controls::kContextCount][ps2controls::kHostButtonCount]{};
    int keys[ps2controls::kActionCount][2]{};

    PS2HostSettings();
};

PS2HostSettings &ps2HostSettings();
// Reads the settings file (missing file: defaults); PS2X_MOUSE_SENS and
// PS2X_MOUSE_INVERT_Y still override the mouse values.
void ps2HostSettingsLoad();
void ps2HostSettingsSave();
std::string ps2HostSettingsPath();

// Window thread, once per frame: applies display mode / window size changes
// (made in the menu or by loading) to the host window.
void ps2HostSettingsApplyWindow();
void ps2HostSettingsRequestWindowApply();
// Pushes the renderer settings (internal resolution, flicker filter) to the GS.
void ps2HostSettingsApplyRenderer();
// The GPU renderer is in use (internal resolution only applies to it).
bool ps2HostSettingsGpuRenderer();
// The internal resolution actually used: the setting, or for Auto (0) the
// multiple picked from the window height.
int ps2HostSettingsEffectiveScale();

// The settings menu is open: the game gets neutral input and the mouse is free.
extern std::atomic<bool> g_ps2SettingsMenuOpen;
// Launch menu: the settings are shown before the game starts (first launch,
// or showAtLaunch); the game thread waits until the player starts the game.
extern std::atomic<bool> g_ps2LaunchMenuPending;
// No settings file existed when the settings were loaded.
bool ps2HostSettingsFirstLaunch();

#endif
