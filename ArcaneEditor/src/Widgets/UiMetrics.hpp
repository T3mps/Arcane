#pragma once

// The editor's UI metrics (settings arc S4, spec s7.3, decision s16.11): one
// editor.ui.scale and one editor.ui.fontSize; every hard pixel size is DERIVED
// from them. Set once per frame by AppearanceApplier::UpdateUi. At scale 1.0
// and font 16 every helper returns its argument exactly.

#include <Arcane/Core/Constant.hpp>

namespace Arcane::Editor::Ui
{
    ARC_CONSTANT("a change would be a bug: the font size every base px / font px was tuned at; Ui::FontPx divides by it")
    inline constexpr float kReferenceFontSize = 16.0f;   // the size the editor's pixel constants were tuned at

    struct Metrics
    {
        float scale = 1.0f;      // editor.ui.scale x (followDpi ? monitor DPI : 1)
        float fontSize = 16.0f;  // editor.ui.fontSize
    };

    namespace Detail { inline Metrics g_metrics{}; }

    [[nodiscard]] inline const Metrics& Current() noexcept { return Detail::g_metrics; }
    inline void SetMetrics(const Metrics& m) noexcept { Detail::g_metrics = m; }

    // A layout pixel size (padding, gap, width) at the current UI scale.
    [[nodiscard]] inline float Px(float base) noexcept { return base * Detail::g_metrics.scale; }
    // A PushFont size relative to the UI font (FontScaleMain applies the scale on top).
    [[nodiscard]] inline float FontPx(float base) noexcept { return base * (Detail::g_metrics.fontSize / kReferenceFontSize); }
    // A pixel extent that tracks text (a pill's line, a row): both factors.
    [[nodiscard]] inline float TextPx(float base) noexcept { return Px(FontPx(base)); }

    struct [[nodiscard]] ScopedMetrics
    {
        explicit ScopedMetrics(const Metrics& m) noexcept : m_saved(Detail::g_metrics) { Detail::g_metrics = m; }
        ~ScopedMetrics() { Detail::g_metrics = m_saved; }
        ScopedMetrics(const ScopedMetrics&) = delete;
        ScopedMetrics& operator=(const ScopedMetrics&) = delete;
    private:
        Metrics m_saved;
    };
}
