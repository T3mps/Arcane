#include <Arcane/Input/InputRebindOperation.hpp>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Arcane
{
    namespace
    {
        // SDL_SCANCODE_LCTRL..SDL_SCANCODE_RGUI (224..231) -> the LOVE token the
        // path compiler's LoveToSdlName table round-trips ("<Keyboard>/scancode/
        // lshift" compiles to SDL_SCANCODE_LSHIFT). nullptr = not a modifier.
        const char* ModifierToken(uint32_t scancode)
        {
            switch (scancode)
            {
            case SDL_SCANCODE_LCTRL:  return "lctrl";
            case SDL_SCANCODE_LSHIFT: return "lshift";
            case SDL_SCANCODE_LALT:   return "lalt";
            case SDL_SCANCODE_LGUI:   return "lgui";
            case SDL_SCANCODE_RCTRL:  return "rctrl";
            case SDL_SCANCODE_RSHIFT: return "rshift";
            case SDL_SCANCODE_RALT:   return "ralt";
            case SDL_SCANCODE_RGUI:   return "rgui";
            default:                  return nullptr;
            }
        }
        bool IsModifierScancode(uint32_t scancode) { return ModifierToken(scancode) != nullptr; }
        bool AnyModifierDown(const InputSnapshot& snap)
        {
            for (uint32_t sc = SDL_SCANCODE_LCTRL; sc <= SDL_SCANCODE_RGUI; ++sc)
                if (snap.ScancodeDown(sc)) return true;
            return false;
        }
    }

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
        heldModifiers_.clear();
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
        // The held modifiers as a chord prefix, press order, "" when none.
        auto heldPrefix = [&]
        {
            std::string prefix;
            for (uint32_t sc : heldModifiers_)
                prefix += std::string("<Keyboard>/scancode/") + ModifierToken(sc) + "+";
            return prefix;
        };

        if (!eligibleDevice_ || *eligibleDevice_ == InputDevice::Kbm)
        {
            // A key or button the UI has claimed this frame (an ImGui text
            // field, a hovered widget) is not a capture -- the same rule the
            // evaluator applies to wantCaptureKeyboard/Mouse.
            if (!snapshot.wantCaptureKeyboard)
            {
                // First pass, modifiers: every one newly down joins the held
                // list; every held one now up leaves it and, when it was the
                // last held and no other modifier is down, completes bare.
                for (uint32_t sc = SDL_SCANCODE_LCTRL; sc <= SDL_SCANCODE_RGUI; ++sc)
                    if (snapshot.ScancodeDown(sc) && !previous_.ScancodeDown(sc))
                        heldModifiers_.push_back(sc);
                for (auto it = heldModifiers_.begin(); it != heldModifiers_.end();)
                {
                    if (snapshot.ScancodeDown(*it)) { ++it; continue; }
                    const uint32_t released = *it;
                    it = heldModifiers_.erase(it);
                    if (heldModifiers_.empty() && !AnyModifierDown(snapshot))
                    {
                        complete(std::string("<Keyboard>/scancode/") + ModifierToken(released));
                        return;
                    }
                }
                // Second pass: the first NON-modifier newly down completes,
                // prefixed by whatever modifiers are held.
                for (uint32_t scancode = 1; scancode < 512; ++scancode)
                {
                    if (IsModifierScancode(scancode)) continue;
                    if (!snapshot.ScancodeDown(scancode) || previous_.ScancodeDown(scancode)) continue;
                    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
                    if (!name || !*name) continue;
                    std::string token(name);
                    std::transform(token.begin(), token.end(), token.begin(),
                        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    complete(heldPrefix() + "<Keyboard>/scancode/" + token);
                    return;
                }
            }
            static constexpr const char* mouseNames[] = {
                "leftButton", "rightButton", "middleButton", "button/4", "button/5" };
            for (uint8_t bit = 0; bit < 5; ++bit)
            {
                if (!snapshot.wantCaptureMouse
                    && (snapshot.mouseButtons & (1u << bit)) && !(previous_.mouseButtons & (1u << bit)))
                {
                    // "<Keyboard>/scancode/lshift+<Mouse>/leftButton" compiles:
                    // CompilePath splits on '+' and each part is a simple path.
                    complete(heldPrefix() + "<Mouse>/" + mouseNames[bit]);
                    return;
                }
            }
        }
        if ((!eligibleDevice_ || *eligibleDevice_ == InputDevice::Gamepad) && snapshot.gamepadConnected)
        {
            // Gamepad completions ignore held keyboard modifiers.
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
