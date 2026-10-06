#pragma once

// diagnostics.* (settings arc S2). Declared in Core, where Diagnostics lives
// and where S6 adds its rows (hangSeconds, reporter, minidump kind, ...).
// ArcaneClient reads drawMarkers.

#include <Arcane/Config/Settings.hpp>

namespace Arcane
{
    struct DiagnosticsSettings
    {
        bool drawMarkers = false;
    };

    ARC_REFLECT_TYPE(DiagnosticsSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "diagnostics", SettingScope::Project, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(DiagnosticsSettings, drawMarkers)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Per-draw GPU markers for PIX/RenderDoc. Pass-level scopes stay on.")
    ARC_END_REFLECT_TYPE()
}
