#pragma once

// Arcane::Editor::EditGesture -- the ONE gesture bracket for cross-frame edit
// gestures (drags, text entries) against the shared CommandStack. Owns the two
// invariants every hand-rolled copy kept breaking (the 2026-07-26 audit's
// CRITICAL 1 class): only the widget that OPENED a gesture may close it, and
// every gesture has a guaranteed close path (abandonment COMMITS -- an edit
// already applied live must land on the undo stack, never strand).
//
// PURE CORE (Slots + the free functions below): decision logic over ImGui
// facts passed in as plain integers/bools -- no imgui.h include, so the
// [editor] units drive the full decision table headlessly (EditGestureTest).
// The ImGui-facing skin (below) landed in Task 2 and stays thin.

#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Util/FunctionRef.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace Arcane::Editor::EditGesture
{
    // The cross-frame slots. ImGui item ids are 32-bit (ImGuiID = unsigned
    // int); the skin's TU static_asserts that so this header never needs
    // imgui.h.
    //
    // `item` is meaningful only while a gesture THIS consumer opened is live.
    // It is NOT an "is one open" flag and must not be read as one: a gesture
    // that JOINED a live drag parks a real item id alongside txn == None, so a
    // non-zero id does not imply an open transaction of ours. `txn` is the
    // authority on that.
    struct Slots
    {
        Arcane::TransactionId txn  = Arcane::TransactionId::None;
        std::uint32_t         item = 0;   // id of the widget that OPENED txn
        // TRUE when the live gesture was opened by BeginOnPopupOpen, in which
        // case `item` is a POPUP id rather than a widget id. It changes who
        // judges abandonment: a widget gesture's owner is whoever holds
        // ActiveId, but a popup's edits are made by FOREIGN widgets we do not
        // submit, so ActiveId inside the popup is never `item` and the widget
        // rule would read as abandonment on every frame. Cleared with the rest
        // of the slots by ClosePending.
        bool                  popup = false;
    };

    enum class EndAction { None, Commit, Cancel };

    // EndOnDeactivate's verdict. Ownership guard FIRST: a row that never
    // opened the gesture still reports its own deactivation on one-frame
    // ActiveId handoffs, and without the guard its pure click would Cancel
    // the owner's live transaction -- Cancel discards WITHOUT reverting
    // (CommandStack.cpp:75-82), so the owner's whole edit would stay applied
    // and permanently un-undoable. The concrete shape: click into a string
    // box, then press on a drag drawn ABOVE it, and in ONE frame the drag
    // activates (parking its LIVE token in the slots) while the string box,
    // submitted after it, reports IsItemDeactivated -- imgui.cpp:6559 answers
    // from DeactivatedItemData.ID, which is the string box's own id.
    // A joined gesture (txn == None, item parked) still evaluates: Commit/
    // Cancel on None no-op at the stack, and the slots must clear either way.
    [[nodiscard]] EndAction EvaluateEnd(const Slots& s, std::uint32_t lastItemId,
                                        bool deactivatedAfterEdit,
                                        bool deactivated) noexcept;

    // Abandonment test for ScopeGuard: a parked gesture whose owner no longer
    // holds ActiveId (widget vanished / window collapsed mid-gesture).
    // `hasPendingCommit` covers the builder-style JOINER edge: txn == None
    // (joined a gizmo drag) but a built command is still owed to the stack.
    //
    // Why the test exists at all: a gesture closes on the OWNING WIDGET's own
    // deactivation, which can only be observed while that widget is still
    // being SUBMITTED. Anything that stops it being submitted mid-gesture
    // strands the transaction open -- its component header (or a category
    // sub-header) collapsing, a search query hiding the field, the selection
    // turning multi (which swaps a drag for text boxes), the host window
    // collapsing or its tab going to the background. That is not cosmetic:
    // CommandStack::InTransaction() gates BOTH the Inspector's structural
    // affordances (CanEditStructure = `editMode && !InTransaction()`,
    // EditorPanels.cpp) and the Ctrl+Z / Ctrl+Y keybinds (EditorAppFrame.cpp),
    // so ONE stranded transaction disables Add/Remove Component and undo
    // editor-wide until some LATER gesture force-closes the orphan and
    // silently absorbs its stale snapshots into an unrelated undo step.
    // ActiveId having moved on is what tells the guard the owner is gone.
    //
    // The collapse case is reachable, not theoretical: a ctrl+click text entry
    // on a DragFloat leaves g.ActiveIdAllowOverlap = !io.MouseDown[0]
    // (imgui_widgets.cpp:5004) and nothing resets it, so with the mouse up the
    // header above is still hoverable -- that flag is exactly what the hover
    // gate tests (imgui.cpp:5091) -- and a click over its arrow presses on
    // MouseDown (imgui_widgets.cpp:7012) and flips is_open in the SAME frame
    // (:7045/:7075), before the field would have been drawn. A mouse-HELD drag
    // is NOT reachable this way: it never sets ActiveIdAllowOverlap, so the
    // same gate rejects every other item.
    //
    // The POPUP arm: when `s.popup` is set, `item` is a popup id and ActiveId is
    // meaningless as an ownership signal, so abandonment is `!popupOpen` -- the
    // same test ShouldClosePopup applies. The guard is still a backstop and not
    // dead weight: if the host panel stops being drawn (document closed, tab
    // backgrounded), EndOnPopupClose stops being called too, and this is the
    // only thing left that can close the gesture.
    [[nodiscard]] bool ShouldCloseAbandoned(const Slots& s, std::uint32_t activeId,
                                            bool hasPendingCommit,
                                            bool popupOpen) noexcept;

    // Activation-time stale check: a still-parked gesture means the previous
    // owner never got to close (click-through to a widget drawn ABOVE it in
    // submission order); close-and-commit it before opening ours.
    //
    // Why closing first is MANDATORY rather than tidy: clicking widget B while
    // widget A is mid-gesture moves ActiveId in ONE frame, and if B is drawn
    // ABOVE A then B activates before A's end-of-gesture call runs. Without
    // this close, B's Begin would see A's transaction still open and return
    // None (CommandStack.cpp:16-17), and A's later Commit(None) would no-op --
    // leaving A's transaction open forever, holding stale `before` bytes.
    // A's own end call still runs later that same frame, but the slots now
    // name B as the owner, so EvaluateEnd's ownership guard makes it return:
    // A does NOT hand B's LIVE token to Commit/Cancel.
    [[nodiscard]] bool ShouldCloseStaleOnActivate(const Slots& s,
                                                  bool hasPendingCommit) noexcept;

    // Popup-lifetime close test -- the FOURTH edit boundary. `open` is whether the
    // popup that owns the parked gesture is still open this frame; `popupId` is
    // the asking site's own popup id.
    //
    // Why a popup needs its own shape rather than reusing the activation pair: a
    // popup's edits are made by FOREIGN widgets we do not submit, so there is no
    // item of ours to observe activating or deactivating. Worse, the reason the
    // activation pair appears to work for ImGui's OWN colour popup is a loan --
    // ColorEdit4 does `g.LastItemData.ID = g.ActiveId` while its picker window is
    // active (imgui_widgets.cpp, "so IsItemActive() will function on
    // ColorEdit4()"), and that only happens when ImGui opened the popup itself. A
    // hand-rolled popup gets no such loan, so IsItemActivated() never fires and
    // the edits would land outside undo entirely.
    //
    // There is no Cancel counterpart on purpose: for a popup, closed IS the end,
    // and the layer's invariant is that abandonment COMMITS -- the edits were
    // applied live and the user watched them happen. Escape and click-away
    // therefore keep the edit and leave exactly one step; Ctrl+Z is the way back.
    [[nodiscard]] bool ShouldClosePopup(const Slots& s, std::uint32_t popupId,
                                        bool open, bool hasPendingCommit) noexcept;

    // ---- ImGui-facing skin (thin; NOT unit-driven -- the pure core above is) ----

    struct GestureState
    {
        Slots slots;
        // Builder-style adopters park the command build here at open; it runs
        // EXACTLY ONCE, at close (release-commit, stale-close, or abandoned
        // close), then clears. Empty for snapshot-style gestures -- their
        // before-state rides the open transaction itself (SnapshotComponent).
        std::function<void()> pendingCommit;
        // A string row's cancel reference, LATCHED AT ACTIVATION rather than
        // rebuilt per frame. ImGui's Escape restores the text it captured when
        // the box was activated (TextToRevertTo, imgui_widgets.cpp:4865-4866,
        // written back at :5300-5308), so the commit guard has to compare
        // against that same activation-time text. A per-frame seed read from
        // the live value diverges the moment anything changes it externally
        // mid-edit (an Outliner rename committing a frame after the Inspector
        // box was activated on the same entity) -- Escape then restored the
        // OLD text, the guard saw a difference, and the documented CANCEL path
        // wrote an undo entry. Latched, the seed equals TextToRevertTo by
        // construction, so an Escape revert is always an equality no-op.
        //
        // It is ONE slot per consumer, so `stringSeedItem` names the row that
        // latched it: two string rows can hand ActiveId over inside ONE frame
        // (click into A, then click B drawn ABOVE it -- B activates and
        // re-latches before A, submitted later, reports its deactivation),
        // which would have A committing against B's seed. Same ownership-guard
        // shape as Slots::item above, and unrelated to the undo stack: these
        // two carry ImGui's cancel semantics, so they stay wired (and are NOT
        // cleared by a gesture close) even where `stack` is null.
        std::string   stringSeed;
        std::uint32_t stringSeedItem = 0;
    };

    // Call IMMEDIATELY after submitting a widget. On that widget's activation:
    // close-and-commit any stale parked gesture, Begin(label()), park the
    // owner id, then park onOpened()'s returned command build. Both callbacks
    // run ONLY on the activation frame (zero cost otherwise). Snapshot-style
    // adopters do their SnapshotComponent fan-out INSIDE onOpened -- it runs
    // AFTER Begin, so the snapshots land in the open transaction -- and
    // return an empty std::function. Null stack = Play mode: whole bracket
    // no-ops (the Inspector's existing rule, now everyone's).
    //
    // Begin returning None means another consumer (a live gizmo drag) already
    // owns the stack -- this gesture then JOINS it: an onOpened fan-out of
    // SnapshotComponent calls still lands in that open transaction, and the
    // drag's own Commit records them, so the edit is never lost.
    //
    // The id parked in Slots::item IS ActiveId: IsItemActivated() is true only
    // when g.ActiveId == g.LastItemData.ID (imgui.cpp:6545-6551 -- it also
    // requires g.ActiveIdPreviousFrame != that id, which is what makes it fire
    // on the activation frame alone rather than every frame of the drag), so
    // GetItemID() here reads ActiveId -- including through a ctrl+click text
    // entry (TempInputText re-derives the id from the same window+label) and
    // through grouped rows, where EndGroup forwards the live component id into
    // LastItemData (imgui.cpp:12480).
    void BeginOnActivate(Arcane::CommandStack* stack, GestureState& st,
                         Arcane::FunctionRef<std::string()> label,
                         Arcane::FunctionRef<std::function<void()>()> onOpened);

    // Owner-guarded close on the widget's deactivation; safe to call for
    // every row every frame. Commit runs pendingCommit first (the built
    // command joins the open transaction via CommandStack::Push's join rule);
    // Cancel discards it -- nothing was applied on a pure click.
    //
    // GetItemID() is g.LastItemData.ID (imgui.cpp:6676-6680), the same value
    // BeginOnActivate recorded at activation, for every widget shape an editor
    // panel draws:
    //   - bare DragFloat/DragInt/Checkbox: ItemAdd stamps it on both frames
    //     (imgui.cpp:11979).
    //   - ctrl+click temp input: the id comes from the same window+label
    //     (imgui_widgets.cpp:2727, :4728) and the temp input skips ItemAdd
    //     (:4791-4792), so the drag's own id stands.
    //   - groups (multi-axis drag rows, multi-box scalar rows): EndGroup
    //     re-points LastItemData.ID at the group's live ActiveId, ELSE at the
    //     child that deactivated inside it (imgui.cpp:12477-12482) -- so the
    //     activation frame yields the child that took ActiveId, and the
    //     deactivation frame that same child.
    //     One such frame is tabbing .x -> .y inside ONE group, where those two
    //     branches disagree (the live-ActiveId one wins and forwards .y while
    //     .x deactivates); any ActiveId handoff between siblings of one group
    //     does the same. But such a frame is also an activation:
    //     BeginOnActivate has already committed .x's gesture and parked .y in
    //     the slots, so this compares .y against .y.
    void EndOnDeactivate(Arcane::CommandStack* stack, GestureState& st);

    // The popup pair. Call BOTH every frame at a popup-hosted edit site, with the
    // site's own popup id (ImGui::GetID on a stable string, the same id passed to
    // ImGui::OpenPopup -- imgui.h:868 has the ImGuiID overload).
    //
    // BeginOnPopupOpen opens the gesture on the first frame the popup is observed
    // open and is a no-op every frame after, so the caller needs no edge tracking.
    // Liveness is `slots.item == popupId`: slots are cleared on close, and we park
    // item only for a popup whose gesture we opened -- so unlike the widget path,
    // where the header warns item is not an "is one open" flag, here it is exactly
    // that. `popupId == 0` is rejected; ImGui never mints a zero id for a real
    // popup and treating it as live would alias the cleared state.
    void BeginOnPopupOpen(Arcane::CommandStack* stack, GestureState& st,
                          std::uint32_t popupId,
                          Arcane::FunctionRef<std::string()> label,
                          Arcane::FunctionRef<std::function<void()>()> onOpened);

    // Owner-guarded close on the popup going away; safe to call every frame. Always
    // COMMITS (via ClosePending) -- see ShouldClosePopup on why there is no Cancel.
    void EndOnPopupClose(Arcane::CommandStack* stack, GestureState& st,
                         std::uint32_t popupId);

    // Commit-close a parked gesture NOW (stale-handoff + abandonment paths).
    // Commit semantics on purpose, for BOTH kinds of orphan:
    //   - mid-drag: the edits are already applied and the user watched them
    //     happen. Cancel drops the transaction WITHOUT reverting
    //     (CommandStack.cpp:75-82), which would leave them applied and
    //     permanently un-undoable -- the same hazard ApplyRegistryMutation
    //     refuses a structural memento over.
    //   - ctrl+click text entry: nothing was applied at all (a temp input
    //     writes only on submit). Commit re-snapshots, drops every component
    //     whose bytes match, and returns before pushing a step or clearing
    //     redo when none differ (CommandStack.cpp:61-62) -- so here Commit
    //     lands exactly where Cancel would.
    void ClosePending(Arcane::CommandStack& stack, GestureState& st);

    // The guaranteed close path: declare as the FIRST local of every
    // panel/document draw scope that opens gestures, so it destructs LAST --
    // it closes abandoned gestures on every exit path, including early
    // returns and collapsed windows where widgets never report deactivation.
    //
    // RAII rather than a call before each ImGui::End(): the defect class this
    // owns IS a missed close path, and a panel with an early return (no
    // selection, no document) already has one. Destructing LAST means the
    // close runs after ImGui::End() on every path -- including frames where
    // ImGui::Begin returned false (collapsed window or background tab), where
    // every widget inside bails before ItemAdd on window->SkipItems (e.g.
    // imgui_widgets.cpp:2722) and so cannot report its own deactivation.
    //
    // NOEXCEPT CONTRACT: the dtor invokes ClosePending, so a parked
    // pendingCommit builder runs from a destructor -- a builder that throws
    // terminates. Repo policy accepts that (the builders only read document
    // state and push a command); do not park a builder that can throw.
    struct ScopeGuard
    {
        Arcane::CommandStack* stack;   // null tolerated (Play mode)
        GestureState&         st;
        ~ScopeGuard();
    };
}
