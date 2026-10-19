#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// The controller the game reads: the player's last-used real gamepad (a
// device GLFW maps as a gamepad), or -1. Phantom joysticks - a DualSense's
// touchpad and motion sensors, gaming keyboards and mice - are never chosen.
int ps2ActiveGamepad();
bool ps2IsRealGamepad(int pad);
// An empty name means automatic selection.
void ps2SetPreferredGamepad(const std::string &name);
// Installs the extra gamepad mappings (and <config>/ps2recomp/gamecontrollerdb.txt);
// call once GLFW is up, i.e. after InitWindow.
void ps2ApplyGamepadMappings();
bool ps2PadDebugEnabled(); // PS2X_PAD_DEBUG
// The emulated game's rumble state, from scePadSetActDirect: the small
// (right/weak) motor then the large (left/strong). A state, not an event; it is
// pushed to the active controller by updateHostInput on the host thread (evdev
// force feedback on Linux, XInput on Windows). PS2X_PAD_RUMBLE=0 disables it.
void ps2PadRumbleSetMotors(uint8_t small, uint8_t large);
void ps2PadRumbleTick();
void ps2PadDebugLog(const char *line);

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

    using VehicleProbe = bool (*)(const uint8_t *rdram);
    void setVehicleProbe(VehicleProbe probe, const uint8_t *rdram)
    {
        m_vehicleProbe = probe;
        m_rdram = rdram;
    }
    // Game-specific "the game's own pause menu is open" check (the D-pad stays
    // with the game there; in a vehicle it otherwise belongs to the radio).
    void setMenuProbe(VehicleProbe probe) { m_menuProbe = probe; }
    // "An in-engine cutscene is playing" - the Skip cutscene key works only then.
    void setCutsceneProbe(VehicleProbe probe) { m_cutsceneProbe = probe; }
    // Free aim: "the player holds an aimable gun on foot". While it is true
    // (and free aim is on), the aim input is not passed to the game as the
    // lock-on button; freeAimHeld() reports it to the game hooks instead.
    void setFreeAimProbe(VehicleProbe probe) { m_freeAimProbe = probe; }
    bool freeAimHeld() const { return m_freeAimHeld.load(std::memory_order_relaxed); }
    // Movement input this frame (stick or keys): free aim faces the camera only
    // while standing.
    bool movingInput() const { return m_movingInput.load(std::memory_order_relaxed); }

private:
    static bool applyFakeMouse(float &yaw, float &pitch); // PS2X_MOUSE_FAKE=<yaw>,<pitch> per camera update (tests)
    VehicleProbe m_vehicleProbe = nullptr;
    VehicleProbe m_menuProbe = nullptr;
    VehicleProbe m_cutsceneProbe = nullptr;
    int m_menuZoomPulse = 0; // pad reads left on a map-zoom hold (+ in / R1, - out / L1)
    VehicleProbe m_freeAimProbe = nullptr;
    std::atomic<bool> m_freeAimHeld{false};
    std::atomic<bool> m_movingInput{false};
    const uint8_t *m_rdram = nullptr;
    std::atomic<bool> m_mouseCaptured{false};
    std::atomic<float> m_lookYaw{0.0f}, m_lookPitch{0.0f}; // radians not yet applied
    std::atomic<int> m_wheelSteps{0};
    int m_weaponPulse = 0; // pad reads left on a scripted weapon-cycle press (+ next, - previous)
};

#endif
