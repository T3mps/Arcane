#include <Arcane/Input/InputDevices.hpp>

#include <Arcane/Base/Log.hpp>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cstdint>

// SDL3 signature reference (verified against vcpkg SDL3 headers):
//   const bool* SDL_GetKeyboardState(int* numkeys)
//     -- SDL3 changed return from const Uint8* (SDL2) to const bool*. CONFIRMED.
//   SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode, SDL_Keymod, bool key_event)
//     -- SDL3 added Keymod + key_event params vs SDL2. CONFIRMED.
//   SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y)
//     -- SDL3 uses float coords vs SDL2 int. Return type is Uint32 typedef. CONFIRMED.
//   SDL_JoystickID* SDL_GetGamepads(int* count) -- allocates; caller SDL_free. CONFIRMED.
//   SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID), bool SDL_GamepadConnected(SDL_Gamepad*),
//   bool SDL_GetGamepadButton(SDL_Gamepad*, SDL_GamepadButton),
//   Sint16 SDL_GetGamepadAxis(SDL_Gamepad*, SDL_GamepadAxis),
//   void SDL_CloseGamepad(SDL_Gamepad*) -- all CONFIRMED from SDL3 gamepad.h.
//
// SDL3 gamepad button enum order deviates from the token table in InputActions.cpp:
//   SDL3 enum: SOUTH=0 EAST=1 WEST=2 NORTH=3 BACK=4 GUIDE=5 START=6
//              LEFT_STICK=7 RIGHT_STICK=8 LEFT_SHOULDER=9 RIGHT_SHOULDER=10
//              DPAD_UP=11 DPAD_DOWN=12 DPAD_LEFT=13 DPAD_RIGHT=14
//   Token table (InputActions.cpp GamepadButtonToken bit order):
//              buttonSouth=0 buttonEast=1 buttonWest=2 buttonNorth=3
//              dpadUp=4 dpadDown=5 dpadLeft=6 dpadRight=7
//              leftShoulder=8 rightShoulder=9 start=10 back=11 guide=12
//              leftStickPress=13 rightStickPress=14
//   The sampler uses an explicit mapping table (kButtonMap below) to bridge
//   the two orderings rather than iterating the SDL enum directly.
//
// Mouse bit mapping: SDL_BUTTON_LMASK(1<<0)/RMASK(1<<2)/MMASK(1<<1) are NOT
//   in LMB=0/RMB=1/MMB=2 snapshot order, so each mask is checked explicitly.

namespace Arcane
{
    namespace
    {
        // SDL_GAMEPAD_BUTTON_* values mapped to token-table bit positions 0..14.
        // Must stay in sync with GamepadButtonToken() in InputActions.cpp.
        const SDL_GamepadButton kButtonMap[15] = {
            SDL_GAMEPAD_BUTTON_SOUTH,           // bit 0  = buttonSouth
            SDL_GAMEPAD_BUTTON_EAST,            // bit 1  = buttonEast
            SDL_GAMEPAD_BUTTON_WEST,            // bit 2  = buttonWest
            SDL_GAMEPAD_BUTTON_NORTH,           // bit 3  = buttonNorth
            SDL_GAMEPAD_BUTTON_DPAD_UP,         // bit 4  = dpadUp
            SDL_GAMEPAD_BUTTON_DPAD_DOWN,       // bit 5  = dpadDown
            SDL_GAMEPAD_BUTTON_DPAD_LEFT,       // bit 6  = dpadLeft
            SDL_GAMEPAD_BUTTON_DPAD_RIGHT,      // bit 7  = dpadRight
            SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,   // bit 8  = leftShoulder
            SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,  // bit 9  = rightShoulder
            SDL_GAMEPAD_BUTTON_START,           // bit 10 = start
            SDL_GAMEPAD_BUTTON_BACK,            // bit 11 = back
            SDL_GAMEPAD_BUTTON_GUIDE,           // bit 12 = guide
            SDL_GAMEPAD_BUTTON_LEFT_STICK,      // bit 13 = leftStickPress
            SDL_GAMEPAD_BUTTON_RIGHT_STICK,     // bit 14 = rightStickPress
        };

        class InputDevicesImpl final : public InputDevices
        {
        public:
            InputDevicesImpl()
            {
                // I2: SDL_WasInit returns the mask of already-initialised subsystems.
                // Video must be up (Window created) before sampling keyboard/mouse.
                if (!SDL_WasInit(SDL_INIT_VIDEO))
                    ARC_WARN("InputDevices created before SDL video is up (create a Window first); keyboard/mouse sampling may be empty.");

                // I1: Gamepad subsystem: ref-counted init; paired QuitSubSystem in dtor.
                // SDL_INIT_GAMEPAD implies SDL_INIT_JOYSTICK per SDL3 init.h.
                m_gamepadSubsystemOk = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
                if (!m_gamepadSubsystemOk)
                    ARC_WARN("InputDevices: SDL_INIT_GAMEPAD failed: {}", SDL_GetError());
            }

            ~InputDevicesImpl() override
            {
                if (m_gamepad)
                {
                    SDL_CloseGamepad(m_gamepad);
                    m_gamepad = nullptr;
                }
                if (m_gamepadSubsystemOk)
                    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
            }

            InputSnapshot Sample(bool captureKeyboard, bool captureMouse) override
            {
                InputSnapshot snap;

                // Capture flags stored verbatim; suppression happens in the evaluator.
                snap.wantCaptureKeyboard = captureKeyboard;
                snap.wantCaptureMouse    = captureMouse;

                // --- Keyboard ---
                // SDL3: SDL_GetKeyboardState returns const bool* (not Uint8* as in SDL2).
                int n = 0;
                const bool* state = SDL_GetKeyboardState(&n);
                if (state)
                {
                    int limit = std::min(n, (int)InputSnapshot::kScancodeWords * 64);
                    for (int sc = 0; sc < limit; ++sc)
                    {
                        if (state[sc])
                        {
                            snap.SetScancode((uint32_t)sc);
                            // SDL3: SDL_GetKeyFromScancode(scancode, modstate, key_event).
                            // Pass SDL_KMOD_NONE + false (no modifier context for name lookup).
                            SDL_Keycode kc = SDL_GetKeyFromScancode(
                                (SDL_Scancode)sc, SDL_KMOD_NONE, false);
                            if (kc != SDLK_UNKNOWN)
                                snap.AddKeycode((uint32_t)kc);
                        }
                    }
                }

                // --- Mouse ---
                // SDL3: SDL_GetMouseState(float* x, float* y) -> SDL_MouseButtonFlags (Uint32).
                // Cursor position is always the real window-space position; we do NOT zero it
                // under wantCaptureMouse (the button STATE is what suppression keys off, and a
                // cursor coordinate stays valid/useful even while ImGui owns the click).
                float mouseX = 0.0f, mouseY = 0.0f;
                SDL_MouseButtonFlags flags = SDL_GetMouseState(&mouseX, &mouseY);
                snap.mouseX = mouseX;
                snap.mouseY = mouseY;
                // Map SDL button masks to snapshot bit positions (LMB=0, RMB=1, MMB=2, X1=3, X2=4).
                // SDL masks: LMASK=1<<0, MMASK=1<<1, RMASK=1<<2, X1MASK=1<<3, X2MASK=1<<4.
                // Snapshot order: leftButton->bit0, rightButton->bit1, middleButton->bit2.
                // Note: SDL_BUTTON_RMASK != bit1 (it is 1<<(3-1)=1<<2=4), so explicit mapping required.
                uint8_t mouseButtons = 0;
                if (flags & SDL_BUTTON_LMASK)  mouseButtons |= (1u << 0);  // leftButton  = bit 0
                if (flags & SDL_BUTTON_RMASK)  mouseButtons |= (1u << 1);  // rightButton = bit 1
                if (flags & SDL_BUTTON_MMASK)  mouseButtons |= (1u << 2);  // middleButton= bit 2
                if (flags & SDL_BUTTON_X1MASK) mouseButtons |= (1u << 3);  // X1          = bit 3
                if (flags & SDL_BUTTON_X2MASK) mouseButtons |= (1u << 4);  // X2          = bit 4
                snap.mouseButtons = mouseButtons;

                // --- Gamepad (lazy, first connected device) ---
                // If cached handle is no longer connected, close it.
                if (m_gamepad && !SDL_GamepadConnected(m_gamepad))
                {
                    SDL_CloseGamepad(m_gamepad);
                    m_gamepad = nullptr;
                }
                // If no handle, try to open the first available gamepad.
                if (!m_gamepad)
                {
                    int count = 0;
                    SDL_JoystickID* ids = SDL_GetGamepads(&count);
                    if (ids)
                    {
                        if (count > 0)
                            m_gamepad = SDL_OpenGamepad(ids[0]);
                        SDL_free(ids);
                    }
                }

                if (m_gamepad)
                {
                    snap.gamepadConnected = true;

                    // Button bits 0..14 in token-table order (see kButtonMap above).
                    uint16_t buttons = 0;
                    for (int bit = 0; bit < 15; ++bit)
                    {
                        if (SDL_GetGamepadButton(m_gamepad, kButtonMap[bit]))
                            buttons |= (uint16_t)(1u << bit);
                    }
                    snap.gamepadButtons = buttons;

                    // Axes 0..3: left/right stick X/Y, clamped to [-1, 1].
                    // Axes 4..5: left/right trigger, clamped to [0, 1].
                    // Sint16 range is -32768..32767; divide by 32767.0f.
                    // 32767.0f: +32767 maps exactly to +1.0; -32768/-32767.0f = -1.00003 is clamped below.
                    const SDL_GamepadAxis kAxes[6] = {
                        SDL_GAMEPAD_AXIS_LEFTX,
                        SDL_GAMEPAD_AXIS_LEFTY,
                        SDL_GAMEPAD_AXIS_RIGHTX,
                        SDL_GAMEPAD_AXIS_RIGHTY,
                        SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
                        SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                    };
                    for (int i = 0; i < 4; ++i)
                    {
                        float v = SDL_GetGamepadAxis(m_gamepad, kAxes[i]) / 32767.0f;
                        if (v < -1.0f) v = -1.0f;
                        if (v >  1.0f) v =  1.0f;
                        snap.gamepadAxes[i] = v;
                    }
                    for (int i = 4; i < 6; ++i)
                    {
                        float v = SDL_GetGamepadAxis(m_gamepad, kAxes[i]) / 32767.0f;
                        if (v < 0.0f) v = 0.0f;
                        if (v > 1.0f) v = 1.0f;
                        snap.gamepadAxes[i] = v;
                    }
                }

                return snap;
            }

        private:
            SDL_Gamepad* m_gamepad          = nullptr;
            bool         m_gamepadSubsystemOk = false;
        };

    }  // anonymous namespace

    std::unique_ptr<InputDevices> InputDevices::Create()
    {
        return std::make_unique<InputDevicesImpl>();
    }

}  // namespace Arcane
