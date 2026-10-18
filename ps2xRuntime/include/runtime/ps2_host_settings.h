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
    // The host button that currently reaches `ps2Button` in `context`,
    // preferring remapped buttons over the classic identity (kNone if none).
    int hostButtonForPs2(int ps2Button, int context);
    // Pad context (OnFoot / InVehicle / Menus), updated by the pad backend
    // each frame; button-prompt text re-resolves when it changes. Menus is the
    // game's pause menu / map: a fixed layout that keeps its own bindings
    // (classic PS2 by default) so gameplay remaps never break menu navigation.
    void setCurrentContext(int context);
    int currentContext();

    enum Context : int
    {
        OnFoot = 0,
        InVehicle = 1,
        Menus = 2,
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
        MenuDown, MenuLeft, MenuRight, MapTarget, SkipCutscene,
        // Appended after the menu keys so saved bindings keep their slots.
        VehicleLeanBack, VehicleLeanForward, ReleaseMouse, kActionCount
    };
    const char *actionName(int action);
    void defaultKeys(int keys[kActionCount][2]);
    // Display name of a binding ("W", "Mouse Left", "-" for none).
    std::string bindingName(int binding);
    // Compact binding name for on-screen prompts ("W", "LMB", "ENTER",
    // "BKSP", "UP"); empty when the binding is unset.
    std::string promptBindingName(int binding);
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
    // Swap interval. On (default) the buffer swap waits for the display, which
    // is what stops the tearing the port had with only SetTargetFPS(60) - that
    // is a software cap, not a sync. It also lets a VRR display follow the
    // frame rate, so it is the only thing VRR needs: the emulation is fixed at
    // 60 VBlanks/s, so there is nothing above 60 to drive it with yet.
    bool vsync = true;
    // Display refresh rate. The emulation is fixed at 60 VBlanks/s, so anything
    // above 60 is *interpolated*: the host presents faster than the game produces
    // pictures and the frames in between are synthesized by warping the last one
    // forward by a fraction of the last camera motion. 60 shows only the game's
    // own frames. A display that cannot refresh that fast simply blocks at the
    // swap (with VSync on), so this is safe to set high.
    enum class FrameRate : int
    {
        Native = 60,
        Hz120 = 120,
        Hz180 = 180,
        Hz240 = 240,
    };
    FrameRate frameRate = FrameRate::Native;
    int internalScale = 1;       // GPU renderer resolution multiplier (1 = native 640x448)
    bool flickerFilter = false;  // the game's interlace flicker filter (a vertical blur)
    // The game's motion blur (VCS accumulates previous frames into a pair of
    // 256x256 buffers and blends the result over the display). Off skips the
    // draws that read those buffers, so the picture stays sharp. Modern replaces
    // it with a shutter blur along the camera's own motion, so the smear follows
    // the camera instead of sitting on the screen as a fixed ghost; screen-space
    // overlays (the HUD, the minimap) are left alone.
    enum class MotionBlur : int
    {
        PS2 = 0,    // as on the console
        Off = 1,    // skip the game's blur; picture stays sharp
        Modern = 2, // blur along the camera's motion
    };
    MotionBlur motionBlur = MotionBlur::PS2;
    // How far the Modern smear reaches, in frames of camera motion (Modern only).
    // 0.5 is a half-frame shutter, 1.0 a full one.
    float motionBlurStrength = 0.5f;
    float drawDistance = 1.0f;  // multiplier on view / detail distances (1 = original)
    float mouseSensitivity = 1.0f;
    bool mouseInvertY = false;
    bool mouseTurnsPlayer = true; // on foot, mouse turns also turn a moving player (no skid stops)
    // Modern free aim: on foot with a gun, the aim button (right mouse; the
    // aim trigger on the Modern/Custom pad layouts) aims where the camera
    // looks, with a crosshair, instead of locking on (the PS2's R1).
    bool freeAim = true;

    bool musicStation = true;          // play the music folder as a radio station (VCS: the unused 10th station slot)
    float musicStationVolume = 0.8f;
    bool musicShuffle = true;
    bool radioOffEnabled = true; // hold D-pad Down / M toggles "Radio Off" (Mods tab)
    bool pnachCheats = true;            // apply .pnach memory patches from enabled mods
    // Button-prompt replacement set: PCSX2-named glyph textures in
    // prompts/<set>/ next to the executable. Auto follows the active device.
    enum class ButtonPrompts : int
    {
        Auto = 0,
        Xbox,
        Steam,
        Keyboard,
        DualSense,
        DualShock4,
        Switch,
        Off, // original PS2 art / whatever texture packs provide
    };
    ButtonPrompts buttonPrompts = ButtonPrompts::Auto;
    std::vector<std::string> disabledMods; // mod folder names switched off
    bool showAtLaunch = false; // open the settings before the game starts, every launch (always on the first)
    bool showFps = false;      // draw the frame rate over the picture (Display options)

    std::string controllerName; // controller to use when several are connected ("" = the first)
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
// What the GPU renderer reported at start-up ("" when it did not start).
std::string ps2HostSettingsGpuInfo();

// The settings menu is open: the game gets neutral input and the mouse is free.
extern std::atomic<bool> g_ps2SettingsMenuOpen;
// Launch menu: the settings are shown before the game starts (first launch,
// or showAtLaunch); the game thread waits until the player starts the game.
extern std::atomic<bool> g_ps2LaunchMenuPending;
// No settings file existed when the settings were loaded.
bool ps2HostSettingsFirstLaunch();
// The launcher started this process (it sets PS2X_LAUNCHED_FROM_LAUNCHER). What
// the settings menu calls leaving the game depends on it: from the launcher that
// goes back to the game selection, on its own it goes all the way to the desktop.
bool ps2HostLaunchedFromLauncher();

#endif
