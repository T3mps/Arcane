#pragma once

// Arcane::Editor widget vocabulary -- the shared ImGui building blocks the
// editor's panels and documents draw rows out of: the two-column field grid,
// the field label cell, the axis-coloured drags and their bar, the header
// band, the std::string InputText, the Range-honouring drags, the
// stable-buffer text commit, and the colour field.
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
    //
    // `ellipsis` is the marker appended to a cut string, and it DEFAULTS TO
    // THREE ASCII DOTS on purpose: that is what every row this function already
    // draws has shipped with, and the golden editor-ui lane renders one of them
    // (the Assets panel's Browse lens). Changing the default would be a visual
    // change to the Browse rows, the preview pane, the Status cards and the
    // Inspector all at once -- so the Graph lens, which wants the real U+2026,
    // passes it rather than moving everyone.
    [[nodiscard]] std::string EllipsisToWidth(std::string_view text, float maxWidth,
                                              std::string_view ellipsis = "...");

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

    // ---- asset panel vocabulary -------------------------------------------
    // Model-free ImGui draw helpers the asset panel's Browse lens and rail
    // draw rows out of (docs/specs/2026-09-06-asset-manager-redesign-design.md,
    // §11.1/§11.2). `AssetPeekTooltip` is deliberately NOT here -- it composes
    // the model plus a thumbnail resolver, so it lives with the panel (Task 9).

    // Pill line height (spec §11.2's pinned 16px). Exposed -- rather than kept
    // file-local to EditorWidgets.cpp, as it was until Plan 3 -- for the same
    // reason kAssetRowThumbSize below is: a caller that positions a pill BY
    // HAND (AssetsPanel.cpp's status cards vertically centre one inside a row
    // rect instead of chaining SameLine) needs the number, and re-declaring it
    // there made two constants nothing kept in step.
    inline constexpr float kPillLineHeight = 16.0f;

    // 12px bordered label (spec §11.2). variant: 0 = neutral (#333333 border,
    // TextDisabled-ish #9a9a9a text), 1 = amber (border #7a5a20, text
    // Theme::kAmber). kPillLineHeight line height; chain several with SameLine.
    void AssetPill(const char* text, int variant = 0);

    // Right-most segmented switch (spec §11.1/§11.2, e.g. the Browse/Graph/
    // Status lens strip). `items` are labels; `enabledMask` bit i gates item
    // i (a cleared bit -> BeginDisabled); returns the clicked index or -1.
    // Drawn with collapsed shared 1px borders and square corners, active =
    // Theme::kButtonActive.
    [[nodiscard]] int SegmentedStrip(const char* id, const char* const* items,
                                     int count, int active, unsigned enabledMask);

    // Row thumb cell size (spec §11.2: "row thumb ... 18px"). Exposed
    // (rather than kept file-local to EditorWidgets.cpp) so a caller that
    // needs to compute a position against RowWithThumb's own thumb rect --
    // e.g. a status badge overlaid on a corner of it -- can do so without
    // re-guessing the value; AssetsPanel.cpp's refused-marker badge (Task
    // 10 fix round 1) is the first such consumer.
    inline constexpr float kAssetRowThumbSize = 18.0f;

    // One selectable asset row (spec §11.1/§11.2): an 18px thumb (`thumb`
    // == 0 falls back to the `iconUtf8` Lucide glyph), then `name`, then
    // `result.trailingPos` names where the caller's own trailing content
    // (pills, right-aligned extras) should START. `indent` shifts where the
    // thumb and name start; the row's own Selectable still spans the full
    // width, so the row stays clickable everywhere regardless of indent.
    // `rowHeight` defaults to the 24px table row (spec §11.2); rail rows --
    // drawn with this SAME helper per §11.1 -- pass 26. (The brief's doc
    // fixed this at 24px, which cannot serve both rows; controller ruling,
    // 2026-09-06, makes it a parameter instead, defaulted to 24 so table
    // call sites stay unchanged.)
    //
    // TASK 10 FIX ROUND 1 (review Critical 1): the thumb and name are pure
    // ImDrawList overdraw now, NOT ImGui::Image/TextUnformatted items --
    // the row's Selectable is therefore the ONE real item this function
    // submits, and stays ImGui's "last submitted item" the instant this
    // call returns. That is what lets a caller hang
    // BeginDragDropSource()/BeginPopupContextItem()/
    // IsItemHovered(ImGuiHoveredFlags_ForTooltip) directly off the return
    // of this call with no separate anchor widget -- the previous design's
    // full-row InvisibleButton "hit anchor" is deleted along with the bug
    // it had (an AllowOverlap item is hoverable only when
    // g.HoveredIdPreviousFrame already names it, imgui.cpp:5112-5118 --  a
    // same-size overlay submitted every frame starves it permanently).
    //
    // The Selectable IS still submitted with SetNextItemAllowOverlap() (see
    // the .cpp), but for the mechanism's actual documented purpose this
    // time: permitting a SMALL foreground item a caller submits AFTER this
    // call to remain clickable despite sitting inside the row's rect (an
    // expander chevron, the rail's hover "+") -- imgui.h's own doc comment
    // for SetNextItemAllowOverlap names exactly this pattern ("Typically
    // useful with InvisibleButton(), Selectable(), TreeNode() covering an
    // area where subsequent items may need to be added"). A caller that
    // never adds such a foreground item pays nothing: the row just settles
    // to hovered=true after, at most, one frame of the mouse entering it.
    // `trailingPos` bypasses the fragile alternative of a bare SameLine()
    // trying to re-derive a text-item's line metrics off a Selectable that
    // never carried them in the first place -- call
    // ImGui::SetCursorScreenPos(result.trailingPos) for the FIRST trailing
    // widget only; ordinary SameLine() chaining resumes correctly after
    // that (a real item -- e.g. AssetPill's own Dummy -- reseeds normal
    // line-tracking for anything chained after it).
    struct [[nodiscard]] AssetRowResult
    {
        bool clicked = false;
        bool hovered = false;
        ImVec2 trailingPos{};
    };
    AssetRowResult RowWithThumb(const char* id, ImTextureID thumb, const char* iconUtf8,
                                const char* name, bool selected, float indent,
                                float rowHeight = 24.0f);

    // ---- status lens vocabulary (Plan 2) -----------------------------------
    // Model-free ImGui draw helpers the Status lens dashboard draws out of
    // (docs/specs/2026-09-06-asset-manager-redesign-design.md §9.2/§11.1/
    // §11.2). Same house idioms as the asset panel vocabulary above: SkipItems
    // guard, ImDrawList overdraw, one real item reserves layout, PushFont for
    // sizes -- AssetPill (:578) is the model for StatTile/MeterBar/
    // TimelineFeed, which are pure display; RowWithThumb (:692) -- which also
    // hosts caller content needing its own id scope -- is the model for
    // BeginCardFrame/EndCardFrame.

    // Bordered card: PushFont'd 24px number, 13px label beneath, optional
    // leading Lucide icon drawn at the ambient UI size (Lucide glyphs are
    // merged into every editor face, EditorFonts.hpp). variant: 0 = neutral;
    // 1 = amber ICON ONLY -- the number always stays in text tokens (spec
    // §11.2's amber rule marks the refused tile's icon, never the count).
    void StatTile(const char* id, const char* number, const char* label,
                  const char* iconUtf8, int variant, const ImVec2& size);

    // One caller-colored segment of a MeterBar. `color` is resolved by the
    // caller (a Theme:: token or otherwise) -- MeterBar draws exactly what it
    // is given and invents no palette of its own.
    struct MeterSegment { const char* label; int count; ImU32 color; };

    // Stacked horizontal bar (spec §11.2: 10-12px tall) over a Theme::kWell
    // track, with segments sized proportional to `count` and 2px gaps between
    // them that let the track show through ("surface gaps"). One legend row
    // beneath: swatch + "label count" per segment, chained left to right. A
    // zero-count segment draws no bar slice but keeps its legend entry (a
    // healthy project still shows e.g. "refused 0" rather than the label
    // vanishing).
    void MeterBar(const char* id, const MeterSegment* segments, int count, float width);

    // Bordered, dynamic-height card region for arbitrary caller content (spec
    // §9.2's Needs-attention / queued / Unreferenced / Scenes cards). `width`
    // <= 0 uses the content region's available width; height is whatever the
    // caller draws between Begin/End, measured via BeginGroup/EndGroup and
    // painted AFTER the fact through an ImDrawListSplitter (2 channels --
    // content on 1 while the caller draws, background fill + 1px border on 0
    // sized from the measured group rect, merged on End) so a dynamic-height
    // card gets a background with no separate pre-measure pass. variant 1
    // borrows the SAME muted-amber acting-on frame AssetPill's amber pill
    // border uses (kPillAmberBorder, EditorWidgets.cpp -- one spec-pinned
    // hex, two consumers); variant 0 is a plain Theme::kSeparator border. 8px
    // inner padding, square corners.
    //
    // The false-return contract matches FieldGrid's, NOT ImGui::Begin's: a
    // false return means the host window is skipping items -- draw NO content
    // and do not call EndCardFrame (nothing was pushed for it to close).
    bool BeginCardFrame(const char* id, int variant = 0, float width = 0.0f);
    void EndCardFrame();

    // One activity-feed row's text: a pre-formatted age ("N min ago" etc.,
    // dim), a title (normal), and a second dim detail line. Plain `const
    // char*` views -- the caller owns the storage for the duration of the
    // call (Task 8's feed builds frame-local std::strings first).
    struct TimelineEntry { const char* age; const char* title; const char* detail; };

    // Plan 2 Task 8 (controller ruling A, sole caller: the Assets panel's
    // activity feed): which row (if any) is hovered/clicked THIS frame, -1
    // for neither. Computed INSIDE TimelineFeed's own per-row loop -- by the
    // time a caller could otherwise react to a hover, ImGui's "last
    // submitted item" is whichever row was drawn LAST, not necessarily the
    // hovered one, so the widget must answer this itself.
    struct TimelineFeedResult { int hoveredIndex = -1; int clickedIndex = -1; };

    // Vertical timeline: a 1px Theme::kSeparator line connecting a 7px
    // Theme::kGrab dot per entry (spec §11.2: "feed dots 7px"), each dot
    // vertically centered on its entry's first text line. Per entry: dim age
    // then normal title on line one, dim detail on line two beneath. Every
    // pixel drawn is unchanged from the pre-Task-8 version -- the only
    // addition is one InvisibleButton per row (submitted before that row's
    // own drawlist paint, so the paint stays pure overdraw) giving each
    // entry its own hover/click hit target; see TimelineFeedResult.
    TimelineFeedResult TimelineFeed(const char* id, const TimelineEntry* entries, int count);

    // ---- colour ---------------------------------------------------------------
    // sRGB <-> linear, the IEC 61966-2-1 piecewise curve. This is the SAME
    // transfer nri::Format::RGBA8_SRGB applies in hardware when a texture is
    // sampled (NriTextureCache.cpp:183), and that is the whole point: before this, a tint
    // and a texture pixel authored as the same number meant DIFFERENT colours in
    // the same multiply (#808080 -> linear 0.216 as a pixel, 0.502 as a tint).
    //
    // Values outside [0,1] pass through monotonically, so an hdr path cannot clamp
    // anything.
    //
    // CURVE IS MIRRORED in data/shaders/tonemap.hlsl (HLSL, branchless min
    // form) -- keep both in step by hand; no test compares them. See the .cpp.
    [[nodiscard]] float SrgbToLinear(float srgb) noexcept;
    [[nodiscard]] float LinearToSrgb(float linear) noexcept;
}
