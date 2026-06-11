#pragma once

// Arcane engine version. Bumped manually at milestone boundaries for now;
// the plugin ABI version (M4) is a separate constant by design.

namespace Arcane
{
    inline constexpr int kVersionMajor = 0;
    inline constexpr int kVersionMinor = 1;

    inline const char* VersionString() { return "Arcane 0.1 (M0)"; }
}
