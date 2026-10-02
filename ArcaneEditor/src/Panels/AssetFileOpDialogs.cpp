#include "Panels/AssetFileOpDialogs.hpp"

#include "Widgets/EditorTheme.hpp"     // Theme::kError / kAmber
#include "Widgets/EditorWidgets.hpp"   // RowWithThumb (the doomed rows)

#include <imgui.h>

#include <cfloat>
#include <string>

namespace Arcane::Editor
{
    std::optional<AssetOpRequest> DrawRenameAssetModal(RenameModalState& st, const AssetPanelServices& sv)
    {
        if (!st.open) return std::nullopt;
        if (st.justOpened) ImGui::OpenPopup("Rename Asset##assetops");
        std::optional<AssetOpRequest> out; ImGui::SetNextWindowSize(ImVec2(380, 0));
        if (ImGui::BeginPopupModal("Rename Asset##assetops", &st.open))
        {
            ImGui::SetNextItemWidth(-FLT_MIN); if (st.justOpened) { ImGui::SetKeyboardFocusHere(); st.justOpened = false; }
            const bool enter = ImGui::InputText("##renamemodal", st.buf, sizeof(st.buf), ImGuiInputTextFlags_EnterReturnsTrue);
            const AssetOpRequest req{ .kind = AssetOpKind::Rename, .guids = { st.guid }, .newStem = st.buf };
            const std::string why = sv.fileOpRefusal ? sv.fileOpRefusal(req) : std::string("unavailable");
            ImGui::TextDisabled("%s", why.c_str()); ImGui::BeginDisabled(!why.empty());
            if (ImGui::Button("Rename", ImVec2(92, 0)) || (enter && why.empty())) { out = req; st.open = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(92, 0))) { st.open = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        return out;
    }

    DeleteModalResult DrawDeleteConfirmModal(DeleteConfirmState& st, const AssetPanelServices& sv)
    {
        if (!st.open) return DeleteModalResult::None;
        if (st.justOpened) { ImGui::OpenPopup("Delete Assets##assetops"); st.justOpened = false; }
        DeleteModalResult out = DeleteModalResult::None; ImGui::SetNextWindowSize(ImVec2(440, 0));
        if (!ImGui::BeginPopupModal("Delete Assets##assetops", nullptr)) return out;
        const DeleteModalText text = DescribeDeleteModal(st.plan, st.request.guids, st.dirtyTitles);
        ImGui::TextUnformatted(text.title.c_str());
        for (const AssetMove& m : st.plan.moves)   // the doomed rows (cascaded children included): thumb + name
            if (!m.files.empty())
                (void)RowWithThumb(("##doomed" + m.guid.ToString()).c_str(),
                                   static_cast<ImTextureID>(sv.resolveAssetThumb ? sv.resolveAssetThumb(m.guid) : 0),
                                   KindIcon(m.kind), m.files.front().from.filename().string().c_str(), false, 0.0f, kTableRowHeight);
        for (const AssetRefusal& r : st.plan.refusals) ImGui::TextColored(Theme::kError, "%s", r.reason.c_str());   // refusals replace the confirm
        if (st.plan.refusals.empty())
        {
            if (!st.plan.referencers.empty())
            {
                ImGui::SeparatorText(("Referenced by (" + std::to_string(st.plan.referencers.size()) + ")").c_str());
                for (const AssetReferencer& r : st.plan.referencers) ImGui::BulletText("%s %s", r.label.c_str(), ReferencerTags(r).c_str());
            }
            if (!st.plan.derived.empty()
                && ImGui::Checkbox(("Also delete " + std::to_string(st.plan.derived.size()) + " derived assets").c_str(), &st.request.cascadeDerived))
                out = DeleteModalResult::Replan;
            if (!text.unsaved.empty()) ImGui::TextColored(Theme::kAmber, "%s", text.unsaved.c_str());
        }
        ImGui::TextDisabled("%s", text.footer.c_str());
        if (ImGui::Button("Cancel", ImVec2(92, 0))) { out = DeleteModalResult::Cancel; st.open = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); ImGui::BeginDisabled(!st.plan.refusals.empty());
        if (ImGui::Button(text.confirm.c_str())) { out = DeleteModalResult::Confirm; st.open = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndDisabled(); ImGui::EndPopup(); return out;
    }
}
