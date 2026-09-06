#include "Panels/AssetsPanel.hpp"

#include "Documents/DocumentHost.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/IconsLucide.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace Arcane::Editor
{
    namespace
    {
        // Toolbar / bottom bar band heights (spec s11.2's values table:
        // "toolbar wells / bottom bar | 24px / 24px"). The default ImGui
        // frame (Inter 16px body over the theme's untouched FramePadding.y=3,
        // EditorTheme.hpp's own comment) stands 22px tall; bumping
        // FramePadding.y to 4 for just the toolbar row (pushed/popped around
        // its controls) is what closes the last 2px to the pinned 24.
        constexpr float kToolbarFramePadY = 4.0f;
        constexpr float kBottomBarHeight  = 24.0f;

        // The lens strip's three labels, fixed regardless of which plan has
        // landed (spec s5: "Plan 1 ships the full three-button strip ...
        // layout pinned from day one, later plans enable, nothing shifts").
        // constexpr on a non-reference array makes every element itself
        // const, so the decayed pointer is `const char* const*` --
        // SegmentedStrip's exact parameter type, no cast needed.
        constexpr const char* kLensLabels[] = { "Browse", "Graph", "Status" };
        constexpr int kLensCount = 3;
        // Bit 0 (Browse) only -- Graph/Status stay disabled until Plan 2/3
        // land (spec s5).
        constexpr unsigned kLensEnabledMask = 0b001u;

        // The unified Create menu, per the CreateFlow mock: Material.../
        // Material Instance..., a separator, then Mesh.../Sprite.../Scene...
        // Every entry draws DISABLED -- CreateAssetKind does not exist until
        // Task 12, which both defines the enum and wires
        // AssetsPanelActions::requestCreateKind to these entries.
        void DrawCreateMenu()
        {
            if (!ImGui::BeginPopup("##createmenu"))
                return;

            ImGui::BeginDisabled();
            ImGui::MenuItem(ICON_LC_PALETTE " Material...");
            ImGui::MenuItem(ICON_LC_LAYERS  " Material Instance...");
            ImGui::Separator();
            ImGui::MenuItem(ICON_LC_BOX          " Mesh...");
            ImGui::MenuItem(ICON_LC_STICKER      " Sprite...");
            ImGui::MenuItem(ICON_LC_CLAPPERBOARD " Scene...");
            ImGui::EndDisabled();

            ImGui::EndPopup();
        }

        // Toolbar band: + Create -> search (flex) -> [per-lens slot, EMPTY in
        // Plan 1 -- only Graph's focus combo uses it, Plan 3] -> lens strip
        // anchored right-most (spec s5). Mutates `state` in place; the
        // create popup's disabled entries mean nothing populates `actions`
        // this task, but the call is wired here so Task 12 has one site to
        // extend rather than a new one.
        void DrawToolbar(AssetsPanelState& state, AssetPanelModel& model, AssetsPanelActions& /*actions*/)
        {
            ImGuiStyle& style = ImGui::GetStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(style.FramePadding.x, kToolbarFramePadY));

            if (ImGui::Button(ICON_LC_PLUS " Create " ICON_LC_CHEVRON_DOWN))
                ImGui::OpenPopup("##createmenu");
            DrawCreateMenu();

            ImGui::SameLine();

            // Search well width = remaining minus the lens strip minus the
            // per-lens slot (0 in Plan 1).
            float stripWidth = 0.0f;
            for (const char* label : kLensLabels)
                stripWidth += ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
            const float searchWidth = std::max(80.0f,
                ImGui::GetContentRegionAvail().x - stripWidth - style.ItemSpacing.x);

            ImGui::SetNextItemWidth(searchWidth);
            ImGui::InputTextWithHint("##assetssearch", ICON_LC_SEARCH " search...",
                                     state.search, sizeof(state.search));
            model.SetSearch(state.search);
            // No rail exists yet to change this (Task 10) -- state.railKind
            // stays -1 (All), so this is a no-op today, wired here so the
            // model/state pair is fed the same way SetSearch is fed.
            model.SetKindFilter(state.railKind);

            ImGui::SameLine();
            const int clickedLens = SegmentedStrip("##lens", kLensLabels, kLensCount,
                                                    static_cast<int>(state.lens), kLensEnabledMask);
            if (clickedLens >= 0)
                state.lens = static_cast<AssetLens>(clickedLens);

            ImGui::PopStyleVar();
        }

        // Bottom bar band: left = context ("N assets - S selected", becoming
        // "X of N shown" once rail or search filters) -- right = the digest
        // chip (amber refused count + dim cooking/unused). Spec s13: the
        // digest never renders an unknown as a zero -- HealthCounts has no
        // "unused" field yet (Plan 2's AssetReferenceIndex adds it), so that
        // segment is ALWAYS the literal em-dash here, never a fabricated 0.
        void DrawBottomBar(const AssetPanelModel& model)
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
            // BeginChild("##assetsbody", ImVec2(0, -kBottomBarHeight))).
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
            const float padY = std::max(0.0f, (kBottomBarHeight - ImGui::GetTextLineHeight()) * 0.5f);

            const HealthCounts health = model.Health();
            const bool filtered = model.Filtered();

            ImGui::SetCursorPosY(padY);
            char left[64];
            if (filtered)
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
                          " \xC2\xB7 %d cooking \xC2\xB7 \xE2\x80\x94 unused", health.queued);
            char digestFull[160];
            std::snprintf(digestFull, sizeof(digestFull), "%s%s", refusedPart, restPart);
            const float digestWidth = ImGui::CalcTextSize(digestFull).x;

            ImGui::SameLine();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightEdgeX - digestWidth));
            ImGui::SetCursorPosY(padY);
            ImGui::TextColored(Theme::kAmber, "%s", refusedPart);
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextDisabled("%s", restPart);

            ImGui::EndChild();
        }
    }

    AssetsPanelActions DrawAssetsPanel(AssetsPanelState& state, AssetPanelModel& model,
                                       const Arcane::Project* project, DocumentHost& /*docs*/,
                                       const AssetsPanelServices& /*services*/,
                                       bool* open)
    {
        // `docs`/`services` are unused THIS task -- Task 10's rows are the
        // first consumer (double-click routing and thumb resolution
        // respectively); kept in the signature per the brief's verbatim
        // contract rather than dropped and re-added next task.
        AssetsPanelActions actions;
        ImGui::Begin("Assets", open);

        DrawToolbar(state, model, actions);

        if (ImGui::BeginChild("##assetsbody", ImVec2(0.0f, -kBottomBarHeight)))
        {
            if (!project)
                ImGui::TextDisabled("No project open (data/-next-to-exe)");
            else
                ImGui::TextDisabled("Browse lens body -- lands in Task 10.");
        }
        ImGui::EndChild();

        DrawBottomBar(model);

        ImGui::End();
        return actions;
    }
}
