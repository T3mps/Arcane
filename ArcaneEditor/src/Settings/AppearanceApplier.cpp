#include "Settings/AppearanceApplier.hpp"

#include "Widgets/EditorTheme.hpp"

namespace Arcane::Editor
{
    void AppearanceApplier::Init(const ImGuiStyle& bootStyle)
    {
        m_baseMetrics = bootStyle;
        m_appliedTheme = EditorThemeSettings{};
        m_themeApplies = 0;
    }

    bool AppearanceApplier::UpdateTheme(const EditorThemeSettings& theme, ImGuiStyle& style)
    {
        if (SameThemeSettings(theme, m_appliedTheme))
            return false;
        Theme::SetLivePalette(ToPalette(theme));
        ApplyEditorThemeColors(style);
        m_appliedTheme = theme;
        ++m_themeApplies;
        return true;
    }
}
