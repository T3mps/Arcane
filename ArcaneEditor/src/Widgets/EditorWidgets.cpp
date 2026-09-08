#include "Widgets/EditorWidgets.hpp"

#include "Widgets/EditorFonts.hpp"   // AssetPill's 12px PushFont
#include "Widgets/EditorTheme.hpp"   // Theme:: tokens -- asset panel vocabulary is chrome

#include <imgui.h>
#include <imgui_internal.h>   // ImGuiTable + ImGuiTableColumn + TableSetColumnWidth
                              // (the field grid's shared split reads
                              // LastResizedColumn/WidthRequest/WidthGiven, none of
                              // which are public), ImTrunc, GetCurrentWindowRead

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <string>
#include <string_view>

namespace Arcane::Editor
{
    namespace
    {
        int StringResizeCallback(ImGuiInputTextCallbackData* data)
        {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                std::string* s = static_cast<std::string*>(data->UserData);
                // Upstream's own belt, spelled `data->Buf == str->c_str()` in
                // misc/cpp/imgui_stdlib.cpp -- the same pointer as data(), which is
                // what the caller below hands ImGui. It catches a user_data that is
                // not the string ImGui was given the buffer of: the assignment two
                // lines down would then point the live widget at an unrelated
                // buffer.
                IM_ASSERT(data->Buf == s->data());
                s->resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = s->data();
            }
            return 0;
        }

        // Astra::Range is authored in double; ImGui's drags take float and int.
        // Clamping through double BEFORE the narrowing cast is the point:
        // converting an out-of-range double to a narrower type is UB, and a Range
        // holds whatever the author typed at the ASTRA_REFLECT_ATTR site.
        [[nodiscard]] float ToFloatClamped(double d) noexcept
        {
            return static_cast<float>(std::clamp(d, static_cast<double>(-FLT_MAX),
                                                    static_cast<double>(FLT_MAX)));
        }

        [[nodiscard]] int ToInt32Clamped(double d) noexcept
        {
            const double lo = static_cast<double>(std::numeric_limits<int32_t>::min());
            const double hi = static_cast<double>(std::numeric_limits<int32_t>::max());
            return static_cast<int>(std::clamp(d, lo, hi));
        }

        // Range::step is the author's drag increment and DEFAULTS TO 0
        // (Attribute.hpp:77), which means "unspecified" -- not "frozen". A zero
        // therefore keeps the speed the widget used before ranges were read at
        // all; handed to ImGui it would instead be REPLACED by a speed derived
        // from the bounds (imgui_widgets.cpp:2546), changing the feel of every
        // range-annotated-but-unstepped field as a side effect of clamping it.
        [[nodiscard]] float DragSpeedFor(const Astra::Range& r, float fallbackSpeed) noexcept
        {
            return r.step > 0.0 ? ToFloatClamped(r.step) : fallbackSpeed;
        }

        // ---------------------------------------------------------------------
        // The two-column field grid (UE's Details-panel shape: label left in
        // one column, value right, one draggable split shared by every
        // section).
        //
        // Begin/End are PRIVATE to this file: FieldGrid is the only public
        // form, so there is no way to open a grid without a guaranteed close.
        // ---------------------------------------------------------------------

        // How much of the panel the label column takes when nothing has been
        // dragged yet. Only ever consulted once per session -- after that
        // InspectorState::labelColWidth is the authority.
        constexpr float kLabelColumnFraction = 0.4f;

        // Open one field region's grid. Returns false exactly when
        // ImGui::BeginTable did (culled/clipped host window), in which case the
        // caller must draw NO rows and must NOT call EndFieldGrid.
        //
        // WIDTH SYNC PROTOCOL. There is no ImGui API to bind two tables'
        // column widths, so InspectorState::labelColWidth is the shared
        // authority and each table is pushed to match it. The push has to
        // happen HERE -- after TableSetupColumn, before the first row -- for
        // two reasons, both from the vendored imgui_tables.cpp:
        //   - ImGui::TableSetColumnWidth asserts !IsLayoutLocked (:2343), and
        //     the first TableNextRow runs the layout (:1923-1924), which locks
        //     it (:1285).
        //   - TableSetupColumn's init width is applied ONLY while the table is
        //     initializing (:1693-1699 -> TableInitColumnDefaults :1637-1643),
        //     so it seeds a brand-new table and does nothing thereafter.
        //
        // Deciding whether to push or to ADOPT is what keeps a user drag from
        // being fought. A drag is not applied when the mouse moves: EndTable
        // records the pending width (:1531-1536) and the NEXT frame's
        // TableBegin applies it to WidthRequest via TableBeginApplyRequests
        // (:687-688), which is reached from TableBeginEx (:644) before
        // BeginTable returns. So by the time this runs, WidthRequest already
        // carries any user change.
        //
        // THE DISCRIMINATOR IS `LastResizedColumn`, NOT A WIDTH COMPARISON.
        // Comparing WidthRequest against WidthGiven looks like it should mean
        // "this split moved", and it is WRONG: imgui_internal.h:3126 says
        // outright that WidthGiven "may be > WidthRequest to honor minimum
        // width, may be < WidthRequest to honor shrinking columns down in
        // tight space" -- a legitimate PERMANENT divergence, not an event.
        // Two reachable ways to sit in it forever: WidthGiven is ImTrunc'd off
        // WidthRequest (:1054), so any fractional width diverges every frame;
        // and :1139 clamps WidthGiven by WidthMax, which a category grid's
        // TreeNode indent alone is enough to trigger. A table stuck in the
        // "moved" branch would adopt its own width over the shared one every
        // frame -- its push branch dead, so it could never follow another
        // section, and its stale number would clobber a real drag elsewhere.
        //
        // LastResizedColumn has exactly two writers, and both facts about
        // them are load-bearing.
        //
        // First, the ctor memset writes 0 -- ImGuiTable's ctor is
        // memset(this, 0, sizeof(*this)) (imgui_internal.h:3332), so 0 is
        // literally the value == 0 tests against -- but that write is never
        // OBSERVABLE by this code: a table whose pool slot was just
        // constructed also has RawData == NULL, which forces IsInitializing
        // = true (imgui_tables.cpp:577) and then :589 overwrites
        // LastResizedColumn to -1 as part of that init, all inside
        // BeginTableEx and before TableBeginApplyRequests runs (:644) and
        // before BeginTable returns to this call site. So `== 0` never fires
        // off the ctor's zero; it always means a real resize.
        //
        // Second, the :689 writer is UNCONDITIONAL, not resize-only: every
        // frame's instance-0 TableBegin runs `LastResizedColumn =
        // ResizedColumn` regardless of whether a resize happened this frame
        // (:687-691) -- ResizedColumn itself is reset to -1 at :691 right
        // after, so on a quiet frame this assigns the idle -1 right back.
        // That per-frame re-arm is what makes `LastResizedColumn == 0` an
        // EVENT rather than a latch: without it, a resize on column 0 once
        // would read as "still resized" on every later frame too.
        //
        // Everything else in the file only reads it. So once BeginTable has
        // returned, `LastResizedColumn == 0` means precisely "column 0 of THIS
        // table just had a queued resize applied this frame", which is the
        // event we want and nothing else.
        //
        // DECISION -- double-click auto-fit does NOT win. It is applied
        // through AutoFitSingleColumn (:695-699), which does not touch
        // LastResizedColumn, so it is not adopted and the shared width is
        // pushed back over it on the same frame. That is intended: ONE split
        // shared by every section is the feature, and a section auto-fitting
        // itself to a width of its own would break exactly that invariant.
        [[nodiscard]] bool BeginFieldGrid(const char* id, float& labelColWidth)
        {
            // Read BEFORE BeginTable: inside a table, "available" is a cell.
            // Truncated so the seed is idempotent under :1054's ImTrunc --
            // a fractional shared width would come back different from every
            // table it is pushed onto, which is noise nothing here needs.
            if (labelColWidth <= 0.0f)
            {
                const float avail = ImGui::GetContentRegionAvail().x;
                if (avail > 0.0f)
                    labelColWidth = ImTrunc(avail * kLabelColumnFraction);
            }
            // NoSavedSettings is passed explicitly even though a table inside a
            // child window inherits it anyway (:299-301): OUR float is the only
            // width authority, and an .ini-restored width would be a second one.
            if (!ImGui::BeginTable(id, 2,
                                   ImGuiTableFlags_Resizable |
                                   ImGuiTableFlags_NoSavedSettings |
                                   ImGuiTableFlags_NoBordersInBodyUntilResize))
                return false;
            // A <= 0 width here leaves the column auto-sized (:1640 stores -1
            // for it), which happens only on a frame where the panel had no
            // width to sample above; the sync below takes over once one exists.
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
                                    labelColWidth);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

            ImGuiTable* table = ImGui::GetCurrentTable();
            ImGuiTableColumn& col = table->Columns[0];
            // The `> 0` on the request is not decoration: an auto-sized column
            // carries WidthRequest == -1 (:1640), which is what the degenerate
            // seeding path above leaves behind, and adopting it would hand the
            // whole panel a negative shared width.
            if (table->LastResizedColumn == 0 && col.WidthRequest > 0.0f)
            {
                labelColWidth = col.WidthRequest;   // the user moved THIS split
            }
            // Both guards keep TableSetColumnWidth away from a state it would
            // turn into a collapsed column rather than a no-op: it ImClamps to
            // at least MinColumnWidth (:2353), so pushing an unseeded 0 would
            // pin the column there; and WidthGiven == 0 is a table that has
            // never laid out (its column is memset in the constructor,
            // imgui_internal.h:3168-3170), where WidthMax is still 0 and the
            // same clamp (:2352) would collapse it -- there the init width
            // above is the seed. Otherwise this runs unconditionally: the
            // early-out at :2354 makes the steady-state push a no-op, and
            // TableSaveSettings returns immediately under NoSavedSettings
            // (:3772-3773), so a repeated push costs nothing.
            else if (labelColWidth > 0.0f && col.WidthGiven > 0.0f)
            {
                ImGui::TableSetColumnWidth(0, labelColWidth);
            }
            return true;
        }

        // Close a grid opened by BeginFieldGrid. Takes no state because the
        // whole width hand-off happens in BeginFieldGrid: a drag only reaches
        // WidthRequest at the NEXT frame's BeginTable (see above), so a
        // write-back here would read a width the drag has not landed in yet and
        // the following frame would push that stale number back over it.
        void EndFieldGrid()
        {
            ImGui::EndTable();
        }

        // ---------------------------------------------------------------------
        // Axis color bars (UE's Details-panel treatment for vector components:
        // X red, Y green, Z blue on the left edge of each component's frame).
        // ---------------------------------------------------------------------

        // Sampled off the UE reference screenshot -- deliberately muted, unlike
        // the saturated primaries ImGui's own component markers use
        // (GDefaultRgbaColorMarkers is 240/20/20, 20/240/20, 20/20/240 --
        // imgui_widgets.cpp:2257-2260).
        constexpr ImU32 kAxisBarColors[3] = {
            IM_COL32(196,  64,  54, 255),   // X
            IM_COL32( 96, 166,  58, 255),   // Y
            IM_COL32( 58, 122, 196, 255),   // Z
        };

        // How wide the strip is, in pixels. Matches ImGuiStyle::ColorMarkerSize's
        // own default (imgui.cpp:1564), which is the width the vendored marker
        // renderer would have used.
        constexpr float kAxisBarWidth = 3.0f;

        // ---------------------------------------------------------------------
        // Header bands (UE's Details treatment for CollapsingHeader/TreeNodeEx:
        // a muted dark band in place of ImGuiCol_Header, which the editor theme
        // spends on SELECTION -- EditorTheme.hpp). Beside the axis palette per
        // Section 2's rule that inspector style constants live in one place.
        // ---------------------------------------------------------------------

        // Sampled off the UE reference screenshot, same as kAxisBarColors --
        // desk call, not measured off UE pixels. Hover/active step up in
        // lightness so the row still visibly responds to input. They sit ABOVE
        // the theme's panel tone (#1e1e1e, EditorTheme.hpp kPanel), so the band
        // still reads as raised against the body it heads.
        constexpr ImU32 kHeaderBandColor        = IM_COL32(48, 48, 52, 255);
        constexpr ImU32 kHeaderBandHoveredColor = IM_COL32(58, 58, 64, 255);
        constexpr ImU32 kHeaderBandActiveColor  = IM_COL32(66, 66, 73, 255);

        // Push/pop as a matched pair so every call site pushes and pops the
        // same 3 colors, rather than trusting three inline pushes (and three
        // inline pops) to stay in sync at each of the two header call sites.
        // Both CollapsingHeader (Framed) and TreeNodeEx (unframed) resolve
        // their background from this same triple, picked by hover/held state
        // (imgui_widgets.cpp:7102 framed, :7123 unframed), so one push covers
        // either caller.
        //
        // PRIVATE to this file for the same reason Begin/EndFieldGrid are:
        // HeaderBand is the only public form, so the pair cannot be split.
        void PushHeaderBandColors()
        {
            ImGui::PushStyleColor(ImGuiCol_Header, kHeaderBandColor);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kHeaderBandHoveredColor);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, kHeaderBandActiveColor);
        }

        void PopHeaderBandColors()
        {
            ImGui::PopStyleColor(3);
        }

        // ---------------------------------------------------------------------
        // Asset panel vocabulary (asset-manager-redesign-design.md §11.1/§11.2):
        // AssetPill, SegmentedStrip, RowWithThumb. Model-free -- the tooltip
        // that additionally needs a thumbnail resolver lives with the panel
        // (Task 9), not here.
        // ---------------------------------------------------------------------

        // Pill geometry (spec §11.2: "12px text, 16px line, 1px #333333
        // border"). The line height itself is the HEADER's kPillLineHeight --
        // it has an out-of-file consumer (see its doc comment there). The
        // neutral border IS a theme token already -- kSeparator is
        // EditorTheme.hpp's own #333333, used today for table borders -- but
        // the amber variant's #7a5a20 has no token of its own, so it is
        // hardcoded here for the same reason kAxisBarColors/kHeaderBandColor
        // above are: a spec-pinned hex with no chrome-ramp equivalent, not an
        // oversight.
        //
        // SECOND CONSUMER (Plan 2, Task 6): BeginCardFrame's variant 1 --
        // "the muted-amber acting-on frame" -- reuses this SAME constant for
        // its border, same TU, no new token. AssetPill and CardFrame are
        // therefore the two places in the codebase that draw the #7a5a20
        // acting-on frame; if a third ever needs it, promote it to
        // EditorTheme.hpp instead of a third hardcode.
        constexpr ImU32 kPillAmberBorder = IM_COL32(0x7a, 0x5a, 0x20, 255);

        // ---------------------------------------------------------------------
        // Status lens vocabulary (Plan 2, asset-manager-redesign-design.md
        // §9.2/§11.1/§11.2): StatTile, MeterBar, BeginCardFrame/EndCardFrame,
        // TimelineFeed. Model-free, same as the asset panel vocabulary above.
        // ---------------------------------------------------------------------

        // BeginCardFrame/EndCardFrame state. A stack, not a single slot: Task
        // 7's Needs-attention section opens one CardFrame per refused/queued
        // entry in a loop, and imgui.h's own ImDrawListSplitter doc comment
        // ("Prefer using your own persistent instance ... as you can stack
        // them") is exactly why this uses a private ImDrawListSplitter per
        // card rather than the ImDrawList's built-in convenience
        // ChannelsSplit/Merge (which "cannot stack a split over another" on
        // the same list) -- each entry in this stack owns its own splitter,
        // so sequential (or, if a caller ever nests them, nested) CardFrames
        // never collide on the same underlying draw list.
        struct CardFrameState
        {
            ImDrawListSplitter splitter;
            ImDrawList* drawList = nullptr;
            ImVec2 pos{};
            float width = 0.0f;
            int variant = 0;
        };

        // File-local by construction (anonymous namespace): ImGui itself is
        // main-thread-only in this codebase, so a single stack shared by every
        // window is safe -- exactly the same assumption FieldGrid/HeaderBand's
        // caller-held state relies on, just inverted (their state lives on the
        // CALLER's stack via an RAII type; Begin/EndCardFrame are plain
        // functions, so the stack has to live somewhere between the two calls).
        //
        // std::deque, not std::vector: CardFrameState holds a live
        // ImDrawListSplitter, whose ImDrawChannel entries own ImVector
        // buffers (_CmdBuffer/_IdxBuffer). A vector's growth reallocation
        // copies existing elements into new storage and destroys the old
        // ones -- CardFrameState's (implicit) copy shallow-copies those
        // ImVector buffer pointers, so the destroyed source's
        // ~ImDrawListSplitter would free memory the "moved" copy still
        // points at. deque never relocates existing elements when it grows,
        // so a splitter's buffers are never copied or moved by container
        // growth, only by BeginCardFrame/EndCardFrame's own push/pop. This
        // matters once nesting is exercised (Plan 3): a nested
        // BeginCardFrame pushes a second element while the outer one is
        // still live on this stack.
        std::deque<CardFrameState> g_cardFrameStack;

        // Inner padding shared by BeginCardFrame/EndCardFrame (spec: 8px).
        constexpr float kCardFramePadding = 8.0f;
    }

    // capacity() + 1 is BufSize's own C++ spelling (imgui.h:2772); the +1 is
    // the terminator slot past capacity(). It cannot under-report the room
    // available, because ImGui's ONLY write into the buffer runs the resize
    // callback above first and then copies into the pointer that callback
    // returned (imgui_widgets.cpp:5423-5447) -- the string is grown to fit
    // before any byte lands in it.
    bool InputTextString(const char* label, std::string* s, ImGuiInputTextFlags flags)
    {
        // Upstream's other belt (same file): CallbackResize is THIS helper's to
        // set, because it also supplies the callback that services it and `s` as
        // that callback's user data. A caller passing the flag is asking for a
        // resize hook it has no parameter to supply.
        IM_ASSERT((flags & ImGuiInputTextFlags_CallbackResize) == 0);
        flags |= ImGuiInputTextFlags_CallbackResize;
        return ImGui::InputText(label, s->data(), s->capacity() + 1, flags,
                                StringResizeCallback, s);
    }

    // The format strings are spelled out only because `flags` sits after them
    // in the signature; both are the header's own defaults (imgui.h:687/692),
    // so nothing about how a value reads changes.
    bool RangedDragFloat(const char* label, float* v, float fallbackSpeed,
                         const std::optional<Astra::Range>& range)
    {
        if (range)
            return ImGui::DragFloat(label, v, DragSpeedFor(*range, fallbackSpeed),
                                    ToFloatClamped(range->min), ToFloatClamped(range->max),
                                    "%.3f", ImGuiSliderFlags_ClampOnInput);
        return ImGui::DragFloat(label, v, fallbackSpeed);
    }

    bool RangedDragInt(const char* label, int* v, const std::optional<Astra::Range>& range)
    {
        if (range)
            // 1.0f is DragInt's own default speed (imgui.h:692), passed
            // explicitly because the bounded overload leaves no way to omit it.
            return ImGui::DragInt(label, v, DragSpeedFor(*range, 1.0f),
                                  ToInt32Clamped(range->min), ToInt32Clamped(range->max),
                                  "%d", ImGuiSliderFlags_ClampOnInput);
        return ImGui::DragInt(label, v);
    }

    // Returns whether the LABEL is hovered. The label is its own ImGui item
    // now, so the row's tail tooltip -- which asks about the LAST item, i.e.
    // the value widget -- would never fire over the name; the caller ORs
    // this in. Asked here, while the label still IS the last item.
    //
    // `dimmed` is UE's disabled-label treatment for a field that cannot be
    // edited. The color push is exactly what ImGui::TextDisabled does
    // (imgui_widgets.cpp:316-322), spelled out so the text can go through
    // TextUnformatted rather than a format string.
    bool FieldLabelCell(const std::string& label, bool dimmed)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        // The value cell holds a FRAMED widget on almost every row, whose
        // text sits FramePadding.y below the row top; bare text draws at
        // DC.CurrLineTextBaseOffset (imgui_widgets.cpp:177), which is 0 here
        // -- so without this the name rides high against its own value. A
        // table cannot fix it afterwards: TableEndCell only raises
        // RowTextBaseline for cells submitted LATER in the row
        // (imgui_tables.cpp:2273), and this is the first one.
        ImGui::AlignTextToFramePadding();
        if (dimmed)
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(label.c_str());
        if (dimmed)
            ImGui::PopStyleColor();
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
        ImGui::TableSetColumnIndex(1);
        // -FLT_MIN is ImGui's "fill the remaining width" spelling
        // (CalcItemWidth resolves a negative width against
        // GetContentRegionAvail, imgui.cpp:12319-12323), and inside a cell
        // that region IS the cell.
        ImGui::SetNextItemWidth(-FLT_MIN);
        return hovered;
    }

    // Paint the axis strip over the left edge of the item just submitted.
    // `component` indexes kAxisBarColors; an index past the palette draws
    // nothing, so a row wider than three components degrades quietly rather
    // than reading out of bounds.
    //
    // An OVERLAY on purpose: it runs AFTER the widget, so it pushes no style
    // and cannot move layout. It sits flush on the frame's corners because
    // nothing in this editor overrides ImGuiStyle::FrameRounding, whose
    // default is 0.0f (imgui.cpp:1532); a rounded frame would instead want
    // the vendored RenderColorComponentMarker (imgui.cpp:4089-4096), which
    // rounds -- but that one is reachable only from DragScalar/SliderScalar
    // (imgui_widgets.cpp:2791, :3384) and so cannot serve the multi-select
    // text boxes, which need the same strip.
    void DrawAxisBar(int component)
    {
        if (component < 0 || component >= IM_ARRAYSIZE(kAxisBarColors))
            return;
        // A window that is skipping items submitted nothing: both DragScalar
        // (imgui_widgets.cpp:2721-2723) and InputTextEx (:4708-4710) return
        // on that flag BEFORE ItemAdd, so g.LastItemData still describes some
        // EARLIER item and a bar taken from its rect would be painted onto
        // that one.
        if (ImGui::GetCurrentWindowRead()->SkipItems)
            return;
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(
            min, ImVec2(min.x + kAxisBarWidth, max.y), kAxisBarColors[component]);
    }

    // WHY NOT DragFloat2/3: the bar needs each component's OWN frame rect,
    // and DragScalarN submits its components internally -- by the time it
    // returns, EndGroup has overwritten g.LastItemData.Rect with the group's
    // bounding box (imgui.cpp:12483), which is the only rect the caller can
    // see. Recovering the components from that would mean re-deriving
    // PushMultiItemsWidths' split arithmetic (imgui.cpp:12283-12291) out
    // here, against an internal layout detail no API contract holds still.
    //
    // This IS DragScalarN's body (imgui_widgets.cpp:2814-2849) specialised to
    // float with no bounds, with the bar added, the trailing
    // visible-label block (:2840-2845) dropped -- already dead for these
    // callers, whose labels are the "##name" hidden-id form -- and the
    // `flags` parameter dropped along with it, which also drops the
    // ImGuiSliderFlags_ColorMarkers branch it gates (:2831-2832): none of
    // these callers pass flags, so that branch was already unreachable
    // here too. An ImGui upgrader re-diffing this against the vendored
    // body should expect both omissions, not just the label block:
    // FindRenderedTextEnd stops at the leading "##" (imgui.cpp:3918) and
    // returns the string start, so :2841's
    // `label != label_end` is false. Everything ids and undo depend on is
    // therefore unchanged: the same BeginGroup/EndGroup, the same
    // PushID(label) + PushID(i) nesting over the same "" child labels, so
    // each component keeps the exact ImGui id DragFloat2/3 gave it, and the
    // caller's BeginGestureIfActivated/EndGesture still read the id EndGroup
    // forwards out of the group (imgui.cpp:12477-12482) exactly as before.
    bool AxisDragFloatN(const char* label, float* v, int count, float speed)
    {
        // DragScalarN's own guard (:2816-2818), kept in the same place and
        // for the same reason: it returns before the group opens, so there
        // is nothing to unwind.
        if (ImGui::GetCurrentWindowRead()->SkipItems)
            return false;
        bool changed = false;
        ImGui::BeginGroup();
        ImGui::PushID(label);
        ImGui::PushMultiItemsWidths(count, ImGui::CalcItemWidth());
        for (int i = 0; i < count; ++i)
        {
            ImGui::PushID(i);
            if (i > 0)
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            // `speed` is the only argument these callers ever varied; every
            // other one is DragFloat's default, and DragFloat's defaults ARE
            // DragFloat2/3's defaults (imgui.h:687-689), so each component
            // behaves exactly as it did inside the combined widget.
            // Written as an if rather than |= only to keep the assignment
            // bool-typed; like DragScalarN's |= it does not short-circuit,
            // so every component is always submitted.
            if (ImGui::DragFloat("", &v[i], speed))
                changed = true;
            DrawAxisBar(i);
            ImGui::PopID();
            ImGui::PopItemWidth();
        }
        ImGui::PopID();
        ImGui::EndGroup();
        return changed;
    }

    std::string EllipsisToWidth(std::string_view text, float maxWidth)
    {
        const std::string full(text);
        if (ImGui::CalcTextSize(full.c_str()).x <= maxWidth)
            return full;

        // Longest prefix such that prefix + "..." fits, by binary search on the
        // byte length -- text metrics are monotonic in the prefix.
        const auto fits = [&](size_t bytes)
        {
            std::string probe(text.substr(0, bytes));
            probe += "...";
            return ImGui::CalcTextSize(probe.c_str()).x <= maxWidth;
        };
        size_t lo = 0, hi = text.size();
        while (lo < hi)
        {
            const size_t mid = (lo + hi + 1) / 2;
            if (fits(mid)) lo = mid;
            else           hi = mid - 1;
        }
        // Never cut mid-codepoint: back off UTF-8 continuation bytes.
        while (lo > 0 && (static_cast<unsigned char>(text[lo]) & 0xC0) == 0x80)
            --lo;
        std::string out(text.substr(0, lo));
        out += "...";
        return out;
    }

    FieldGrid::FieldGrid(const char* id, float& labelColWidth)
        : m_open(BeginFieldGrid(id, labelColWidth))
    {
    }

    FieldGrid::~FieldGrid()
    {
        // Guarded, not unconditional: BeginTable returning false pushed no
        // table, and EndTable on nothing asserts.
        if (m_open)
            EndFieldGrid();
    }

    HeaderBand::HeaderBand()
    {
        PushHeaderBandColors();
    }

    HeaderBand::~HeaderBand()
    {
        PopHeaderBandColors();
    }

    bool StableTextEdit(const char* imguiLabel, TextCommitState& st, std::uint64_t key,
                        std::string_view current, float width,
                        Arcane::FunctionRef<void(const char*)> commit)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*s",
                      static_cast<int>(current.size()), current.data());
        if (st.activeKey == key)
            std::memcpy(buf, st.buf, sizeof(buf));
        ImGui::SetNextItemWidth(width);
        ImGui::InputText(imguiLabel, buf, sizeof(buf));
        if (ImGui::IsItemActive())
        {
            st.activeKey = key;
            std::memcpy(st.buf, buf, sizeof(st.buf));
            return false;
        }
        if (st.activeKey != key)
            return false;
        const bool committed = ImGui::IsItemDeactivatedAfterEdit();
        st.activeKey = 0;
        // Compare the POST-InputText local, NOT st.buf (last frame's typed
        // snapshot): on the deactivation frame ImGui has already written its
        // final text back into `buf`, and for Escape that text is what the
        // field held when it gained focus. InputTextEx sets revert_edit
        // (imgui_widgets.cpp:5212), copies TextToRevertTo over the edited text
        // (:5300-5308), and ImStrncpy's the result into the caller's buffer
        // (:5447) -- so an escaped edit arrives here with buf == current and
        // commits nothing.
        //
        // It arrives here AT ALL because deactivated-after-edit remembers the
        // whole edit, not this frame. Every earlier keystroke's MarkItemEdited
        // latched g.ActiveIdHasBeenEditedBefore (imgui.cpp:4898); the escape
        // frame's ClearActiveID (imgui_widgets.cpp:5452-5453 -> imgui.cpp:
        // 4863-4866 -> SetActiveID :4797) copies that latch into
        // DeactivatedItemData.HasBeenEditedBefore (:4808); and
        // IsItemDeactivatedAfterEdit reads exactly that (:6562-6565). The
        // revert's own value_changed -> MarkItemEdited only re-sets the same
        // already-true flag (:4899-4900) -- it is not what makes the item
        // report edited. So the flag means "was edited at some point", the
        // escape being invisible to it and visible ONLY in the buffer, which
        // is why comparing st.buf committed the abandoned text.
        if (committed && current != buf)
        {
            commit(buf);
            return true;
        }
        return false;
    }

    // A rect + 1px border from the window drawlist around PushFont-sized
    // text, sized to the pill's OWN 16px line height rather than ImGui's
    // frame padding -- a pill is a much smaller chip than a normal widget.
    // variant 0 borrows Theme::kSeparator for its border (the same #333333
    // spec §11.2 pins) and Theme::kGrab for its text (the spec's #9a9a9a --
    // "TextDisabled-ish" in name only; kTextDim is a different gray, #737373,
    // so kGrab is the token that actually matches). variant 1 is the amber
    // attention pill: kPillAmberBorder (no token exists for #7a5a20) and
    // Theme::kAmber, whose own value already IS the spec's #ffa61a.
    void AssetPill(const char* text, int variant)
    {
        if (ImGui::GetCurrentWindowRead()->SkipItems)
            return;

        ImGui::PushFont(GetEditorFonts().interRegular, 12.0f);

        const ImVec2 textSize = ImGui::CalcTextSize(text);
        const float paddingX = ImGui::GetStyle().FramePadding.x;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size(textSize.x + paddingX * 2.0f, kPillLineHeight);

        const ImU32 borderColor = (variant == 1) ? kPillAmberBorder
                                                  : ImGui::GetColorU32(Theme::kSeparator);
        const ImU32 textColor   = (variant == 1) ? ImGui::GetColorU32(Theme::kAmber)
                                                  : ImGui::GetColorU32(Theme::kGrab);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderColor);
        dl->AddText(ImVec2(pos.x + paddingX, pos.y + (kPillLineHeight - textSize.y) * 0.5f),
                    textColor, text);

        // A real item, not just drawlist paint: Dummy reserves the layout
        // space so a caller chaining several pills (or a row's trailing-pill
        // run) with SameLine gets correct advancement.
        ImGui::Dummy(size);

        ImGui::PopFont();
    }

    // N ImGui::Buttons with zero ItemSpacing, so each button's own
    // FrameBorderSize edge (the theme's global style.FrameBorderSize = 1,
    // EditorTheme.hpp) sits flush against its neighbour's rather than
    // doubling up -- "collapsed shared borders" falls out of that geometry,
    // not extra drawing. FrameRounding is pinned to 0 for the "square
    // corners" requirement, even though that already IS the theme's default
    // (EditorTheme.hpp never touches FrameRounding, so it stays ImGui's
    // stock 0) -- pinned explicitly because this widget's contract depends
    // on the value, not on the theme happening to agree with it today.
    int SegmentedStrip(const char* id, const char* const* items, int count,
                       int active, unsigned enabledMask)
    {
        int clicked = -1;

        ImGui::PushID(id);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

        for (int i = 0; i < count; ++i)
        {
            if (i > 0)
                ImGui::SameLine();

            const bool isActive  = (i == active);
            const bool isEnabled = (enabledMask & (1u << i)) != 0;

            if (isActive)
                ImGui::PushStyleColor(ImGuiCol_Button, Theme::kButtonActive);
            if (!isEnabled)
                ImGui::BeginDisabled();

            ImGui::PushID(i);
            if (ImGui::Button(items[i]))
                clicked = i;
            ImGui::PopID();

            if (!isEnabled)
                ImGui::EndDisabled();
            if (isActive)
                ImGui::PopStyleColor();
        }

        ImGui::PopStyleVar(2);
        ImGui::PopID();
        return clicked;
    }

    // Selectable reserves the FULL row -- SpanAllColumns so the click/hover
    // surface covers every column when a caller draws this inside a table
    // row, and the window's own work rect otherwise (imgui.cpp sets every
    // window's ParentWorkRect = WorkRect in Begin(), so SpanAllColumns
    // degrades to "span this window" outside a table rather than asserting,
    // which is what lets this same helper draw both the table rows and the
    // (table-free) rail rows per spec §11.1).
    //
    // TASK 10 FIX ROUND 1 (review Critical 1): the thumb and name are now
    // PURE ImDrawList overdraw (AddImage/AddText, no ItemAdd of their own)
    // rather than ImGui::Image/TextUnformatted items. The Selectable is
    // therefore the ONLY real item this function submits -- it stays
    // ImGui's "last submitted item" for as long as the caller wants it to,
    // which is the whole point: the previous design's real Image/Text items
    // silently became the "last item" instead, so a caller's
    // BeginDragDropSource/BeginPopupContextItem/tooltip calls (which all key
    // off "the last item") landed on the NAME TEXT's tiny rect rather than
    // the row -- the bug that motivated a since-deleted InvisibleButton
    // "hit anchor" overlay, which had its own, worse bug (see below).
    //
    // The Selectable is submitted with SetNextItemAllowOverlap() so a
    // caller MAY still add a small foreground item after this call returns
    // (an expander chevron, the rail's hover "+") without it being starved
    // by the Selectable's own hover claim -- imgui.cpp:5089-5118 is precise
    // about the direction this has to run: the flag belongs on the
    // BACKGROUND item (this Selectable) and grants permission to whatever
    // is submitted AFTER it, never the reverse. This is also exactly why
    // the deleted InvisibleButton design was broken instead of merely
    // suboptimal: it flagged the Selectable (background, correct side) but
    // then overlaid a SAME-SIZE button over the ENTIRE row rather than a
    // small sub-area -- so every single frame the mouse was anywhere on the
    // row, that overlay re-claimed HoveredId, and the AllowOverlap
    // precondition ("g.HoveredIdPreviousFrame == this Selectable's id",
    // imgui.cpp:5117) could never be satisfied. A caller that adds no
    // foreground item at all pays nothing for the flag: nothing else ever
    // contests the row's hover, so it settles to hovered=true after, at
    // most, one frame.
    AssetRowResult RowWithThumb(const char* id, ImTextureID thumb, const char* iconUtf8,
                                const char* name, bool selected, float indent,
                                float rowHeight)
    {
        AssetRowResult result;
        if (ImGui::GetCurrentWindowRead()->SkipItems)
            return result;

        ImGui::PushID(id);
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();

        // Ruling 4 (desk pass, 2026-09-07: "Fix it since it's cheap") --
        // plain SpanAllColumns pads the Selectable's highlight bb by half of
        // style.ItemSpacing.y on EACH side (imgui_widgets.cpp's Selectable(),
        // the NoPadWithHalfSpacing-gated block just past the bb computation)
        // regardless of the explicit `rowHeight` passed in here: with the
        // stock ItemSpacing.y=4, that is +2px top and +2px bottom, so this
        // row's own 24px table pitch painted a 28px highlight (measured by
        // the automated mock-vs-editor comparison), same bleed on the 26px
        // rail. This is the ONE Selectable every asset/child/rail row in
        // AssetsPanel.cpp goes through, so one flag here fixes all three;
        // DrawGroupRow's own Selectable carries the identical fix
        // separately, since group rows don't route through RowWithThumb.
        ImGui::SetNextItemAllowOverlap();
        result.clicked = ImGui::Selectable("##row", selected,
                                           ImGuiSelectableFlags_SpanAllColumns |
                                           ImGuiSelectableFlags_AllowDoubleClick |
                                           ImGuiSelectableFlags_NoPadWithHalfSpacing,
                                           ImVec2(0.0f, rowHeight));
        result.hovered = ImGui::IsItemHovered();

        ImDrawList* dl = ImGui::GetWindowDrawList();

        const float thumbY = rowMin.y + (rowHeight - kAssetRowThumbSize) * 0.5f;
        if (thumb != 0)
        {
            dl->AddImage(thumb, ImVec2(rowMin.x + indent, thumbY),
                        ImVec2(rowMin.x + indent + kAssetRowThumbSize, thumbY + kAssetRowThumbSize));
        }
        else
        {
            // Icon fallback: a Lucide glyph centered WITHIN the same 18px
            // cell, under whichever font is active (every editor face
            // carries the merged icon range, EditorFonts.cpp).
            const ImVec2 iconSize = ImGui::CalcTextSize(iconUtf8);
            dl->AddText(ImVec2(rowMin.x + indent + (kAssetRowThumbSize - iconSize.x) * 0.5f,
                              rowMin.y + (rowHeight - iconSize.y) * 0.5f),
                       ImGui::GetColorU32(ImGuiCol_Text), iconUtf8);
        }

        // The name anchors at indent + the thumb CELL's fixed width --
        // there is no per-glyph icon-rect to diverge from any more (nothing
        // above is a real item), so both the thumb and icon-fallback paths
        // already agree on where the cell ends.
        const float nameX = rowMin.x + indent + kAssetRowThumbSize + ImGui::GetStyle().ItemInnerSpacing.x;
        const ImVec2 nameSize = ImGui::CalcTextSize(name);
        dl->AddText(ImVec2(nameX, rowMin.y + (rowHeight - nameSize.y) * 0.5f),
                   ImGui::GetColorU32(ImGuiCol_Text), name);

        // Where the caller's own trailing content (pills, right-aligned
        // extras) should START -- see the header's own doc comment on
        // `trailingPos` for why this replaces the old bare-SameLine()
        // convention (there is no longer a real name ITEM for SameLine to
        // read line metrics off of).
        result.trailingPos = ImVec2(nameX + nameSize.x + ImGui::GetStyle().ItemInnerSpacing.x,
                                    rowMin.y + (rowHeight - kPillLineHeight) * 0.5f);

        // Put the flow cursor back at the row's true bottom: the NEXT
        // sibling (another row, in the common no-trailing-content case)
        // must not start mid-row. CursorPosPrevLine is untouched by this
        // (imgui.cpp's SetCursorScreenPos), which no longer matters to a
        // caller wanting trailing content -- it seeds its own cursor from
        // `result.trailingPos` instead of a bare SameLine().
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMin.y + rowHeight));

        ImGui::PopID();
        return result;
    }

    // Bordered card: leading icon (ambient size) + PushFont'd 24px number on
    // the first line, 13px label on the second. variant 1 tints ONLY the icon
    // Theme::kAmber; the number is always ImGuiCol_Text (spec §11.2's amber
    // rule -- amber marks the refused tile's icon, never the count). Fill is
    // Theme::kPanelRaised (a step up from the panel it sits on, same "raised"
    // read as a highlighted metric), border Theme::kSeparator regardless of
    // variant -- amber stays confined to the icon exactly as documented.
    //
    // `id` scopes the tile the same way RowWithThumb's does -- nothing inside
    // needs it today (the only real item is the closing Dummy, which -- like
    // AssetPill's -- carries no string id of its own), but a caller drawing
    // several tiles in one row still gets a clean, collision-free ID scope
    // for whatever gets added under a tile later (a hover tooltip, say).
    void StatTile(const char* id, const char* number, const char* label,
                  const char* iconUtf8, int variant, const ImVec2& size)
    {
        if (ImGui::GetCurrentWindowRead()->SkipItems)
            return;

        ImGui::PushID(id);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                          ImGui::GetColorU32(Theme::kPanelRaised));
        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                   ImGui::GetColorU32(Theme::kSeparator));

        constexpr float kPad = 8.0f;
        const bool hasIcon = iconUtf8 != nullptr && iconUtf8[0] != '\0';

        // Icon measured at the AMBIENT font (whatever is active when StatTile
        // is called, typically the 16px UI default) -- BEFORE any PushFont,
        // and drawn below only AFTER the number's 24px scope is popped, so
        // measure and draw always share one font scope. (2026-09-07 review
        // fix: the draw call used to run INSIDE the number's PushFont(24)
        // below -- ImDrawList::AddText's 2-arg overload resolves font/size
        // from whatever is active AT THE CALL, not at CalcTextSize time -- so
        // the glyph actually rendered at 24px while `iconSize` and the
        // vertical-centering math both used the smaller ambient measurement:
        // mis-centered, and `iconAdvance` under-reserved room for it.)
        const ImVec2 iconSize = hasIcon ? ImGui::CalcTextSize(iconUtf8) : ImVec2(0.0f, 0.0f);
        const float iconAdvance = hasIcon ? iconSize.x + ImGui::GetStyle().ItemInnerSpacing.x : 0.0f;

        ImGui::PushFont(GetEditorFonts().interRegular, 24.0f);
        const ImVec2 numberSize = ImGui::CalcTextSize(number);
        const float rowY = pos.y + kPad;
        dl->AddText(ImVec2(pos.x + kPad + iconAdvance, rowY), ImGui::GetColorU32(ImGuiCol_Text), number);
        ImGui::PopFont();

        if (hasIcon)
        {
            const ImU32 iconColor = (variant == 1) ? ImGui::GetColorU32(Theme::kAmber)
                                                    : ImGui::GetColorU32(ImGuiCol_Text);
            // Same ambient scope as the CalcTextSize above (font popped back
            // by now), centered against the number's measured line height.
            dl->AddText(ImVec2(pos.x + kPad, rowY + (numberSize.y - iconSize.y) * 0.5f),
                       iconColor, iconUtf8);
        }

        ImGui::PushFont(GetEditorFonts().interRegular, 13.0f);
        dl->AddText(ImVec2(pos.x + kPad, rowY + numberSize.y + 2.0f),
                   ImGui::GetColorU32(Theme::kTextDim), label);
        ImGui::PopFont();

        // A real item, not just drawlist paint -- see AssetPill's own comment
        // on its closing Dummy for why (SameLine chaining across a tile row).
        ImGui::Dummy(size);
        ImGui::PopID();
    }

    // Stacked bar over a Theme::kWell track (the "surface" the 2px gaps
    // between segments reveal), then one legend row -- swatch + "label
    // count" -- chained left to right, one entry per segment REGARDLESS of
    // count (a zero-count segment draws no slice but keeps its legend entry,
    // per the contract).
    //
    // `id` scopes the bar the same headroom reason as StatTile's -- nothing
    // inside needs it today.
    void MeterBar(const char* id, const MeterSegment* segments, int count, float width)
    {
        if (ImGui::GetCurrentWindowRead()->SkipItems)
            return;

        ImGui::PushID(id);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        constexpr float kBarHeight  = 12.0f;   // spec §11.2: "10-12px tall"
        constexpr float kSegmentGap = 2.0f;    // spec §11.2: "2px gaps"
        constexpr float kSwatchSize = 8.0f;
        constexpr float kLegendGapY = 6.0f;
        constexpr float kLegendGapX = 14.0f;

        dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + kBarHeight),
                          ImGui::GetColorU32(Theme::kWell));

        int total = 0;
        int visible = 0;
        for (int i = 0; i < count; ++i)
        {
            if (segments[i].count > 0)
            {
                total += segments[i].count;
                ++visible;
            }
        }

        if (total > 0)
        {
            const float totalGap = kSegmentGap * static_cast<float>(visible > 0 ? visible - 1 : 0);
            const float usable = (width - totalGap) > 0.0f ? (width - totalGap) : 0.0f;
            float x = pos.x;
            int drawn = 0;
            for (int i = 0; i < count; ++i)
            {
                if (segments[i].count <= 0)
                    continue;
                const float w = usable * (static_cast<float>(segments[i].count) / static_cast<float>(total));
                dl->AddRectFilled(ImVec2(x, pos.y), ImVec2(x + w, pos.y + kBarHeight), segments[i].color);
                x += w;
                ++drawn;
                if (drawn < visible)
                    x += kSegmentGap;
            }
        }

        // Legend: one row, direct labels -- "label count" per segment, a
        // small color swatch leading each. ImGui::GetTextLineHeight() is the
        // ambient font's line height; nothing here pushes a size, so this
        // reads at the caller's current font (16px UI default in practice).
        const float legendY = pos.y + kBarHeight + kLegendGapY;
        const float legendRowHeight = ImGui::GetTextLineHeight();
        float legendX = pos.x;
        char buf[64];
        for (int i = 0; i < count; ++i)
        {
            const float swatchY = legendY + (legendRowHeight - kSwatchSize) * 0.5f;
            dl->AddRectFilled(ImVec2(legendX, swatchY), ImVec2(legendX + kSwatchSize, swatchY + kSwatchSize),
                             segments[i].color);
            legendX += kSwatchSize + ImGui::GetStyle().ItemInnerSpacing.x;

            std::snprintf(buf, sizeof(buf), "%s %d", segments[i].label, segments[i].count);
            dl->AddText(ImVec2(legendX, legendY), ImGui::GetColorU32(ImGuiCol_Text), buf);
            legendX += ImGui::CalcTextSize(buf).x + kLegendGapX;
        }

        ImGui::Dummy(ImVec2(width, kBarHeight + kLegendGapY + legendRowHeight));
        ImGui::PopID();
    }

    // Opens a card: pushes `id` (caller content -- buttons, a Recook/Problems
    // pair -- needs its own id scope, RowWithThumb's reasoning) and a fresh
    // ImDrawListSplitter, redirects the drawlist to channel 1 (content), and
    // seats the cursor `kCardFramePadding` in from the card's top-left. The
    // background+border cannot be drawn yet -- the card's height is whatever
    // the caller draws next -- so EndCardFrame paints it retroactively into
    // channel 0 once the content's extent is known.
    //
    // The false-return contract mirrors FieldGrid's, not ImGui::Begin's:
    // SkipItems returns false WITHOUT pushing anything, so the caller must
    // not call EndCardFrame.
    bool BeginCardFrame(const char* id, int variant, float width)
    {
        if (ImGui::GetCurrentWindowRead()->SkipItems)
            return false;

        ImGui::PushID(id);

        const float w = (width > 0.0f) ? width : ImGui::GetContentRegionAvail().x;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        CardFrameState& st = g_cardFrameStack.emplace_back();
        st.drawList = dl;
        st.pos = pos;
        st.width = w;
        st.variant = variant;
        st.splitter.Split(dl, 2);
        st.splitter.SetCurrentChannel(dl, 1);

        ImGui::SetCursorScreenPos(ImVec2(pos.x + kCardFramePadding, pos.y + kCardFramePadding));
        ImGui::BeginGroup();
        return true;
    }

    // Closes the card opened by the matching BeginCardFrame. EndGroup's own
    // item rect is the "measured group rect" the header promises: it is the
    // union of every REAL item submitted since BeginGroup (imgui.cpp), which
    // is exactly why every widget in this file ends in one real item (Dummy,
    // a Selectable, ...) rather than pure ImDrawList paint -- a caller who
    // filled a card with nothing but raw AddText/AddImage would measure as
    // zero-height here.
    void EndCardFrame()
    {
        IM_ASSERT(!g_cardFrameStack.empty());
        CardFrameState& st = g_cardFrameStack.back();

        ImGui::EndGroup();
        const ImVec2 groupMax = ImGui::GetItemRectMax();

        const ImVec2 frameMin = st.pos;
        const ImVec2 frameMax(st.pos.x + st.width, groupMax.y + kCardFramePadding);

        // Channel 0, UNDER the content already painted into channel 1 --
        // Merge() below flattens 0-then-1, so this fill+border sits behind
        // the caller's content despite being drawn chronologically after it.
        st.splitter.SetCurrentChannel(st.drawList, 0);
        const ImU32 borderColor = (st.variant == 1) ? kPillAmberBorder
                                                    : ImGui::GetColorU32(Theme::kSeparator);
        st.drawList->AddRectFilled(frameMin, frameMax, ImGui::GetColorU32(Theme::kChrome));
        st.drawList->AddRect(frameMin, frameMax, borderColor);
        st.splitter.Merge(st.drawList);

        // Reserve the FULL card (frame, not just the padded content group) as
        // one real item so a caller stacking several cards -- or chaining a
        // SameLine sibling -- gets correct advancement, same discipline as
        // every other widget in this file.
        ImGui::SetCursorScreenPos(frameMin);
        ImGui::Dummy(ImVec2(frameMax.x - frameMin.x, frameMax.y - frameMin.y));

        g_cardFrameStack.pop_back();
        ImGui::PopID();
    }

    // Vertical line + dot per entry, each dot centered on its entry's first
    // text line (age + title); a dim detail line follows beneath. Connects
    // consecutive dots with individual 1px segments rather than one long
    // line up front -- every dot shares the same x, so the segments compose
    // into one continuous line with no separate measuring pass needed.
    //
    // `id` scopes the feed for the same headroom reason as StatTile's --
    // nothing inside needs it today.
    //
    // Plan 2 Task 8 (controller ruling A): each row gets its own
    // InvisibleButton, sized to that row's two text lines and submitted
    // BEFORE the row's own drawlist paint (an InvisibleButton draws
    // nothing, so paint order is unaffected either way -- submitted first
    // purely so every subsequent AddText/AddLine/AddCircleFilled call below
    // stays pure overdraw, this file's usual discipline). IsItemHovered()
    // is checked immediately after each row's own button, which is the only
    // point at which "last submitted item" reliably names THAT row -- a
    // caller checking hover after TimelineFeed returns would only ever see
    // the LAST row's button. Rows never overlap (disjoint Y ranges), so at
    // most one can be hovered/clicked in a given frame.
    TimelineFeedResult TimelineFeed(const char* id, const TimelineEntry* entries, int count)
    {
        TimelineFeedResult result;
        if (ImGui::GetCurrentWindowRead()->SkipItems)
            return result;

        ImGui::PushID(id);
        if (count <= 0)
        {
            ImGui::PopID();
            return result;
        }

        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        constexpr float kDotSize   = 7.0f;    // spec §11.2: "feed dots 7px"
        constexpr float kDotRadius = kDotSize * 0.5f;
        constexpr float kTextGap   = 8.0f;    // dot column -> text column, and age -> title
        constexpr float kEntryGap  = 6.0f;    // between one entry's detail line and the next dot
        constexpr float kLineGap   = 2.0f;    // age/title line -> detail line

        const float lineX = pos.x + kDotRadius;
        const float textX = pos.x + kDotSize + kTextGap;
        const float lineHeight = ImGui::GetTextLineHeight();
        const float width = ImGui::GetContentRegionAvail().x;
        const float rowHeight = lineHeight * 2.0f + kLineGap;   // age/title line + detail line
        // Review fix (Important 3): a caller can genuinely hit exactly-zero
        // avail (a crushed dock column) -- InvisibleButton's own
        // IM_ASSERT(size_arg.x != 0.0f) would fire on `width` unfloored.
        // Floored ONLY for the button call; the closing Dummy below still
        // reserves the real (unfloored) `width` so the feed's measured
        // footprint is unaffected by this floor.
        const float buttonWidth = std::max(1.0f, width);

        float y = pos.y;
        ImVec2 prevDotCenter{};
        for (int i = 0; i < count; ++i)
        {
            // Per-entry hit target -- placed first (see the function's own
            // comment on why paint order doesn't care).
            ImGui::SetCursorScreenPos(ImVec2(pos.x, y));
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("##row", ImVec2(buttonWidth, rowHeight));
            // Optional rider (Task 8 review): ForTooltip, not a bare hover
            // check -- the panel's caller cannot re-run DrawAssetPeekTooltip's
            // own IsItemHovered(ForTooltip) after this function returns (see
            // this function's own header comment on why), so THIS is the
            // one place that delay gate can still run. Gating hoveredIndex
            // on it here, rather than downstream, is what keeps the feed's
            // tooltip on the same ~0.5s delay the panel's other three peek
            // sites get, instead of popping instantly.
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                result.hoveredIndex = i;
            if (clicked)
                result.clickedIndex = i;
            ImGui::PopID();

            const ImVec2 dotCenter(lineX, y + lineHeight * 0.5f);
            if (i > 0)
                dl->AddLine(prevDotCenter, dotCenter, ImGui::GetColorU32(Theme::kSeparator), 1.0f);

            // Line one: dim age, then normal title immediately after it.
            dl->AddText(ImVec2(textX, y), ImGui::GetColorU32(Theme::kTextDim), entries[i].age);
            const float ageWidth = ImGui::CalcTextSize(entries[i].age).x;
            dl->AddText(ImVec2(textX + ageWidth + kTextGap, y),
                       ImGui::GetColorU32(ImGuiCol_Text), entries[i].title);
            y += lineHeight + kLineGap;

            // Line two: dim detail.
            dl->AddText(ImVec2(textX, y), ImGui::GetColorU32(Theme::kTextDim), entries[i].detail);
            y += lineHeight;

            // Dot drawn last so it sits visually on top of the connecting line.
            dl->AddCircleFilled(dotCenter, kDotRadius, ImGui::GetColorU32(Theme::kGrab));

            prevDotCenter = dotCenter;
            if (i + 1 < count)
                y += kEntryGap;
        }

        // Cursor drifted through the per-row SetCursorScreenPos calls above
        // -- reset to `pos` so this closing Dummy reserves the identical
        // (width, total height) footprint the pre-Task-8 version did.
        ImGui::SetCursorScreenPos(pos);
        ImGui::Dummy(ImVec2(width, y - pos.y));
        ImGui::PopID();
        return result;
    }

    // CURVE IS MIRRORED in data/shaders/tonemap.hlsl (HLSL, branchless min
    // form), which cites THIS file -- so an edit here changes rendered output.
    // NOTHING PINS THE TWO AGAINST EACH OTHER: no test evaluates both and
    // compares. Keeping them in step is manual until one does.
    float SrgbToLinear(float srgb) noexcept
    {
        // Guard the low end FIRST: std::pow of a negative base with a
        // fractional exponent is NaN, and a negative channel is reachable
        // (an HDR-authored value edited down, a script write).
        if (srgb <= 0.0f)     return srgb;
        if (srgb <= 0.04045f) return srgb / 12.92f;
        return std::pow((srgb + 0.055f) / 1.055f, 2.4f);
    }

    float LinearToSrgb(float linear) noexcept
    {
        if (linear <= 0.0f)       return linear;
        if (linear <= 0.0031308f) return linear * 12.92f;
        return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    }

}
