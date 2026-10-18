#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// The raylib gamepad slot to read (-1: none). The first slot holding a real
// gamepad: slot 0 can be a non-gamepad joystick (a DualSense's motion
// sensors) or one of several pads (Steam Input adds a virtual one).
// The settings' chosen controller (by name) wins when it is connected.
int ps2ActiveGamepad();
void ps2SetPreferredGamepad(const std::string &name);

// True when the raylib slot holds a device GLFW recognises as a gamepad: a
// keyboard or mouse exposing a phantom joystick node is not one, and such a
// device can never be auto-selected or read.
bool ps2IsRealGamepad(int pad);

// The emulated game's rumble state, from scePadSetActDirect: the small
// (right/weak) motor then the large (left/strong). This is a state, not an
// event - the game sets it and it stays until changed - so it is stored here
// and pushed to the active controller by updateHostInput on the host thread.
// Nothing in raylib's GLFW backend can do this itself, so each platform drives
// its own device layer (evdev force feedback on Linux, XInput on Windows).
void ps2PadRumbleSetMotors(uint8_t small, uint8_t large);

// The live analog trigger positions (0..1), left then right. Written by the pad
// backend every frame; read by the game-side driving hook so the vehicle's own
// gas pedal can follow the trigger (see game_overrides.cpp).
extern std::atomic<float> g_ps2PadTriggerLeft;
extern std::atomic<float> g_ps2PadTriggerRight;

// Push the current motor state to the active controller. Host (window) thread,
// once a frame, from updateHostInput.
void ps2PadRumbleTick();

// Installs the gamepad mappings GLFW does not ship (plus the user file in the
// config folder). Safe to call at any time: it retries until GLFW is
// initialised, so map it right after InitWindow and before the first pad read.
void ps2ApplyGamepadMappings();

// PS2X_PAD_DEBUG=1 support: a one-line-per-event trace of pad and prompt
// decisions, written to padlog.txt (working directory and the config folder).
bool ps2PadDebugEnabled();
void ps2PadDebugLog(const char *line);

// The active input device, from the gamepad's reported name (the keyboard when
// no gamepad is active). Drives the runtime's prompt wording and the
// button-prompt replacement set.
enum class PS2InputDevice
{
    Keyboard,
    Xbox,
    Steam,
    DualSense,  // PS5
    DualShock,  // PS3/PS4
    Switch,
    Generic
};
PS2InputDevice ps2InputDeviceKind();

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);

    // Host (window) thread, once per displayed frame: mouse capture and
    // motion for mouse look.
    void updateHostInput();

    // Game-specific "player is driving" check, used by the keyboard (W/S drive)
    // and the optional modern gamepad driving controls.
    // Mouse look: radians of yaw (positive = turn right) and pitch (positive =
    // look up) accumulated since the last call; a game hook applies them to
    // its camera. Returns false when the mouse is not captured.
    bool consumeMouseLook(float &yaw, float &pitch)
    {
        yaw = m_lookYaw.exchange(0.0f, std::memory_order_relaxed);
        pitch = m_lookPitch.exchange(0.0f, std::memory_order_relaxed);
        return m_mouseCaptured.load(std::memory_order_relaxed) || applyFakeMouse(yaw, pitch);
    }

    // The left stick the game last read (keyboard WASD, gamepad, script)
    // is pushed: the player is being moved.
    bool movingInput() const { return m_movingInput.load(std::memory_order_relaxed); }
    void setMovingInput(bool moving) { m_movingInput.store(moving, std::memory_order_relaxed); }

    using VehicleProbe = bool (*)(const uint8_t *rdram);
    void setVehicleProbe(VehicleProbe probe, const uint8_t *rdram)
    {
        m_vehicleProbe = probe;
        m_rdram = rdram;
    }

    // Whether the game's pause menu / map is open: the pad then uses the fixed
    // menu layout instead of the gameplay one.
    using MenuProbe = bool (*)(const uint8_t *rdram);
    void setMenuProbe(MenuProbe probe, const uint8_t *rdram)
    {
        m_menuProbe = probe;
        m_menuRdram = rdram;
    }

    // Whether a cutscene is playing: the Skip cutscene key only sends Cross
    // then, so it cannot fire during normal gameplay. With no probe set the
    // key is allowed everywhere.
    using CutsceneProbe = bool (*)(const uint8_t *rdram);
    void setCutsceneProbe(CutsceneProbe probe, const uint8_t *rdram)
    {
        m_cutsceneProbe = probe;
        m_cutsceneRdram = rdram;
    }

    // Free aim: whether the player is on foot holding a weapon that free aim
    // applies to (game-specific), and whether the aim button is held for it
    // this frame - the pad then withholds the lock-on button.
    using FreeAimProbe = bool (*)(const uint8_t *rdram);
    void setFreeAimProbe(FreeAimProbe probe, const uint8_t *rdram)
    {
        m_freeAimProbe = probe;
        m_freeAimRdram = rdram;
    }
    bool freeAimHeld() const { return m_freeAimHeld.load(std::memory_order_relaxed); }

private:
    static bool applyFakeMouse(float &yaw, float &pitch); // PS2X_MOUSE_FAKE=<yaw>,<pitch> per camera update (tests)
    FreeAimProbe m_freeAimProbe = nullptr;
    const uint8_t *m_freeAimRdram = nullptr;
    std::atomic<bool> m_freeAimHeld{false};
    VehicleProbe m_vehicleProbe = nullptr;
    const uint8_t *m_rdram = nullptr;
    MenuProbe m_menuProbe = nullptr;
    const uint8_t *m_menuRdram = nullptr;
    CutsceneProbe m_cutsceneProbe = nullptr;
    const uint8_t *m_cutsceneRdram = nullptr;
    std::atomic<bool> m_mouseCaptured{false};
    std::atomic<float> m_lookYaw{0.0f}, m_lookPitch{0.0f}; // radians not yet applied
    std::atomic<int> m_wheelSteps{0};
    std::atomic<bool> m_movingInput{false};
    int m_weaponPulse = 0; // pad reads left on a scripted weapon-cycle press (+ next, - previous)
    int m_menuZoomPulse = 0; // the same for the map zoom (wheel in the menu context)
};

#endif
