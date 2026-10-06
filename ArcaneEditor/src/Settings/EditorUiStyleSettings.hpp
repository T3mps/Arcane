#pragma once

// editor.ui.* style metrics and density (settings S6-28, spec s16.11; the
// inventory's Fonts/Style, Asset Browser and Inspector SETTING rows):
// Preferences > Appearance > Style and Density, machine-wide, Live. Its own
// struct and tree path, beside EditorUiSettings (fonts and scale), whose
// header keeps that page to fonts and scale.
//
// A plain struct with no Arcane include on purpose: ApplyEditorTheme
// (Widgets/EditorTheme.hpp, header-only) takes it, and ArcaneCrashReporter
// reaches that header through a bare ArcaneEditor/src include path with no
// cvar registry of its own -- it passes EditorUiStyleSettings{}.
//
// Every default is the value the editor drew before the sweep, so the
// defaults are byte-identical (s10.2). Pixel fields are BASES at scale 1.0
// and font 16: their drawn size goes through Ui::Px / Ui::TextPx.

namespace Arcane::Editor
{
    struct EditorUiStyleSettings
    {
        // ImGuiStyle metrics ApplyEditorTheme writes (EditorTheme.hpp states why each differs from stock).
        float frameBorderSize = 1.0f;
        bool  dockNodeCloseButton = false;
        float tabOverlineSize = 2.0f;
        float disabledAlpha = 0.45f;
        float tabRounding = 2.0f;
        // Density.
        float tableRowHeight = 24.0f;    // the asset tables' row (Ui::TextPx)
        float assetRowThumbPx = 18.0f;   // a row's thumb cell (Ui::TextPx)
        float assetRefThumbPx = 20.0f;   // an asset reference field's thumb (Ui::TextPx)
        float propertyDragSpeed = 0.01f; // PropertyGrid FloatRow / VecRow default drag speed
        int   intStep = 1;               // PropertyGrid IntRow step button
        int   intStepFast = 100;         // ... with Ctrl held
    };
}
