#include "Panels/AssetPanelCommon.hpp"

#include "Panels/AssetsPanel.hpp"         // AssetsPanelState's full definition (RevealAssetInBrowser)
#include "Panels/CreateAssetDialog.hpp"   // CreateAssetKind
#include "Widgets/IconsLucide.h"

#include <imgui.h>

#include <string>

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

    // Panel-split spec s7.2 (Task 3). Ported verbatim from the Unreferenced
    // card's own Reveal click handler (pre-split AssetsPanel.cpp) minus the
    // trailing `state.lens = Browse` write, which is the HOST's job now
    // (the caller sets the lens after this returns -- see
    // EditorApp::ConsumeBrowserActions's `revealInBrowse` consumer).
    //
    // Ruling 10 (asset-manager Plan 2 Task 8 review): clearing filters
    // alone does not guarantee the row is VISIBLE -- a collapsed ancestor
    // group (or a collapsed mount root, e.g. diag://'s own default-closed
    // state, GroupDefaultOpen) still hides it. Walk `e->folder` up through
    // every ancestor (GroupParentOf -- the same chain DrawGroupRow's own
    // nesting walks, terminating at "" for a mount's own root) and force
    // each one open, through BOTH writers DrawGroupRow's own toggle uses:
    // the state mirror (GroupIsOpen's source of truth for the chevron
    // glyph) and the model (SetGroupOpen -- the actual Rows() rebuild
    // trigger). The mount root itself is included: it is simply the LAST
    // non-empty value this loop visits before GroupParentOf finally
    // returns "".
    //
    // Addendum (controller ruling, extending Ruling 10): a folded 1:1
    // derived sprite with zero inbound is STILL unused-eligible (kind
    // Sprite), so it can be a Reveal target while living under its
    // texture's own CLOSED fold. The ancestry walk above opens every
    // ancestor GROUP but says nothing about fold state, which is a
    // separate flag keyed by the PARENT TEXTURE's guid (`foldedUnder`),
    // not by folder -- so it gets its own write, the same two-map
    // spelling the fold chevron's own toggle uses (the row expander
    // handler: state.childrenOpen + model.SetChildrenOpen, both keyed by
    // the parent's guid).
    void RevealAssetInBrowser(AssetsPanelState& state, AssetPanelModel& model,
                              const Arcane::Guid& guid)
    {
        const AssetPanelEntry* e = model.Find(guid);
        if (!e)
            return;   // stale by the time the action was consumed -- nothing to reveal

        model.SetSearch("");
        state.search[0] = '\0';
        model.SetKindFilter(-1);
        state.railKind = -1;   // -1 = All, the rail's own spelling

        for (std::string folder = e->folder; !folder.empty(); folder = GroupParentOf(folder))
        {
            state.groupOpen[folder] = true;
            model.SetGroupOpen(folder, true);
        }

        if (e->foldedUnder.IsValid())
        {
            state.childrenOpen[e->foldedUnder] = true;
            model.SetChildrenOpen(e->foldedUnder, true);
        }

        model.Select(guid);
    }
}
