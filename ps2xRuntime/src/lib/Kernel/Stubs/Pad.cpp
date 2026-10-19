#include <cstdlib>
#include <vector>
#include <atomic>
#include <sstream>
#include "Common.h"
#include "Pad.h"
#include "runtime/ps2_pad.h"

extern std::atomic<uint64_t> g_ps2PresentedFrameCounter;

namespace ps2_stubs
{
    namespace
    {
        constexpr uint8_t kPadModeDigital = 0x41;
        constexpr uint8_t kPadModeDualShock = 0x73;
        constexpr uint8_t kPadAnalogCenter = 0x80;
        constexpr int32_t kPadTypeDigital = 4;
        constexpr int32_t kPadTypeDualShock = 7;
        constexpr int32_t kPadStateDisconnected = 0;
        constexpr int32_t kPadStateExecCmd = 5;
        constexpr int32_t kPadStateStable = 6;
        constexpr size_t kPadPortCount = 2;
        constexpr size_t kPadSlotCount = 1;

        constexpr uint16_t kPadBtnSelect = 1u << 0;
        constexpr uint16_t kPadBtnL3 = 1u << 1;
        constexpr uint16_t kPadBtnR3 = 1u << 2;
        constexpr uint16_t kPadBtnStart = 1u << 3;
        constexpr uint16_t kPadBtnUp = 1u << 4;
        constexpr uint16_t kPadBtnRight = 1u << 5;
        constexpr uint16_t kPadBtnDown = 1u << 6;
        constexpr uint16_t kPadBtnLeft = 1u << 7;
        constexpr uint16_t kPadBtnL2 = 1u << 8;
        constexpr uint16_t kPadBtnR2 = 1u << 9;
        constexpr uint16_t kPadBtnL1 = 1u << 10;
        constexpr uint16_t kPadBtnR1 = 1u << 11;
        constexpr uint16_t kPadBtnTriangle = 1u << 12;
        constexpr uint16_t kPadBtnCircle = 1u << 13;
        constexpr uint16_t kPadBtnCross = 1u << 14;
        constexpr uint16_t kPadBtnSquare = 1u << 15;

        struct PadInputState
        {
            uint16_t buttons = 0xFFFF; // active-low
            uint8_t rx = kPadAnalogCenter;
            uint8_t ry = kPadAnalogCenter;
            uint8_t lx = kPadAnalogCenter;
            uint8_t ly = kPadAnalogCenter;
            // Real pressure per PS2 button, indexed by its bit (Cross is 14,
            // Square 15, ...). Zero means "no analog source", and the button is
            // then reported as fully pressed or not pressed at all. The host
            // fills this from the analog triggers, which stand in for the
            // pressure-sensitive face buttons the DualShock 2 had (accelerate
            // and brake in a vehicle).
            uint8_t pressure[16] = {};
        };

        struct PadPortState
        {
            bool open = false;
            bool analogMode = false;  // real pads power up DIGITAL (CURID=4, mode 0x41)
            bool pressureEnabled = false;
            bool lastUsedOverride = false;
            bool lastUsedBackend = false;
            bool lastReadOk = false;
            uint16_t buttonMask = 0xFFFFu;
            uint32_t dmaAddr = 0u;
            uint32_t reqState = 0u;
            uint32_t transientState = 0u;
            PadInputState lastInput{};
            uint8_t lastData[32]{};
            uint32_t readCount = 0u;
            uint32_t lastReadDataAddr = 0u;
        };

        std::mutex g_padOverrideMutex;
        std::mutex g_padStateMutex;
        bool g_padOverrideEnabled = false;
        PadInputState g_padOverrideState{};
        PadPortState g_padPorts[kPadPortCount]{};
        int g_padReadLogCount = 0;

        uint8_t axisToByte(float axis)
        {
            axis = std::clamp(axis, -1.0f, 1.0f);
            const float mapped = (axis + 1.0f) * 127.5f;
            return static_cast<uint8_t>(std::lround(mapped));
        }

        void setButton(PadInputState &state, uint16_t mask, bool pressed)
        {
            if (pressed)
            {
                state.buttons = static_cast<uint16_t>(state.buttons & ~mask);
            }
        }

        int findFirstGamepad()
        {
            for (int i = 0; i < 4; ++i)
            {
                if (IsGamepadAvailable(i))
                {
                    return i;
                }
            }
            return -1;
        }

        void applyGamepadState(PadInputState &state)
        {
            if (!IsWindowReady())
            {
                return;
            }

            const int gamepad = findFirstGamepad();
            if (gamepad < 0)
            {
                return;
            }

            // Raylib mapping (PS2 -> raylib buttons/axes):
            // D-Pad -> LEFT_FACE_*, Cross/Circle/Square/Triangle -> RIGHT_FACE_*
            // L1/R1 -> TRIGGER_1, L2/R2 -> TRIGGER_2, L3/R3 -> THUMB
            // Select/Start -> MIDDLE_LEFT/MIDDLE_RIGHT
            state.lx = axisToByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X));
            state.ly = axisToByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_Y));
            state.rx = axisToByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_X));
            state.ry = axisToByte(GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_Y));

            setButton(state, kPadBtnUp, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_UP));
            setButton(state, kPadBtnDown, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN));
            setButton(state, kPadBtnLeft, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT));
            setButton(state, kPadBtnRight, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT));

            setButton(state, kPadBtnCross, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
            setButton(state, kPadBtnCircle, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT));
            setButton(state, kPadBtnSquare, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT));
            setButton(state, kPadBtnTriangle, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP));

            setButton(state, kPadBtnL1, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1));
            setButton(state, kPadBtnR1, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1));
            setButton(state, kPadBtnL2, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2));
            setButton(state, kPadBtnR2, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2));

            setButton(state, kPadBtnL3, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_THUMB));
            setButton(state, kPadBtnR3, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_THUMB));

            setButton(state, kPadBtnSelect, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_LEFT));
            setButton(state, kPadBtnStart, IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT));
        }

        void applyKeyboardState(PadInputState &state, bool allowAnalog)
        {
            if (!IsWindowReady())
            {
                return;
            }

            // Keyboard mapping (PS2 -> keys):
            // D-Pad: arrows, Square/Cross/Circle/Triangle: Z/X/C/V
            // L1/R1: Q/E, L2/R2: 1/3, Start/Select: Enter/RightShift
            // L3/R3: LeftCtrl/RightCtrl, Analog left: WASD
            setButton(state, kPadBtnUp, IsKeyDown(KEY_UP));
            setButton(state, kPadBtnDown, IsKeyDown(KEY_DOWN));
            setButton(state, kPadBtnLeft, IsKeyDown(KEY_LEFT));
            setButton(state, kPadBtnRight, IsKeyDown(KEY_RIGHT));

            setButton(state, kPadBtnSquare, IsKeyDown(KEY_Z));
            setButton(state, kPadBtnCross, IsKeyDown(KEY_X));
            setButton(state, kPadBtnCircle, IsKeyDown(KEY_C));
            setButton(state, kPadBtnTriangle, IsKeyDown(KEY_V));

            setButton(state, kPadBtnL1, IsKeyDown(KEY_Q));
            setButton(state, kPadBtnR1, IsKeyDown(KEY_E));
            setButton(state, kPadBtnL2, IsKeyDown(KEY_ONE));
            setButton(state, kPadBtnR2, IsKeyDown(KEY_THREE));

            setButton(state, kPadBtnStart, IsKeyDown(KEY_ENTER));
            setButton(state, kPadBtnSelect, IsKeyDown(KEY_RIGHT_SHIFT));
            setButton(state, kPadBtnL3, IsKeyDown(KEY_LEFT_CONTROL));
            setButton(state, kPadBtnR3, IsKeyDown(KEY_RIGHT_CONTROL));

            if (!allowAnalog)
            {
                return;
            }

            float ax = 0.0f;
            float ay = 0.0f;
            if (IsKeyDown(KEY_D))
                ax += 1.0f;
            if (IsKeyDown(KEY_A))
                ax -= 1.0f;
            if (IsKeyDown(KEY_S))
                ay += 1.0f;
            if (IsKeyDown(KEY_W))
                ay -= 1.0f;

            if (ax != 0.0f || ay != 0.0f)
            {
                state.lx = axisToByte(ax);
                state.ly = axisToByte(ay);
            }
        }

        void resetPadStateLocked()
        {
            for (PadPortState &portState : g_padPorts)
            {
                portState = PadPortState{};
            }
        }

        PadPortState *lookupPadPortStateLocked(int port, int slot)
        {
            if (port < 0 || port >= static_cast<int>(kPadPortCount))
            {
                return nullptr;
            }
            if (slot < 0 || slot >= static_cast<int>(kPadSlotCount))
            {
                return nullptr;
            }
            return &g_padPorts[port];
        }

        void initializePadPortLocked(PadPortState &portState, uint32_t dmaAddr)
        {
            portState.open = true;
            portState.analogMode = false;  // real pads open DIGITAL
            portState.pressureEnabled = false;
            portState.buttonMask = 0xFFFFu;
            portState.dmaAddr = dmaAddr;
            portState.reqState = 0u;
            portState.transientState = 0u;
        }

        void queueExecCmdStateLocked(PadPortState &portState)
        {
            portState.transientState = static_cast<uint32_t>(kPadStateExecCmd);
        }

        uint8_t pressureValue(const PadInputState &state, const PadPortState &portState, uint16_t mask)
        {
            if (!portState.pressureEnabled)
            {
                return 0u;
            }
            if ((portState.buttonMask & mask) == 0u)
            {
                return 0u;
            }
            // A real pressure - an analog trigger standing in for one of the
            // DualShock 2's pressure-sensitive buttons - wins over the digital
            // answer. Find the button's bit index to look it up.
            int index = -1;
            for (int bit = 0; bit < 16; ++bit)
                if ((mask & (1u << bit)) != 0u)
                {
                    index = bit;
                    break;
                }
            if (index >= 0 && state.pressure[index] != 0u)
            {
                return state.pressure[index];
            }
            return ((state.buttons & mask) == 0u) ? 0xFFu : 0u;
        }

        void fillPadStatus(uint8_t *data, const PadInputState &state, const PadPortState &portState)
        {
            std::memset(data, 0, 32);
            data[1] = portState.analogMode ? kPadModeDualShock : kPadModeDigital;
            data[2] = static_cast<uint8_t>(state.buttons & 0xFFu);
            data[3] = static_cast<uint8_t>((state.buttons >> 8) & 0xFFu);
            data[4] = state.rx;
            data[5] = state.ry;
            data[6] = state.lx;
            data[7] = state.ly;
            data[8] = pressureValue(state, portState, kPadBtnRight);
            data[9] = pressureValue(state, portState, kPadBtnLeft);
            data[10] = pressureValue(state, portState, kPadBtnUp);
            data[11] = pressureValue(state, portState, kPadBtnDown);
            data[12] = pressureValue(state, portState, kPadBtnTriangle);
            data[13] = pressureValue(state, portState, kPadBtnCircle);
            data[14] = pressureValue(state, portState, kPadBtnCross);
            data[15] = pressureValue(state, portState, kPadBtnSquare);
            data[16] = pressureValue(state, portState, kPadBtnL1);
            data[17] = pressureValue(state, portState, kPadBtnL2);
            data[18] = pressureValue(state, portState, kPadBtnR1);
            data[19] = pressureValue(state, portState, kPadBtnR2);
        }

        // PS2X_PAD_SCRIPT="<tick>:<button>[+<button>...]:<ticks>;..." presses buttons on
        // port 0 for <ticks> frames starting at <tick>. The clock defaults to
        // host presentations; PS2X_PAD_SCRIPT_CLOCK=vsync selects guest fields.
        // Buttons: up down left right start select l1 l2 l3 r1 r2 r3 cross circle square triangle,
        // plus the left stick pushed fully: sleft sright sup sdown (bits 16-19).
        uint32_t scriptedPadButtonsDown(PS2Runtime *runtime)
        {
            struct Step
            {
                uint64_t start = 0;
                uint64_t frames = 0;
                uint32_t mask = 0;
            };
            static const std::vector<Step> s_steps = []()
            {
                std::vector<Step> steps;
                const char *env = std::getenv("PS2X_PAD_SCRIPT");
                if (!env)
                    return steps;
                static const std::pair<const char *, uint32_t> kNames[] = {
                    {"sleft", 0x10000}, {"sright", 0x20000}, {"sup", 0x40000}, {"sdown", 0x80000},
                    {"rleft", 0x100000}, {"rright", 0x200000}, {"rup", 0x400000}, {"rdown", 0x800000},
                    {"select", 0x0001}, {"l3", 0x0002}, {"r3", 0x0004}, {"start", 0x0008},
                    {"up", 0x0010}, {"right", 0x0020}, {"down", 0x0040}, {"left", 0x0080},
                    {"l2", 0x0100}, {"r2", 0x0200}, {"l1", 0x0400}, {"r1", 0x0800},
                    {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000}, {"square", 0x8000}};
                std::stringstream all(env);
                std::string item;
                while (std::getline(all, item, ';'))
                {
                    const size_t c1 = item.find(':');
                    const size_t c2 = item.find(':', c1 == std::string::npos ? c1 : c1 + 1);
                    if (c1 == std::string::npos || c2 == std::string::npos)
                        continue;
                    Step step;
                    step.start = std::strtoull(item.substr(0, c1).c_str(), nullptr, 10);
                    step.frames = std::strtoull(item.substr(c2 + 1).c_str(), nullptr, 10);
                    std::stringstream names(item.substr(c1 + 1, c2 - c1 - 1));
                    std::string name;
                    while (std::getline(names, name, '+'))
                        for (const auto &[n, bit] : kNames)
                            if (name == n)
                                step.mask |= bit;
                    steps.push_back(step);
                }
                return steps;
            }();
            if (s_steps.empty())
                return 0u;
            static const bool useVsync = []()
            {
                const char *clock = std::getenv("PS2X_PAD_SCRIPT_CLOCK");
                return clock && std::string(clock) == "vsync";
            }();
            const bool guestClock = useVsync && runtime;
            const uint64_t tick = guestClock
                ? runtime->memory().gs().vsyncTick.load(std::memory_order_acquire)
                : g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
            uint32_t mask = 0u;
            for (const Step &step : s_steps)
                if (tick >= step.start && tick - step.start < step.frames)
                    mask |= step.mask;
            static const bool trace = std::getenv("PS2X_PAD_SCRIPT_TRACE") != nullptr;
            static uint32_t lastMask = 0u;
            if (trace && mask != lastMask)
            {
                std::ostringstream record;
                record << "[pad:script] clock=" << (guestClock ? "vsync" : "frame")
                       << " tick=" << tick << " mask=0x" << std::hex << mask << '\n';
                std::cerr << record.str();
            }
            lastMask = mask;
            return mask;
        }

        bool readPadPortData(int port, int slot, PS2Runtime *runtime, uint8_t *outData, uint32_t dataAddr)
        {
            if (!outData)
            {
                return false;
            }

            PadPortState portState;
            {
                std::lock_guard<std::mutex> lock(g_padStateMutex);
                const PadPortState *sharedPortState = lookupPadPortStateLocked(port, slot);
                if (!sharedPortState || !sharedPortState->open)
                {
                    return false;
                }
                portState = *sharedPortState;
            }

            PadInputState state;
            bool useOverride = false;
            {
                std::lock_guard<std::mutex> lock(g_padOverrideMutex);
                if (g_padOverrideEnabled)
                {
                    state = g_padOverrideState;
                    useOverride = true;
                }
            }

            bool usedBackend = false;
            if (!useOverride)
            {
                uint8_t backendData[32]{};
                if (runtime && runtime->padBackend().readState(port, slot, backendData, sizeof(backendData)))
                {
                    state.buttons = static_cast<uint16_t>(backendData[2] | (backendData[3] << 8));
                    state.rx = backendData[4];
                    state.ry = backendData[5];
                    state.lx = backendData[6];
                    state.ly = backendData[7];
                    // The backend carries per-button pressure at offset 8, by bit
                    // index (Cross is 14, Square 15, ...).
                    for (int bit = 0; bit < 16; ++bit)
                        state.pressure[bit] = backendData[8 + bit];
                    usedBackend = true;
                }
                else
                {
                    applyGamepadState(state);
                    applyKeyboardState(state, portState.analogMode);
                }
            }

            if (port == 0)
            {
                const uint32_t scripted = scriptedPadButtonsDown(runtime);
                state.buttons = static_cast<uint16_t>(state.buttons & ~scripted);
                if (scripted & 0x10000u) state.lx = 0x00;
                if (scripted & 0x20000u) state.lx = 0xFF;
                if (scripted & 0x40000u) state.ly = 0x00;
                if (scripted & 0x80000u) state.ly = 0xFF;
                if (scripted & 0x100000u) state.rx = 0x00;
                if (scripted & 0x200000u) state.rx = 0xFF;
                if (scripted & 0x400000u) state.ry = 0x00;
                if (scripted & 0x800000u) state.ry = 0xFF;
            }

            fillPadStatus(outData, state, portState);
            {
                static uint64_t s_padReads = 0;
                if ((++s_padReads % 3000u) == 1u)
                {
                    std::cerr << "[pad] reads=" << s_padReads << " port=" << port << " slot=" << slot
                              << " buttons=0x" << std::hex << state.buttons << " data0-3="
                              << static_cast<uint32_t>(outData[0]) << "," << static_cast<uint32_t>(outData[1]) << ","
                              << static_cast<uint32_t>(outData[2]) << "," << static_cast<uint32_t>(outData[3])
                              << std::dec << " frame=" << g_ps2PresentedFrameCounter.load() << std::endl;
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_padStateMutex);
                if (PadPortState *sharedPortState = lookupPadPortStateLocked(port, slot))
                {
                    sharedPortState->lastInput = state;
                    std::memcpy(sharedPortState->lastData, outData, sizeof(sharedPortState->lastData));
                    sharedPortState->lastUsedOverride = useOverride;
                    sharedPortState->lastUsedBackend = usedBackend;
                    sharedPortState->lastReadOk = true;
                    sharedPortState->lastReadDataAddr = dataAddr;
                    ++sharedPortState->readCount;
                }
            }

            return true;
        }
    }

    void PadSyncCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void scePadEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            resetPadStateLocked();
        }
        setReturnS32(ctx, 1);
    }

    void scePadEnterPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->pressureEnabled = true;
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadExitPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->pressureEnabled = false;
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadGetButtonMask(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        const uint16_t mask = portState ? portState->buttonMask : 0xFFFFu;
        setReturnS32(ctx, static_cast<int32_t>(mask));
    }

    void scePadGetDmaStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        const uint32_t dmaAddr = portState ? portState->dmaAddr : getRegU32(ctx, 6);
        setReturnU32(ctx, dmaAddr);
    }

    void scePadGetFrameCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        static std::atomic<uint32_t> frameCount{0};
        setReturnU32(ctx, frameCount++);
    }

    void scePadGetModVersion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // Arbitrary non-zero module version.
        setReturnS32(ctx, 0x0200);
    }

    void scePadGetPortMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 2);
    }

    void scePadGetReqState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        setReturnS32(ctx, static_cast<int32_t>(portState ? portState->reqState : 0u));
    }

    void scePadGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // Most games use one slot unless multitap is active.
        setReturnS32(ctx, 1);
    }

    void scePadGetState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        int32_t state = kPadStateDisconnected;
        if (portState && portState->open)
        {
            if (portState->transientState != 0u)
            {
                state = static_cast<int32_t>(portState->transientState);
                portState->transientState = 0u;
            }
            else
            {
                state = kPadStateStable;
            }
        }
        setReturnS32(ctx, state);
    }

    void scePadInfoAct(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t act = static_cast<int32_t>(getRegU32(ctx, 6));
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        if (act < 0)
        {
            setReturnS32(ctx, 2); // small + large motors
            return;
        }
        setReturnS32(ctx, (act < 2) ? 1 : 0);
    }

    void scePadInfoComb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // No combined modes reported.
        setReturnS32(ctx, 0);
    }

    void scePadInfoMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const int32_t infoMode = static_cast<int32_t>(getRegU32(ctx, 6)); // a2
        const int32_t index = static_cast<int32_t>(getRegU32(ctx, 7));    // a3
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        const int32_t currentId = portState->analogMode ? kPadTypeDualShock : kPadTypeDigital;
        switch (infoMode)
        {
        case 1: // PAD_MODECURID
            setReturnS32(ctx, currentId);
            return;
        case 2: // PAD_MODECUREXID
            setReturnS32(ctx, currentId);
            return;
        case 3: // PAD_MODECUROFFS
            setReturnS32(ctx, 0);
            return;
        case 4: // PAD_MODETABLE
            if (index == -1)
            {
                setReturnS32(ctx, 1); // one available mode
            }
            else if (index == 0)
            {
                setReturnS32(ctx, currentId);
            }
            else
            {
                setReturnS32(ctx, 0);
            }
            return;
        default:
            setReturnS32(ctx, 0);
            return;
        }
    }

    void scePadInfoPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        setReturnS32(ctx, (portState && portState->open) ? 1 : 0);
    }

    void scePadInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            resetPadStateLocked();
        }
        setReturnS32(ctx, 1);
    }

    void scePadInit2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        scePadInit(rdram, ctx, runtime);
    }

    void scePadPortClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->open = false;
        portState->pressureEnabled = false;
        portState->reqState = 0u;
        portState->transientState = 0u;
        setReturnS32(ctx, 1);
    }

    void scePadPortOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t dmaAddr = getRegU32(ctx, 6);
        uint8_t *dmaStr = getMemPtr(rdram, dmaAddr);
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || (dmaAddr != 0u && !dmaStr))
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->open = true;
        portState->analogMode = false;     // real pads open DIGITAL
        portState->pressureEnabled = false;
        portState->buttonMask = 0xFFFFu;
        portState->dmaAddr = dmaAddr;
        portState->reqState = 0u;
        portState->transientState = 0u;
        if (dmaStr)
        {
            ps2TraceGuestRangeWrite(rdram, dmaAddr, 32u, "scePadPortOpen", ctx);
            std::memset(dmaStr, 0, 32);
        }
        setReturnS32(ctx, 1);
    }

    void scePadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const int slot = static_cast<int>(getRegU32(ctx, 5));
        const uint32_t dataAddr = getRegU32(ctx, 6);
        uint8_t *data = getMemPtr(rdram, dataAddr);
        if (!data)
        {
            setReturnS32(ctx, 0);
            return;
        }

        ps2TraceGuestRangeWrite(rdram, dataAddr, 32u, "scePadRead", ctx);
        if (!readPadPortData(port, slot, runtime, data, dataAddr))
        {
            setReturnS32(ctx, 0);
            return;
        }

        PS2_IF_AGRESSIVE_LOGS({
            if (g_padReadLogCount < 48)
            {
                const int gamepad = findFirstGamepad();
                const bool gamepadStartPressed =
                    (gamepad >= 0) && IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT);
                const bool startPressed = (data[2] != 0xFFu || data[3] != 0xFFu ||
                                           IsKeyDown(KEY_ENTER) || gamepadStartPressed);
                static const bool s_padReadLog = std::getenv("PS2X_PAD_DEBUG") != nullptr;
                if (startPressed && s_padReadLog)
                {
                    const uint32_t guestButtons =
                        (static_cast<uint32_t>(static_cast<uint8_t>(data[2] ^ 0xFFu)) << 8) |
                        static_cast<uint32_t>(static_cast<uint8_t>(data[3] ^ 0xFFu));
                    std::printf("[padread] port=%d slot=%d data2=0x%02x data3=0x%02x guestButtons=0x%04x enter=%d gamepadStart=%d\n",
                                port, slot, data[2], data[3], guestButtons,
                                IsKeyDown(KEY_ENTER) ? 1 : 0, gamepadStartPressed ? 1 : 0);
                    ++g_padReadLogCount;
                }
            }
        });

        setReturnS32(ctx, 1);
    }

    void scePadReqIntToStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t state = getRegU32(ctx, 4);
        const uint32_t strAddr = getRegU32(ctx, 5);
        char *buf = reinterpret_cast<char *>(getMemPtr(rdram, strAddr));
        if (!buf)
        {
            setReturnS32(ctx, -1);
            return;
        }

        const char *text = (state == 0) ? "COMPLETE" : "BUSY";
        std::strncpy(buf, text, 31);
        buf[31] = '\0';
        setReturnS32(ctx, 0);
    }

    void scePadSetActAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 1);
    }

    void scePadSetActDirect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        // libpad hands over two actuator bytes: the small (right/weak) motor,
        // then the large (left/strong). They are the game's rumble state rather
        // than an event, so store them and let the host push them out.
        const uint8_t *data = getConstMemPtr(rdram, getRegU32(ctx, 6));
        if (data != nullptr)
            ps2PadRumbleSetMotors(data[0], data[1]);
        setReturnS32(ctx, 1);
    }

    void scePadSetButtonInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (portState && portState->open)
        {
            portState->buttonMask = static_cast<uint16_t>(getRegU32(ctx, 6));
            portState->reqState = 0u;
            queueExecCmdStateLocked(*portState);
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetMainMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->analogMode = (getRegU32(ctx, 6) != 0u);
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadSetReqState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (portState && portState->open)
        {
            portState->reqState = static_cast<uint32_t>(getRegU32(ctx, 6) ? 1u : 0u);
            queueExecCmdStateLocked(*portState);
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetVrefParam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 1);
    }

    void scePadSetWarningLevel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void scePadStateIntToStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t state = getRegU32(ctx, 4);
        const uint32_t strAddr = getRegU32(ctx, 5);
        char *buf = reinterpret_cast<char *>(getMemPtr(rdram, strAddr));
        if (!buf)
        {
            setReturnS32(ctx, -1);
            return;
        }

        const char *text = "UNKNOWN";
        if (state == 6)
        {
            text = "STABLE";
        }
        else if (state == 1)
        {
            text = "FINDPAD";
        }
        else if (state == 5)
        {
            text = "EXECCMD";
        }
        else if (state == 0)
        {
            text = "DISCONNECTED";
        }

        std::strncpy(buf, text, 31);
        buf[31] = '\0';
        setReturnS32(ctx, 0);
    }

    PadDebugSnapshot getPadDebugSnapshot()
    {
        PadDebugSnapshot snapshot{};
        {
            std::lock_guard<std::mutex> lock(g_padOverrideMutex);
            snapshot.overrideEnabled = g_padOverrideEnabled;
            snapshot.overrideButtons = g_padOverrideState.buttons;
            snapshot.overrideRx = g_padOverrideState.rx;
            snapshot.overrideRy = g_padOverrideState.ry;
            snapshot.overrideLx = g_padOverrideState.lx;
            snapshot.overrideLy = g_padOverrideState.ly;
        }

        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            snapshot.readLogCount = g_padReadLogCount;
            for (size_t port = 0; port < kPadDebugPortCount; ++port)
            {
                for (size_t slot = 0; slot < kPadDebugSlotCount; ++slot)
                {
                    const PadPortState &src = g_padPorts[port];
                    PadDebugPortSnapshot &dst = snapshot.ports[port][slot];
                    dst.open = src.open;
                    dst.analogMode = src.analogMode;
                    dst.pressureEnabled = src.pressureEnabled;
                    dst.lastUsedOverride = src.lastUsedOverride;
                    dst.lastUsedBackend = src.lastUsedBackend;
                    dst.lastReadOk = src.lastReadOk;
                    dst.buttonMask = src.buttonMask;
                    dst.lastButtons = src.lastInput.buttons;
                    dst.dmaAddr = src.dmaAddr;
                    dst.reqState = src.reqState;
                    dst.readCount = src.readCount;
                    dst.lastReadDataAddr = src.lastReadDataAddr;
                    dst.rx = src.lastInput.rx;
                    dst.ry = src.lastInput.ry;
                    dst.lx = src.lastInput.lx;
                    dst.ly = src.lastInput.ly;
                    std::memcpy(dst.lastData, src.lastData, sizeof(dst.lastData));
                }
            }
        }
        return snapshot;
    }

    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry)
    {
        std::lock_guard<std::mutex> lock(g_padOverrideMutex);
        g_padOverrideEnabled = true;
        g_padOverrideState.buttons = buttons;
        g_padOverrideState.lx = lx;
        g_padOverrideState.ly = ly;
        g_padOverrideState.rx = rx;
        g_padOverrideState.ry = ry;
    }

    void clearPadOverrideState()
    {
        std::lock_guard<std::mutex> lock(g_padOverrideMutex);
        g_padOverrideEnabled = false;
        g_padOverrideState = PadInputState{};
    }
}
