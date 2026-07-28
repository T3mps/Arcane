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
// The ImGui-facing skin arrives in Task 2 and stays thin.

#include <Arcane/Edit/CommandStack.hpp>

#include <cstdint>

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
}
