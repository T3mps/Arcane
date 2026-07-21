#include "EditorFonts.hpp"

#include "IconsLucide.h"

#include <imgui.h>

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Grimoire
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
    }

    void InstallEditorFonts(float sizePx)
    {
        ImGuiIO& io = ImGui::GetIO();
        const std::filesystem::path dir = ExeDir();
        const std::string roboto = (dir / "data" / "font" / "Roboto-Regular.ttf").string();
        const std::string lucide = (dir / "data" / "font" / "lucide" / "lucide.ttf").string();

        // Base font first -> becomes the default (ProggyClean is never added).
        io.Fonts->AddFontFromFileTTF(roboto.c_str(), sizePx);

        // Merge the lucide icon glyphs into the same atlas.
        static const ImWchar range[] = { ICON_LC_MIN, ICON_LC_MAX, 0 };
        ImFontConfig cfg;
        cfg.MergeMode        = true;
        cfg.GlyphMinAdvanceX = sizePx;   // monospace icon cell
        cfg.GlyphOffset.y    = 3.0f;     // baseline nudge; tune at desk (Task 4)
        io.Fonts->AddFontFromFileTTF(lucide.c_str(), sizePx, &cfg, range);
    }
}
