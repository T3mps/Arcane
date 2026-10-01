#pragma once

// PropertyGrid (inspector-ownership spec s3.2 / input-editor spec s3.1): the
// section header, the two-column row region and the row primitives the scene
// Inspector draws components with, extracted so every Inspector PAGE (a
// document's map/action/binding pages, later the asset page) uses the same
// rows, wells, labels and bands. Built on the reflection-free widget layer
// (EditorWidgets.hpp: FieldGrid / FieldLabelCell / HeaderBand /
// InputTextString); knows nothing about Astra reflection.
//
// Row primitives return "an edit was COMMITTED this frame" (deactivate-after-
// edit for text/number rows, a click for checkbox/combo/button rows) so a
// page can route the write through its own undoable command. Drag/typing in
// flight commits nothing. While a drag or typed edit is in flight the row
// keeps its own draft and writes the in-flight number back into `value`
// every frame (a page may preview it) but commits nothing; on commit `value`
// holds the gesture's final number. Escape during a numeric drag cancels (no
// commit; LastRowEvents().cancelled reports it). TextRow selects all its
// text on activation (single-line rows).
// An optional `validate` returns a refusal reason: a refused value draws red
// (RefusedFieldStyle) with the reason as a hover tooltip; Enter on a refused value keeps the text and re-arms the box; focus
// loss with a refused value reverts without committing. Mirrors the Input
// Actions rename box.
//
// ROWS STAY UNDO-AGNOSTIC. THE EDITGESTURE-AFTER-ROW CONTRACT (node-page spec
// s4.1(f)) is how a page brackets them:
//   1. ACTIVATION. After IntRow / FloatRow / SliderRow / VecRow / ColorRow the
//      VALUE widget is g.LastItemData -- decorations submit BEFORE it
//      (SetNextRowDecor) -- so EditGesture::BeginOnActivate(stack, st, label,
//      onOpened) called right after the row fires on the activation frame,
//      grouped rows included (EndGroup forwards the active id,
//      imgui.cpp:12477-12482). ColorRow's popup is bracketed separately:
//      BeginOnPopupOpen / EndOnPopupClose on *popupIdOut.
//   2. LIVE WRITE-THROUGH. A live-preview page writes `value` to its live
//      target whenever the two differ -- including the seed Escape restored.
//   3. CLOSE. EditGesture::EndAfterRow(stack, st, grid.LastRowEvents().cancelled).
//      A grouped row's Escape is invisible to IsItemDeactivated, so
//      EndOnDeactivate alone leaves the gesture parked until a ScopeGuard;
//      EndAfterRow closes it at the row, before any later row can activate.
//      Required for an adopter without a ScopeGuard.
// After an Escape the close commits an UNCHANGED value: the page's
// before == after guard (ShaderEditorDocument.cpp:5954-5957) or CommandStack's
// unchanged-snapshot drop pushes nothing.
//
// A row's ImGui id must include the TARGET's id -- the caller pushes it
// around the Rows scope (Task 10 does; the scene body's component rows
// already sit under the entity's id); the label alone is not an identity.

#include "Widgets/EditorWidgets.hpp"   // FieldGrid (Rows holds one)
#include <imgui.h>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Arcane::Editor
{
    // Persistent, per-panel-instance: the shared label split (UE's one split
    // per Details panel) and the in-flight TextRow / numeric drafts. Owned by
    // whoever owns the window (InspectorState::grid for the scene panel; one
    // per Inspector instance in InspectorWindowsState).
    struct PropertyGridState
    {
        float labelColWidth = 0.0f;   // 0 = not seeded yet; the first grid seeds it
        // One in-flight TextRow edit. A draft outlives the row that made it:
        // `commit` is bound to the TARGET the row was drawn for (UE commits
        // through the property handle the widget was built with), so an edit
        // deactivated while the row was not drawn can still land (CommitOrphans).
        struct TextDraft
        {
            std::string text;
            std::string seed;                             // value at activation
            bool active = false;                          // active on its last draw
            int lastFrame = 0;                            // ImGui frame it was last drawn
            std::function<void(std::string)> commit;      // bound to the row's target
            // Enter on a value `validate` refused: keep `text` (no re-seed) and
            // re-arm focus once (`focusPending`, the rename box's one-shot).
            // The hold expires when the row is not drawn, when focus returns,
            // or 3 frames after `holdFrame` if focus never came back.
            bool hold = false;
            int holdFrame = 0;                            // ImGui frame the hold began
            bool focusPending = false;                    // SetKeyboardFocusHere on the next draw
        };
        std::unordered_map<unsigned int, TextDraft> textDrafts;   // keyed by ImGui id
        // One in-flight numeric gesture per IntRow/FloatRow/VecRow (drag, held step
        // button, Ctrl+click text). The ROW owns the number while the widget
        // is active (UE SSpinBox InternalValue): pages re-derive their locals
        // from the draft every frame, and ImGui's drag accumulator is consumed
        // by what it applied last frame, so without this the gesture restarts
        // every frame and the release commits the untouched original. `seed`
        // is the value at activation; a commit is reported only when the
        // released value differs from it (UE: LastSliderCommittedValue != New).
        // node-page s4.1(a): one draft for 1-4 components (IntRow/FloatRow/
        // SliderRow = 1, VecRow = n, ColorRow's boxes = 4). Commit rule: the
        // widget deactivated after an edit AND at least one component differs
        // from its seed. Key unchanged: GetID("##value") under PushID(label).
        struct NumericDraft { double value[4]{}; double seed[4]{}; int count = 1; bool active = false; };
        std::unordered_map<unsigned int, NumericDraft> numericDrafts;   // keyed by ImGui id
        // Section(label, open, trailing): each trailing control's measured width
        // (last frame), keyed by the header's id -- right-aligns it on the band.
        std::unordered_map<unsigned int, float> trailingWidths;
        // ColorRow (node-page s4.1(c)): the popup's Old swatch, latched at open
        // (one slot -- one colour popup at a time), and the popup id live last
        // frame, so the close frame can commit once.
        float colorOriginal[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
        ImGuiID colorPopupLive = 0;
        // TEST SEAM (PropertyGridTest): when non-null every row records the
        // centre of its VALUE widget under its label. Production: nullptr.
        std::unordered_map<std::string, ImVec2>* probe = nullptr;
    };

    // What the LAST row reported this frame (node-page s4.1(d)); every row
    // resets it. `cancelled` = Escape mid-drag on a numeric/vec/slider/colour
    // row: the seed was restored and ActiveId cleared, and the row returned false.
    struct RowEvents { bool overrideToggled = false; bool resetClicked = false; bool cancelled = false; };

    // One-shot decoration for the NEXT row (ImGui SetNextItem* style;
    // node-page s4.1(d)). `overridden` and `reset` are mutually exclusive
    // (IM_ASSERT): on an instance the checkbox is the only override control.
    //  - overridden: the label cell draws Checkbox("##override") then the
    //    ellipsized label; while *overridden == false the value sits inside
    //    BeginDisabled (inherited rows read dimmed + read-only). A toggle writes
    //    *overridden and raises RowEvents::overrideToggled -- the PAGE routes the
    //    undo step (as ShaderEditorDocument.cpp:5802-5827 does today).
    //  - reset: ICON_LC_ROTATE_CCW "##reset" ("Reset to default"), right-aligned
    //    in the value cell, drawn only when resetActive; otherwise the slot is
    //    reserved but empty so values stay aligned. A click raises resetClicked.
    // SUBMISSION ORDER (binding, R3): every decoration is submitted BEFORE the
    // value widget -- the reset button is placed at the cell's right edge, the
    // cursor returns to the cell start and the value is sized
    // -(resetW + ItemSpacing.x) -- so the VALUE widget is always LastItemData
    // when the row returns. Tab visiting reset before the value is accepted.
    // Honoured by Checkbox/Int/Float/Slider/Vec/Color/Combo rows; Text,
    // ReadOnly, Button and Meter rows take none (IM_ASSERT).
    struct RowDecor
    {
        bool* overridden = nullptr;   // instance override cell (UE shape)
        bool  reset = false;          // base/default reset slot
        bool  resetActive = false;    // value differs from its default: button drawn; else the slot is empty
    };

    class PropertyGrid
    {
    public:
        explicit PropertyGrid(PropertyGridState& state) : m_state(state) {}

        // Full-width headers -- draw these OUTSIDE a Rows scope.
        [[nodiscard]] bool Section(const char* label, bool defaultOpen = true);
        // A header with a control on its band (UE's header-row widgets): `trailing`
        // is drawn right-aligned on the header's line, under PushID(label); the
        // header takes ImGuiTreeNodeFlags_AllowOverlap so clicks reach the control.
        [[nodiscard]] bool Section(const char* label, bool defaultOpen, const std::function<void()>& trailing);
        // Tree-style sub-header (the scene Inspector's category band). When it
        // returns true the caller draws its content and calls EndSubSection().
        [[nodiscard]] bool SubSection(std::string_view label, bool defaultOpen = true);
        void EndSubSection();

        // The two-column region rows go into. Bool-convertible like FieldGrid:
        // false = the host window is culled; submit NO rows.
        struct [[nodiscard]] Rows
        {
            Rows(PropertyGrid& grid, const char* id);
            ~Rows();
            explicit operator bool() const noexcept { return static_cast<bool>(m_grid); }
            Rows(const Rows&) = delete;
            Rows& operator=(const Rows&) = delete;
        private:
            FieldGrid m_grid;
        };

        bool TextRow(const char* label, std::string_view current,
                     std::function<void(std::string)> commit, bool dimmed = false,
                     std::function<std::optional<std::string>(std::string_view)> validate = {});   // commit is STORED in the draft (see TextDraft)
        // `validate` (called synchronously, never stored) returns a refusal
        // reason: a refused value draws red (outline + text) with the reason as
        // a hover tooltip, typing or held; Enter on a refused value
        // keeps the text and re-arms the box; focus loss with a refused value
        // reverts without committing. Mirrors the Input Actions rename box.
        // CommitOrphans flushes an orphaned ACTIVE draft through `commit`
        // WITHOUT `validate`: a commit that does not re-validate can land a
        // refused value that way.
        bool CheckboxRow(const char* label, bool& value);
        // Numeric rows: true once per gesture, on deactivate-after-edit AND
        // value != seed; value follows the gesture every frame; Escape
        // mid-drag = cancel (LastRowEvents().cancelled), no commit.
        // IntRow: no range = InputInt with step buttons (the input page's
        // Priority row); a range = DragInt + ClampOnInput.
        bool IntRow(const char* label, int& value,
                    const std::optional<Astra::Range>& range = std::nullopt, const char* format = "%d");
        // FloatRow: a range routes through RangedDragFloat (DragSpeedFor + ClampOnInput).
        bool FloatRow(const char* label, float& value, float speed = 0.01f,
                      const std::optional<Astra::Range>& range = std::nullopt, const char* format = "%.2f");
        // SliderRow (drafting pick, 9.28): SliderFloat(min, max, format), the
        // widget material Float params use today; no clamp flags.
        bool SliderRow(const char* label, float& value, float min, float max, const char* format = "%.3f");
        // 2-4 float components through AxisDragFloatN (axis bars, per-component
        // ids). Same draft/commit/Escape rules as FloatRow, across all n.
        bool VecRow(const char* label, float* v, int n, float speed = 0.01f,
                    const std::optional<Astra::Range>& range = std::nullopt, const char* format = "%.3f");
        // FieldLabelCell + a 4-channel draft + ColorValue("##value"). The boxes
        // behave like VecRow; the popup writes through every frame and commits
        // on the frame it closes if the value differs from colorOriginal.
        // *popupIdOut feeds the caller's EditGesture popup pair; `hdr` reaches
        // ColorPopupBody (T3's ConstColor) and lifts the boxes' 0..1 clamp.
        bool ColorRow(const char* label, float linear[4], ImGuiID* popupIdOut = nullptr, bool hdr = false);
        int  ComboRow(const char* label, const char* const* items, int count, int current);
        void ReadOnlyRow(const char* label, std::string_view text);
        int  ButtonRow(const char* label, const char* const* buttons, int count,
                       unsigned enabledMask = ~0u);
        void MeterRow(const char* label, float fraction01, const char* overlay);
        // Flush drafts whose box was deactivated while its row was not drawn
        // (window hidden/closed, section collapsed, selection moved): commit
        // the text if it changed, then drop the draft. The row's `validate` is
        // NOT consulted (it is never stored): the commit must re-validate if a
        // refused value must never land. Call ONCE per frame per
        // state BEFORE any window that draws this state Begins.
        void CommitOrphans();

        void SetNextRowDecor(const RowDecor& decor) { m_decor = decor; m_hasDecor = true; }
        [[nodiscard]] RowEvents LastRowEvents() const { return m_events; }
        // TEST SEAM, public for model-aware wrappers (s4.2's AssetRow): records
        // the LAST item's centre under `label` when PropertyGridState::probe is
        // set. No-op in production.
        void ProbeItem(const char* label);
        // A custom value widget on a decorated row (s5.3): opens the label /
        // override / reset cell under PushID(label) exactly as the built-in rows
        // do (honours SetNextRowDecor, resets LastRowEvents). Draw ONE value
        // widget, then EndCustomRow probes it and pops.
        void BeginCustomRow(const char* label, bool dimmed) { BeginValueCell(label, dimmed); }
        void EndCustomRow(const char* label) { EndValueCell(label); }

        PropertyGridState& State() noexcept { return m_state; }

    private:
        // Value rows: label cell (+ the pending RowDecor: override checkbox,
        // reset slot, inherited BeginDisabled) + PushID(label) / ProbeItem +
        // PopID. Plain rows (Text/ReadOnly/Button/Meter) reset the events and
        // IM_ASSERT that no decoration is pending.
        void BeginValueCell(const char* label, bool dimmed);
        void EndValueCell(const char* label);
        void BeginPlainRow();
        PropertyGridState& m_state;
        RowEvents m_events{};
        RowDecor m_decor{};
        bool m_hasDecor = false;
        bool m_valueDisabled = false;   // BeginValueCell opened a BeginDisabled for an inherited row
    };
}
