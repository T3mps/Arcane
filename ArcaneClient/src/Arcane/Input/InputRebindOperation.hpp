#pragma once

#include <Arcane/Base/Api.hpp>
#include <Arcane/Input/InputActions.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Arcane
{
    enum class InputRebindState { Invalid, Waiting, Completed, Canceled, TimedOut };

    struct InputRebindResult
    {
        InputRebindState state = InputRebindState::Invalid;
        Guid bindingId;
        std::string replacementPath;
    };

    class ARC_API InputRebindOperation
    {
    public:
        void Begin(const Guid& bindingId, std::optional<InputDevice> eligibleDevice,
                   float timeoutSeconds, const InputSnapshot& currentSnapshot);
        void Observe(const InputSnapshot& snapshot, float dt);
        void Cancel();
        [[nodiscard]] const InputRebindResult& Result() const noexcept { return result_; }
        // Seconds left in the capture; 0 once it is no longer Waiting.
        [[nodiscard]] float Remaining() const noexcept { return result_.state == InputRebindState::Waiting ? remaining_ : 0.0f; }

    private:
        InputRebindResult result_;
        std::optional<InputDevice> eligibleDevice_;
        InputSnapshot previous_;
        float remaining_ = 0.0f;
        // Modifier scancodes newly pressed during THIS capture, in press order.
        // The first non-modifier completes with them prefixed as a "+<" chord;
        // a modifier released while it is the last one held completes bare.
        // Modifiers already down at Begin are never seeded (the initiating
        // control must be released and re-pressed -- the existing rule).
        std::vector<uint32_t> heldModifiers_;
    };
}
