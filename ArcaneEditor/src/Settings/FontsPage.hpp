#pragma once

// Preferences > Appearance > Fonts and Scale (settings arc S4, spec s7.3).
// The page scans the family list (the bundled families plus the user's
// Fonts folder; Refresh re-scans) and hands it to the node's "font" rows,
// which draw as the window's standard rows below the page -- a family
// combo with provenance, Clear override, reset and the window's undo --
// beside editor.ui.fontSize / scale / followDpi; then a sample in both faces.

#include "Widgets/EditorFonts.hpp"

#include <Arcane/Config/CVarTypes.hpp>

#include <filesystem>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    // The tree node the page and the editor.ui.* cvars share.
    inline constexpr std::string_view kFontsPageCategory = "Appearance/Fonts and Scale";

    struct FontsPageState
    {
        std::filesystem::path exeDir;         // empty = this process's exe dir
        std::filesystem::path userFontsDir;   // empty = Paths EditorUserDir/Fonts (none when exeDir is faked)
        std::vector<EditorFontFamily> families;
        bool listDirty = true;                // re-scan on the next draw (Refresh button)
    };

    // editor.ui.<field> at the EditorUser rung + Publish (a direct write: no
    // window undo; the page's rows go through the standard row machinery).
    bool SetUiSetting(std::string_view field, const Arcane::CVarValue& value);
    void DrawFontsPage(void* user);                                              // user = FontsPageState*
}
