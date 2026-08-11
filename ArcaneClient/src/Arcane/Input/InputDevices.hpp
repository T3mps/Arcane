#pragma once

// SDL-facing sampler: fills an InputSnapshot from SDL keyboard/mouse/
// gamepad state. Owns the gamepad handle (lazy re-scan on disconnect,
// oracle: Input.lua's cachedPad). Capture flags are passed in by the host
// (typically ImGuiLayer::WantCaptureKeyboard/Mouse) -- this module never
// touches ImGui. Requires the SDL video subsystem (a created Window).

#include <Arcane/Base/Api.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include <memory>

namespace Arcane
{
    class ARCANE_API InputDevices
    {
    public:
        static std::unique_ptr<InputDevices> Create();
        virtual ~InputDevices() = default;

        virtual InputSnapshot Sample(bool captureKeyboard,
                                     bool captureMouse) = 0;
    };
}
