#include "App/PlayMode.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Scene/SceneResources.hpp>   // SceneRoot -- the one resource the seed cannot carry

#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>

#include <utility>

namespace Arcane::Editor
{
    bool PlaySession::Play(Arcane::Runtime& runtime, Arcane::PluginHost* host, PlayTopology topology)
    {
        if (m_mode == EditorMode::Play) return true;   // already playing: no-op success

        const Arcane::PluginVTable* plugin = host ? host->Vtable() : nullptr;

        if (plugin && plugin->SaveState)
        {
            // Route through the plugin so it snapshots its own scene, including native
            // resources (e.g. the physics world) that the raw registry snapshot omits.
            // Mirrors PluginHost's hot-reload SaveState buffer pattern.
            m_snapshot.clear();
            Astra::BinaryWriter w(m_snapshot);
            plugin->SaveState(w);
            if (w.HasError()) return false;
            m_usedPlugin = true;
        }
        else
        {
            auto snap = runtime.SnapshotRegistry();
            if (!snap.IsOk()) return false;                 // Result is falsy on a Save error
            m_snapshot = std::move(*snap.GetValue());        // Astra::Result<T,E>::GetValue() -> T*
            m_usedPlugin = false;
        }

        // Play starts from the AUTHORED state, the way ArcaneRuntime boots --
        // not from the world the Edit passes have been minting and reconciling.
        // That world is authoring state: the paused reconcile zeroes a body's
        // velocity on every author move (by design, "don't fling on resume"),
        // so carrying it into Play lost an authored RigidBody2D::velocity
        // whenever the entity had been dragged after the velocity was set --
        // editor Play and the standalone host disagreed. Dropping the world
        // here makes the first Play frame's EnsurePhysics mint a fresh one,
        // whose PASS 2 applies every authored velocity (2026-09-12 review).
        // AFTER the snapshot: what Stop restores is the registry, and the world
        // is never part of it either way (RestoreRegistry strips the same two).
        runtime.ResetPhysics();

        // The TOPOLOGY switch happens AFTER the snapshot and the physics reset, so
        // what Stop restores is the authored state regardless of which one was
        // entered -- the world set below is built FROM that same authored state.
        switch (topology)
        {
            case PlayTopology::Standalone: break;
            case PlayTopology::ListenServer: runtime.SetNetMode(Arcane::NetMode::ListenServer); break;
            case PlayTopology::ClientOnly:   runtime.SetNetMode(Arcane::NetMode::Client); break;
            case PlayTopology::EmbeddedServer:
            {
                // The second world: same ProcessContext, same module (attached below), the
                // SAME scene -- a registry-only snapshot of the authored world, restored
                // into a fresh DedicatedServer Runtime. Nothing else is shared (spec s7).
                auto seed = runtime.SnapshotRegistry();
                if (!seed.IsOk()) return false;
                m_serverSeed = std::move(*seed.GetValue());

                // THE COMPONENT REGISTRY IS SHARED, AND IT HAS TO BE (spec s4, the
                // N-worlds-on-one-module invariant; Runtime's secondary ctor states it
                // in full). A module registers its component types ONCE per DLL load,
                // on the PRIMARY Runtime's ComponentRegistry -- so a server world with
                // a registry of ITS OWN would resolve none of them, and the restore
                // immediately below would answer Astra's UnknownComponent for every
                // module-defined type in the authored scene. PluginHost::AttachRuntime
                // REFUSES a secondary that does not share, which is what turns this
                // from a convention into an enforced one.
                m_server.emplace(runtime.Process(), Arcane::NetMode::DedicatedServer,
                                 runtime.Components());
                if (!m_server->RestoreRegistry(m_serverSeed))
                {
                    ARC_ERROR("Play (embedded server): the authored scene failed to restore into the server world");
                    m_server.reset();
                    m_serverSeed.clear();
                    return false;
                }
                if (const Arcane::SceneRoot* sr = runtime.Registry().GetResource<Arcane::SceneRoot>())
                    m_server->Registry().SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{*sr});   // resources are not in the snapshot (GameModule.hpp's LoadState says the same)
                m_server->ResetPhysics();
                // Attaching instantiates the module's Server-masked factories into this
                // world. A REFUSAL is fatal to the whole Play: a server world the module
                // does not serve is a world with no gameplay in it at all, so tear it
                // back down rather than enter Play with a silently inert second world.
                if (host && !host->AttachRuntime(*m_server))
                {
                    ARC_ERROR("Play (embedded server): the plugin host refused the server world");
                    m_server.reset();
                    m_serverSeed.clear();
                    return false;
                }
                runtime.SetNetMode(Arcane::NetMode::Client);
                break;
            }
        }

        // Committed only now: every `return false` above leaves the session in Edit,
        // so Topology() never describes a topology that was not actually entered.
        m_topology = topology;
        runtime.Loop().SetPaused(false);
        m_mode = EditorMode::Play;
        return true;
    }

    bool PlaySession::Stop(Arcane::Runtime& runtime, Arcane::PluginHost* host)
    {
        if (m_mode == EditorMode::Edit) return true;    // already stopped: no-op success

        // The embedded server world dies FIRST: DetachRuntime clears its
        // module-registered systems and resets its registry while the module image is
        // still mapped (its entities' component descriptors point into it), which is
        // exactly what makes destroying the Runtime right after safe.
        if (m_server)
        {
            if (host) host->DetachRuntime(*m_server);
            m_server.reset();
            m_serverSeed.clear();
        }
        // Back to one Standalone world before the restore: SetNetMode clears and
        // re-instantiates the module's systems for the new role, and the schedulers
        // survive the registry swap the restore performs right after.
        if (runtime.Mode() != Arcane::NetMode::Standalone)
            runtime.SetNetMode(Arcane::NetMode::Standalone);
        m_topology = PlayTopology::Standalone;

        const Arcane::PluginVTable* plugin = host ? host->Vtable() : nullptr;

        bool ok;
        if (m_usedPlugin && plugin && plugin->LoadState)
        {
            // Restore via the plugin's LoadState -- it re-establishes native resources
            // (physics world, scene root) AFTER RestoreRegistry, which Arcane Editor cannot
            // do itself without knowing the plugin's scene.
            Astra::BinaryReader r(m_snapshot);
            ok = plugin->LoadState(r);
        }
        else
        {
            ok = runtime.RestoreRegistry(m_snapshot);
        }

        runtime.Loop().SetPaused(true);
        m_mode = EditorMode::Edit;
        return ok;
    }

    void PlaySession::TickServer(double realDt)
    {
        if (!m_server) return;
        m_server->EnsurePhysics();
        m_server->Loop().Advance(realDt);
    }
}
