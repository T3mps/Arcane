#include <ProblemsPanel.hpp>

#include <IconsLucide.h>
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
                                                         ProblemsUiState& ui, bool* open)
    {
        std::optional<Arcane::DiagLocator> clicked;

        ImGui::Begin("Problems", open);

        const std::size_t nErr  = store.Count(Arcane::DiagSeverity::Error);
        const std::size_t nWarn = store.Count(Arcane::DiagSeverity::Warning);

        ImGui::TextColored(ImVec4(0.90f, 0.35f, 0.35f, 1.0f), ICON_LC_CIRCLE_X " %zu", nErr);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.30f, 1.0f), ICON_LC_TRIANGLE_ALERT " %zu", nWarn);
        ImGui::SameLine();
        ImGui::Checkbox("Warnings", &ui.showWarnings);
        ImGui::SameLine();
        ImGui::Checkbox("Info", &ui.showInfo);

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##problemsearch", "Search", ui.search, sizeof(ui.search));
        ImGui::Separator();

        const Arcane::DiagSeverity floorSeverity =
            ui.showInfo     ? Arcane::DiagSeverity::Info
          : ui.showWarnings ? Arcane::DiagSeverity::Warning
                            : Arcane::DiagSeverity::Error;

        const std::vector<Arcane::Diagnostic> rows = store.Filtered(floorSeverity, ui.search);

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
                    ImVec4 col(0.80f, 0.80f, 0.80f, 1.0f);
                    const char* icon = ICON_LC_INFO;
                    if (d->severity == Arcane::DiagSeverity::Error)
                    {
                        col = ImVec4(0.90f, 0.35f, 0.35f, 1.0f);
                        icon = ICON_LC_CIRCLE_X;
                    }
                    else if (d->severity == Arcane::DiagSeverity::Warning)
                    {
                        col = ImVec4(0.95f, 0.77f, 0.30f, 1.0f);
                        icon = ICON_LC_TRIANGLE_ALERT;
                    }

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
