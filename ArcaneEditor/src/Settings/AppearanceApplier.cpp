#include "Settings/AppearanceApplier.hpp"

#include "Widgets/EditorTheme.hpp"
#include "Widgets/UiMetrics.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace Arcane::Editor
{
    void AppearanceApplier::Init(const ImGuiStyle& bootStyle)
    {
        m_baseMetrics = bootStyle;
        m_appliedTheme = EditorThemeSettings{};
        m_themeApplies = 0;
        m_appliedUi = EditorUiSettings{};
        m_appliedScale = 1.0f;
        m_pendingFonts.reset();
        m_metricsApplies = 0;
        m_fontRebuilds = 0;
        Ui::SetMetrics({});
    }

    bool AppearanceApplier::UpdateTheme(const EditorThemeSettings& theme, ImGuiStyle& style)
    {
        if (SameThemeSettings(theme, m_appliedTheme))
            return false;
        Theme::SetLivePalette(ToPalette(theme));
        ApplyEditorThemeColors(style, theme.unfocusedOverlineAlpha);
        m_appliedTheme = theme;
        ++m_themeApplies;
        return true;
    }

    bool AppearanceApplier::UpdateUi(const EditorUiSettings& ui, float displayScale, ImGuiStyle& style)
    {
        bool changed = false;
        if (ui.fontFamily != m_appliedUi.fontFamily || ui.monoFontFamily != m_appliedUi.monoFontFamily
            || ui.fontSize != m_appliedUi.fontSize)
        {
            m_pendingFonts = ui;
            changed = true;
        }
        const float scale = EffectiveUiScale(ui, displayScale);
        if (scale != m_appliedScale)
        {
            Recompose(style, scale);
            m_appliedScale = scale;
            ++m_metricsApplies;
            changed = true;
        }
        m_appliedUi = ui;
        Ui::SetMetrics(Ui::Metrics{ scale, ui.fontSize });
        return changed;
    }

    std::optional<EditorUiSettings> AppearanceApplier::TakeFontRebuild()
    {
        std::optional<EditorUiSettings> out = std::exchange(m_pendingFonts, std::nullopt);
        if (out)
            ++m_fontRebuilds;
        return out;
    }

    void AppearanceApplier::Recompose(ImGuiStyle& style, float scale) const
    {
        ImGuiStyle next = m_baseMetrics;   // the unscaled boot metrics
        std::copy(std::begin(style.Colors), std::end(style.Colors), std::begin(next.Colors));   // the live theme stays
        next.FontSizeBase = style.FontSizeBase;
        next.FontScaleDpi = style.FontScaleDpi;
        if (scale != 1.0f)
            next.ScaleAllSizes(scale);
        next.FontScaleMain = scale;
        style = next;
    }
}
