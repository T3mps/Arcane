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
    struct Slots
    {
        Arcane::TransactionId txn  = Arcane::TransactionId::None;
        std::uint32_t         item = 0;   // id of the widget that OPENED txn
    };

    enum class EndAction { None, Commit, Cancel };

    // EndOnDeactivate's verdict. Ownership guard FIRST: a row that never
    // opened the gesture still reports its own deactivation on one-frame
    // ActiveId handoffs, and without the guard its pure click would Cancel
    // the owner's live transaction -- Cancel discards WITHOUT reverting.
    // A joined gesture (txn == None, item parked) still evaluates: Commit/
    // Cancel on None no-op at the stack, and the slots must clear either way.
    [[nodiscard]] EndAction EvaluateEnd(const Slots& s, std::uint32_t lastItemId,
                                        bool deactivatedAfterEdit,
                                        bool deactivated) noexcept;

    // Abandonment test for ScopeGuard: a parked gesture whose owner no longer
    // holds ActiveId (widget vanished / window collapsed mid-gesture).
    // `hasPendingCommit` covers the builder-style JOINER edge: txn == None
    // (joined a gizmo drag) but a built command is still owed to the stack.
    [[nodiscard]] bool ShouldCloseAbandoned(const Slots& s, std::uint32_t activeId,
                                            bool hasPendingCommit) noexcept;

    // Activation-time stale check: a still-parked gesture means the previous
    // owner never got to close (click-through to a widget drawn ABOVE it in
    // submission order); close-and-commit it before opening ours.
    [[nodiscard]] bool ShouldCloseStaleOnActivate(const Slots& s,
                                                  bool hasPendingCommit) noexcept;

    // ---- ImGui-facing skin (thin; NOT unit-driven -- the pure core above is) ----

    struct GestureState
    {
        Slots slots;
        // Builder-style adopters park the command build here at open; it runs
        // EXACTLY ONCE, at close (release-commit, stale-close, or abandoned
        // close), then clears. Empty for snapshot-style gestures -- their
        // before-state rides the open transaction itself (SnapshotComponent).
        std::function<void()> pendingCommit;
        // The string row's activation-time cancel reference and its owner,
        // moved verbatim from InspectorState (see the String arm in
        // InspectorView.cpp for why the seed is latched at activation).
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
    void BeginOnActivate(Arcane::CommandStack* stack, GestureState& st,
                         Arcane::FunctionRef<std::string()> label,
                         Arcane::FunctionRef<std::function<void()>()> onOpened);

    // Owner-guarded close on the widget's deactivation; safe to call for
    // every row every frame. Commit runs pendingCommit first (the built
    // command joins the open transaction via CommandStack::Push's join rule);
    // Cancel discards it -- nothing was applied on a pure click.
    void EndOnDeactivate(Arcane::CommandStack* stack, GestureState& st);

    // Commit-close a parked gesture NOW (stale-handoff + abandonment paths).
    // Commit semantics on purpose: the live edit already applied, so closing
    // must land it on the undo stack -- Cancel would discard WITHOUT
    // reverting and strand an un-undoable edit.
    void ClosePending(Arcane::CommandStack& stack, GestureState& st);

    // The generalized GestureCloseGuard: declare as the FIRST local of every
    // panel/document draw scope that opens gestures, so it destructs LAST --
    // it closes abandoned gestures on every exit path, including early
    // returns and collapsed windows where widgets never report deactivation.
    struct ScopeGuard
    {
        Arcane::CommandStack* stack;   // null tolerated (Play mode)
        GestureState&         st;
        ~ScopeGuard();
    };
}
