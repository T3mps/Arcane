#pragma once

// Play-in-editor state machine (Epic 04, Task 8). Play() snapshots the scene and
// unpauses the RunLoop; Stop() restores the snapshot and re-pauses -- so play-time
// mutation is discarded and edit-mode edits (the snapshot content) are the authored
// state that survives a Play/Stop cycle. Arcane Editor boots in Edit (paused); see
// EditorApp::Init.
//
// Snapshot path: when a plugin vtable exporting SaveState is supplied, Play/Stop route
// through the plugin's GamePlugin_SaveState/LoadState so the plugin captures AND
// re-establishes its own native resources (e.g. the Manifold2D physics world), which
// the raw registry snapshot omits -- restoring via Runtime::RestoreRegistry alone would
// leave those resources gone and the plugin's systems dereferencing null. With no
// plugin (a pure-ECS or test host) Play/Stop fall back to Runtime::SnapshotRegistry/
// RestoreRegistry.

#include <cstddef>
#include <vector>

namespace Arcane { class Runtime; struct PluginVTable; }

namespace Arcane::Editor
{
    enum class EditorMode { Edit, Play };

    class PlaySession
    {
    public:
        [[nodiscard]] EditorMode Mode() const noexcept { return m_mode; }
        [[nodiscard]] bool IsPlaying() const noexcept { return m_mode == EditorMode::Play; }

        // Edit -> Play: snapshots the scene, then unpauses the RunLoop. Routes through
        // `plugin`'s SaveState when it is non-null and exports one; otherwise uses
        // Runtime::SnapshotRegistry. No-op success (does not re-snapshot) if already playing.
        bool Play(Arcane::Runtime& runtime, const Arcane::PluginVTable* plugin = nullptr);

        // Play -> Edit: restores the snapshot via the SAME path Play used (plugin
        // LoadState or Runtime::RestoreRegistry), then re-pauses the RunLoop. No-op
        // success if already stopped (Edit).
        bool Stop(Arcane::Runtime& runtime, const Arcane::PluginVTable* plugin = nullptr);

    private:
        EditorMode              m_mode = EditorMode::Edit;
        std::vector<std::byte>  m_snapshot;
        bool                    m_usedPlugin = false;   // which path Play used; Stop matches it
    };
}
