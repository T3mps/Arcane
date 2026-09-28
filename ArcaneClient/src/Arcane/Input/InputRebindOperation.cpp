#include <Arcane/Input/InputRebindOperation.hpp>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Arcane
{
    void InputRebindOperation::Begin(const Guid& bindingId,
                                     std::optional<InputDevice> eligibleDevice,
                                     float timeoutSeconds,
                                     const InputSnapshot& currentSnapshot)
    {
        result_ = {};
        result_.bindingId = bindingId;
        if (bindingId.IsNil() || timeoutSeconds <= 0.0f || !std::isfinite(timeoutSeconds))
            return;
        result_.state = InputRebindState::Waiting;
        eligibleDevice_ = eligibleDevice;
        remaining_ = timeoutSeconds;
        previous_ = currentSnapshot;
    }

    void InputRebindOperation::Observe(const InputSnapshot& snapshot, float dt)
    {
        if (result_.state != InputRebindState::Waiting) return;
        remaining_ -= std::max(0.0f, dt);
        if (remaining_ <= 0.0f)
        {
            result_.state = InputRebindState::TimedOut;
            return;
        }

        auto complete = [&](std::string path)
        {
            result_.replacementPath = std::move(path);
            result_.state = InputRebindState::Completed;
        };

        if (!eligibleDevice_ || *eligibleDevice_ == InputDevice::Kbm)
        {
            for (uint32_t scancode = 1; scancode < 512; ++scancode)
            {
                if (!snapshot.ScancodeDown(scancode) || previous_.ScancodeDown(scancode)) continue;
                const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
                if (!name || !*name) continue;
                std::string token(name);
                std::transform(token.begin(), token.end(), token.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                complete("<Keyboard>/scancode/" + token);
                return;
            }
            static constexpr const char* mouseNames[] = {
                "leftButton", "rightButton", "middleButton", "button/4", "button/5" };
            for (uint8_t bit = 0; bit < 5; ++bit)
            {
                if ((snapshot.mouseButtons & (1u << bit)) && !(previous_.mouseButtons & (1u << bit)))
                {
                    complete(std::string("<Mouse>/") + mouseNames[bit]);
                    return;
                }
            }
        }
        if ((!eligibleDevice_ || *eligibleDevice_ == InputDevice::Gamepad) && snapshot.gamepadConnected)
        {
            static constexpr const char* buttonNames[] = {
                "buttonSouth", "buttonEast", "buttonWest", "buttonNorth",
                "dpadUp", "dpadDown", "dpadLeft", "dpadRight",
                "leftShoulder", "rightShoulder", "start", "back", "guide",
                "leftStickPress", "rightStickPress" };
            for (uint8_t bit = 0; bit < 15; ++bit)
            {
                if ((snapshot.gamepadButtons & (1u << bit)) && !(previous_.gamepadButtons & (1u << bit)))
                {
                    complete(std::string("<Gamepad>/") + buttonNames[bit]);
                    return;
                }
            }
            static constexpr const char* axisNames[] = {
                "leftStick/x", "leftStick/y", "rightStick/x", "rightStick/y",
                "leftTrigger", "rightTrigger" };
            for (uint8_t axis = 0; axis < 6; ++axis)
            {
                if (std::abs(snapshot.gamepadAxes[axis]) > 0.5f &&
                    std::abs(previous_.gamepadAxes[axis]) <= 0.5f)
                {
                    complete(std::string("<Gamepad>/") + axisNames[axis]);
                    return;
                }
            }
        }
        previous_ = snapshot;
    }

    void InputRebindOperation::Cancel()
    {
        if (result_.state == InputRebindState::Waiting)
            result_.state = InputRebindState::Canceled;
    }
}
