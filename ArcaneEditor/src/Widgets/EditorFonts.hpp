#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct ImFont;

namespace Arcane::Editor
{
    // Handles to the editor fonts installed by InstallEditorFonts (valid once the ImGui
    // atlas is built). `interRegular` is Fonts[0] -- the implicit UI default; push the
    // others with ImGui::PushFont / PopFont. lucide icon glyphs (ICON_LC_*) are merged
    // into every face, so icons render under whichever font is active.
    struct EditorFontSet
    {
        ImFont* interRegular = nullptr;   // the UI face (editor.ui.fontFamily; Inter by default)
        ImFont* roboto       = nullptr;   // alternate face (Roboto), pushable
        ImFont* brand        = nullptr;   // display wordmark (Aldo the Apache); push at a size
        ImFont* mono         = nullptr;   // monospace (editor.ui.monoFontFamily; JetBrains Mono by default); push via MonoFont
    };

    struct EditorFontFamily
    {
        std::string name;              // what editor.ui.fontFamily holds
        std::filesystem::path file;
        bool bundled = true;
    };
    // The bundled Inter, Roboto, JetBrains Mono (in that order), then every .ttf/.otf
    // in `userFontsDir` by file stem, sorted. `userFontsDir` may be empty or absent.
    // A user stem equal to a bundled family's name is named "user:<stem>" (S4-GATE),
    // so the value a pick stores resolves to the user's file, never the bundled one.
    [[nodiscard]] std::vector<EditorFontFamily> ListEditorFontFamilies(const std::filesystem::path& exeDir,
                                                                       const std::filesystem::path& userFontsDir);
    // What the family combo shows: the name, or "<stem>  (user)" for a user font.
    [[nodiscard]] std::string EditorFontFamilyLabel(const EditorFontFamily& family);
    // The file of the family named `name`, else of `fallback`, else the first family's.
    [[nodiscard]] std::filesystem::path ResolveEditorFontFamily(const std::vector<EditorFontFamily>& families,
                                                                std::string_view name, std::string_view fallback);

    struct EditorFontRequest
    {
        std::filesystem::path uiFace;
        std::filesystem::path monoFace;
        float sizePx = 16.0f;
    };
    [[nodiscard]] EditorFontRequest DefaultEditorFontRequest(const std::filesystem::path& exeDir);   // today's faces at 16 px

    // Install on the CURRENT context: the UI face (Fonts[0]), Roboto, the mono
    // face, each with merged lucide icons, then the brand face. Before the first
    // frame. The bundled faces resolve exe-relative. A requested face that is
    // missing or unparseable never asserts: it WARNs and the bundled default
    // (Inter / JetBrains Mono) loads in its place, so Fonts[0] stays the UI face.
    const EditorFontSet& InstallEditorFonts(const EditorFontRequest& request);
    const EditorFontSet& InstallEditorFonts(float sizePx = 16.0f);   // DefaultEditorFontRequest(exe dir) at sizePx: the boot path
    // Remove the installed set and install `request` (settings S4). OUTSIDE an ImGui frame only.
    const EditorFontSet& ReinstallEditorFonts(const EditorFontRequest& request);
    // Drop the installed handles (GetEditorFonts() all-null again) WITHOUT touching
    // any atlas: for whoever destroys the context that owns them, and tests.
    void ForgetEditorFonts();
    [[nodiscard]] int EditorFontInstallCount();   // installs since process start (tests: "once")

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
