#pragma once

// editor.ui.* (settings arc S4, spec s7.3): fonts and scale, Preferences >
// Appearance > Fonts and Scale (the type's tree path, shared with the S4-17
// page), machine-wide. S6 adds the inventory's other editor.ui.* rows; a row
// that is not about fonts or scale belongs in a struct with its own path.

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
