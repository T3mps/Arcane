#pragma once

// The runtime host's debug HUD (settings arc S6-25; inventory R4): on in
// Debug/Release, where the goldens are captured, off in Dist, where a shipped
// game must not show engine stats. A player may still turn it on.

#include <Arcane/Config/Settings.hpp>

namespace Arcane
{
#if defined(ARC_BUILD_DIST)
    inline constexpr bool kRuntimeHudDefault = false;
#else
    inline constexpr bool kRuntimeHudDefault = true;
#endif

    struct RuntimeHudSettings
    {
        bool show = kRuntimeHudDefault;
    };

    ARC_REFLECT_TYPE(RuntimeHudSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "runtime.hud", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(RuntimeHudSettings, show)
            ARC_REFLECT_ATTR(Tooltip, "Show the runtime's stats window (backend, quads, draws, visibility). Off by default "
                                      "in shipped builds.")
    ARC_END_REFLECT_TYPE()
}
