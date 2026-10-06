#pragma once

// Preferences > Appearance > Fonts and Scale (settings arc S4, spec s7.3).
// The page draws the UI and monospace family pickers (the bundled families
// plus the user's Fonts folder) and a sample; it owns its node's "font"
// rows. editor.ui.fontSize / scale / followDpi sit on the same node and draw
// as the window's standard rows below the page (provenance, reset, undo).

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

    bool SetUiSetting(std::string_view field, const Arcane::CVarValue& value);   // editor.ui.<field>, EditorUser + Publish
    void DrawFontsPage(void* user);                                              // user = FontsPageState*
}
