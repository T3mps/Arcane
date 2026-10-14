#include "Settings/FontsPage.hpp"

#include "Project/ModuleBuild.hpp"
#include "Settings/SettingsWindow.hpp"

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Platform/Paths.hpp>

#include <imgui.h>

#include <string>

namespace Arcane::Editor
{
    bool SetUiSetting(std::string_view field, const Arcane::CVarValue& value)
    {
        Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
        const Arcane::CVarHandle h = reg.Find("editor.ui." + std::string(field));
        if (h.IsStale()) return false;
        const bool ok = reg.Set(h, value, Arcane::SetBy::EditorUser, "editor", Arcane::CVarContext::Editor) == Arcane::SetResult::Applied;
        reg.Publish();
        return ok;
    }

    void DrawFontsPage(void* user)
    {
        FontsPageState& st = *static_cast<FontsPageState*>(user);
        if (st.listDirty)
        {
            const std::filesystem::path exe = st.exeDir.empty() ? ModuleBuild::ExeDir() : st.exeDir;
            const std::filesystem::path fonts = !st.userFontsDir.empty() ? st.userFontsDir
                : (st.exeDir.empty() ? Arcane::Paths::Get(Arcane::Paths::Location::EditorUserDir) / "Fonts" : std::filesystem::path{});
            st.families = ListEditorFontFamilies(exe, fonts);
            st.listDirty = false;
        }
        // The node's UI font / Monospace font rows (standard rows, drawn below
        // the page) pick from this list: provenance, reset and undo are theirs.
        SetSettingsFontFamilies(&st.families);
        ImGui::TextDisabled("Font changes rebuild the atlas at the next frame; scale applies at once.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh")) st.listDirty = true;
        ImGui::SetItemTooltip("Re-scan the Fonts folder for .ttf and .otf files.");
        ImGui::SeparatorText("Sample");
        ImGui::TextUnformatted("The quick brown fox jumps over the lazy dog. 0123456789");
        {
            const MonoFont mono;
            ImGui::TextUnformatted("mono: Sprite.cpp:42  0x1F  {} => ++i");
        }
    }
}
