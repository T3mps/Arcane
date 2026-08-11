#pragma once

// Per-frame device snapshot: the seam between SDL device state and the
// action evaluator (spec: 2026-06-12-arcane-input-actions-design.md).
// Fixed-size POD, memcpy-serializable -- the contract that keeps input
// recording/replay a later feature instead of a redesign. Tests fabricate
// these directly; InputDevices::Sample fills them from SDL.

#include <cstdint>
#include <type_traits>

namespace Arcane
{
    struct InputSnapshot
    {
        static constexpr uint32_t kScancodeWords   = 8;   // 512 scancodes
        static constexpr uint32_t kMaxKeycodesDown = 16;

        // Keyboard: physical scancodes as a bitset (SDL scancode index),
        // plus the layout-mapped keycodes currently down (SDL_Keycode
        // values; capped -- 16 simultaneous keys exceeds USB rollover).
        uint64_t scancodes[kScancodeWords]      = {};
        uint32_t keycodesDown[kMaxKeycodesDown] = {};
        uint32_t keycodeCount                   = 0;

        // Mouse: bit (sdlButton - 1): LMB=bit0, RMB=bit1, MMB=bit2, X1, X2.
        uint8_t mouseButtons = 0;

        // Mouse cursor position in WINDOW pixels (top-left origin, +y down).
        float mouseX = 0.0f;
        float mouseY = 0.0f;

        // Mouse wheel: ACCUMULATED vertical scroll delta for THIS frame (SDL emits
        // wheel as discrete events, not a state, so the sampler sums event->wheel.y
        // since the last Sample and resets each frame). +y = scroll up (zoom in),
        // -y = scroll down (zoom out); 0 when the wheel did not move this frame.
        float wheelY = 0.0f;

        // First connected gamepad. Button bits follow the GamepadButton
        // order in InputActions.cpp; axes are lx ly rx ry (-1..1, +y down)
        // then lt rt (0..1).
        bool     gamepadConnected = false;
        uint16_t gamepadButtons   = 0;
        float    gamepadAxes[6]   = {};

        // UI capture: when set, keyboard-/mouse-sourced controls read as
        // released (ImGui text fields / drags). Gamepad is never captured.
        bool wantCaptureKeyboard = false;
        bool wantCaptureMouse    = false;

        bool ScancodeDown(uint32_t sc) const
        {
            return sc < 512 && (scancodes[sc >> 6] >> (sc & 63)) & 1;
        }
        void SetScancode(uint32_t sc)
        {
            if (sc < 512) scancodes[sc >> 6] |= (uint64_t)1 << (sc & 63);
        }
        bool KeycodeDown(uint32_t kc) const
        {
            for (uint32_t i = 0; i < keycodeCount; ++i)
                if (keycodesDown[i] == kc) return true;
            return false;
        }
        void AddKeycode(uint32_t kc)
        {
            if (keycodeCount < kMaxKeycodesDown)
                keycodesDown[keycodeCount++] = kc;
        }
    };

    static_assert(std::is_trivially_copyable_v<InputSnapshot>,
                  "snapshot must stay POD (replay/serialization contract)");
}
