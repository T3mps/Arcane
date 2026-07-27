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

        // ---- retargeting -------------------------------------------------
        // After a successful save-as or open: adopt the file and go clean.
        void Adopt(std::filesystem::path path, Arcane::Guid id,
                   const Arcane::CommandStack& stack);
        // After New Scene: back to Untitled, no id, clean. The caller clears the
        // undo stack around this (no entity handle survives a registry reset).
        void Reset(const Arcane::CommandStack& stack);

        // ---- confirm flow ------------------------------------------------
        // True  = nothing unsaved, act now.
        // False = the intent is parked; draw the confirm modal, then read
        //         Pending()/PendingPath(), act, and ClearPending().
        // A second request while one is pending is IGNORED and returns false --
        // the modal is app-modal, so replacing what the user is being asked
        // about would resolve their answer against the wrong action.
        bool Request(SceneIntent intent, std::filesystem::path payload,
                     const Arcane::CommandStack& stack);

        [[nodiscard]] SceneIntent Pending() const noexcept { return m_pending; }
        [[nodiscard]] const std::filesystem::path& PendingPath() const noexcept
        {
            return m_pendingPath;
        }
        // Cancel, or acknowledge that the intent has been performed.
        void ClearPending() noexcept;

    private:
        std::filesystem::path m_path;          // empty = never saved
        Arcane::Guid          m_id;            // nil until saved
        std::uint64_t         m_savedStateId = 0;   // 0 == an empty stack, i.e. clean

        SceneIntent           m_pending = SceneIntent::None;
        std::filesystem::path m_pendingPath;
    };
}
