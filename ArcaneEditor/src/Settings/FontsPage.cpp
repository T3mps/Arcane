#include "Settings/FontsPage.hpp"

#include "Project/ModuleBuild.hpp"
#include "Settings/EditorUiSettings.hpp"
#include "Settings/SettingsHost.hpp"

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>
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

    namespace
    {
        void FamilyCombo(const char* label, const char* field, const std::string& current, const std::vector<EditorFontFamily>& families)
        {
            if (!ImGui::BeginCombo(label, current.c_str())) return;
            for (std::size_t i = 0; i < families.size(); ++i)
            {
                const EditorFontFamily& f = families[i];
                const std::string item = f.bundled ? f.name : f.name + "  (user)";
                ImGui::PushID(static_cast<int>(i));   // a .ttf and an .otf of one stem list twice
                // The debounced archive writes the pick now; the exit-time
                // archive alone would lose it to a crash.
                if (ImGui::Selectable(item.c_str(), f.name == current) && SetUiSetting(field, Arcane::CVarValue::String(f.name)))
                    NoteSettingEdited(Arcane::SetBy::EditorUser, "editor.ui." + std::string(field));
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
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
        // A copy: a pick publishes mid-draw, and the next combo reads after it.
        const EditorUiSettings ui = Arcane::Settings<EditorUiSettings>();
        ImGui::TextDisabled("Font changes rebuild the atlas at the next frame; scale applies at once.");
        FamilyCombo("UI font", "fontFamily", ui.fontFamily, st.families);
        FamilyCombo("Monospace font", "monoFontFamily", ui.monoFontFamily, st.families);
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
