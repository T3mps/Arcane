#include "Panels/AssetsPanel.hpp"

#include "Documents/DocumentHost.hpp"
#include "Panels/AssetActivityLog.hpp"    // AssetActivityEntry/Kind (Task 8's feed, the first reader)
#include "Panels/AssetBrowserPanel.hpp"   // DrawAssetBrowserBody -- the Browse lens's body (Task 6, panel-split)
#include "Panels/AssetGraphPanel.hpp"   // DrawAssetGraphBody + SeedAssetGraphFocus -- the Graph lens's body + boot-scene seed (Task 5, panel-split)
#include "Panels/AssetStatusPanel.hpp"    // DrawAssetStatusBody -- the Status lens's body (Task 4, panel-split)
#include "Widgets/EditorFonts.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/IconsLucide.h"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/Project.hpp>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Arcane::Editor
{
    namespace
    {
        // Plan 3 Task 5: the width of the toolbar's PER-LENS slot when the
        // Graph lens fills it with its focus combo (spec s5 -- the slot is
        // empty on every other lens, so this width leaves the layout too).
        // A fixed width, not a content-derived one: the search well's flex
        // math subtracts it BEFORE the combo is drawn, and a label-derived
        // width would make the search box jump every time the user picked a
        // differently-named scene. 230px is the BOARD's own value, read off
        // `OptionD.dc.html`'s focus well (`width: 230px`) rather than
        // guessed -- longer names ellipsize inside the combo rather than
        // stealing the search well's room.
        constexpr float kGraphFocusComboWidth = 230.0f;

        // The Graph lens's "no scope root" label -- spelled ONCE, because
        // the combo's preview, the combo's own first entry and the bottom
        // bar's "focus:" clause must all read identically (ruling 6's nil
        // focus, in words).
        constexpr const char* kGraphFocusEverything = "everything";
        // ...and what the same three places say when `graphFocus` names an
        // asset the model no longer has an entry for -- a scene deleted
        // while it was the focus. NOT "everything": the projection does not
        // fall back to everything-mode there (AssetGraphViewModel::Build
        // either builds a tombstone-rooted view or, for a guid the reference
        // index cannot explain either, nothing at all), so saying
        // "everything" would describe a graph that is not on screen.
        constexpr const char* kGraphFocusMissing = "(missing)";

        // The lens strip's three labels, fixed regardless of which plan has
        // landed (spec s5: "Plan 1 ships the full three-button strip ...
        // layout pinned from day one, later plans enable, nothing shifts").
        // constexpr on a non-reference array makes every element itself
        // const, so the decayed pointer is `const char* const*` --
        // SegmentedStrip's exact parameter type, no cast needed.
        constexpr const char* kLensLabels[] = { "Browse", "Graph", "Status" };
        constexpr int kLensCount = 3;
        // All three bits: Browse (0) since Plan 1, Status (2) since Plan 2
        // Task 7, Graph (1) since Plan 3 Task 5 -- the mask is now saturated
        // and there is no fourth lens to gate. The strip's LAYOUT never
        // changed across any of the three: the same three buttons have been
        // drawn since Plan 1 at the same widths in the same order (spec
        // s5's "later plans enable, nothing shifts"), and only this mask ever
        // moved. Kept as a named constant rather than folded away, because
        // SegmentedStrip's own signature takes one and a future lens would
        // otherwise have nowhere to say "not yet".
        constexpr unsigned kLensEnabledMask = 0b111u;

        // Task 10 (spec s6/s11.2) fixed geometry. kTableRowHeight moved to
        // AssetPanelCommon.hpp in Task 4 (panel-split): the Status lens's
        // Unreferenced card (AssetStatusPanel.cpp, its own TU now) draws its
        // rows at this exact pitch too, so the constant needs to be
        // reachable from both TUs -- see that header's own comment.
        // kRailWidth/kRailRowHeight/kChildIndent/kGroupIndent moved to
        // AssetBrowserPanel.cpp in Task 6 (panel-split): the rail and the
        // grouped table are Browse-only now, and nothing left in this file
        // still reads them. kTooltipWidth/kTooltipThumbSize stay HERE --
        // DrawAssetPeekTooltip (below, still this file's own -- Browse,
        // Graph and Status all call it) is their only reader.
        constexpr float kTooltipWidth     = 210.0f;
        constexpr float kTooltipThumbSize = 64.0f;

        // KindIcon/KindLabel (the row icon glyph / the peek tooltip's kind
        // pill text): Panels/AssetPanelModel.hpp's shared definitions, as of
        // Task 15 -- this file's own copies (originally lifted from
        // AssetBrowser.cpp's internal-linkage duplicates) are retired in
        // favor of the one canonical source every representation now shares.

        }   // end anonymous namespace: CookStateLabel below is genuinely
            // cross-lens too (Task 6, panel-split) -- DrawAssetPeekTooltip
            // (this file) and the preview pane (AssetBrowserPanel.cpp, its
            // own TU as of this task) format the same CookState the same
            // way, and two copies is exactly the drift risk every other
            // promotion on this file already guards against. Promoted to
            // Arcane::Editor scope + declared in AssetPanelCommon.hpp, same
            // fix as BootSceneGuid/ScenesByName/DrawAssetPeekTooltip above.
            // Body unchanged from before the promotion.

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

    // SubkindPillText below is genuinely cross-lens too (Task 5,
    // panel-split) -- a Graph node's body pill reads a material's subkind
    // the same way a Browse row does, and AssetGraphPanel.cpp (its own TU
    // as of Task 5) needs the SAME text, not a second copy. Promoted to
    // Arcane::Editor scope + declared in AssetPanelCommon.hpp, same fix as
    // BootSceneGuid/ScenesByName/DrawAssetPeekTooltip/PillWidth above. Body
    // unchanged from before the promotion.

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

    // ContentRelativePath/RailKindCreatable/GroupIsOpen/ChildrenAreOpen moved
    // to AssetBrowserPanel.cpp in Task 6 (panel-split): all four were
    // Browse-only (the preview pane's path row, the rail's "+" gate, the
    // grouped table's fold-state reads), and nothing left in this file calls
    // any of them.

    // BootSceneGuid/ScenesByName below are genuinely cross-lens (Browse's
    // boot pill/preview, Status's Scenes rollup, Graph's focus combo/seed),
    // and as of Task 4 (panel-split) Status is its own TU
    // (AssetStatusPanel.cpp) -- an anonymous namespace's internal linkage
    // would hide these from it, the same "cross-panel helper" problem
    // RevealAssetInBrowser (AssetPanelCommon.*) already solved once.
    // Promoted to Arcane::Editor scope + declared in AssetPanelCommon.hpp;
    // every caller in THIS file keeps calling them unqualified (ordinary
    // enclosing-namespace lookup still finds them). Bodies unchanged from
    // before the promotion.

    // The project's recorded boot scene as a guid -- the ONE parse of
    // `Manifest().bootScene` this file does, for every lens that marks the
    // boot scene (the Browse lens's "boot" pill, the Status lens's scene
    // cards, spec s6). No project, or an empty/unparseable bootScene,
    // resolves to the NIL guid, which no real asset guid ever equals, so
    // the marker simply never lights up rather than needing a second
    // "is there one at all" flag at each call site.
    //
    // Cheap enough to call once per lens body per frame; deliberately NOT
    // called per ROW (the lens bodies hoist it into a local first).
    Arcane::Guid BootSceneGuid(const Arcane::Project* project)
    {
        return project
             ? Arcane::Guid::FromString(project->Manifest().bootScene).value_or(Arcane::Guid::Nil())
             : Arcane::Guid::Nil();
    }

    // Every Scene entry, name-sorted (ties broken on mount path, so the
    // order is total even for two scenes with the same stem). Two
    // consumers: the Status lens's Scenes rollup and the Graph lens's
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

    namespace
    {
        // What the Graph lens's current scope root is CALLED -- the combo's
        // preview text and the bottom bar's "focus:" clause, one spelling so
        // the two bands can never disagree about what is on screen. The
        // returned pointer is either a literal or borrowed from the model's
        // entry (stable for the frame -- Find()'s own doc comment; nothing
        // between here and the draw mutates the model).
        //
        // fileName, not name: the render comparison against
        // `OptionD-Graph-FINAL.png` caught the stem spelling naming the SAME
        // scene two ways one band apart -- the graph's own node header says
        // "main.arcscene" (DrawGraphNode) and the Status lens's scene cards
        // say "main.arcscene" (DrawSceneCard), so a toolbar reading "main"
        // was the panel's only dissenting voice. The board agrees
        // (`focus: main.arcscene`).
        const char* GraphFocusLabel(const AssetPanelModel& model, const Arcane::Guid& focus)
        {
            if (!focus.IsValid())
                return kGraphFocusEverything;
            const AssetPanelEntry* e = model.Find(focus);
            return e ? e->fileName.c_str() : kGraphFocusMissing;
        }

        }   // end anonymous namespace: OpenAssetRow below is genuinely
            // cross-lens too (Task 5, panel-split) -- a Graph node's
            // double-click routes an open exactly the way a Browse row's does
            // (spec s6's verbatim-behavior clause; see the def's own comment),
            // and AssetGraphPanel.cpp (its own TU as of Task 5) needs the SAME
            // routing, not a second copy. Promoted to Arcane::Editor scope +
            // declared in AssetPanelCommon.hpp, same fix as BootSceneGuid/
            // ScenesByName above. Body unchanged from before the promotion.

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
            else
                docs.OpenPath(*path);
        }
        else
            ARC_WARN("Assets: '{}' did not resolve to a file", e.mountPath);
    }

    namespace
    {
        // Toolbar band: + Create -> search (flex) -> [per-lens slot: Graph's
        // focus combo, Plan 3 Task 5; EMPTY on Browse and Status] -> lens
        // strip anchored right-most (spec s5). Mutates `state` in place; the
        // create popup's entries are LIVE from Task 12 -- they set
        // `actions.requestCreateKind`, which EditorApp routes to the one
        // BeginCreateAsset entry.
        void DrawToolbar(AssetsPanelState& state, AssetPanelModel& model, AssetPanelActions& actions)
        {
            ImGuiStyle& style = ImGui::GetStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(style.FramePadding.x, kAssetPanelToolbarFramePadY));

            if (ImGui::Button(ICON_LC_PLUS " Create " ICON_LC_CHEVRON_DOWN))
                ImGui::OpenPopup("##createmenu");
            DrawCreateMenu(actions);

            ImGui::SameLine();

            // Search well width = remaining minus the lens strip minus the
            // per-lens slot (0 unless the Graph lens is showing its focus
            // combo). Each subtracted widget also costs the ItemSpacing.x
            // that its own SameLine inserts BEFORE it, so the slot's charge
            // is its width PLUS one spacing -- the strip's is already
            // spelled the same way on the line below.
            float stripWidth = 0.0f;
            for (const char* label : kLensLabels)
                stripWidth += ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
            const bool  showFocusCombo = (state.lens == AssetLens::Graph);
            const float focusSlotWidth = showFocusCombo
                                       ? kGraphFocusComboWidth + style.ItemSpacing.x
                                       : 0.0f;
            // The 80px floor is what keeps the search well usable (and its
            // width non-negative) on a panel too narrow to pay for all three
            // bands -- ImGui then simply lets the row overflow to the right
            // rather than this code handing it a negative width.
            const float searchWidth = std::max(80.0f,
                ImGui::GetContentRegionAvail().x - stripWidth - style.ItemSpacing.x - focusSlotWidth);

            ImGui::SetNextItemWidth(searchWidth);
            ImGui::InputTextWithHint("##assetssearch", ICON_LC_SEARCH " search...",
                                     state.search, sizeof(state.search));
            model.SetSearch(state.search);
            model.SetKindFilter(state.railKind);

            // ---- the per-lens slot: Graph's focus combo (spec s5/s10) -----
            // Scope the graph to ONE scene, or to "everything" (ruling 6's
            // nil focus). Writing state.graphFocus is all this takes: the
            // lens's own dirty check compares graphBuiltFocus and rebuilds
            // the projection on the next frame, so there is no rebuild call
            // to make here.
            if (showFocusCombo)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(kGraphFocusComboWidth);
                // "focus: <name>" -- the BOARD's exact preview string
                // (`OptionD.dc.html`: `<span>focus:</span> main.arcscene`),
                // per the controller's board-strings-win ruling. The board
                // paints its "focus:" half in kTextDim and the name in kText;
                // BeginCombo's preview is a single string in a single colour,
                // so the two-tone half of that is not expressible here without
                // replacing the combo with a hand-drawn widget -- not invented,
                // see the Task 5 fix report.
                char focusPreview[160];
                std::snprintf(focusPreview, sizeof(focusPreview), "focus: %s",
                              GraphFocusLabel(model, state.graphFocus));
                if (ImGui::BeginCombo("##graphfocus", focusPreview))
                {
                    if (ImGui::Selectable(kGraphFocusEverything, !state.graphFocus.IsValid()))
                        state.graphFocus = Arcane::Guid{};
                    // PushID per row, keyed by the guid: two scenes may share
                    // a stem ("main.arcscene" in two folders), and ImGui would
                    // otherwise give both Selectables the SAME id -- clicking
                    // either would activate the first.
                    for (const AssetPanelEntry* s : ScenesByName(model))
                    {
                        ImGui::PushID(s->guid.ToString().c_str());
                        // fileName for the same reason GraphFocusLabel uses
                        // it: this list and the Status lens's scene cards are
                        // the same scenes, and they read identically there.
                        if (ImGui::Selectable(s->fileName.c_str(), s->guid == state.graphFocus))
                            state.graphFocus = s->guid;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::SameLine();
            const int clickedLens = SegmentedStrip("##lens", kLensLabels, kLensCount,
                                                    static_cast<int>(state.lens), kLensEnabledMask);
            if (clickedLens >= 0)
                state.lens = static_cast<AssetLens>(clickedLens);

            ImGui::PopStyleVar();
        }

        // Bottom bar band: left = context ("N assets - S selected", becoming
        // "X of N shown" once rail or search filters) -- right = the digest
        // chip (amber refused count + dim cooking/unused). All three numbers
        // are LIVE as of Plan 2 Task 4: `unused` used to render as a literal
        // em-dash (spec s13 -- the digest never fabricates a 0 for a number it
        // cannot know) because HealthCounts had no such field; the model's
        // AssetReferenceIndex now supplies it.
        //
        // Plan 2 Task 8 note, superseded by panel-split Task 3: `state` used
        // to arrive non-const so the digest click-through could switch
        // `state.lens` directly, the same "the Draw* function mutates state
        // in place" convention DrawToolbar's own SegmentedStrip handling
        // uses a few lines above this one. Spec s7.1 retires that -- Status
        // is a cross-window command's target now, not a sibling lens of
        // this same panel -- so the click raises `actions.showStatus`
        // instead and `state` reverts to a plain read (the left context's
        // "X of N shown"/Graph-focus text below); `services` arrives beside
        // `actions` for the digest's own R1 gate (s7.3). This function has
        // no header declaration to keep in step (it is file-local, like
        // every other Draw* helper above) -- only its single call site in
        // DrawAssetsPanel changes.
        void DrawBottomBar(const AssetsPanelState& state, const AssetPanelModel& model,
                           const AssetPanelServices& services, AssetPanelActions& actions)
        {
            if (!ImGui::BeginChild("##assetsbottombar", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            {
                ImGui::EndChild();
                return;
            }

            // A hairline divider from the body above, painted directly
            // rather than via ImGui::Separator() -- that call consumes its
            // own layout row, which would push this child past the 24px the
            // caller already reserved for it (DrawAssetsPanel's
            // BeginChild("##assetsbody", ImVec2(0, -kAssetPanelBottomBarHeight))).
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p0 = ImGui::GetWindowPos();
                dl->AddLine(p0, ImVec2(p0.x + ImGui::GetWindowWidth(), p0.y),
                           ImGui::GetColorU32(ImGuiCol_Separator));
            }

            // The right edge of the content region, captured before drawing
            // anything -- GetCursorPosX() + GetContentRegionAvail().x is
            // invariant here (no columns/tables in play), so it is safe to
            // read once and reuse for the right-aligned digest below.
            const float rightEdgeX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            const float padY = std::max(0.0f, (kAssetPanelBottomBarHeight - ImGui::GetTextLineHeight()) * 0.5f);

            const HealthCounts health = model.Health();
            const bool filtered = model.Filtered();

            ImGui::SetCursorPosY(padY);
            // 128, not 64: Graph's form below embeds a SCENE NAME, and a real
            // one ("prototype_courtyard_lighting") overflows 64 on its own --
            // snprintf would then truncate mid-name with no other symptom.
            char left[128];
            // Graph reports what the SCOPED projection is showing out of the
            // whole project, plus the scope root itself -- N is
            // realNodeCount, which counts real ASSET nodes only (ruling 12:
            // neither the synthetic "+N more" companions nor tombstones are
            // assets). The wording is the BOARD's, verbatim (`OptionD.dc.html`:
            // `6 of 15 assets &middot; focus: main.arcscene`) per the
            // controller's board-strings-win ruling -- the word "assets" was
            // missing from the brief's format string.
            //
            // THE GATE (review finding I1). N is printed ONLY when the
            // projection on screen was built for the focus and the entries
            // this bar is about to name. It is not always: the Status lens's
            // "Focus in Graph" button flips `state.lens` from INSIDE the
            // already-dispatched Status body, so DrawGraphLens does not run
            // that frame at all -- yet this bar, which runs after the body,
            // already reads the NEW lens and would otherwise pair the PREVIOUS
            // build's realNodeCount (often 0 -- the lens may never have been
            // opened) with the new focus name. Spec §13: the bar never renders
            // an unknown as a zero; unknown is an em dash. Self-corrects on
            // the following frame, when the Graph body has actually run.
            //
            // The predicate itself is AssetsGraphProjectionIsCurrent (declared
            // in the header, defined at the bottom of this file) rather than a
            // conjunction spelled here, so the canvas test can ask the panel's
            // own question instead of restating it.
            const bool graphCurrent = AssetsGraphProjectionIsCurrent(state, model);
            // Status keeps ONE fixed form regardless of filter state (the lens
            // has no search box of its own to filter against); every other
            // lens keeps Browse's existing forms VERBATIM (plan doc Step 4).
            if (state.lens == AssetLens::Graph)
            {
                char shown[16];
                if (graphCurrent)
                    std::snprintf(shown, sizeof(shown), "%d", state.graph.realNodeCount);
                else
                    std::snprintf(shown, sizeof(shown), "\xE2\x80\x94");   // U+2014 EM DASH
                std::snprintf(left, sizeof(left), "%s of %d assets \xC2\xB7 focus: %s",
                              shown, health.total,
                              GraphFocusLabel(model, state.graphFocus));
            }
            else if (state.lens == AssetLens::Status)
                std::snprintf(left, sizeof(left), "%d assets \xC2\xB7 %d need attention",
                              health.total, health.refused + health.queued);
            else if (filtered)
                std::snprintf(left, sizeof(left), "%d of %d shown",
                              model.ShownAssetCount(), health.total);
            else
                std::snprintf(left, sizeof(left), "%d assets \xC2\xB7 %d selected",
                              health.total, model.selected.IsValid() ? 1 : 0);
            ImGui::TextUnformatted(left);

            // Digest chip, right-aligned. `refusedPart` + `restPart`
            // concatenated character-for-character is what gets DRAWN below
            // (two colored segments, zero SameLine spacing between them), so
            // measuring their concatenation is exactly the width that draw
            // occupies.
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
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightEdgeX - digestWidth));
            ImGui::SetCursorPosY(padY);
            const ImVec2 digestScreenPos = ImGui::GetCursorScreenPos();
            ImGui::TextColored(Theme::kAmber, "%s", refusedPart);
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextDisabled("%s", restPart);

            // Digest click-through (spec s5; the routing + gate are now
            // spec s7.1/s7.3, panel-split Task 3): an InvisibleButton laid
            // over the rect just drawn -- captured BEFORE the two
            // TextColored/TextDisabled calls above, since neither is meant
            // to change appearance on hover/press, a bare hit-test overlay
            // is the smaller change (this file already overlays a
            // full-body InvisibleButton for the identical reason in
            // DrawAttentionCard). Raises actions.showStatus rather than
            // writing state.lens directly -- Status is a cross-window
            // command's target now. The counts drawn above never grey
            // (s7.3: information first, the chip never disappears); only
            // the click affordance goes inert when Status is closed
            // (R1: nothing opens a panel except the Window menu), with a
            // tooltip on hover explaining why.
            ImGui::SetCursorScreenPos(digestScreenPos);
            if (ImGui::InvisibleButton("##digestclick", ImVec2(digestWidth, ImGui::GetTextLineHeight())) &&
                services.statusOpen)
                actions.showStatus = true;
            if (!services.statusOpen && ImGui::IsItemHovered())
                ImGui::SetTooltip("Asset Status is closed \xE2\x80\x94 open it from Window \xE2\x96\xB8");

            ImGui::EndChild();
        }

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
    }   // end anonymous namespace: DrawAssetPeekTooltip below is genuinely
        // cross-lens too (Browse rows/child rows, the preview pane's Derived
        // list, the Graph lens's node hover ALL use it, and as of Task 4
        // (panel-split) so does the Status lens's activity feed, now in its
        // own TU, AssetStatusPanel.cpp) -- promoted to Arcane::Editor scope +
        // declared in AssetPanelCommon.hpp, same fix as BootSceneGuid/
        // ScenesByName above. Every caller in THIS file keeps calling it
        // unqualified. Body unchanged from before the promotion; the two
        // `= false` defaults moved to the header declaration (a default may
        // be specified only once across a function's declarations), so this
        // defining declaration no longer repeats them.

    // ---- Task 10: the peek tooltip (spec s8) ---------------------------
    // Cross-lens per Task 4's promotion above: rows, child rows and (later,
    // Task 11) the preview pane's Derived list all hover the same asset.
    // Text-and-images only -- never a button (a tooltip is not
    // interactable).
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
    // `withEdgeSummary` (Plan 3 Task 4): the Graph lens's one extra line
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

    // Panel-split Task 5: promoted here alongside BootSceneGuid/ScenesByName/
    // DrawAssetPeekTooltip/PillWidth (AssetPanelCommon.hpp) -- the Graph
    // lens's node context menu is this SAME items list (its own doc comment
    // below already says so), and AssetGraphPanel.cpp (its own TU as of this
    // task) needs the SAME copy, not a second one. Declared in
    // AssetPanelCommon.hpp; body unchanged from before the promotion.
    // ---- Task 10: the unified asset context menu's ITEMS (spec s6) -----
    // The menu BODY, with no popup bracket of its own, so that every
    // representation of an asset raises the SAME menu from the SAME code:
    // a Browse row opens it through BeginPopupContextItem
    // (DrawRowContextMenu just below), a Graph node opens it through
    // ed::ShowNodeContextMenu + BeginPopup inside a CanvasPopupScope
    // (DrawGraphLens, Plan 3 Task 4). Extracted in Task 4 for exactly that
    // second caller -- spec s6 says "the unified context menu, every
    // lens", and two copies of a list is how "unified" quietly stops being
    // true.
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

    // AttachRowInteractions/DrawRowContextMenu/DrawRail/DrawGroupRow/
    // DrawNameHeaderRow/DrawAssetRow/DrawChildRow/DrawTable/DrawDerivedRow/
    // PreviewPaneSplitter/DrawPreviewPane/DrawAssetBrowserBody (renamed from
    // DrawBrowseLens) moved to AssetBrowserPanel.cpp in Task 6
    // (panel-split): the Browse lens's body, extracted as pure motion --
    // none of the twelve had a caller left in this file, so nothing here
    // needed a promotion. See AssetBrowserPanel.cpp's own header comment
    // for the full section-by-section accounting.

    // ---- Plan 2 Task 7: AssetPill's own width, WITHOUT drawing it ------
    // 12px text plus the two FramePadding.x cheeks (EditorWidgets.cpp's
    // AssetPill, verbatim). The attention card positions its trailing
    // pill by hand and has to ellipsize the NAME against whatever room is
    // left after it, so it needs the pill's width one item early.
    float PillWidth(const char* text)
    {
        ImGui::PushFont(GetEditorFonts().interRegular, 12.0f);
        const float w = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::PopFont();
        return w;
    }

    AssetPanelActions DrawAssetsPanel(AssetsPanelState& state, AssetPanelModel& model,
                                      const Arcane::Project* project, DocumentHost& docs,
                                      const AssetPanelServices& services,
                                      bool* open)
    {
        AssetPanelActions actions;
        ImGui::Begin("Assets", open);

        // Task 5 round 1 fix: the Graph lens's boot-scene graphFocus seed
        // MUST run before DrawToolbar -- its focus combo (gated on
        // lens==Graph) reads `state.graphFocus` this same frame, and so does
        // the bottom bar after the body. Seeding it any later than here (the
        // first cut put it inside DrawAssetGraphBody's own preamble, which
        // runs AFTER DrawToolbar) let the toolbar read the unseeded value on
        // the one frame a project opens with the Graph lens active while the
        // bottom bar read the seeded one -- a one-frame "focus: everything"
        // vs. "focus: <boot scene>" split between the two bands. See
        // SeedAssetGraphFocus's own comment (AssetGraphPanel.hpp) for the
        // full account.
        SeedAssetGraphFocus(state, project);

        DrawToolbar(state, model, actions);

        // 2026-09-07 fix (mock parity): DrawToolbar's own trailing widget is
        // SegmentedStrip (the lens strip), which pushes ItemSpacing to
        // (x,0) for its OWN internal buttons so they sit flush against each
        // other ("collapsed shared borders", EditorWidgets.cpp) and pops it
        // correctly before returning. But ImGui bakes each item's "next
        // line" cursor advance in AT PLACEMENT TIME using whatever
        // ItemSpacing was active THEN -- popping a style var afterward
        // restores the STYLE STRUCT, not a cursor position that already
        // advanced under the zeroed value. So the toolbar's own trailing
        // edge silently inherited that zero too, and the body below sat
        // flush against it with NO gap, live, even though nothing here ever
        // asked for that -- confirmed by an automation pixel-scan of the
        // live capture (0px) against the redline (7px, kAssetPanelToolbarBodyGapPx's
        // own comment). Fix: an EXPLICIT Dummy for the gap, itself wrapped
        // in a zeroed ItemSpacing so nothing implicit adds to either side
        // of it -- deliberately not trusting ImGui's automatic per-item
        // spacing a second time for this exact seam. Vertical-only; the
        // horizontal flush gutters DrawAssetBrowserBody's own SameLine(0,0)
        // chain established are untouched.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0.0f, kAssetPanelToolbarBodyGapPx));
        ImGui::PopStyleVar();

        if (ImGui::BeginChild("##assetsbody", ImVec2(0.0f, -kAssetPanelBottomBarHeight)))
        {
            if (!project)
                ImGui::TextDisabled("No project open (data/-next-to-exe)");
            else if (state.lens == AssetLens::Browse)
                // Panel-split Task 6: the Browse lens's body moved out to
                // its own TU (AssetBrowserPanel.cpp), same shape as Graph/
                // Status below it.
                DrawAssetBrowserBody(state, model, project, docs, services, actions);
            else if (state.lens == AssetLens::Graph)
                // Plan 3 Task 3, USER-REACHABLE as of Task 5: the toolbar's
                // Graph button is enabled (kLensEnabledMask == 0b111) and the
                // Status lens's scene cards jump straight here. The
                // device-less canvas test (AssetsGraphCanvasTest.cpp) still
                // sets `state.lens` directly, which is now one route among
                // three rather than the branch's only reachability.
                DrawAssetGraphBody(state, model, project, docs, services, actions);
            else if (state.lens == AssetLens::Status)
                // Panel-split Task 4: the Status lens's body moved out to
                // its own TU (AssetStatusPanel.cpp) -- no `state` argument
                // (spec s6: Status carries no state struct at all).
                DrawAssetStatusBody(model, project, docs, services, actions);
        }
        ImGui::EndChild();

        DrawBottomBar(state, model, services, actions);

        ImGui::End();
        return actions;
    }
}
