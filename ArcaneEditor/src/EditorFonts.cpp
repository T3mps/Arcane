#include "EditorFonts.hpp"

#include "IconsLucide.h"

#include <Arcane/Base/Log.hpp>

#include <imgui.h>

#include <filesystem>
#include <string>

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
            ImFont* face = io.Fonts->AddFontFromFileTTF(facePath.c_str(), sizePx, &baseCfg);
            if (!face)
            {
                ARC_WARN("Arcane Editor: failed to load font '{}'", facePath);
                return nullptr;
            }

            ImFontConfig cfg;
            cfg.MergeMode        = true;
            cfg.GlyphMinAdvanceX = sizePx;   // monospace icon cell
            cfg.GlyphOffset.y    = 3.0f;     // baseline nudge (matches prior tuning)
            if (!io.Fonts->AddFontFromFileTTF(lucidePath.c_str(), sizePx, &cfg, kIconRange))
                ARC_WARN("Arcane Editor: failed to merge icon font '{}' into '{}'", lucidePath, facePath);

            return face;
        }
    }

    const EditorFontSet& InstallEditorFonts(float sizePx)
    {
        ImGuiIO& io = ImGui::GetIO();
        const std::filesystem::path dir = ExeDir();
        const std::string lucide = (dir / "data" / "font" / "lucide" / "lucide.ttf").string();

        // Inter FIRST -> becomes Fonts[0], the implicit editor default (ProggyClean is
        // never added). The 18pt optical cut is Inter's UI/body design (24/28pt are for
        // display sizes); static weights, since ImGui's rasterizer ignores variable-font
        // axes. Roboto loads next as a pushable alternate face.
        const std::string inter =
            (dir / "data" / "font" / "inter" / "static" / "Inter_18pt-Regular.ttf").string();
        const std::string roboto =
            (dir / "data" / "font" / "roboto" / "static" / "Roboto-Regular.ttf").string();

        g_fonts = EditorFontSet{};
        g_fonts.interRegular = AddFaceWithIcons(io, inter,  lucide, sizePx);
        g_fonts.roboto       = AddFaceWithIcons(io, roboto, lucide, sizePx);
        return g_fonts;
    }

    const EditorFontSet& GetEditorFonts() { return g_fonts; }
}
