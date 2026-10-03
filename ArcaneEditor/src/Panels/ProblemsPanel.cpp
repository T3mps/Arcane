#include <Panels/ProblemsPanel.hpp>

#include <Panels/ConsoleModel.hpp>   // ProblemsTabTitle (s8.2)
#include <Panels/SeverityStyle.hpp>
#include <Project/OsShell.hpp>   // row context menu: Show in Explorer (s8.2)
#include <Widgets/EditorTheme.hpp>
#include <Widgets/EditorWidgets.hpp>
#include <Arcane/Base/Log.hpp>
#include <imgui.h>

#include <filesystem>
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
                                                         ProblemsUiState& ui, const RouteFacts& facts,
                                                         bool suppressBadges, bool* open)
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
                    // Classified per frame: File rows stat the disk, which is
                    // fine at Problems' row counts (tens, not thousands).
                    const bool live = IsRoutable(d->locator, facts);
                    const LinkRowResult r = LinkRow("##row", d->message, live, st.icon,
                                                    ImGui::ColorConvertFloat4ToU32(st.color));
                    if (r.clicked)
                        clicked = d->locator;
                    if (const std::optional<std::filesystem::path> path = LocatorPath(d->locator, facts))
                        if (ImGui::BeginPopupContextItem("##rowctx"))
                        {
                            if (ImGui::MenuItem("Open", nullptr, false, live))
                                clicked = d->locator;
                            if (ImGui::MenuItem("Show in Explorer"))
                                if (const auto sr = OsShell::ShowInExplorer(*path); sr != OsShell::ShellResult::Ok)
                                    ARC_WARN("Problems: could not show {} -- {}", path->string(), OsShell::Describe(sr));
                            if (ImGui::MenuItem("Copy path"))
                                ImGui::SetClipboardText(path->string().c_str());
                            ImGui::EndPopup();
                        }

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
