#include "Settings/LayoutPage.hpp"

#include "Panels/LayoutLibrary.hpp"
#include "Settings/LayoutSettings.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/IconsLucide.h"
#include "Widgets/UiMetrics.hpp"

#include <Arcane/Config/CVarRegistry.hpp>

#include <imgui.h>

namespace Arcane::Editor
{
    namespace
    {
        LayoutLibrary Library(const LayoutPageState& st) { return LayoutLibrary(st.dir.empty() ? NamedLayoutDir() : st.dir); }

        bool SetString(Arcane::CVarHandle h, std::string value)
        {
            Arcane::CVarRegistry& reg = Arcane::CVarRegistry::Get();
            const bool ok = reg.Set(h, Arcane::CVarValue::String(std::move(value)), Arcane::SetBy::EditorUser,
                                    "editor", Arcane::CVarContext::Editor) == Arcane::SetResult::Applied;
            reg.PublishImmediate();
            return ok;
        }
    }

    bool SaveLayoutAs(LayoutPageState& st, std::string_view name, std::string_view iniText)
    {
        std::string error;
        if (!Library(st).Save(name, iniText, &error))
        {
            st.status = "Not saved: " + error;
            return false;
        }
        st.status = "Saved '" + std::string(name) + "'";
        st.listDirty = true;
        return true;
    }

    bool SetDefaultLayout(LayoutPageState& st, std::string_view name)
    {
        const bool ok = SetString(cvar_layoutDefault.Handle(), std::string(name));
        st.status = name.empty() ? "New projects open with the factory layout" : "New projects open with '" + std::string(name) + "'";
        return ok;
    }

    bool DeleteLayout(LayoutPageState& st, std::string_view name)
    {
        if (!Library(st).Delete(name)) { st.status = "Could not delete '" + std::string(name) + "'"; return false; }
        if (cvar_layoutDefault.Get() == name) (void)SetDefaultLayout(st, "");
        st.status = "Deleted '" + std::string(name) + "'";
        st.listDirty = true;
        return true;
    }

    void RequestLoadLayout(LayoutPageState& st, std::string_view name) { st.loadRequest = std::string(name); }

    bool SetOpenPanelsAtStart(const PanelVisibility& vis)
    {
        return SetString(cvar_layoutOpenPanelsAtStart.Handle(), FormatOpenPanels(vis));
    }

    void DrawLayoutPage(void* user)
    {
        LayoutPageState& st = *static_cast<LayoutPageState*>(user);
        const LayoutLibrary lib = Library(st);
        if (st.listDirty) { st.names = lib.List(); st.listDirty = false; }

        ImGui::TextDisabled("Layouts are files, not settings: %s", lib.Dir().generic_string().c_str());

        ImGui::SeparatorText("Save");
        ImGui::SetNextItemWidth(Ui::Px(240.0f));
        ImGui::InputTextWithHint("##layoutname", "Layout name", st.nameBuf, sizeof st.nameBuf);
        ImGui::SameLine();
        const std::optional<std::string> why = ValidateLayoutName(st.nameBuf);
        ImGui::BeginDisabled(why.has_value());
        if (ImGui::Button("Save Current As"))
        {
            std::size_t size = 0;
            const char* ini = ImGui::SaveIniSettingsToMemory(&size);
            if (SaveLayoutAs(st, st.nameBuf, std::string_view(ini, size))) st.nameBuf[0] = '\0';
        }
        ImGui::EndDisabled();
        if (why && st.nameBuf[0] != '\0') ImGui::TextColored(Theme::kWarning, ICON_LC_TRIANGLE_ALERT " %s", why->c_str());

        ImGui::SeparatorText("Saved layouts");
        const std::string def = cvar_layoutDefault.Get();
        if (st.names.empty()) ImGui::TextDisabled("No saved layouts yet.");
        for (const std::string& name : st.names)
        {
            ImGui::PushID(name.c_str());
            ImGui::TextUnformatted(name.c_str());
            if (name == def) { ImGui::SameLine(); ImGui::TextDisabled("(default)"); }
            ImGui::SameLine(Ui::Px(260.0f));
            if (ImGui::SmallButton("Load")) RequestLoadLayout(st, name);
            ImGui::SameLine();
            if (ImGui::SmallButton(name == def ? "Unset Default" : "Set As Default")) (void)SetDefaultLayout(st, name == def ? "" : name);
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) (void)DeleteLayout(st, name);
            ImGui::PopID();
        }

        ImGui::SeparatorText("Factory");
        if (ImGui::Button("Reset To Factory")) st.resetRequested = true;
        ImGui::SameLine();
        ImGui::TextDisabled("Rebuilds the stock dock layout and shows the panels below.");

        ImGui::SeparatorText("Panels at start");
        PanelVisibility vis = ParseOpenPanels(cvar_layoutOpenPanelsAtStart.Get());
        bool changed = false;
        for (const PanelInfo& p : kPanels)
        {
            if (p.permanent) continue;
            changed |= ImGui::Checkbox(p.name, &vis.visible[static_cast<std::size_t>(p.id)]);
        }
        if (changed) (void)SetOpenPanelsAtStart(vis);

        if (!st.status.empty()) ImGui::TextDisabled("%s", st.status.c_str());
    }
}
