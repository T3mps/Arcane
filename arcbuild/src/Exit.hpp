#pragma once

#include <optional>

namespace arcbuild
{
    constexpr int kExitOk           = 0;
    constexpr int kExitRefused      = 2;
    constexpr int kExitProbeRebuild = 3;

    [[nodiscard]]
    inline int ExitFromChild(
        std::optional<int> childExit)
    {
        return childExit
            ? *childExit
            : kExitRefused;
    }
}