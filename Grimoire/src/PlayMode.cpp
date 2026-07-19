#include "PlayMode.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginABI.hpp>

#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>

#include <utility>

namespace Grimoire
{
    bool PlaySession::Play(Arcane::Runtime& runtime, const Arcane::PluginVTable* plugin)
    {
        if (m_mode == EditorMode::Play) return true;   // already playing: no-op success

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

        runtime.Loop().SetPaused(false);
        m_mode = EditorMode::Play;
        return true;
    }

    bool PlaySession::Stop(Arcane::Runtime& runtime, const Arcane::PluginVTable* plugin)
    {
        if (m_mode == EditorMode::Edit) return true;    // already stopped: no-op success

        bool ok;
        if (m_usedPlugin && plugin && plugin->LoadState)
        {
            // Restore via the plugin's LoadState -- it re-establishes native resources
            // (physics world, scene root) AFTER RestoreRegistry, which Grimoire cannot
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
}
