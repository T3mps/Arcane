#pragma once

#include <Arcane/Core/Constant.hpp>

// Arcane engine version. Bumped manually at milestone boundaries for now;
// the plugin ABI version (M4) is a separate constant by design.

namespace Arcane
{
    ARC_CONSTANT("a change would be a bug: the engine release identity (major); a release bumps it, nothing tunes it")
    inline constexpr int kVersionMajor = 0;
    ARC_CONSTANT("a change would be a bug: the engine release identity (minor); a release bumps it, nothing tunes it")
    inline constexpr int kVersionMinor = 1;

    inline const char* VersionString() { return "Arcane 0.1 (M5)"; }
}
