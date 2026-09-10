#include "Panels/AssetStatusPanel.hpp"

#include "Panels/AssetActivityLog.hpp"   // AssetActivityEntry/Kind -- the activity feed's rows
#include "Panels/AssetPanelModel.hpp"    // AssetPanelModel/AssetPanelEntry/HealthCounts/CookState/KindIcon/KindLabel
#include "Widgets/EditorFonts.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/IconsLucide.h"

#include <Arcane/Guid.hpp>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

// AssetStatusPanel (panel-split arc): the "Asset Status" window. Task 4 moved
// the dashboard BODY here as pure motion out of AssetsPanel.cpp's
// DrawStatusLens -- see AssetStatusPanel.hpp's own comment for why the
// exported body carries no `state` parameter -- together with that body's
// private helpers (the attention/queued cards, the Unreferenced/Scenes cards,
// the activity-feed age/title formatters) and the Status-only geometry
// constants they share, none of which any other view ever called.
//
// Task 7 added the SHELL at the bottom of this file -- DrawAssetStatusPanel,
// the window itself: its own ImGui::Begin("Asset Status"), NO toolbar (spec
// s9.1) and the bottom bar (spec s9.2) on AssetPanelCommon's shared band
// skeleton. The bar's right slot stays EMPTY until Task 8 wires spec s9.3's
// recency line.
//
// BootSceneGuid, ScenesByName, DrawAssetPeekTooltip and PillWidth are NOT
// here: all four are genuinely cross-panel (the Browser and/or Graph panels
// call them too), so their declarations live on AssetPanelCommon.hpp and --
// as of Task 7, which retired AssetsPanel.cpp where they used to sit --
// their bodies live in AssetPanelCommon.cpp. Nothing about any of the four
// has CHANGED beyond which file holds it.
namespace Arcane::Editor
{
    namespace
    {
        // Plan 2 Task 7 (spec s9.2/s11.2, redline
        // `renders/OptionE-Status-FINAL.png`) fixed geometry for the Status
        // lens.
        //
        // kStatusTileHeight is measured off the board (the tile band spans
        // y=84..148 at the render's native size) and is also exactly what the
        // tile's own content needs: StatTile's 8px pad + the 24px number's
        // line + its 2px gap + the 13px label's line + 8px pad lands just
        // inside 64. kStatusTileMinWidth is a floor for a very narrow panel,
        // so four tiles never collapse to nothing.
        //
        // kStatusSectionGap is an EXPLICIT gap, on top of ImGui's own
        // ItemSpacing.y on each side of it (4px + 6px + 4px = 14px between
        // one section's last item and the next section's label -- the board's
        // ~13px). The pill line height this file needs, to vertically centre a
        // pill it positions BY HAND rather than by SameLine, is the widget
        // layer's own exported kPillLineHeight (EditorWidgets.hpp) -- it used
        // to be restated here as a second 16px constant nothing kept in step.
        constexpr float kStatusTileHeight      = 64.0f;
        constexpr float kStatusTileMinWidth    = 72.0f;
        constexpr float kStatusSectionGap      = 6.0f;
        constexpr float kStatusProgressHeight  = 4.0f;   // queued card's strip
        constexpr float kStatusSelectionBorder = 2.0f;   // spec s10's node rule, applied to cards

        // Plan 2 Task 8 additions to the same fixed-geometry block above.
        //
        // kStatusRightColumnWidth: the board's two-column split below the
        // meter -- "Needs attention"/"Unreferenced" left, "Activity"/
        // "Scenes" right, side by side at the SAME starting Y (the render's
        // own layout, not this plan's invention). The spec does not pin an
        // exact split -- an implementer tuning value, not a §11.2 figure,
        // same footing as kPreviewCompactHeaderMinWidth's own precedent
        // comment above -- chosen wide enough for a feed row's longest
        // realistic line ("crate_albedo.png source changed -> queued")
        // without crowding the left column on a normal panel width.
        // kStatusProgressCaptionGap is the small vertical gap between the
        // queued card's progress strip and its new "N of M cooked" caption
        // (Task 7 review ruling B) -- the same 2px register as
        // TimelineFeed's own kLineGap and MeterBar's own kSegmentGap.
        constexpr float kStatusRightColumnWidth    = 300.0f;
        constexpr float kStatusProgressCaptionGap  = 2.0f;

        // ---- Plan 2 Task 7: one needs-attention card (spec s9.2) -----------
        // Two shapes, ONE function, because everything except the trailing
        // content, the frame variant and the second line is identical:
        //
        //   * REFUSED wears the muted-amber acting-on frame (BeginCardFrame
        //     variant 1 -- the same `#7a5a20` spec s11.2 pins for the amber
        //     pill), the amber warning glyph, its refusal detail line, and
        //     the Recook/Problems pair.
        //   * QUEUED is a neutral frame with a dim clock, the dim "arccook
        //     running..." note the board puts on the NAME ROW beside the
        //     name (spec s11: "the mocks are the redline"), and the derived
        //     progress strip beneath.
        //
        // `queuedProgress` is Ruling 12's DERIVED fraction --
        // cooked / (cooked + queued) from HealthCounts, the very numbers the
        // meter above already shows. There is no per-asset cook progress to
        // read anywhere in the engine, and this card refuses to fabricate one.
        //
        // `queuedCooked`/`queuedCookedAndQueued` (Task 7 review ruling B):
        // the SAME two HealthCounts numbers `queuedProgress` was already
        // derived from, passed through a second time so the queued card can
        // spell the fraction out in words ("N of M cooked") instead of
        // leaving the bare strip to speak for itself. Meaningless when
        // `refused` -- callers pass 0/0 for the refused card.
        void DrawAttentionCard(AssetPanelModel& model, const AssetPanelServices& services,
                               AssetPanelActions& actions, const AssetPanelEntry& e,
                               bool refused, float queuedProgress,
                               int queuedCooked, int queuedCookedAndQueued)
        {
            const ImVec2 cardMin   = ImGui::GetCursorScreenPos();
            const float  cardWidth = ImGui::GetContentRegionAvail().x;
            // The guid string is the card's id scope, same convention every
            // row in this file uses (DrawAssetRow/DrawChildRow's PushID).
            const std::string cardId = e.guid.ToString();
            if (!BeginCardFrame(cardId.c_str(), refused ? 1 : 0, cardWidth))
                return;   // SkipItems: nothing was pushed, so nothing to End

            const ImGuiStyle& style = ImGui::GetStyle();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // The card's inner padding, DERIVED rather than duplicated:
            // BeginCardFrame seats the cursor exactly one padding in from the
            // frame's top-left corner, so this difference IS
            // EditorWidgets.cpp's kCardFramePadding without a second copy of
            // that constant living here to drift from it.
            const ImVec2 innerMin = ImGui::GetCursorScreenPos();
            const float  pad      = innerMin.x - cardMin.x;
            const float  innerW   = std::max(1.0f, cardWidth - pad * 2.0f);

            // Line 1 stands as tall as the buttons it hosts (the refused
            // card); the queued card keeps the same pitch so the two card
            // shapes line up in a mixed list.
            const float rowH  = ImGui::GetFrameHeight();
            const float line2 = refused ? ImGui::GetTextLineHeight() : kStatusProgressHeight;
            // Task 7 review ruling B: the queued card grows a THIRD line --
            // the "N of M cooked" caption beneath the progress strip --
            // measured at StatTile's own 13px label size. A brief
            // PushFont/PopFont pair purely to read GetTextLineHeight(); the
            // refused card never carries this line, so it costs it nothing.
            float line3 = 0.0f;
            if (!refused)
            {
                ImGui::PushFont(GetEditorFonts().interRegular, 13.0f);
                line3 = ImGui::GetTextLineHeight();
                ImGui::PopFont();
            }
            const float bodyH = rowH + style.ItemSpacing.y + line2
                              + (refused ? 0.0f : (kStatusProgressCaptionGap + line3));

            // ONE body hit target, submitted FIRST and covering the whole card
            // body, with SetNextItemAllowOverlap so the two buttons submitted
            // AFTER it still take the hover and the click where they overlap
            // -- imgui.h's own documented use of that flag ("covering an area
            // where subsequent items may need to be added"), and the shape
            // RowWithThumb + the rail's hover "+" already run on. Everything
            // else this function draws is either pure drawlist paint or a
            // non-interactive Dummy (AssetPill), so the rest of the card body
            // stays clickable.
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::InvisibleButton("##cardbody", ImVec2(innerW, bodyH)))
                model.Select(e.guid);
            // Immediately after the hit item, exactly like AttachRowInteractions
            // does for a table row -- the peek tooltip keys off the LAST
            // submitted item.
            DrawAssetPeekTooltip(model, services, e.guid);

            // ---- buttons: right-aligned on line 1, drawn BEFORE the name so
            // the name's ellipsis budget can be measured against where they
            // actually start.
            float buttonsLeft = innerMin.x + innerW;
            if (refused)
            {
                const float recookW   = ImGui::CalcTextSize("Recook").x + style.FramePadding.x * 2.0f;
                const float problemsW = ImGui::CalcTextSize("Problems").x + style.FramePadding.x * 2.0f;
                buttonsLeft = innerMin.x + innerW - recookW - problemsW - style.ItemSpacing.x;
                ImGui::SetCursorScreenPos(ImVec2(buttonsLeft, innerMin.y));
                // "Panel reports, app performs": neither button does any work
                // here -- EditorApp::ConsumeAssetPanelActions owns both effects.
                if (ImGui::Button("Recook"))
                    actions.recook = e.guid;
                ImGui::SameLine();
                // Panel-split spec s7.3/R1 (Task 3): Problems comes under
                // the same focus-if-open rule as the other three deep
                // links -- greyed + tooltipped when Problems is closed,
                // never un-hiding it (see ConsumeAssetPanelActions's own
                // updated comment for the host half of this change).
                ImGui::BeginDisabled(!services.problemsOpen);
                if (ImGui::Button("Problems"))
                    actions.showProblems = true;
                ImGui::EndDisabled();
                if (!services.problemsOpen &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Problems is closed \xE2\x80\x94 open it from Window \xE2\x96\xB8");
            }

            // ---- line 1: 18px thumb, state glyph, name, trailing content.
            // Drawlist paint (plus one positioned AssetPill), the same
            // technique RowWithThumb uses, so none of it competes with the hit
            // target above for ImGui's "last item".
            float x = innerMin.x;
            const float thumbY = innerMin.y + (rowH - kAssetRowThumbSize) * 0.5f;
            const std::uint64_t thumb = services.resolveAssetThumb ? services.resolveAssetThumb(e.guid) : 0;
            if (thumb != 0)
            {
                dl->AddImage(static_cast<ImTextureID>(thumb), ImVec2(x, thumbY),
                            ImVec2(x + kAssetRowThumbSize, thumbY + kAssetRowThumbSize));
            }
            else
            {
                // The same well-plus-centred-kind-icon fallback the preview
                // pane's own thumb uses, at the row's 18px size.
                dl->AddRectFilled(ImVec2(x, thumbY),
                                  ImVec2(x + kAssetRowThumbSize, thumbY + kAssetRowThumbSize),
                                  ImGui::GetColorU32(Theme::kWell));
                const char* kindIcon = KindIcon(e.kind);
                const ImVec2 ks = ImGui::CalcTextSize(kindIcon);
                dl->AddText(ImVec2(x + (kAssetRowThumbSize - ks.x) * 0.5f,
                                   thumbY + (kAssetRowThumbSize - ks.y) * 0.5f),
                           ImGui::GetColorU32(ImGuiCol_Text), kindIcon);
            }
            x += kAssetRowThumbSize + style.ItemInnerSpacing.x;

            // State glyph: amber triangle for refused, dim clock for queued.
            // Amber never carries the meaning ALONE -- the glyph shape, the
            // detail line and the card's own frame all say the same thing
            // (spec s11.2's amber rule).
            const char* stateIcon = refused ? ICON_LC_TRIANGLE_ALERT : ICON_LC_CLOCK;
            const ImVec2 stateSize = ImGui::CalcTextSize(stateIcon);
            dl->AddText(ImVec2(x, innerMin.y + (rowH - stateSize.y) * 0.5f),
                       ImGui::GetColorU32(refused ? Theme::kAmber : Theme::kTextDim), stateIcon);
            x += stateSize.x + style.ItemInnerSpacing.x;

            // What follows the name on this line, measured BEFORE it so the
            // name can be ellipsized against what is genuinely left.
            constexpr const char* kRunningText = "arccook running...";
            const char* kindText   = KindLabel(e.kind);
            const float trailingW  = refused ? PillWidth(kindText)
                                             : ImGui::CalcTextSize(kRunningText).x;
            // buttonsLeft is only the LEFT EDGE OF THE BUTTONS on the refused
            // shape (Recook/Problems); the queued shape has no buttons, so
            // buttonsLeft there is already the card's plain right edge and
            // needs no extra gap subtracted before it. Applying the
            // button-row's ItemSpacing unconditionally would over-ellipsize
            // the queued name by that many px for a gap that doesn't exist.
            const float nameBudget = std::max(0.0f, buttonsLeft - (refused ? style.ItemSpacing.x : 0.0f)
                                                     - trailingW - style.ItemInnerSpacing.x - x);

            const std::string name = EllipsisToWidth(e.fileName, nameBudget);
            const ImVec2 nameSize  = ImGui::CalcTextSize(name.c_str());
            dl->AddText(ImVec2(x, innerMin.y + (rowH - nameSize.y) * 0.5f),
                       ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
            x += nameSize.x + style.ItemInnerSpacing.x;

            if (refused)
            {
                ImGui::SetCursorScreenPos(ImVec2(x, innerMin.y + (rowH - kPillLineHeight) * 0.5f));
                AssetPill(kindText);
            }
            else
            {
                dl->AddText(ImVec2(x, innerMin.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f),
                           ImGui::GetColorU32(Theme::kTextDim), kRunningText);
            }

            // ---- line 2
            const float line2Y = innerMin.y + rowH + style.ItemSpacing.y;
            if (refused)
            {
                // The refusal reason, from the HOST's own cook-diagnostic row
                // (services.cookDetailFor) -- this panel never reads
                // diagnostics itself. Composed as the board writes it,
                // "cook refused - <detail>"; the bare "cook refused" is the
                // fallback when the host has no permanent row for this guid
                // (possible: CookStateOf can also reach Refused through a
                // provider answer this session's map no longer backs).
                std::string line = "cook refused";
                if (services.cookDetailFor)
                {
                    if (const std::optional<std::string> detail = services.cookDetailFor(e.guid);
                        detail && !detail->empty())
                    {
                        line += " \xE2\x80\x94 ";   // em dash, the board's own separator
                        line += *detail;
                    }
                }
                // Task 7 review ruling C: the board's own tail, pointing at
                // the Problems pane for the full diagnostic -- appended
                // UNCONDITIONALLY, whether or not a per-guid detail resolved
                // above. Already in TextDisabled tone: the whole line below
                // draws in Theme::kTextDim, tail included, so no separate
                // color segment is needed for "in TextDisabled tone".
                //
                // Review fix (Important 2): the tail must be MEASURED
                // before the clamp, not appended before it -- a long
                // refusal detail is exactly the case EllipsisToWidth's own
                // "..." would otherwise cut the tail from first (it sits at
                // the string's end), silently dropping the ONE thing this
                // line exists to point the user at. Same "measure trailing
                // content, then budget the rest" order every other
                // PillWidth caller uses (PillWidth itself lives in
                // AssetPanelCommon.cpp as of Task 7 -- it is cross-panel,
                // and this card is no longer one of two callers in one file).
                constexpr const char* kProblemsTail = " \xC2\xB7 details in Problems";
                const float tailWidth = ImGui::CalcTextSize(kProblemsTail).x;
                const std::string shown = EllipsisToWidth(line, std::max(0.0f, innerW - tailWidth))
                                        + kProblemsTail;
                dl->AddText(ImVec2(innerMin.x, line2Y), ImGui::GetColorU32(Theme::kTextDim),
                           shown.c_str());
            }
            else
            {
                // Ruling 12's derived strip: kGrab fill over a kWell track.
                const float frac = std::clamp(queuedProgress, 0.0f, 1.0f);
                dl->AddRectFilled(ImVec2(innerMin.x, line2Y),
                                  ImVec2(innerMin.x + innerW, line2Y + kStatusProgressHeight),
                                  ImGui::GetColorU32(Theme::kWell));
                if (frac > 0.0f)
                    dl->AddRectFilled(ImVec2(innerMin.x, line2Y),
                                      ImVec2(innerMin.x + innerW * frac, line2Y + kStatusProgressHeight),
                                      ImGui::GetColorU32(Theme::kGrab));

                // Task 7 review ruling B: the strip alone never said WHAT
                // fraction it was a fraction OF -- this caption makes the
                // pipeline semantics explicit.
                char caption[32];
                std::snprintf(caption, sizeof(caption), "%d of %d cooked",
                             queuedCooked, queuedCookedAndQueued);
                ImGui::PushFont(GetEditorFonts().interRegular, 13.0f);
                dl->AddText(ImVec2(innerMin.x, line2Y + kStatusProgressHeight + kStatusProgressCaptionGap),
                           ImGui::GetColorU32(Theme::kTextDim), caption);
                ImGui::PopFont();
            }

            EndCardFrame();

            // Selection: a 2px kSelection border over the frame EndCardFrame
            // just painted (spec s8's interaction contract -- the Status lens
            // highlights its CARDS, the way the table highlights its rows).
            // Drawn after the fact against the full-card rect EndCardFrame
            // reserves as its closing item, so it needs no separate measure.
            if (model.selected == e.guid)
            {
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                    ImGui::GetColorU32(Theme::kSelection),
                                                    0.0f, 0, kStatusSelectionBorder);
            }
        }

        // ---- Plan 2 Task 8: activity-feed age/title formatting --------------
        // File-local, TimelineFeed's sole caller: the widget itself stays
        // model-free (EditorWidgets.hpp's own doc comment), so every bit of
        // "what does this entry MEAN" policy lives here instead.
        std::string FormatActivityAge(std::chrono::steady_clock::time_point when)
        {
            const auto elapsed = std::chrono::steady_clock::now() - when;
            const long long secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            if (secs < 60)
                return "just now";
            char buf[32];
            const long long mins = secs / 60;
            if (mins < 60)
            {
                std::snprintf(buf, sizeof(buf), "%lld min ago", mins);
                return buf;
            }
            std::snprintf(buf, sizeof(buf), "%lld h ago", mins / 60);
            return buf;
        }

        // Title per AssetActivityKind (plan doc Step 2). SourceChanged is the
        // one kind whose title depends on live model state rather than the
        // entry alone: "-> queued" only when the guid STILL resolves AND its
        // kind still cooks (Texture/Sprite, the exact CookStateOf rule) --
        // an entry whose asset has since vanished, or that never had a real
        // cook pipeline, reads as the plain form.
        std::string ActivityTitle(const AssetPanelModel& model, const AssetActivityEntry& entry)
        {
            switch (entry.kind)
            {
                case AssetActivityKind::Cooked:      return "cooked";
                case AssetActivityKind::CookRefused: return "cook refused";
                case AssetActivityKind::Created:     return "created";
                case AssetActivityKind::Deleted:     return "deleted";
                case AssetActivityKind::SourceChanged:
                {
                    const AssetPanelEntry* e = model.Find(entry.guid);
                    const bool cooks = e && (e->kind == AssetKind::Texture || e->kind == AssetKind::Sprite);
                    return cooks ? "source changed \xE2\x86\x92 queued" : "source changed";
                }
            }
            return "";
        }

        // ---- Plan 2 Task 8: the Unreferenced card (spec s9.2, step 1) ------
        // One CardFrame holding an inset Theme::kWell well of rows -- 18px
        // thumb + a chip-style fileName (AssetPill, the same chip idiom every
        // other row in this file uses for a name label) + a small Reveal
        // button -- one row per model.UnusedGuids(), that ordering already
        // pinned (Task 4) so this card never needs its own sort. ItemSpacing.y
        // is zeroed for the row loop so the drawn well height (rowH * count,
        // computed up front so the fill can be painted BEHIND the rows)
        // matches the rows' own actual pitch exactly -- the same "vertical-
        // only, don't trust automatic per-item spacing" fix
        // DrawAssetBrowserPanel's own toolbar-gap comment states
        // (AssetBrowserPanel.cpp).
        void DrawUnreferencedCard(AssetPanelModel& model, const AssetPanelServices& services,
                                  AssetPanelActions& actions)
        {
            const ImVec2 cardMin   = ImGui::GetCursorScreenPos();
            const float  cardWidth = ImGui::GetContentRegionAvail().x;
            if (!BeginCardFrame("##unreferenced", 0, cardWidth))
                return;

            const std::vector<Arcane::Guid> unused = model.UnusedGuids();
            if (unused.empty())
            {
                ImGui::TextDisabled("everything is referenced");
            }
            else
            {
                const ImGuiStyle& style = ImGui::GetStyle();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 wellMin = ImGui::GetCursorScreenPos();
                // Review fix (Important 1): GetContentRegionAvail() at the
                // INNER cursor measures to the ambient window's right edge,
                // not the card's own right border -- BeginCardFrame doesn't
                // constrain caller content width (its own doc comment), so
                // that avail is `cardWidth - pad`, one pad short of what a
                // caller actually wants. `pad`, DERIVED the same way
                // DrawAttentionCard's own `innerW` is (cardMin vs the seated
                // cursor), then subtracted TWICE -- once for each side --
                // is what actually stops the well/Reveal button at the
                // card's inner content edge instead of its outer border.
                const float  pad       = wellMin.x - cardMin.x;
                const float  wellWidth = std::max(1.0f, cardWidth - pad * 2.0f);
                const float  rowH      = kTableRowHeight;
                dl->AddRectFilled(wellMin,
                                  ImVec2(wellMin.x + wellWidth, wellMin.y + rowH * static_cast<float>(unused.size())),
                                  ImGui::GetColorU32(Theme::kWell));

                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));
                for (const Arcane::Guid& guid : unused)
                {
                    const AssetPanelEntry* e = model.Find(guid);
                    if (!e)
                        continue;   // pruned between UnusedGuids() and now -- skip, don't fabricate a row

                    ImGui::PushID(guid.ToString().c_str());
                    const ImVec2 rowMin = ImGui::GetCursorScreenPos();

                    // Thumb: same 18px well-plus-kind-icon fallback
                    // DrawAttentionCard's own line 1 uses.
                    const float thumbY = rowMin.y + (rowH - kAssetRowThumbSize) * 0.5f;
                    const std::uint64_t thumb = services.resolveAssetThumb ? services.resolveAssetThumb(guid) : 0;
                    if (thumb != 0)
                    {
                        dl->AddImage(static_cast<ImTextureID>(thumb), ImVec2(rowMin.x, thumbY),
                                    ImVec2(rowMin.x + kAssetRowThumbSize, thumbY + kAssetRowThumbSize));
                    }
                    else
                    {
                        dl->AddRectFilled(ImVec2(rowMin.x, thumbY),
                                          ImVec2(rowMin.x + kAssetRowThumbSize, thumbY + kAssetRowThumbSize),
                                          ImGui::GetColorU32(Theme::kWell));
                        const char* kindIcon = KindIcon(e->kind);
                        const ImVec2 ks = ImGui::CalcTextSize(kindIcon);
                        dl->AddText(ImVec2(rowMin.x + (kAssetRowThumbSize - ks.x) * 0.5f,
                                           thumbY + (kAssetRowThumbSize - ks.y) * 0.5f),
                                   ImGui::GetColorU32(ImGuiCol_Text), kindIcon);
                    }

                    // Reveal, right-aligned within the well.
                    const float revealW = ImGui::CalcTextSize("Reveal").x + style.FramePadding.x * 2.0f;
                    const float revealX = wellMin.x + wellWidth - revealW;

                    // Name, chip-style -- AssetPill, vertically centered the
                    // same way DrawAttentionCard positions its own trailing
                    // pill (rowH - kPillLineHeight, halved).
                    ImGui::SetCursorScreenPos(ImVec2(rowMin.x + kAssetRowThumbSize + style.ItemInnerSpacing.x,
                                                     rowMin.y + (rowH - kPillLineHeight) * 0.5f));
                    AssetPill(e->fileName.c_str());

                    ImGui::SetCursorScreenPos(ImVec2(revealX, rowMin.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
                    // Panel-split spec s7.1/s7.2 (Task 3): this card no
                    // longer performs the reveal itself -- it raises
                    // actions.revealInBrowse and the HOST calls
                    // RevealAssetInBrowser (AssetPanelCommon.*), which
                    // carries the exact Ruling-10 sequence (clear filters,
                    // walk ancestry, force the derived fold, select) this
                    // button used to run in place, plus the `state.lens =
                    // Browse` write this button no longer needs to make.
                    // R1/s7.3: greyed + tooltipped when Browse is closed --
                    // nothing opens a panel except the Window menu.
                    ImGui::BeginDisabled(!services.browserOpen);
                    if (ImGui::Button("Reveal"))
                        actions.revealInBrowse = guid;
                    ImGui::EndDisabled();
                    if (!services.browserOpen &&
                        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Asset Browser is closed \xE2\x80\x94 open it from Window \xE2\x96\xB8");

                    // Reserve the FULL row as one item -- EndCardFrame's own
                    // EndGroup measures the union of real items, so every row
                    // needs at least one spanning the whole (wellWidth, rowH)
                    // rect, not just the Reveal button's own small one.
                    ImGui::SetCursorScreenPos(rowMin);
                    ImGui::Dummy(ImVec2(wellWidth, rowH));

                    ImGui::PopID();
                }
                ImGui::PopStyleVar();

                ImGui::TextDisabled("nothing points at these");
            }

            EndCardFrame();
        }

        // ---- Plan 2 Task 8: one Scenes-rollup card (spec s9.2, step 3) -----
        // name (+ boot pill, the SAME source DrawAssetRow's own pill uses) ·
        // a count line derived from the reference index · a "Focus in Graph"
        // button, LIVE as of Plan 3 Task 5 (it was a BeginDisabled
        // placeholder for exactly as long as the Graph lens itself was
        // unreachable -- never a stub that pretended to do something).
        //
        // Panel-split spec s7.1 (Task 3): the button used to write
        // state.graphFocus/state.lens and call model.Select DIRECTLY -- the
        // precedent named above (the digest chip's own click-through in
        // the shared bottom bar), from back when switching lenses inside one
        // panel was not a host effect. The split invalidated that rationale:
        // Graph is a SEPARATE WINDOW as of Task 7, so this card raises
        // actions.focusInGraph instead and takes no `state` parameter at
        // all -- the host (EditorApp::ConsumeAssetPanelActions) sets the
        // focus, selects, and brings the Asset Graph tab forward, after this
        // frame's draw. R1/s7.3: greyed + tooltipped when Graph is closed.
        void DrawSceneCard(AssetPanelModel& model, const AssetPanelServices& services,
                           AssetPanelActions& actions, const AssetPanelEntry& e,
                           const Arcane::Guid& bootGuid)
        {
            if (!BeginCardFrame(e.guid.ToString().c_str(), 0, ImGui::GetContentRegionAvail().x))
                return;

            ImGui::Text("%s %s", KindIcon(e.kind), e.fileName.c_str());
            if (bootGuid.IsValid() && e.guid == bootGuid)
            {
                ImGui::SameLine();
                AssetPill("boot", 1);
            }

            // n = distinct outbound targets (spec s9.1's dual-edge
            // accounting -- References AND DerivesFrom both count towards a
            // scene's own dependency count). The index's manifest already
            // dedups; this counts distinct DEFENSIVELY rather than trust
            // that invariant a second time from a display-only consumer.
            std::vector<Arcane::Guid> targets;
            if (const AssetReferenceIndex::Node* node = model.RefIndex().Find(e.guid))
            {
                targets.reserve(node->outbound.size());
                for (const Arcane::AssetRef& ref : node->outbound)
                    targets.push_back(ref.target);
                std::sort(targets.begin(), targets.end());
                targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
            }

            int needsAttention = 0;
            for (const Arcane::Guid& target : targets)
                if (const AssetPanelEntry* t = model.Find(target);
                    t && (t->cook == CookState::Refused || t->cook == CookState::Queued))
                    ++needsAttention;

            char line[64];
            if (needsAttention > 0)
                std::snprintf(line, sizeof(line), "%d assets \xC2\xB7 %d need attention",
                             static_cast<int>(targets.size()), needsAttention);
            else
                std::snprintf(line, sizeof(line), "%d assets \xC2\xB7 all cooked",
                             static_cast<int>(targets.size()));
            ImGui::TextDisabled("%s", line);

            // Panel-split spec s7.1 (Task 3): the focus-before-lens ordering
            // this button used to encode locally (comment retired along
            // with the writes it explained) now lives at the host's
            // consumer -- EditorApp::ConsumeAssetPanelActions writes
            // graphFocus and Select before bringing the Asset Graph tab
            // forward. Spec s7.1 (Task 7): the ordering DANCE itself has
            // dissolved -- the host writes both before any next-frame draw,
            // and the Graph panel's own rebuild trigger (graphBuiltStamp/
            // graphBuiltFocus vs the model) does the rest. Select still
            // matters: it is what makes the graph CENTER on this scene
            // rather than merely contain it.
            ImGui::BeginDisabled(!services.graphOpen);
            if (ImGui::Button("Focus in Graph"))
                actions.focusInGraph = e.guid;
            ImGui::EndDisabled();
            if (!services.graphOpen && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Asset Graph is closed \xE2\x80\x94 open it from Window \xE2\x96\xB8");

            EndCardFrame();
        }
    }

    // ---- Plan 2 Task 7/8: the Status lens body (spec s9.2) -------------
    // Tiles row, cook-pipeline meter, then a two-column split matching
    // the board's own layout (`renders/OptionE-Status-FINAL.png`):
    // "Needs attention"/"Unreferenced" left, "Activity"/"Scenes" right,
    // both starting at the same Y. A table rather than hand-rolled
    // column math -- ImGui's own per-cell auto-height handles two
    // UNEQUAL-height columns without this file inventing a second
    // version of that logic; NoSavedSettings for the same reason every
    // other table in this file carries it (session-only layout, nothing
    // to persist to imgui.ini).
    // Panel-split spec s7.1 (Task 3): `state`'s only uses inside this
    // body were the two deep-link writes (the Unreferenced card's
    // Reveal, the Scenes card's Focus in Graph) -- both now raise
    // through `actions` instead, so `state` goes unread here, same as
    // `docs` beside it. Left in the signature rather than dropped: this
    // is still the shared body shape DrawAssetBrowserBody/
    // DrawAssetGraphBody carry too, minus the `state` those two need and
    // this one does not (spec s6: Status has no state struct).
    void DrawAssetStatusBody(AssetPanelModel& model, const Arcane::Project* project,
                             DocumentHost& /*docs*/, const AssetPanelServices& services,
                             AssetPanelActions& actions)
    {
        // AlwaysUseWindowPadding: a bordered-less child gets NO padding by
        // default, and the dashboard -- unlike the Browse lens's flush
        // rail/table/pane chain -- is a padded page (the board insets its
        // whole content from the panel edge).
        if (!ImGui::BeginChild("##statusbody", ImVec2(0.0f, 0.0f),
                               ImGuiChildFlags_AlwaysUseWindowPadding))
        {
            ImGui::EndChild();
            return;
        }

        const HealthCounts health = model.Health();
        const ImGuiStyle& style = ImGui::GetStyle();

        // ---- tiles row: four equal-width tiles carved out of the content
        // region (spec s9.2's "assets / cook refused / awaiting cook /
        // unreferenced"). Only the refused tile is amber, and only its
        // ICON is -- StatTile's variant 1 keeps the number in text tokens
        // (spec s11.2).
        {
            const float tileW = std::max(kStatusTileMinWidth,
                (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * 3.0f) * 0.25f);
            const ImVec2 tileSize(tileW, kStatusTileHeight);
            char num[16];
            // ImDrawList::AddText rasterizes at the call, so one scratch
            // buffer serves all four tiles.
            const auto tile = [&](const char* id, int value, const char* label,
                                  const char* icon, int variant)
            {
                std::snprintf(num, sizeof(num), "%d", value);
                StatTile(id, num, label, icon, variant, tileSize);
            };
            tile("##tileassets",  health.total,   "assets",        nullptr,                 0);
            ImGui::SameLine();
            tile("##tilerefused", health.refused, "cook refused",  ICON_LC_TRIANGLE_ALERT,  1);
            ImGui::SameLine();
            tile("##tilequeued",  health.queued,  "awaiting cook", ICON_LC_CLOCK,           0);
            ImGui::SameLine();
            tile("##tileunused",  health.unused,  "unreferenced",  ICON_LC_CIRCLE_SLASH,    0);
        }

        // ---- cook pipeline meter. Grays plus amber, and the icon/label
        // pair carries the meaning in every case -- colour alone never
        // does (MeterBar draws a swatch AND the label AND the count).
        ImGui::Dummy(ImVec2(0.0f, kStatusSectionGap));
        ImGui::TextDisabled("Cook pipeline");
        const MeterSegment segments[] = {
            { "cooked",  health.cooked,  ImGui::GetColorU32(Theme::kGrab)    },
            { "queued",  health.queued,  ImGui::GetColorU32(Theme::kTextDim) },
            { "refused", health.refused, ImGui::GetColorU32(Theme::kAmber)   },
        };
        MeterBar("##cookmeter", segments, static_cast<int>(std::size(segments)),
                 ImGui::GetContentRegionAvail().x);

        // ---- two-column body: LEFT (Needs attention -> Unreferenced),
        // RIGHT (Activity -> Scenes) -- the board's own side-by-side
        // placement (OptionE-Status-FINAL.png: "Needs attention" and
        // "Activity" sit at the same Y).
        ImGui::Dummy(ImVec2(0.0f, kStatusSectionGap));
        // Review fix (Important 3): kStatusRightColumnWidth is an
        // implementer tuning value, not a floor -- unclamped, a narrow
        // dock could let the fixed column crush (or exceed) the whole
        // available width, starving the stretch column and leaving
        // TimelineFeed's per-row hit target with a zero/negative avail
        // (ImGui::InvisibleButton asserts on exactly zero). Same
        // "sane-range clamp" discipline ClampPreviewForLayout already
        // uses for the preview pane -- capped to a fraction of what is
        // actually available THIS frame, floored so it is never <= 0.
        const float rightColumnWidth = std::max(1.0f,
            std::min(kStatusRightColumnWidth, ImGui::GetContentRegionAvail().x * 0.45f));
        if (ImGui::BeginTable("##statuscolumns", 2, ImGuiTableFlags_NoSavedSettings))
        {
            ImGui::TableSetupColumn("##left",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##right", ImGuiTableColumnFlags_WidthFixed, rightColumnWidth);
            ImGui::TableNextRow();

            // ---- LEFT: needs attention, then Unreferenced.
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Needs attention");

            // model.Entries() is an unordered_map -- its own doc comment
            // requires a displaying consumer to sort. Name, then mount
            // path as the tie-break: the exact ordering
            // AssetPanelModel::UnusedGuids already pins for the
            // Unreferenced card, so the lists cannot read as sorted by
            // different rules.
            std::vector<const AssetPanelEntry*> refused, queued;
            for (const auto& [guid, entry] : model.Entries())
            {
                if (entry.cook == CookState::Refused)     refused.push_back(&entry);
                else if (entry.cook == CookState::Queued) queued.push_back(&entry);
            }
            const auto byName = [](const AssetPanelEntry* a, const AssetPanelEntry* b)
            { return a->name != b->name ? a->name < b->name : a->mountPath < b->mountPath; };
            std::sort(refused.begin(), refused.end(), byName);
            std::sort(queued.begin(), queued.end(), byName);

            if (refused.empty() && queued.empty())
            {
                // Empty state (a desk item -- the board only draws the
                // populated form, so this one line is the whole design).
                ImGui::TextDisabled("nothing needs attention");
            }
            else
            {
                // Ruling 12: ONE fraction for every queued card, derived
                // from the same HealthCounts the meter shows. Zero-safe
                // -- a non-empty `queued` list implies health.queued > 0,
                // but the guard costs nothing and does not depend on
                // that reasoning.
                const int cookedAndQueued = health.cooked + health.queued;
                const float queuedProgress = cookedAndQueued > 0
                    ? static_cast<float>(health.cooked) / static_cast<float>(cookedAndQueued)
                    : 0.0f;

                for (const AssetPanelEntry* e : refused)
                    DrawAttentionCard(model, services, actions, *e, /*refused=*/true, 0.0f, 0, 0);
                for (const AssetPanelEntry* e : queued)
                    DrawAttentionCard(model, services, actions, *e, /*refused=*/false, queuedProgress,
                                      health.cooked, cookedAndQueued);
            }

            ImGui::Dummy(ImVec2(0.0f, kStatusSectionGap));
            ImGui::TextDisabled("Unreferenced");
            DrawUnreferencedCard(model, services, actions);

            // ---- RIGHT: Activity, then Scenes.
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("Activity");
            if (!services.activity || services.activity->Size() == 0)
            {
                ImGui::TextDisabled("no activity yet");
            }
            else
            {
                // Frame-lifetime string storage (plan doc Step 2): build
                // every row's std::strings into a vector reserved to the
                // EXACT final count first (Size(), never re-grown after),
                // so the vector never reallocates once we start taking
                // .c_str() pointer views into it below -- the SSO trap
                // this file's brief calls out by name.
                struct ActivityRow { std::string age, title, detail; Arcane::Guid guid; };
                std::vector<ActivityRow> rows;
                rows.reserve(services.activity->Size());
                services.activity->ForEachNewestFirst([&](const AssetActivityEntry& entry)
                {
                    ActivityRow row;
                    row.age   = FormatActivityAge(entry.when);
                    row.title = ActivityTitle(model, entry);
                    const std::string base = entry.name.empty() ? entry.guid.ToString() : entry.name;
                    row.detail = entry.detail.empty() ? base : (base + " \xE2\x80\x94 " + entry.detail);
                    row.guid  = entry.guid;
                    rows.push_back(std::move(row));
                });

                std::vector<TimelineEntry> feedEntries;
                feedEntries.reserve(rows.size());
                for (const ActivityRow& row : rows)
                    feedEntries.push_back(TimelineEntry{ row.age.c_str(), row.title.c_str(), row.detail.c_str() });

                const TimelineFeedResult feedResult = TimelineFeed("##activityfeed", feedEntries.data(),
                                                                   static_cast<int>(feedEntries.size()));
                if (feedResult.hoveredIndex >= 0)
                    DrawAssetPeekTooltip(model, services,
                                        rows[static_cast<std::size_t>(feedResult.hoveredIndex)].guid,
                                        /*forceShow=*/true);
                if (feedResult.clickedIndex >= 0)
                    model.Select(rows[static_cast<std::size_t>(feedResult.clickedIndex)].guid);
            }

            ImGui::Dummy(ImVec2(0.0f, kStatusSectionGap));
            ImGui::TextDisabled("Scenes");

            // bootGuid: the SAME helper DrawAssetBrowserBody reads for
            // DrawAssetRow's "boot" pill, never a second parse.
            const Arcane::Guid bootGuid = BootSceneGuid(project);

            // The SAME list, in the same order, the Graph lens's focus
            // combo offers (Task 5 hoisted it out of here).
            const std::vector<const AssetPanelEntry*> scenes = ScenesByName(model);

            if (scenes.empty())
                ImGui::TextDisabled("no scenes");
            else
                for (const AssetPanelEntry* e : scenes)
                    DrawSceneCard(model, services, actions, *e, bootGuid);

            ImGui::EndTable();
        }

        ImGui::EndChild();
    }

    // ---- Panel-split Task 7: the window (spec s5/s9) -------------------
    AssetPanelActions DrawAssetStatusPanel(AssetPanelModel& model, const Arcane::Project* project,
                                           DocumentHost& docs, const AssetPanelServices& services,
                                           bool* open)
    {
        AssetPanelActions actions;
        if (!ImGui::Begin("Asset Status", open))
        {
            // Collapsed, or a docked tab that is not the selected one:
            // ImGui has skipped this window's contents entirely. End is
            // still owed (Begin/End pair unconditionally).
            ImGui::End();
            return actions;
        }

        // NO TOOLBAR (spec s9.1): the stat tiles start at the top of the
        // body, and there is deliberately no kAssetPanelToolbarBodyGapPx
        // Dummy either -- that gap exists to separate a toolbar ROW from the
        // body beneath it, and this panel has no toolbar row to separate.
        // The body supplies the page inset itself (DrawAssetStatusBody's
        // `##statusbody` child, AlwaysUseWindowPadding) -- the same nesting
        // this dashboard has always drawn under, when the outer child was
        // the shared shell's `##assetsbody`.
        if (ImGui::BeginChild("##assetstatusbody", ImVec2(0.0f, -kAssetPanelBottomBarHeight)))
        {
            if (!project)
                ImGui::TextDisabled("No project open (data/-next-to-exe)");
            else
                DrawAssetStatusBody(model, project, docs, services, actions);
        }
        ImGui::EndChild();

        // ---- bottom bar band (spec s9.2) -----------------------------
        // LEFT: the one fixed form this view has always used, regardless of
        // filter state (it has no search box of its own to filter against).
        // RIGHT: EMPTY this task. Spec s9.3 gives the slot the activity
        // ring's recency line ("last change 2m ago - uv_marker.png") and Task
        // 8 wires it; the health-digest chip is NOT a stand-in -- its whole
        // job is to point AT this panel, so pointing it at itself would be
        // wrong on its own terms. Nothing else fills the gap in the meantime:
        // spec s13's rule is that a bar never renders a fact it does not have.
        {
            const AssetPanelBottomBar bar = BeginAssetPanelBottomBar("##assetstatusbottombar");
            if (bar.visible)
            {
                const HealthCounts health = model.Health();
                char left[64];
                std::snprintf(left, sizeof(left), "%d assets \xC2\xB7 %d need attention",
                              health.total, health.refused + health.queued);
                ImGui::TextUnformatted(left);
            }
            EndAssetPanelBottomBar();
        }

        ImGui::End();
        return actions;
    }
}
