#include "PlayMode.hpp"

#include <Arcane/Base/Runtime.hpp>

#include <utility>

namespace Grimoire
{
    bool PlaySession::Play(Arcane::Runtime& runtime)
    {
        if (m_mode == EditorMode::Play) return true;   // already playing: no-op success

        auto snap = runtime.SnapshotRegistry();
        if (!snap.IsOk()) return false;                 // Result is falsy on a Save error
        m_snapshot = std::move(*snap.GetValue());        // Astra::Result<T,E>::GetValue() -> T*

        runtime.Loop().SetPaused(false);
        m_mode = EditorMode::Play;
        return true;
    }

    bool PlaySession::Stop(Arcane::Runtime& runtime)
    {
        if (m_mode == EditorMode::Edit) return true;    // already stopped: no-op success

        const bool ok = runtime.RestoreRegistry(m_snapshot);
        runtime.Loop().SetPaused(true);
        m_mode = EditorMode::Edit;
        return ok;
    }
}
