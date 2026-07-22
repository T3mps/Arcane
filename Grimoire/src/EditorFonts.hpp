#pragma once

struct ImFont;

namespace Grimoire
{
    // Handles to the editor fonts installed by InstallEditorFonts (valid once the ImGui
    // atlas is built). `interRegular` is Fonts[0] -- the implicit UI default; push the
    // others with ImGui::PushFont / PopFont. lucide icon glyphs (ICON_LC_*) are merged
    // into every face, so icons render under whichever font is active.
    struct EditorFontSet
    {
        ImFont* interRegular = nullptr;   // default UI face (Inter)
        ImFont* roboto       = nullptr;   // alternate face (Roboto), pushable
    };

    // Install the editor fonts on the CURRENT ImGui context: Inter (default) + Roboto,
    // each with merged lucide icons. Call once in Init, after the editor ImGuiLayer is up
    // and its context is current, before the first frame. Paths resolve exe-relative.
    const EditorFontSet& InstallEditorFonts(float sizePx = 16.0f);

    // The set installed by the most recent InstallEditorFonts call (all-null before that).
    const EditorFontSet& GetEditorFonts();
}
