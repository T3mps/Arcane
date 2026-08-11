#pragma once

// SceneSession: which scene the editor is editing, whether it has unsaved
// changes, and the one-at-a-time unsaved-changes confirm machine.
//
// PURE state -- no ImGui, no filesystem access. The host performs every effect
// (dialogs, reading and writing files, resetting the registry); this decides
// WHETHER it may, and remembers what was asked for while the user answers.
// Same split as ConsoleBuffer and DocumentHost, and the reason the whole flow
// is unit-tested headlessly.
//
// The scene is a SESSION, not a DocumentHost document: the Viewport is already
// the scene view, and the Edit-mode registry is already the authored state
// (see PlayMode.hpp). Materials are documents because they are side artifacts.

#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Guid.hpp>

#include <filesystem>
#include <string>

namespace Arcane::Editor
{
    // What the user asked for that a dirty scene is standing in the way of.
    enum class SceneIntent
    {
        None = 0,
        NewScene,
        OpenScene,     // PendingPath() = the .arcscene to open
        OpenProject,   // PendingPath() = the .arcproj to switch to
        Exit,
    };

    class SceneSession
    {
    public:
        // ---- identity ----------------------------------------------------
        // Empty until the scene has been saved somewhere.
        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }
        [[nodiscard]] const Arcane::Guid& Id() const noexcept { return m_id; }
        // "Untitled" for a never-saved scene, else the file stem.
        [[nodiscard]] std::string DisplayName() const;

        // ---- dirty -------------------------------------------------------
        // Compares the stack's CURRENT state against the one recorded at save.
        //
        // ASSUMPTION, and it is load-bearing: the CommandStack is a faithful
        // proxy for authored change. True today -- the RunLoop is paused in Edit
        // mode and every Outliner, Inspector and gizmo edit is bracketed through
        // the stack. Anything that mutates the registry OUTSIDE the stack will
        // not mark the scene dirty, and whoever adds such a path owns fixing
        // this.
        [[nodiscard]] bool IsDirty(const Arcane::CommandStack& stack) const noexcept
        {
            return stack.StateId() != m_savedStateId;
        }
        void MarkSaved(const Arcane::CommandStack& stack) noexcept
        {
            m_savedStateId = stack.StateId();
        }
        // The recorded save baseline, for per-entity dirty queries
        // (CommandStack::TouchedSinceState -- the Outliner's asterisks).
        [[nodiscard]] std::uint64_t SavedStateId() const noexcept { return m_savedStateId; }

        // ---- retargeting -------------------------------------------------
        // After a successful save-as or open: adopt the file and go clean.
        //
        // Deliberately does NOT touch m_pending. Adopt is called mid-flow, from
        // the Save branch of the confirm modal (save, then adopt the file) --
        // the host reads Pending()/TakePending() AFTER this call to know what
        // to perform next. Clearing it here would erase the intent before the
        // host ever got to act on it.
        void Adopt(std::filesystem::path path, Arcane::Guid id,
                   const Arcane::CommandStack& stack);
        // After New Scene: back to Untitled, no id, clean. The caller clears the
        // undo stack around this (no entity handle survives a registry reset).
        //
        // Deliberately does NOT touch m_pending, for the same reason as Adopt:
        // Reset can itself be the performed action for a parked NewScene intent,
        // and the host still needs TakePending() to have returned it first.
        void Reset(const Arcane::CommandStack& stack);

        // ---- confirm flow ------------------------------------------------
        // True  = nothing unsaved, act now.
        // False = the intent is parked; draw the confirm modal, then either
        //         perform it via TakePending() (Save/Discard) or abandon it
        //         via ClearPending() (Cancel).
        // A second request while one is pending is IGNORED and returns false --
        // the modal is app-modal, so replacing what the user is being asked
        // about would resolve their answer against the wrong action.
        bool Request(SceneIntent intent, std::filesystem::path payload,
                     const Arcane::CommandStack& stack);

        // The parked intent and its payload, taken together.
        struct PendingRequest
        {
            SceneIntent           intent = SceneIntent::None;
            std::filesystem::path path;
        };

        // Read-only observers, for deciding whether to draw the confirm modal
        // at all. Do not use these to decide whether to perform the intent --
        // that reads it without consuming it, which is exactly the bug this
        // machine exists to prevent (see TakePending).
        [[nodiscard]] SceneIntent Pending() const noexcept { return m_pending; }
        [[nodiscard]] const std::filesystem::path& PendingPath() const noexcept
        {
            return m_pendingPath;
        }

        // Returns the parked intent and payload AND clears them, atomically.
        // This is the ONLY path that performs a parked intent -- the host
        // calls it once it is ready to act (after Save has adopted the file,
        // or immediately for Discard), so there is no way to read an intent
        // without also consuming it, and therefore no way to forget to clear
        // it and wedge the session.
        [[nodiscard]] PendingRequest TakePending() noexcept;

        // Abandons the parked intent without performing it: the Cancel branch
        // of the confirm modal. Unlike TakePending, nothing happens as a
        // result -- the user's original action is simply dropped.
        void ClearPending() noexcept;

    private:
        std::filesystem::path m_path;          // empty = never saved
        Arcane::Guid          m_id;            // nil until saved
        std::uint64_t         m_savedStateId = 0;   // 0 == an empty stack, i.e. clean

        SceneIntent           m_pending = SceneIntent::None;
        std::filesystem::path m_pendingPath;
    };
}
