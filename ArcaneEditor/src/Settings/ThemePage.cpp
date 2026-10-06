#include "Settings/ThemePage.hpp"

#include "Settings/EditorThemeSettings.hpp"
#include "Settings/SettingsHost.hpp"
#include "Settings/ThemePresets.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/IconsLucide.h"

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>

#include <string>

namespace Arcane::Editor
{
    namespace
    {
        std::string PresetLabel(std::string_view name) { return name == "HighContrast" ? "High Contrast" : std::string(name); }

        bool ApplyFile(ThemePageState& st, const std::filesystem::path& path, std::string_view verb)
        {
            const auto file = ReadThemeFile(path);
            if (!file)
            {
                st.status = std::string(verb) + " failed: " + file.error();
                return false;
            }
            Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
            const std::size_t n = ApplyThemeToRegistry(*file, reg);
            reg.Publish();
            // Queue every token for the debounced archive write (S4-17 carried
            // gap): without it a crash before exit loses the preset/import.
            for (const auto& [field, colour] : file->colors)
                if (const std::string name = ThemeCvarName(field); !reg.Find(name).IsStale())
                    NoteSettingEdited(Arcane::SetBy::EditorUser, name);
            st.status = std::string(verb) + " " + PresetLabel(file->name) + " (" + std::to_string(n) + " colours)";
            if (!file->unknownKeys.empty())
                st.status += "; ignored " + std::to_string(file->unknownKeys.size()) + " unknown key(s), first: " + file->unknownKeys.front();
            return true;
        }

        // editor.theme.unfocusedOverlineAlpha: the page's one non-colour
        // setting (S6-26), saved the same way as a swatch edit.
        void DrawOverlineAlpha(const Arcane::CVarRegistry& reg)
        {
            const std::string name = ThemeCvarName("unfocusedOverlineAlpha");
            float a = Arcane::Settings<EditorThemeSettings>().unfocusedOverlineAlpha;
            ImGui::SeparatorText("Tabs");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
            if (ImGui::SliderFloat("Unfocused tab overline opacity", &a, 0.0f, 1.0f, "%.2f"))
            {
                Arcane::CVarRegistry& mut = Arcane::CVarRegistry::Get();
                const Arcane::CVarHandle h = mut.Find(name);
                if (!h.IsStale()
                    && mut.Set(h, Arcane::CVarValue::Float32(a), Arcane::SetBy::EditorUser, "editor", Arcane::CVarContext::Editor)
                           == Arcane::SetResult::Applied)
                {
                    mut.Publish();
                    NoteSettingEdited(Arcane::SetBy::EditorUser, name);
                }
            }
            if (ImGui::IsItemHovered())
            {
                const auto info = reg.Explain(name);
                ImGui::SetTooltip("%s\n%s", info ? info->help.c_str() : "", name.c_str());
            }
        }

        void DrawSwatches()
        {
            std::string_view group;
            const Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
            for (const ThemeToken& t : kThemeTokens)
            {
                if (t.group != group)
                {
                    group = t.group;
                    ImGui::SeparatorText(std::string(group).c_str());
                }
                ImGui::PushID(t.field.data(), t.field.data() + t.field.size());
                ImVec4 c = Theme::Live().*(t.palette);
                if (ImGui::ColorEdit4("##swatch", &c.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf
                                                       | ImGuiColorEditFlags_AlphaBar))
                    (void)SetThemeToken(t.field, c);
                ImGui::SameLine();
                ImGui::TextUnformatted(t.displayName.data(), t.displayName.data() + t.displayName.size());
                if (ImGui::IsItemHovered())
                {
                    const std::string name = ThemeCvarName(t.field);
                    const auto info = reg.Explain(name);
                    ImGui::SetTooltip("%s\n%s", info ? info->help.c_str() : "", name.c_str());
                }
                ImGui::PopID();
            }
            DrawOverlineAlpha(reg);
        }

        void DrawPreview(ThemePageState& st)
        {
            ImGui::SeparatorText("Preview");
            ImGui::Button("Button");
            ImGui::SameLine();
            if (IconToggle(ICON_LC_EYE "##pvtoggle", st.previewToggle)) st.previewToggle = !st.previewToggle;
            if (ImGui::BeginTabBar("##pvtabs"))
            {
                if (ImGui::BeginTabItem("Selected")) { ImGui::TextUnformatted("The selected tab carries the accent overline."); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Other")) { ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##pvtext", st.previewText, sizeof st.previewText);
            ImGui::TextDisabled("Dim text");
            ImGui::TextColored(Theme::kError, ICON_LC_CIRCLE_X " An error row");
            ImGui::TextColored(Theme::kWarning, ICON_LC_TRIANGLE_ALERT " A warning row");
        }

        void DrawContrast()
        {
            ImGui::SeparatorText("Contrast");
            for (const ContrastRow& row : ContrastReport(Theme::Live()))
            {
                ImGui::Text("%.*s  %.2f:1", static_cast<int>(row.label.size()), row.label.data(), row.ratio);
                if (!row.ok)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(Theme::kWarning, ICON_LC_TRIANGLE_ALERT " below %.1f:1", row.minRatio);
                }
            }
        }
    }

    bool ApplyThemePresetByName(ThemePageState& st, std::string_view presetName)
    {
        return ApplyFile(st, ThemePresetDir() / (std::string(presetName) + ".arctheme"), "Applied");
    }

    bool ImportThemeFrom(ThemePageState& st, const std::filesystem::path& path)
    {
        return ApplyFile(st, path, "Imported");
    }

    bool ExportThemeTo(ThemePageState& st, std::filesystem::path path)
    {
        if (path.extension() != ".arctheme") path += ".arctheme";
        std::string error;
        if (!WriteThemeFile(path, path.stem().string(), Theme::Live(), &error))
        {
            st.status = "Export failed: " + error;
            return false;
        }
        st.status = "Exported " + path.filename().string();
        return true;
    }

    bool SetThemeToken(std::string_view field, const ImVec4& display)
    {
        Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
        const Arcane::CVarHandle h = reg.Find(ThemeCvarName(field));
        if (h.IsStale()) return false;
        const bool ok = reg.Set(h, Arcane::CVarValue::Color(ToSettingColor(display)), Arcane::SetBy::EditorUser,
                                "editor", Arcane::CVarContext::Editor) == Arcane::SetResult::Applied;
        reg.Publish();
        if (ok) NoteSettingEdited(Arcane::SetBy::EditorUser, ThemeCvarName(field));   // the debounced archive, not only exit
        return ok;
    }

    void DrawThemePage(void* user)
    {
        ThemePageState& st = *static_cast<ThemePageState*>(user);
        ImGui::TextDisabled("Colours apply live and are saved for all projects.");
        for (std::string_view name : kThemePresetNames)
        {
            const std::string label = PresetLabel(name) + "##preset";
            if (ImGui::Button(label.c_str())) (void)ApplyThemePresetByName(st, name);
            ImGui::SameLine();
        }
        if (ImGui::Button("Import...") && st.requestImport) st.requestImport();
        ImGui::SameLine();
        if (ImGui::Button("Export...") && st.requestExport) st.requestExport();
        if (!st.status.empty()) ImGui::TextDisabled("%s", st.status.c_str());
        if (ImGui::BeginTable("##themepage", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableNextColumn();
            DrawSwatches();
            ImGui::TableNextColumn();
            DrawPreview(st);
            DrawContrast();
            ImGui::EndTable();
        }
    }
}
