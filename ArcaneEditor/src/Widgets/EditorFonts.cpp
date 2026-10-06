#include "Widgets/EditorFonts.hpp"

#include "Widgets/IconsLucide.h"

#include <Arcane/Base/Log.hpp>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Arcane::Editor
{
    namespace
    {
        std::filesystem::path ExeDir()
        {
#ifdef _WIN32
            wchar_t buf[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, buf, MAX_PATH) != 0)
                return std::filesystem::path(buf).parent_path();
#endif
            return std::filesystem::current_path();
        }

        EditorFontSet g_fonts;   // handles from the most recent InstallEditorFonts
        int g_installCount = 0;  // InstallEditorFonts calls since process start

        // A cheap probe before ImGui sees the bytes, for the failures NO error-
        // recovery setting can catch: AddFontFromMemoryTTF hard-IM_ASSERTs
        // font_data_size > 100 (imgui_draw.cpp, a plain assert, not a user
        // error), and stb_truetype reads the sfnt / TTC header with no bounds
        // check. So a short or non-font file (e.g. a 1-byte .ttf in the user's
        // Fonts folder) never reaches ImGui. This only rejects obvious
        // non-fonts; LoadFontQuiet's scoped assert covers the parse failures
        // the font loader itself detects.
        bool LooksLikeFontFile(const std::string& path)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
            if (ec || size <= 100)
                return false;   // missing, unreadable, or below ImGui's size floor
            std::ifstream in(std::filesystem::path(path), std::ios::binary);
            unsigned char h[16]{};
            if (!in.read(reinterpret_cast<char*>(h), sizeof(h)))
                return false;   // missing, unreadable, or shorter than the header
            const auto tag = [&](const char* t) { return std::memcmp(h, t, 4) == 0; };
            const bool sfnt1 = h[0] == 0x00 && h[1] == 0x01 && h[2] == 0x00 && h[3] == 0x00;
            return sfnt1 || tag("OTTO") || tag("true") || tag("typ1") || tag("ttcf");
        }

        // AddFontFromFileTTF that NEVER asserts: a missing or unparseable file
        // returns null. ImFontFlags_NoLoadError covers the missing-file path
        // (ImFontAtlas::AddFontFromFileTTF); the font loader's parse errors are
        // IM_ASSERT_USER_ERRORs, which assert while io.ConfigErrorRecoveryEnableAssert
        // is set (its default), so that is cleared for this one call and restored.
        // On failure ImFontAtlas::AddFont rolls the font back out of Fonts[].
        ImFont* LoadFontQuiet(ImGuiIO& io, const std::string& path, float sizePx,
                              ImFontConfig cfg, const ImWchar* ranges = nullptr)
        {
            if (!LooksLikeFontFile(path))
                return nullptr;
            cfg.Flags |= ImFontFlags_NoLoadError;
            const bool assertWas = io.ConfigErrorRecoveryEnableAssert;
            io.ConfigErrorRecoveryEnableAssert = false;
            ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), sizePx, &cfg, ranges);
            io.ConfigErrorRecoveryEnableAssert = assertWas;
            return font;
        }

        // Add one TTF at sizePx as a fresh base face, then MERGE the lucide icon range
        // into it (merge attaches to the last-added base font), so ICON_LC_* renders
        // under this face. Returns the base ImFont* (null + WARN on load failure).
        ImFont* AddFaceWithIcons(ImGuiIO& io, const std::string& facePath,
                                 const std::string& lucidePath, float sizePx)
        {
            static const ImWchar kIconRange[] = { ICON_LC_MIN, ICON_LC_MAX, 0 };

            // ImGui 1.92's font system loads glyphs on-demand and no longer clips a font
            // to GlyphRanges -- so a TEXT font that carries Private-Use glyphs in the icon
            // block (Inter has 478 in lucide's E038..E6FB range: circled digits, boxed
            // letters, stylistic sets) would SHADOW the merged icon font, because the base
            // face is consulted first for a codepoint it also defines. GlyphExcludeRanges
            // removes the icon block from the base so the lucide merge below owns it.
            ImFontConfig baseCfg;
            baseCfg.GlyphExcludeRanges = kIconRange;
            ImFont* face = LoadFontQuiet(io, facePath, sizePx, baseCfg);
            if (!face)
            {
                ARC_WARN("Arcane Editor: failed to load font '{}'", facePath);
                return nullptr;
            }

            ImFontConfig cfg;
            cfg.MergeMode        = true;
            cfg.GlyphMinAdvanceX = sizePx;   // monospace icon cell
            cfg.GlyphOffset.y    = 3.0f * sizePx / 16.0f;   // the baseline nudge scales with the face (3 px at 16, unchanged)
            if (!LoadFontQuiet(io, lucidePath, sizePx, cfg, kIconRange))
                ARC_WARN("Arcane Editor: failed to merge icon font '{}' into '{}'", lucidePath, facePath);

            return face;
        }

        // A requested face that fails to load falls back to the bundled default at
        // once: a persisted user font that went missing or is corrupt must not cost
        // the editor its UI or mono face. The failed AddFont already popped itself,
        // so the fallback takes its place in Fonts[].
        ImFont* AddFaceOrBundled(ImGuiIO& io, const std::filesystem::path& requested,
                                 const std::filesystem::path& bundled,
                                 const std::string& lucidePath, float sizePx)
        {
            if (ImFont* face = AddFaceWithIcons(io, requested.string(), lucidePath, sizePx))
                return face;
            if (requested == bundled)
                return nullptr;
            ARC_WARN("Arcane Editor: font '{}' did not load; using the bundled '{}'",
                     requested.string(), bundled.string());
            return AddFaceWithIcons(io, bundled.string(), lucidePath, sizePx);
        }
    }

    EditorFontRequest DefaultEditorFontRequest(const std::filesystem::path& dir)
    {
        return { dir / "data" / "font" / "inter" / "static" / "Inter_18pt-Regular.ttf",
                 dir / "data" / "font" / "jetbrainsmono" / "JetBrainsMono-Regular.ttf", 16.0f };
    }

    std::vector<EditorFontFamily> ListEditorFontFamilies(const std::filesystem::path& exeDir,
                                                         const std::filesystem::path& userFontsDir)
    {
        std::vector<EditorFontFamily> out = {
            { "Inter",          exeDir / "data" / "font" / "inter" / "static" / "Inter_18pt-Regular.ttf", true },
            { "Roboto",         exeDir / "data" / "font" / "roboto" / "static" / "Roboto-Regular.ttf",     true },
            { "JetBrains Mono", exeDir / "data" / "font" / "jetbrainsmono" / "JetBrainsMono-Regular.ttf", true },
        };
        std::error_code ec;
        if (!userFontsDir.empty() && std::filesystem::is_directory(userFontsDir, ec))
        {
            std::vector<EditorFontFamily> user;
            for (const auto& e : std::filesystem::directory_iterator(userFontsDir, ec))
            {
                if (!e.is_regular_file(ec))
                    continue;
                std::string ext = e.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".ttf" || ext == ".otf")
                    user.push_back({ e.path().stem().string(), e.path(), false });
            }
            std::sort(user.begin(), user.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
            out.insert(out.end(), user.begin(), user.end());
        }
        return out;
    }

    std::filesystem::path ResolveEditorFontFamily(const std::vector<EditorFontFamily>& families,
                                                  std::string_view name, std::string_view fallback)
    {
        for (const auto& f : families)
            if (f.name == name)
                return f.file;
        for (const auto& f : families)
            if (f.name == fallback)
                return f.file;
        return families.empty() ? std::filesystem::path{} : families.front().file;
    }

    const EditorFontSet& InstallEditorFonts(const EditorFontRequest& r)
    {
        ImGuiIO& io = ImGui::GetIO();
        const std::filesystem::path dir = ExeDir();
        const std::string lucide = (dir / "data" / "font" / "lucide" / "lucide.ttf").string();

        // The UI face FIRST -> becomes Fonts[0], the implicit editor default
        // (ProggyClean is never added). The default is Inter's 18pt optical cut,
        // its UI/body design (24/28pt are for display sizes); static weights,
        // since ImGui's rasterizer ignores variable-font axes. Roboto loads next
        // as a pushable alternate face.
        const std::string roboto =
            (dir / "data" / "font" / "roboto" / "static" / "Roboto-Regular.ttf").string();

        // Brand wordmark face (Aldo the Apache) -- a display face used ONLY for the "Arcane"
        // toolbar wordmark, rendered via PushFont(brand, size) at a display size. No lucide
        // merge (it never shows icons) and no icon-range exclude (it carries no PUA glyphs).
        const std::string brand =
            (dir / "data" / "font" / "aldotheapache" / "AldotheApache.ttf").string();

        const EditorFontRequest bundled = DefaultEditorFontRequest(dir);
        g_fonts = EditorFontSet{};
        g_fonts.interRegular = AddFaceOrBundled(io, r.uiFace, bundled.uiFace, lucide, r.sizePx);
        g_fonts.roboto       = AddFaceWithIcons(io, roboto, lucide, r.sizePx);
        // Mono AFTER Roboto (Fonts[0] stays the UI face), merged with lucide at the UI
        // size so ICON_LC_* work in mono rows; AddFaceWithIcons' GlyphExcludeRanges
        // keeps any PUA glyph of its own out of the icon block, as for Inter.
        g_fonts.mono         = AddFaceOrBundled(io, r.monoFace, bundled.monoFace, lucide, r.sizePx);
        g_fonts.brand        = LoadFontQuiet(io, brand, r.sizePx, ImFontConfig{});
        if (!g_fonts.brand)
            ARC_WARN("Arcane Editor: failed to load brand font '{}'", brand);
        ++g_installCount;
        return g_fonts;
    }

    const EditorFontSet& InstallEditorFonts(float sizePx)
    {
        EditorFontRequest r = DefaultEditorFontRequest(ExeDir());
        r.sizePx = sizePx;
        return InstallEditorFonts(r);
    }

    const EditorFontSet& ReinstallEditorFonts(const EditorFontRequest& request)
    {
        ImGuiIO& io = ImGui::GetIO();
        for (ImFont* f : { g_fonts.interRegular, g_fonts.roboto, g_fonts.mono, g_fonts.brand })
            if (f)
                io.Fonts->RemoveFont(f);   // the NRI backend re-uploads the atlas textures (RendererHasTextures)
        return InstallEditorFonts(request);
    }

    void ForgetEditorFonts() { g_fonts = EditorFontSet{}; }

    int EditorFontInstallCount() { return g_installCount; }

    const EditorFontSet& GetEditorFonts() { return g_fonts; }

    MonoFont::MonoFont()
    {
        if (ImFont* f = g_fonts.mono)
        {
            ImGui::PushFont(f, 0.0f);
            m_pushed = true;
        }
    }

    MonoFont::~MonoFont()
    {
        if (m_pushed)
            ImGui::PopFont();
    }
}
