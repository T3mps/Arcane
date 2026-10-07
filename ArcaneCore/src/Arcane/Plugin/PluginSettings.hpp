#pragma once

// plugin.hotReload.* (settings arc S6; inventory Part 1 "Plugin"). The game
// module watcher's debounce and the versioned-copy retry loop in PluginHost.
// The inventory rows say "Editor Dev"; this is Core code, so the struct is
// Audience::Game + Flags(Dev): Dev compiles it out of Dist, which is what the
// Editor audience was protecting (plan Open questions). Live: PluginHost reads
// the block at each poll and each reload.

#include <Arcane/Config/Settings.hpp>

#include <cstdint>

namespace Arcane
{
    struct PluginHotReloadSettings
    {
        std::int32_t settleMs    = 250;
        std::int32_t copyRetries = 5;
        std::int32_t copyRetryMs = 50;
    };

    ARC_REFLECT_TYPE(PluginHotReloadSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "plugin.hotReload", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(PluginHotReloadSettings, settleMs)
            ARC_REFLECT_ATTR(Range, 0.0, 5000.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "How long (ms) a rebuilt game module must stay unchanged before it hot-reloads. Raise it for slow linkers or antivirus scans.")
        ARC_REFLECT_FIELD(PluginHotReloadSettings, copyRetries)
            ARC_REFLECT_ATTR(Range, 1.0, 50.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Attempts to copy the game module DLL before a reload gives up (the file may still be locked).")
        ARC_REFLECT_FIELD(PluginHotReloadSettings, copyRetryMs)
            ARC_REFLECT_ATTR(Range, 1.0, 1000.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Wait (ms) between game module DLL copy attempts.")
    ARC_END_REFLECT_TYPE()
}
