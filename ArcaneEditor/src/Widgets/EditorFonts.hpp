#pragma once

struct ImFont;

namespace Arcane::Editor
{
    // Handles to the editor fonts installed by InstallEditorFonts (valid once the ImGui
    // atlas is built). `interRegular` is Fonts[0] -- the implicit UI default; push the
    // others with ImGui::PushFont / PopFont. lucide icon glyphs (ICON_LC_*) are merged
    // into every face, so icons render under whichever font is active.
    struct EditorFontSet
    {
        ImFont* interRegular = nullptr;   // default UI face (Inter)
        ImFont* roboto       = nullptr;   // alternate face (Roboto), pushable
        ImFont* brand        = nullptr;   // display wordmark (Aldo the Apache); push at a size
        ImFont* mono         = nullptr;   // monospace (JetBrains Mono, OFL); push via MonoFont
    };

    // Install the editor fonts on the CURRENT ImGui context: Inter (default) + Roboto +
    // JetBrains Mono, each with merged lucide icons. Call once in Init, after the editor
    // ImGuiLayer is up and its context is current, before the first frame. Paths resolve
    // exe-relative.
    const EditorFontSet& InstallEditorFonts(float sizePx = 16.0f);

    // The set installed by the most recent InstallEditorFonts call (all-null before that).
    const EditorFontSet& GetEditorFonts();

    // Push the mono face at the CURRENT size for this scope (PushFont(font, 0.0f),
    // imgui.h:531). Pushes NOTHING when the face is not installed (headless
    // tests install no fonts), so the font stack is unchanged either way.
    struct [[nodiscard]] MonoFont
    {
        MonoFont();
        ~MonoFont();
        MonoFont(const MonoFont&) = delete;
        MonoFont& operator=(const MonoFont&) = delete;
    private:
        bool m_pushed = false;
    };
}
