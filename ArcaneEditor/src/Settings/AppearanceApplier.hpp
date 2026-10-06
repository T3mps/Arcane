#pragma once

// AppearanceApplier (settings arc S4, spec s7.1/s7.3): turns the published
// editor.theme.* (and, from S4-15, editor.ui.*) blocks into the editor's
// ImGui style. Runs once per frame OUTSIDE an ImGui frame (after the cvar
// publish barrier). It compares against what it last applied, so an
// unchanged block costs one memcmp and the boot defaults apply nothing.

#include "Settings/EditorThemeSettings.hpp"

#include <imgui.h>

namespace Arcane::Editor
{
    class AppearanceApplier
    {
    public:
        // Records the boot style (ApplyEditorTheme already ran): Dark tokens.
        void Init(const ImGuiStyle& bootStyle);

        // True when `theme` differs from the last applied block: the live
        // palette was swapped and the style's COLOURS re-applied (metrics untouched).
        bool UpdateTheme(const EditorThemeSettings& theme, ImGuiStyle& style);

        [[nodiscard]] int ThemeApplies() const noexcept { return m_themeApplies; }

    private:
        EditorThemeSettings m_appliedTheme{};
        ImGuiStyle m_baseMetrics{};   // the boot style; S4-15 recomposes the UI scale from it
        int m_themeApplies = 0;
    };
}
