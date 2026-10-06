#pragma once

// One UI scale for every hard pixel size (spec s16.11): a size is a marked
// base times the UI scale. UiPx IS Ui::Px (Widgets/UiMetrics.hpp, S4): the
// effective editor.ui.scale (x the monitor DPI under followDpi) that
// AppearanceApplier publishes once per frame -- one scale, never a second
// read of the cvar. At the default scale (1.0) UiPx(b) == b exactly.
//
// UiStyle() is the published editor.ui style/density block
// (Settings/EditorUiStyleSettings.hpp) for the widgets that read it per draw.

#include "Settings/EditorUiStyleSettings.hpp"
#include "Widgets/UiMetrics.hpp"

#include <Arcane/Config/Settings.hpp>

namespace Arcane::Editor
{
    [[nodiscard]] inline float UiPx(float base) noexcept { return Ui::Px(base); }

    [[nodiscard]] inline const EditorUiStyleSettings& UiStyle() { return Arcane::Settings<EditorUiStyleSettings>(); }
}
