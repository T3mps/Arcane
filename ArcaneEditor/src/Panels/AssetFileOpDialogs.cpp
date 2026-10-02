#include "Panels/AssetFileOpDialogs.hpp"

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
}
