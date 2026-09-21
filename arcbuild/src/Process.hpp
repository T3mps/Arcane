#pragma once

#include "Output.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace arcbuild
{
    class ProcessRunner
    {
    public:
        explicit ProcessRunner(
            IOutput& output)
            : output_(output)
        {
        }

        [[nodiscard]]
        std::optional<int> RunStreaming(
            const std::string& commandLine,
            std::string_view prefix) const;

    private:
        IOutput& output_;
    };
}
