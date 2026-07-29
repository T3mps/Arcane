#pragma once

// Arcane::Editor widget vocabulary -- the shared ImGui building blocks the
// editor's panels and documents draw rows out of: the two-column field grid,
// the field label cell, the axis-coloured drags and their bar, the header
// band, the std::string InputText, the Range-honouring drags, and the
// stable-buffer text commit.
//
// REFLECTION-FREE ON PURPOSE. This layer knows about Astra::Range -- a plain
// [min, max, step] value -- and nothing else from reflection. Resolving a
// FieldInfo to a Range is the Inspector's job, not a widget's, so the
// FieldInfo-taking convenience overloads live one level up. That is what lets
// non-Inspector callers (documents, tool panels) reach the same widgets.

#include <Arcane/Util/FunctionRef.hpp>

#include <Astra/Reflection/Attribute.hpp>   // Astra::Range ONLY -- see above

#include <imgui.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Arcane::Editor
{
    // std::string-backed InputText. misc/cpp/imgui_stdlib is NOT vendored, so
    // this inlines its CallbackResize pattern: ImGui tells the callback how
    // long the text is about to be, the string resizes to fit, and the
    // callback hands back the (possibly reallocated) data() pointer.
    //
    // While the widget is ACTIVE, ImGui edits its own copy of the text and
    // ignores the buffer passed in -- it re-reads that buffer only when
    // WantReloadUserBuf is set (imgui_widgets.cpp:4834-4849, restated at
    // :5417-5419). Call sites may therefore pass a per-frame local reseeded
    // from live data every frame without fighting the user's typing.
    bool InputTextString(const char* label, std::string* s, ImGuiInputTextFlags flags = 0);

    // Drags that honour an Astra::Range when the caller resolved one, and are
    // otherwise the exact call these sites made before ranges were read.
    //
    // ClampOnInput is what makes the bound real. Dragging clamps on its own,
    // but Ctrl+click text entry into the same widget is clamped ONLY under
    // this flag (imgui_widgets.cpp:2783 -> :2703-2706), so without it a typed
    // value passes the bounds untouched. Deliberately NOT AlwaysClamp, which
    // is ClampOnInput|ClampZeroRange (imgui.h:2034) and would change which
    // degenerate ranges bind -- see BindingRange in InspectorView.cpp, which
    // encodes that same binding rule for the rows ImGui does not clamp.
    [[nodiscard]] bool RangedDragFloat(const char* label, float* v, float fallbackSpeed,
                                       const std::optional<Astra::Range>& range);

    [[nodiscard]] bool RangedDragInt(const char* label, int* v,
                                     const std::optional<Astra::Range>& range);

    // One field row's label cell. Opens the row, writes the display name into
    // column 0, and leaves the cursor in column 1 with the next item sized to
    // fill it. Returns whether the LABEL is hovered -- asked here, while the
    // label still IS the last item, because a row's tail tooltip asks about
    // the VALUE widget and would never fire over the name.
    //
    // `dimmed` is UE's disabled-label treatment for a field that cannot be
    // edited.
    [[nodiscard]] bool FieldLabelCell(const std::string& label, bool dimmed);

    // Paint the axis strip (X red, Y green, Z blue) over the left edge of the
    // item just submitted. `component` indexes the palette; an index past it
    // draws nothing. An OVERLAY on purpose: it runs AFTER the widget, so it
    // pushes no style and cannot move layout.
    void DrawAxisBar(int component);

    // The component drags for a single-selection Vec2/Vec3 row, spelled out
    // rather than calling ImGui::DragFloat2/3 so each component's OWN frame
    // rect is reachable for the bar. Each component keeps the exact ImGui id
    // DragFloat2/3 gave it.
    [[nodiscard]] bool AxisDragFloatN(const char* label, float* v, int count, float speed);

    // Truncate `text` with a trailing ellipsis so it fits `maxWidth` pixels of
    // the CURRENT font; unchanged when it already fits. UTF-8-safe: a cut never
    // lands mid-codepoint. For value buttons whose label is data (a mount path,
    // a guid) -- an unsized button grows with its label and pushes its row
    // neighbours off the panel.
    [[nodiscard]] std::string EllipsisToWidth(std::string_view text, float maxWidth);

    // Two-column field region (UE's Details-panel shape: label left in one
    // column, value right, one draggable split shared by every section).
    //
    // `labelColWidth` is the CALLER'S shared width authority -- there is no
    // ImGui API to bind two tables' column widths, so every grid seeds its
    // label column from this float and adopts a user drag back into it. Pass
    // the same float to every grid that should share one split.
    //
    // Bool-convertible: ImGui::BeginTable can refuse (culled/clipped host
    // window) -- draw NO rows then, and the dtor must not End what never
    // began.
    //
    // [[nodiscard]] on the TYPE, so an unnamed temporary -- `FieldGrid(id, w);`
    // as a statement, which Begins and immediately Ends and draws nothing --
    // is a warning rather than a silently empty panel. Both live call sites
    // are named locals and are unaffected.
    struct [[nodiscard]] FieldGrid
    {
        FieldGrid(const char* id, float& labelColWidth);
        ~FieldGrid();
        explicit operator bool() const noexcept { return m_open; }
        FieldGrid(const FieldGrid&) = delete;
        FieldGrid& operator=(const FieldGrid&) = delete;
    private:
        bool m_open = false;
    };

    // UE's Details treatment for CollapsingHeader/TreeNodeEx: a muted dark
    // band in place of ImGuiCol_Header (the editor theme's selection accent).
    // Scope it TIGHT around the header call only -- the band colors must not
    // leak into tooltips/popups, which read the theme's own ImGuiCol_* set.
    //
    // [[nodiscard]] for the same reason as FieldGrid above: a temporary pushes
    // and pops the colors in one statement, styling nothing.
    struct [[nodiscard]] HeaderBand
    {
        HeaderBand();
        ~HeaderBand();
        HeaderBand(const HeaderBand&) = delete;
        HeaderBand& operator=(const HeaderBand&) = delete;
    };

    // One inline stable-buffer text edit: seeds from `current`, holds typed
    // text across frames while active (keyed by `key`, unique per edit site),
    // fires `commit(newText)` EXACTLY ONCE on deactivate-after-edit when the
    // text actually changed. Mutation happens only inside `commit`, so an
    // abandoned edit (window closed mid-typing) mutates nothing and needs no
    // undo coverage -- this is the single-shot cousin of EditGesture, not a
    // replacement for it.
    //
    // Escape REVERTS (Enter and click-away commit). ImGui restores the
    // pre-focus text into the buffer as it deactivates, and the commit test
    // reads that post-widget buffer, so an escaped edit compares equal to
    // `current` and commits nothing -- see the citation chain in the .cpp.
    //
    // CAP CAVEAT, and it bounds that revert: the buffer below is 64 bytes, so
    // a `current` of 63+ characters is seeded TRUNCATED. ImGui reverts to what
    // it was handed at focus -- the truncation -- which then differs from the
    // live value, so touching such a field commits the shortened text no
    // matter how the edit ends. Pre-existing and equally true of Enter and
    // click-away; the revert removes the typed text, not the cap.
    struct TextCommitState
    {
        std::uint64_t activeKey = 0;   // 0 = no edit in flight
        char          buf[64]   = {};
    };

    bool StableTextEdit(const char* imguiLabel, TextCommitState& st, std::uint64_t key,
                        std::string_view current, float width,
                        Arcane::FunctionRef<void(const char*)> commit);
}
