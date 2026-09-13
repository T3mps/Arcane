#include "Panels/AssetPanelCommon.hpp"

#include "Documents/DocumentHost.hpp"      // OpenAssetRow routes a non-scene open through it
#include "Panels/AssetBrowserPanel.hpp"    // AssetBrowserPanelState's full definition (RevealAssetInBrowser)
#include "Panels/AssetPanelModel.hpp"      // AssetPanelEntry/CookState/KindIcon/KindLabel/GroupParentOf
#include "Panels/CreateAssetDialog.hpp"    // CreateAssetKind
#include "Widgets/EditorFonts.hpp"         // PillWidth measures in AssetPill's own font
#include "Widgets/EditorTheme.hpp"         // Theme::kAmber -- the digest chip's refused segment
#include "Widgets/EditorWidgets.hpp"       // AssetPill
#include "Widgets/IconsLucide.h"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/Project.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// AssetPanelCommon (panel-split arc): what all three asset panels share --
// the AssetPanelActions/AssetPanelServices contracts and the band constants
// (header), the unified create menu, RevealAssetInBrowser, the eight
// cross-panel helpers, and the bottom-bar skeleton + health-digest chip.
//
// Tasks 4-6 promoted each cross-panel helper's LINKAGE to Arcane::Editor
// scope (declared on this header) while its BODY stayed in AssetsPanel.cpp,
// which still existed and still called most of them. Task 7 retires that
// file, so every one of those bodies lands HERE -- the destination the
// relocation ruling gives a helper with two or more calling TUs, which all
// eight have. Each body below is unchanged from before its move; only the
// file it sits in did.
//
// What did NOT come here, and why:
//   * GraphFocusLabel + the kGraphFocus* constants -> AssetGraphPanel.cpp.
//     Their two callers (the focus combo, the Graph bottom bar's "focus:"
//     clause) are both that panel's now, so one calling TU: it moves INTO
//     that TU rather than becoming a shared symbol nothing else asks for.
//   * kAssetsPreviewPaneDefaultWidth -> AssetBrowserPanel.hpp, beside the
//     state field whose default references it (the preview pane is
//     Browser-only, spec s9.4).
//   * AssetsGraphProjectionIsCurrent -- DELETED, not moved (spec s7.4).
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
        ImGui::Separator();
        // The editor<->IDE surface, step 3: code, not content -- lands under
        // Source/ (CreateKindRoot), same icon the Source rows carry.
        entry(ICON_LC_FILE_CODE    " C++ Class...", CreateAssetKind::CppClass);
        ImGui::EndDisabled();
    }

    void DrawCreateMenu(AssetPanelActions& actions)
    {
        if (!ImGui::BeginPopup("##createmenu"))
            return;
        DrawCreateMenuEntries(actions, /*enabled=*/true);
        ImGui::EndPopup();
    }

    // Task 8 dedupe: the identical "no project" message all three panels'
    // `if (!project)` guard drew inline.
    void DrawAssetPanelNoProjectMessage()
    {
        ImGui::TextDisabled("No project open (data/-next-to-exe)");
    }

    // Panel-split spec s7.2 (Task 3). Ported verbatim from the Unreferenced
    // card's own Reveal click handler (pre-split AssetsPanel.cpp) minus the
    // trailing `state.lens = Browse` write, which is the HOST's job now
    // (the caller focuses the Asset Browser tab after this returns -- see
    // EditorApp::ConsumeAssetPanelActions's `revealInBrowse` consumer).
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
    void RevealAssetInBrowser(AssetBrowserPanelState& state, AssetPanelModel& model,
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

    // =====================================================================
    // The cross-panel helper set (bodies homed here in Task 7)
    // =====================================================================

    // The project's recorded boot scene as a guid -- the ONE parse of
    // `Manifest().bootScene` any panel does, for every view that marks the
    // boot scene (the Browser's "boot" pill, the Status panel's scene cards,
    // the Graph panel's focus seed, spec s6). No project, or an empty/
    // unparseable bootScene, resolves to the NIL guid, which no real asset
    // guid ever equals, so the marker simply never lights up rather than
    // needing a second "is there one at all" flag at each call site.
    //
    // Cheap enough to call once per panel body per frame; deliberately NOT
    // called per ROW (the bodies hoist it into a local first).
    Arcane::Guid BootSceneGuid(const Arcane::Project* project)
    {
        return project
             ? Arcane::Guid::FromString(project->Manifest().bootScene).value_or(Arcane::Guid::Nil())
             : Arcane::Guid::Nil();
    }

    // Every Scene entry, name-sorted (ties broken on mount path, so the
    // order is total even for two scenes with the same stem). Two
    // consumers: the Status panel's Scenes rollup and the Graph panel's
    // focus combo -- the same list in the same order in both, which is
    // exactly what makes "the scene I just saw in Status" findable in the
    // combo. The sort is over POINTERS into model.Entries(), whose
    // iteration order is an unordered_map's and therefore not stable
    // frame-to-frame; sorting is what makes the combo's contents
    // deterministic at all, not merely tidy.
    std::vector<const AssetPanelEntry*> ScenesByName(const AssetPanelModel& model)
    {
        std::vector<const AssetPanelEntry*> scenes;
        for (const auto& [guid, entry] : model.Entries())
            if (entry.kind == AssetKind::Scene)
                scenes.push_back(&entry);
        std::sort(scenes.begin(), scenes.end(),
                  [](const AssetPanelEntry* a, const AssetPanelEntry* b)
                  { return a->name != b->name ? a->name < b->name : a->mountPath < b->mountPath; });
        return scenes;
    }

    // Materials-only subkind pill text (spec s3.1/s6: Fullscreen -> "post",
    // Sprite -> "sprite", Mesh -> "mesh"). nullptr when there is nothing to
    // show (non-material, or a material whose surface could not be read).
    const char* SubkindPillText(const AssetPanelEntry& e)
    {
        if (e.kind != AssetKind::Material || !e.surface)
            return nullptr;
        switch (*e.surface)
        {
            case Arcane::MaterialSurface::Sprite:     return "sprite";
            case Arcane::MaterialSurface::Mesh:       return "mesh";
            case Arcane::MaterialSurface::Fullscreen: return "post";
        }
        return nullptr;
    }

    const char* CookStateLabel(CookState cook)
    {
        switch (cook)
        {
            case CookState::Cooked:  return "Cooked";
            case CookState::Queued:  return "Queued";
            case CookState::Refused: return "Refused";
            case CookState::Unknown: return "Unknown";
        }
        return "Unknown";
    }

    // ---- Plan 2 Task 7: AssetPill's own width, WITHOUT drawing it ------
    // 12px text plus the two FramePadding.x cheeks (EditorWidgets.cpp's
    // AssetPill, verbatim). The Status panel's attention card positions its
    // trailing pill by hand and has to ellipsize the NAME against whatever
    // room is left after it, so it needs the pill's width one item early;
    // the Graph panel's node chrome budgets the same way.
    float PillWidth(const char* text)
    {
        ImGui::PushFont(GetEditorFonts().interRegular, 12.0f);
        const float w = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::PopFont();
        return w;
    }

    // Resolve + route a double-click / Enter-open. Copied VERBATIM from
    // AssetBrowser.cpp:162-179's routing: a scene is not a DocumentHost
    // document (it replaces the editing session), so its path comes back
    // in `actions.openScene` for the host to load under the unsaved-
    // changes guard; every other kind opens through `docs`.
    void OpenAssetRow(const AssetPanelEntry& e, const Arcane::Project* project,
                      DocumentHost& docs, AssetPanelActions& actions)
    {
        if (!project)
            return;
        const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(e.guid));
        if (path)
        {
            if (e.kind == AssetKind::Scene)
                actions.openScene = *path;
            else if (e.kind == AssetKind::Source)
                actions.openInIde = *path;   // the IDE is a source file's editor, not a document
            else
                docs.OpenPath(*path);
        }
        else
            ARC_WARN("Assets: '{}' did not resolve to a file", e.mountPath);
    }

    // ---- Task 10: the unified asset context menu's ITEMS (spec s6) -----
    // The menu BODY, with no popup bracket of its own, so that every
    // representation of an asset raises the SAME menu from the SAME code:
    // a Browser row opens it through BeginPopupContextItem
    // (DrawRowContextMenu, AssetBrowserPanel.cpp), a Graph node opens it
    // through ed::ShowNodeContextMenu + BeginPopup inside a CanvasPopupScope
    // (DrawAssetGraphBody). Extracted in Task 4 for exactly that second
    // caller -- spec s6 says "the unified context menu, every lens", and two
    // copies of a list is how "unified" quietly stops being true.
    //
    // `kindSpecific` gates the Material/Scene/Texture leading entries --
    // Type::Child rows are always folded 1:1 sprites, so none of those
    // three ever apply to one and the caller passes false to skip them.
    //
    // Deliberately does NOT touch the selection: "right-click selects" is
    // the OPENING gesture's business (it must happen once, when the menu
    // opens, not on every frame the popup is drawn), so each caller does
    // it at its own open site.
    void DrawAssetMenuItems(AssetPanelActions& actions, const AssetPanelEntry& e,
                            bool kindSpecific)
    {
        if (kindSpecific)
        {
            bool any = false;
            if (e.kind == AssetKind::Material)
            {
                any = true;
                if (ImGui::MenuItem("New Instance..."))
                    actions.createInstanceOf = e.guid;
            }
            if (e.kind == AssetKind::Scene)
            {
                any = true;
                if (ImGui::MenuItem("Set as Boot Scene"))
                    actions.setBootScene = e.guid;
            }
            if (e.kind == AssetKind::Texture)
            {
                any = true;
                if (ImGui::MenuItem("Create Sprite"))
                    actions.createSpriteFrom = e.guid;
            }
            if (any)
                ImGui::Separator();
        }

        if (ImGui::BeginMenu("Create"))
        {
            // Live since Task 13 -- see DrawCreateMenuEntries's own
            // comment on the `enabled` parameter.
            DrawCreateMenuEntries(actions, /*enabled=*/true);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Show in Explorer"))
            actions.showInExplorer = e.guid;
        if (ImGui::MenuItem("Copy Path"))
            actions.copyPath = e.guid;
        if (ImGui::MenuItem("Copy Guid"))
            actions.copyGuid = e.guid;
    }

    namespace
    {
        // The peek tooltip's own fixed geometry. DrawAssetPeekTooltip below
        // is their only reader, which is why they came here with it rather
        // than to the header's shared-constant block.
        constexpr float kTooltipWidth     = 210.0f;
        constexpr float kTooltipThumbSize = 64.0f;

        // ---- Plan 3 Task 4: the Graph tooltip's edge-summary lines ---------
        // Read off the SAME AssetReferenceIndex the graph itself is projected
        // from, so the numbers can never disagree with the wires on screen.
        // Two dim lines: the counts, then up to three of the outbound targets
        // BY NAME (Interactions-FINAL's "one extra line" carries both a count
        // clause and named neighbours; splitting them is what keeps a
        // three-filename list inside the 210px tooltip).
        //
        // "M out" counts DISTINCT targets, not raw refs: one source may name
        // one target twice (two material slots pointing at one material), and
        // "2 out" beside a single listed name reads as a bug in the panel
        // rather than as a fact about the asset. `inbound` needs no such care
        // -- AssetReferenceIndex keeps it sorted-unique by contract.
        //
        // File-local here for the same reason it was file-local before the
        // move: its ONE caller is DrawAssetPeekTooltip, immediately below.
        void DrawGraphEdgeSummary(const AssetPanelModel& model, const Arcane::Guid& guid)
        {
            const AssetReferenceIndex::Node* node = model.RefIndex().Find(guid);
            if (!node)
                return;   // never walked and never named as a target

            std::vector<Arcane::Guid> targets;
            targets.reserve(node->outbound.size());
            for (const Arcane::AssetRef& r : node->outbound)
                if (std::find(targets.begin(), targets.end(), r.target) == targets.end())
                    targets.push_back(r.target);

            ImGui::TextDisabled("%d in \xC2\xB7 %d out",
                                static_cast<int>(node->inbound.size()),
                                static_cast<int>(targets.size()));
            if (targets.empty())
                return;

            constexpr std::size_t kNamedTargets = 3;
            std::string line;
            for (std::size_t i = 0; i < targets.size() && i < kNamedTargets; ++i)
            {
                if (i != 0)
                    line += ", ";
                // A target with no entry is a tombstone (or an asset this
                // walk has not reached yet): the short-guid form is the same
                // name the graph's own ghost node wears for it.
                const AssetPanelEntry* t = model.Find(targets[i]);
                line += t ? t->fileName : targets[i].ToString().substr(0, 8);
            }
            if (targets.size() > kNamedTargets)
            {
                char more[24];
                std::snprintf(more, sizeof(more), " +%d more",
                              static_cast<int>(targets.size() - kNamedTargets));
                line += more;
            }
            // Wrapped, unlike the fixed-width lines above it: three file names
            // routinely overrun 210px, and the tooltip's SetNextWindowSize
            // pins the WIDTH only (height is auto), so wrapping grows the box
            // instead of clipping the text.
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    }

    // ---- Task 10: the peek tooltip (spec s8) ---------------------------
    // Cross-panel: Browser rows, child rows and the preview pane's Derived
    // list, the Graph panel's node hover, and the Status panel's activity
    // feed all hover the same asset. Text-and-images only -- never a button
    // (a tooltip is not interactable).
    //
    // `forceShow` (Plan 2 Task 8): TimelineFeed draws every row's hover
    // hit-test INSIDE its own per-row loop (EditorWidgets.cpp), so by
    // the time this file's caller can react to the result, ImGui's
    // "last submitted item" is whichever row TimelineFeed drew LAST --
    // never necessarily the hovered one. The activity feed already
    // knows (from TimelineFeedResult::hoveredIndex, computed at the
    // right moment) that a specific row IS hovered, so it passes true
    // here to skip the (now-wrong) IsItemHovered() re-check entirely.
    // BeginTooltip() itself positions near the mouse regardless of
    // "last item", so this is safe. Every existing call site keeps the
    // default and is unaffected.
    //
    // `withEdgeSummary` (Plan 3 Task 4): the Graph panel's one extra line
    // (Interactions-FINAL's Graph column -- "same tooltip + one extra
    // line"). Off for every other caller, which is why it is a defaulted
    // parameter rather than a second helper: the anatomy above is spec
    // s8's and must stay ONE list, not two that drift.
    void DrawAssetPeekTooltip(const AssetPanelModel& model, const AssetPanelServices& services,
                              const Arcane::Guid& guid, bool forceShow,
                              bool withEdgeSummary)
    {
        if (!forceShow && !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            return;
        const AssetPanelEntry* e = model.Find(guid);
        if (!e)
            return;

        ImGui::SetNextWindowSize(ImVec2(kTooltipWidth, 0.0f));
        ImGui::BeginTooltip();

        const std::uint64_t thumb = services.resolveAssetThumb ? services.resolveAssetThumb(guid) : 0;
        if (thumb != 0)
        {
            ImGui::Image(static_cast<ImTextureID>(thumb), ImVec2(kTooltipThumbSize, kTooltipThumbSize));
        }
        else
        {
            const ImVec2 boxMin = ImGui::GetCursorScreenPos();
            const char* icon = KindIcon(e->kind);
            const ImVec2 iconSize = ImGui::CalcTextSize(icon);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(boxMin.x + (kTooltipThumbSize - iconSize.x) * 0.5f,
                       boxMin.y + (kTooltipThumbSize - iconSize.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), icon);
            ImGui::Dummy(ImVec2(kTooltipThumbSize, kTooltipThumbSize));
        }

        ImGui::TextUnformatted(e->fileName.c_str());
        ImGui::SameLine();
        AssetPill(KindLabel(e->kind));
        if (const char* sub = SubkindPillText(*e))
        {
            ImGui::SameLine();
            AssetPill(sub);
        }
        if (e->isInstance)
        {
            ImGui::SameLine();
            AssetPill("inst");
        }

        ImGui::TextDisabled("%s", e->mountPath.c_str());
        ImGui::TextDisabled("%s", CookStateLabel(e->cook));
        ImGui::TextDisabled("%s", e->guid.ToString().c_str());
        if (withEdgeSummary)
            DrawGraphEdgeSummary(model, guid);

        ImGui::EndTooltip();
    }

    // =====================================================================
    // The shared bottom-bar skeleton + digest chip (spec s9.2, Task 7)
    // =====================================================================
    // Pre-split there was ONE bottom bar with a three-way branch on the
    // active lens. Post-split there are three bars in three windows, and the
    // only parts that differ are the left context string and what (if
    // anything) occupies the right slot -- so the band itself, its divider
    // and its right-alignment math are spelled once here and each panel
    // supplies the two ends.

    AssetPanelBottomBar BeginAssetPanelBottomBar(const char* id)
    {
        AssetPanelBottomBar bar;
        if (!ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            return bar;   // not visible; the caller still owes EndAssetPanelBottomBar
        bar.visible = true;

        // A hairline divider from the body above, painted directly rather
        // than via ImGui::Separator() -- that call consumes its own layout
        // row, which would push this child past the 24px the caller already
        // reserved for it (its body child ends at
        // ImVec2(0, -kAssetPanelBottomBarHeight)).
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetWindowPos();
            dl->AddLine(p0, ImVec2(p0.x + ImGui::GetWindowWidth(), p0.y),
                       ImGui::GetColorU32(ImGuiCol_Separator));
        }

        // The right edge of the content region, captured before drawing
        // anything -- GetCursorPosX() + GetContentRegionAvail().x is
        // invariant here (no columns/tables in play), so it is safe to
        // read once and reuse for a right-aligned slot.
        bar.rightEdgeX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        bar.padY = std::max(0.0f, (kAssetPanelBottomBarHeight - ImGui::GetTextLineHeight()) * 0.5f);

        ImGui::SetCursorPosY(bar.padY);
        return bar;
    }

    void EndAssetPanelBottomBar()
    {
        ImGui::EndChild();
    }

    void DrawAssetPanelHealthDigest(const AssetPanelBottomBar& bar, const AssetPanelModel& model,
                                    const AssetPanelServices& services, AssetPanelActions& actions)
    {
        const HealthCounts health = model.Health();

        // `refusedPart` + `restPart` concatenated character-for-character is
        // what gets DRAWN below (two colored segments, zero SameLine spacing
        // between them), so measuring their concatenation is exactly the
        // width that draw occupies.
        char refusedPart[48];
        std::snprintf(refusedPart, sizeof(refusedPart), "%s %d refused",
                      ICON_LC_TRIANGLE_ALERT, health.refused);
        char restPart[96];
        std::snprintf(restPart, sizeof(restPart),
                      " \xC2\xB7 %d cooking \xC2\xB7 %d unused", health.queued, health.unused);
        char digestFull[160];
        std::snprintf(digestFull, sizeof(digestFull), "%s%s", refusedPart, restPart);
        const float digestWidth = ImGui::CalcTextSize(digestFull).x;

        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), bar.rightEdgeX - digestWidth));
        ImGui::SetCursorPosY(bar.padY);
        const ImVec2 digestScreenPos = ImGui::GetCursorScreenPos();
        ImGui::TextColored(Theme::kAmber, "%s", refusedPart);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextDisabled("%s", restPart);

        // Digest click-through (spec s5; the routing + gate are spec
        // s7.1/s7.3, panel-split Task 3): an InvisibleButton laid over the
        // rect just drawn -- captured AFTER the two TextColored/TextDisabled
        // calls above and rewound onto them, since neither is meant to change
        // appearance on hover/press, so a bare hit-test overlay is the
        // smaller change (the Status panel's DrawAttentionCard already
        // overlays a full-body InvisibleButton for the identical reason).
        // Raises actions.showStatus rather than writing any sibling panel's
        // state -- Status is a cross-WINDOW command's target now. The counts
        // drawn above never grey (s7.3: information first, the chip never
        // disappears); only the click affordance goes inert when Status is
        // closed (R1: nothing opens a panel except the Window menu), with a
        // tooltip on hover explaining why.
        ImGui::SetCursorScreenPos(digestScreenPos);
        if (ImGui::InvisibleButton("##digestclick", ImVec2(digestWidth, ImGui::GetTextLineHeight())) &&
            services.statusOpen)
            actions.showStatus = true;
        if (!services.statusOpen && ImGui::IsItemHovered())
            ImGui::SetTooltip("Asset Status is closed \xE2\x80\x94 open it from Window \xE2\x96\xB8");
    }
}
