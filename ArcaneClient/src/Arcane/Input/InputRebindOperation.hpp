#pragma once

#include <Arcane/Base/Api.hpp>
#include <Arcane/Input/InputActions.hpp>

#include <optional>
#include <string>

namespace Arcane
{
    enum class InputRebindState { Invalid, Waiting, Completed, Canceled, TimedOut };

    struct InputRebindResult
    {
        InputRebindState state = InputRebindState::Invalid;
        Guid bindingId;
        std::string replacementPath;
    };

    class ARCANE_API InputRebindOperation
    {
    public:
        void Begin(const Guid& bindingId, std::optional<InputDevice> eligibleDevice,
                   float timeoutSeconds, const InputSnapshot& currentSnapshot);
        void Observe(const InputSnapshot& snapshot, float dt);
        void Cancel();
        [[nodiscard]] const InputRebindResult& Result() const noexcept { return result_; }

    private:
        InputRebindResult result_;
        std::optional<InputDevice> eligibleDevice_;
        InputSnapshot previous_;
        float remaining_ = 0.0f;
    };
}
