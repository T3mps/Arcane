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
// RestoreRegistry. Play/Stop take the PluginHost (not a bare vtable) since the
// Core-DLL split's Task 7: the same object is what attaches/detaches the embedded
// server world below, so one parameter carries both jobs.
//
// TOPOLOGIES (Core-DLL split, spec docs/specs/2026-09-15-core-dll-split-design.md
// s7, plan 1 Task 7). The snapshot/restore above is unchanged in ALL of them -- what
// a topology decides is how many WORLDS the Play session runs and what NET ROLE each
// one plays:
//   Standalone     -- one world, both roles. Today's PIE, unchanged.
//   ListenServer   -- one world, ListenServer: it presents AND has authority (R3).
//   EmbeddedServer -- TWO worlds in THIS process: the editor's, re-roled Client, and
//                     a second DedicatedServer Runtime on the same ProcessContext,
//                     seeded from a registry-only snapshot of the authored scene.
//   ClientOnly     -- one world, Client. The authority lives in a SEPARATE
//                     ArcaneServer.exe the EDITOR spawns (Project/ServerLaunch.hpp);
//                     this class knows nothing about that process.

#include <Arcane/Base/Runtime.hpp>   // std::optional<Runtime> m_server needs the COMPLETE type

#include <cstddef>
#include <optional>
#include <vector>

namespace Arcane { class PluginHost; struct PluginVTable; }

namespace Arcane::Editor
{
    enum class EditorMode { Edit, Play };

    // Play-mode dropdown (Task 6, runtime-host-fold arc): where the transport's
    // Play button sends the click. Viewport = PlaySession above, unchanged
    // (snapshot/restore, tinted toggle). SeparateWindow = the SceneSession
    // LaunchStandalone intent, whose effect is EditorApp::DoLaunchStandalone
    // -- a fire-and-forget spawn of ArcaneRuntime.exe on the active scene;
    // PlaySession/the toggle are never touched by it (there is nothing for
    // Stop to restore). Declared here (rather than inline in
    // EditorApp.hpp, where the persisted m_playMode member lives) so
    // EditorPanels.hpp's DrawSimTimeToolbar can see it without EditorPanels
    // depending on EditorApp.hpp (which itself includes EditorPanels.hpp --
    // that would invert the panel/app layering). EditorApp.hpp and
    // EditorPanels.cpp both already include this header.
    //
    // THE LAST THREE ARE THE Task-7 ROWS, and they are PERSISTED AS AN INT in
    // the editor's ini ("[EditorPlayMode][State]", EditorApp.cpp) -- so append
    // new ones at the END and never renumber: an existing desk's saved line
    // would otherwise silently name a different mode. The ini read's range
    // check has SeparateServerProcess as its upper bound for the same reason.
    // ListenServer/EmbeddedServer stay IN the viewport (they are PlaySession
    // topologies); SeparateServerProcess is the viewport world as a CLIENT
    // plus a spawned ArcaneServer.exe.
    enum class PlayLaunchMode
    {
        Viewport,
        SeparateWindow,
        ListenServer,
        EmbeddedServer,
        SeparateServerProcess,
    };

    // What Play() is being asked to STAND UP -- distinct from PlayLaunchMode,
    // which is a UI row. SeparateWindow has no topology at all (it never enters
    // Play), and SeparateServerProcess maps onto ClientOnly here plus a spawn
    // the EditorApp performs; the other three map one-to-one.
    enum class PlayTopology { Standalone, ListenServer, EmbeddedServer, ClientOnly };

    class PlaySession
    {
    public:
        [[nodiscard]] EditorMode Mode() const noexcept { return m_mode; }
        [[nodiscard]] bool IsPlaying() const noexcept { return m_mode == EditorMode::Play; }
        [[nodiscard]] PlayTopology Topology() const noexcept { return m_topology; }

        // Edit -> Play: snapshots the scene, then unpauses the RunLoop. Routes through
        // `host`'s vtable SaveState when it is non-null and exports one; otherwise uses
        // Runtime::SnapshotRegistry. No-op success (does not re-snapshot) if already
        // playing. `topology` additionally decides the world set -- see the enum above.
        //
        // FALSE means NOTHING WAS ENTERED: a failed snapshot, a failed restore into
        // the embedded server world, or a PluginHost that REFUSED to attach it
        // (AttachRuntime enforces the one-ComponentRegistry invariant). The session
        // stays in Edit and any half-built server world is destroyed.
        bool Play(Arcane::Runtime& runtime, Arcane::PluginHost* host = nullptr,
                  PlayTopology topology = PlayTopology::Standalone);

        // Play -> Edit: tears down the embedded server world (if any), returns the
        // editor's world to Standalone, restores the snapshot via the SAME path Play
        // used (plugin LoadState or Runtime::RestoreRegistry), then re-pauses the
        // RunLoop. No-op success if already stopped (Edit).
        bool Stop(Arcane::Runtime& runtime, Arcane::PluginHost* host = nullptr);

        // The embedded DedicatedServer world while playing EmbeddedServer, else null.
        // Non-owning view of an optional this object owns -- never outlives Stop().
        [[nodiscard]] Arcane::Runtime* ServerWorld() noexcept
        { return m_server ? &*m_server : nullptr; }

        // Advance the embedded server world by one host frame. No-op when there is
        // none. It is the SERVER world's own loop: the module's FixedUpdate hook is
        // bound to the PRIMARY (the editor's world) and is deliberately NOT re-run
        // here, the same rule MultiRuntimeReloadTest's StepAll documents -- what this
        // world runs is its own instantiated (Server-masked) system set.
        void TickServer(double realDt);

    private:
        EditorMode              m_mode = EditorMode::Edit;
        std::vector<std::byte>  m_snapshot;
        bool                    m_usedPlugin = false;   // which path Play used; Stop matches it

        PlayTopology            m_topology = PlayTopology::Standalone;
        // The embedded DedicatedServer world and the registry-only seed it was
        // built from. The seed is kept for the session's life purely as the bytes
        // that produced this world -- Stop drops both together.
        std::optional<Arcane::Runtime> m_server;
        std::vector<std::byte>         m_serverSeed;
    };
}
