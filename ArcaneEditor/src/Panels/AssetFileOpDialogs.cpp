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

    std::optional<AssetOpRequest> DrawNewFolderModal(NewFolderState& st, const Arcane::Project& project)
    {
        if (!st.open) return std::nullopt;
        if (st.justOpened) ImGui::OpenPopup("New Folder##assetops");
        std::optional<AssetOpRequest> out;
        if (ImGui::BeginPopupModal("New Folder##assetops", &st.open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextDisabled("In Content/%s", st.parent.c_str()); if (st.justOpened) { ImGui::SetKeyboardFocusHere(); st.justOpened = false; }
            const bool enter = ImGui::InputText("##newfolder", st.name, sizeof(st.name), ImGuiInputTextFlags_EnterReturnsTrue);
            const CreateNameCheck ck = ValidateCreateName(st.name, project.Root() / "Content" / st.parent, "");   // rule 3 catches a file or folder
            ImGui::TextDisabled("%s", ck.ok ? "" : ck.message.c_str()); ImGui::BeginDisabled(!ck.ok);
            if (ImGui::Button("Create", ImVec2(92, 0)) || (enter && ck.ok)) { out = AssetOpRequest{ .kind = AssetOpKind::NewFolder, .newStem = st.name, .destFolder = st.parent }; st.open = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndDisabled(); ImGui::SameLine(); if (ImGui::Button("Cancel", ImVec2(92, 0))) { st.open = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        else if (!ImGui::IsPopupOpen("New Folder##assetops")) st.open = false;   // closed from outside (its parent went): never a stale open flag
        return out;
    }

    MoveToResult DrawMoveToModal(MoveToState& st, NewFolderState& nf, const AssetPanelModel& model,
                                 const AssetPanelServices& sv, const Arcane::Project& project)
    {
        MoveToResult result;
        if (!st.open) return result;
        if (st.justOpened) { ImGui::OpenPopup("Move Assets##assetops"); st.justOpened = false; }
        const std::vector<FolderChoice> folders = BuildContentFolderChoices(model);
        for (int i = 0; i < static_cast<int>(folders.size()) && !st.selectAfterCreate.empty(); ++i)   // a folder just made from here
            if (folders[static_cast<std::size_t>(i)].relative == st.selectAfterCreate) { st.folderIndex = i; st.selectAfterCreate.clear(); }
        if (ImGui::BeginPopupModal("Move Assets##assetops", &st.open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Move %d %s", static_cast<int>(st.guids.size()), st.guids.size() == 1 ? "asset" : "assets");
            (void)DrawLocationCombo(folders, st.folderIndex);
            const AssetOpRequest req{ .kind = AssetOpKind::Move, .guids = st.guids, .destFolder = folders[static_cast<std::size_t>(st.folderIndex)].relative };
            if (ImGui::Button("New Folder...")) { nf = {}; nf.open = nf.justOpened = true; nf.parent = req.destFolder; }
            const std::string why = sv.fileOpRefusal ? sv.fileOpRefusal(req) : std::string("unavailable");
            ImGui::TextColored(Theme::kError, "%s", why.c_str()); ImGui::BeginDisabled(!why.empty());   // the first refusal inline
            if (ImGui::Button("Move", ImVec2(92, 0))) { result.move = req; st.open = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndDisabled(); ImGui::SameLine(); if (ImGui::Button("Cancel", ImVec2(92, 0))) { st.open = false; ImGui::CloseCurrentPopup(); }
            // Nested (carry ruling): opened INSIDE Move's popup scope, so its
            // OpenPopup runs at popup-stack level 1 and keeps Move open behind it.
            result.newFolder = DrawNewFolderModal(nf, project);
            ImGui::EndPopup();
        }
        else if (!ImGui::IsPopupOpen("Move Assets##assetops")) st.open = false;   // closed from outside: a stale flag would gate the top-level New Folder off
        return result;
    }
}
