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
// commit). TextRow selects all its text on activation (single-line rows).
// An optional `validate` returns a refusal reason: a refused value draws red
// (RefusedFieldStyle) with the reason as a hover tooltip; Enter on a refused value keeps the text and re-arms the box; focus
// loss with a refused value reverts without committing. Mirrors the Input
// Actions rename box.
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
        // One in-flight numeric gesture per IntRow/FloatRow (drag, held step
        // button, Ctrl+click text). The ROW owns the number while the widget
        // is active (UE SSpinBox InternalValue): pages re-derive their locals
        // from the draft every frame, and ImGui's drag accumulator is consumed
        // by what it applied last frame, so without this the gesture restarts
        // every frame and the release commits the untouched original. `seed`
        // is the value at activation; a commit is reported only when the
        // released value differs from it (UE: LastSliderCommittedValue != New).
        struct NumericDraft { double value = 0.0; double seed = 0.0; bool active = false; };
        std::unordered_map<unsigned int, NumericDraft> numericDrafts;   // keyed by ImGui id
        // TEST SEAM (PropertyGridTest): when non-null every row records the
        // centre of its VALUE widget under its label. Production: nullptr.
        std::unordered_map<std::string, ImVec2>* probe = nullptr;
    };

    class PropertyGrid
    {
    public:
        explicit PropertyGrid(PropertyGridState& state) : m_state(state) {}

        // Full-width headers -- draw these OUTSIDE a Rows scope.
        [[nodiscard]] bool Section(const char* label, bool defaultOpen = true);
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
        bool IntRow(const char* label, int& value);                          // true once per gesture, on deactivate-after-edit AND value != seed; value follows the gesture every frame
        bool FloatRow(const char* label, float& value, float speed = 0.01f); // same rule; Escape mid-drag = cancel, no commit
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

        PropertyGridState& State() noexcept { return m_state; }

    private:
        void Probe(const char* label);
        PropertyGridState& m_state;
    };
}
