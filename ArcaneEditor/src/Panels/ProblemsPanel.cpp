#include <Panels/ProblemsPanel.hpp>

#include <Panels/ConsoleModel.hpp>   // ProblemsTabTitle (s8.2)
#include <Panels/SeverityStyle.hpp>
#include <Widgets/EditorTheme.hpp>
#include <Widgets/EditorWidgets.hpp>
#include <imgui.h>

#include <map>
#include <vector>

namespace Arcane::Editor
{
    const char* ScopeLabel(Arcane::DiagScope scope) noexcept
    {
        switch (scope)
        {
            case Arcane::DiagScope::Project:  return "Project";
            case Arcane::DiagScope::Assets:   return "Assets";
            case Arcane::DiagScope::Scene:    return "Scene";
            case Arcane::DiagScope::Plugin:   return "Plugin";
            case Arcane::DiagScope::Material: return "Materials";
            case Arcane::DiagScope::Shader:   return "Shaders";
        }
        return "Other";
    }

    std::optional<Arcane::DiagLocator> DrawProblemsPanel(const DiagnosticStore& store,
                                                         ProblemsUiState& ui, bool suppressBadges,
                                                         bool* open)
    {
        std::optional<Arcane::DiagLocator> clicked;

        const std::size_t nErr  = store.Count(Arcane::DiagSeverity::Error);
        const std::size_t nWarn = store.Count(Arcane::DiagSeverity::Warning);
        const bool tint = !suppressBadges && nErr + nWarn > 0;
        if (tint) ImGui::PushStyleColor(ImGuiCol_Text, nErr > 0 ? Theme::kError : Theme::kWarning);
        ImGui::Begin(suppressBadges ? "Problems###Problems" : ProblemsTabTitle(nErr, nWarn).c_str(), open);
        if (tint) ImGui::PopStyleColor();   // only the tab label is tinted

        const std::size_t nInfo = store.Count(Arcane::DiagSeverity::Info);

        // Three count-carrying severity toggles (s8.2): no always-drawn "0".
        (void)SeverityToggleFor("problems_err",  Arcane::DiagSeverity::Error,   nErr,  ui.showError);   ImGui::SameLine();
        (void)SeverityToggleFor("problems_warn", Arcane::DiagSeverity::Warning, nWarn, ui.showWarning); ImGui::SameLine();
        (void)SeverityToggleFor("problems_info", Arcane::DiagSeverity::Info,    nInfo, ui.showInfo);    ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##problemsearch", "Search", ui.search, sizeof(ui.search));
        ImGui::Separator();

        const std::vector<Arcane::Diagnostic> rows =
            store.Filtered(MaskFrom(ui.showError, ui.showWarning, ui.showInfo), ui.search);

        // Group by scope, preserving the severity-sorted order Snapshot produced.
        std::map<Arcane::DiagScope, std::vector<const Arcane::Diagnostic*>> grouped;
        for (const Arcane::Diagnostic& d : rows)
            grouped[d.scope].push_back(&d);

        if (rows.empty())
            ImGui::TextDisabled("No problems.");

        int uid = 0;
        for (const auto& [scope, list] : grouped)
        {
            ImGui::PushID(uid++);
            if (ImGui::CollapsingHeader((std::string(ScopeLabel(scope)) + " (" +
                                         std::to_string(list.size()) + ")").c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (const Arcane::Diagnostic* d : list)
                {
                    ImGui::PushID(uid++);
                    const SeverityStyle st = StyleFor(d->severity);
                    const ImVec4 col = st.color;
                    const char* icon = st.icon;

                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    const std::string label = std::string(icon) + " " + d->message;
                    // Selectable spans the row so the whole line is the hit target.
                    if (ImGui::Selectable(label.c_str()) &&
                        d->locator.kind != Arcane::DiagLocator::Kind::None)
                    {
                        clicked = d->locator;
                    }
                    ImGui::PopStyleColor();

                    if (!d->detail.empty())
                    {
                        ImGui::Indent();
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextDisabled("%s", d->detail.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::Unindent();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::PopID();
        }

        ImGui::End();
        return clicked;
    }
}
