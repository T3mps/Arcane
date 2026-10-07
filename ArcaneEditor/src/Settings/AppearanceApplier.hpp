#pragma once

// AppearanceApplier (settings arc S4, spec s7.1/s7.3): turns the published
// editor.theme.* and editor.ui.* blocks into the editor's
// ImGui style. Runs once per frame OUTSIDE an ImGui frame (after the cvar
// publish barrier). It compares against what it last applied, so an
// unchanged block costs one memcmp and the boot defaults apply nothing.

#include "Settings/EditorThemeSettings.hpp"
#include "Settings/EditorUiSettings.hpp"
#include "Settings/EditorUiStyleSettings.hpp"

#include <imgui.h>

#include <optional>

namespace Arcane::Editor
{
    class AppearanceApplier
    {
    public:
        // Records the boot style (ApplyEditorTheme already ran): Dark tokens,
        // and the editor.ui style block it was applied with.
        void Init(const ImGuiStyle& bootStyle, const EditorUiStyleSettings& bootUiStyle = EditorUiStyleSettings{});

        // True when `theme` differs from the last applied block: the live
        // palette was swapped and the style's COLOURS re-applied (metrics untouched).
        bool UpdateTheme(const EditorThemeSettings& theme, ImGuiStyle& style);

        [[nodiscard]] int ThemeApplies() const noexcept { return m_themeApplies; }

        // Per frame, before UpdateUi: true when the editor.ui style metrics
        // (EditorUiStyleSettings) differ from the last applied block. They are
        // written into the unscaled base and the style recomposed at the
        // current UI scale (colours untouched; settings S6-28).
        bool UpdateStyle(const EditorUiStyleSettings& uiStyle, ImGuiStyle& style);
        [[nodiscard]] int StyleApplies() const noexcept { return m_styleApplies; }

        // Per frame, after UpdateTheme: a scale change recomposes the style
        // from the boot metrics now; a font change is QUEUED and handed out by
        // the next frame's TakeFontRebuild (outside the ImGui frame). Also
        // publishes Ui::Metrics. True when anything changed.
        bool UpdateUi(const EditorUiSettings& ui, float displayScale, ImGuiStyle& style);
        [[nodiscard]] std::optional<EditorUiSettings> TakeFontRebuild();
        [[nodiscard]] int MetricsApplies() const noexcept { return m_metricsApplies; }
        [[nodiscard]] int FontRebuilds() const noexcept { return m_fontRebuilds; }

    private:
        void Recompose(ImGuiStyle& style, float scale) const;

        EditorThemeSettings m_appliedTheme{};
        ImGuiStyle m_baseMetrics{};   // the unscaled boot style; UpdateUi recomposes the UI scale from it
        int m_themeApplies = 0;
        EditorUiStyleSettings m_appliedStyle{};
        int m_styleApplies = 0;
        EditorUiSettings m_appliedUi{};
        float m_appliedScale = 1.0f;
        std::optional<EditorUiSettings> m_pendingFonts;
        int m_metricsApplies = 0;
        int m_fontRebuilds = 0;
    };
}
