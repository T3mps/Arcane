#pragma once

// editor.ui.* (settings arc S4, spec s7.3): fonts and scale, Preferences >
// Appearance, machine-wide. S6 adds the inventory's other editor.ui.* rows.

#include <string>

namespace Arcane::Editor
{
    struct EditorUiSettings
    {
        std::string fontFamily = "Inter";
        std::string monoFontFamily = "JetBrains Mono";
        float fontSize = 16.0f;
        float scale = 1.0f;
        bool followDpi = false;
    };

    // ui.scale x (followDpi ? displayScale : 1), clamped to [0.5, 4].
    [[nodiscard]] float EffectiveUiScale(const EditorUiSettings& ui, float displayScale) noexcept;
}
