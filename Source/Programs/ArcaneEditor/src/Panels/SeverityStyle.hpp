#pragma once

// DiagSeverity -> {icon, colour}: the one map Problems rows, the toggles and
// the chip read (node-page phase s8.2). Header-only.

#include <Arcane/Base/Diagnostics.hpp>
#include <Widgets/EditorTheme.hpp>
#include <Widgets/EditorWidgets.hpp>
#include <Widgets/IconsLucide.h>

#include <cstddef>

namespace Arcane::Editor
{
    struct SeverityStyle
    {
        const char* icon;
        ImVec4      color;
    };

    [[nodiscard]] inline SeverityStyle StyleFor(Arcane::DiagSeverity s) noexcept
    {
        switch (s)
        {
            case Arcane::DiagSeverity::Error:   return { ICON_LC_CIRCLE_X, Theme::kError };
            case Arcane::DiagSeverity::Warning: return { ICON_LC_TRIANGLE_ALERT, Theme::kWarning };
            case Arcane::DiagSeverity::Info:    break;
        }
        return { ICON_LC_INFO, Theme::kText };
    }

    // SeverityToggle styled by StyleFor(s): the Problems and Console toolbars'
    // one call per severity. Flips `on` on click; returns the click.
    inline bool SeverityToggleFor(const char* id, Arcane::DiagSeverity s, std::size_t count, bool& on)
    {
        const SeverityStyle st = StyleFor(s);
        return SeverityToggle(id, st.icon, st.color, count, on);
    }
}
