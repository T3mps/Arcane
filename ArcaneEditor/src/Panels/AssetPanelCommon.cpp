#include "Panels/AssetPanelCommon.hpp"

#include "Panels/CreateAssetDialog.hpp"   // CreateAssetKind
#include "Widgets/IconsLucide.h"

#include <imgui.h>

namespace Arcane::Editor
{
    void DrawCreateMenuEntries(AssetPanelActions& actions, bool enabled)
    {
        ImGui::BeginDisabled(!enabled);
        const auto entry = [&](const char* label, CreateAssetKind kind)
        {
            if (ImGui::MenuItem(label))
                actions.requestCreateKind = static_cast<int>(kind);
        };
        entry(ICON_LC_PALETTE " Material...",         CreateAssetKind::Material);
        entry(ICON_LC_LAYERS  " Material Instance...", CreateAssetKind::MaterialInstance);
        ImGui::Separator();
        entry(ICON_LC_BOX          " Mesh...",   CreateAssetKind::Mesh);
        entry(ICON_LC_STICKER      " Sprite...", CreateAssetKind::Sprite);
        entry(ICON_LC_CLAPPERBOARD " Scene...",  CreateAssetKind::Scene);
        ImGui::EndDisabled();
    }

    void DrawCreateMenu(AssetPanelActions& actions)
    {
        if (!ImGui::BeginPopup("##createmenu"))
            return;
        DrawCreateMenuEntries(actions, /*enabled=*/true);
        ImGui::EndPopup();
    }
}
