#include "EditorWidgets.hpp"

#include <imgui.h>
#include <imgui_internal.h>   // ImGuiTable + ImGuiTableColumn + TableSetColumnWidth
                              // (the field grid's shared split reads
                              // LastResizedColumn/WidthRequest/WidthGiven, none of
                              // which are public), ImTrunc, GetCurrentWindowRead

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
}
