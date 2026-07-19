#pragma once

// Play-in-editor state machine (Epic 04, Task 8). Play() snapshots the registry
// (via Runtime::SnapshotRegistry) and unpauses the RunLoop; Stop() restores the
// snapshot (via Runtime::RestoreRegistry) and re-pauses -- so play-time mutation
// is discarded and edit-mode edits (the snapshot content) are the authored state
// that survives a Play/Stop cycle. Grimoire boots in Edit (paused); see
// GrimoireApp::Init.

#include <cstddef>
#include <vector>

namespace Arcane { class Runtime; }

namespace Grimoire
{
    enum class EditorMode { Edit, Play };

    class PlaySession
    {
    public:
        [[nodiscard]] EditorMode Mode() const noexcept { return m_mode; }
        [[nodiscard]] bool IsPlaying() const noexcept { return m_mode == EditorMode::Play; }

        // Edit -> Play: snapshots the registry, then unpauses the RunLoop.
        // No-op success (does not re-snapshot) if already playing.
        bool Play(Arcane::Runtime& runtime);

        // Play -> Edit: restores the snapshot, then re-pauses the RunLoop.
        // No-op success if already stopped (Edit).
        bool Stop(Arcane::Runtime& runtime);

    private:
        EditorMode              m_mode = EditorMode::Edit;
        std::vector<std::byte>  m_snapshot;
    };
}
