#include "Scene/SceneSession.hpp"

namespace Arcane::Editor
{
    std::string SceneSession::DisplayName() const
    {
        if (m_path.empty()) return "Untitled";
        return m_path.stem().string();
    }

    void SceneSession::Adopt(std::filesystem::path path, Arcane::Guid id,
                             const Arcane::CommandStack& stack)
    {
        m_path = std::move(path);
        m_id   = id;
        MarkSaved(stack);
    }

    void SceneSession::Reset(const Arcane::CommandStack& stack)
    {
        m_path.clear();
        m_id = Arcane::Guid{};
        MarkSaved(stack);
    }

    bool SceneSession::Request(SceneIntent intent, std::filesystem::path payload,
                               const Arcane::CommandStack& stack)
    {
        if (intent == SceneIntent::None) return false;
        if (m_pending != SceneIntent::None) return false;   // one at a time

        // LaunchStandalone ALSO parks on a nil id: the standalone runtime loads the
        // scene from DISK by guid, so a never-saved scene is exactly as unready as
        // a dirty one for this intent (the old LaunchStandalone guard, now here).
        const bool unready = IsDirty(stack) ||
            (intent == SceneIntent::LaunchStandalone && !m_id.IsValid());
        if (!unready)
            return true;

        m_pending     = intent;
        m_pendingPath = std::move(payload);
        return false;
    }

    SceneSession::PendingRequest SceneSession::TakePending() noexcept
    {
        PendingRequest req{m_pending, std::move(m_pendingPath)};
        m_pending = SceneIntent::None;
        m_pendingPath.clear();
        return req;
    }

    void SceneSession::ClearPending() noexcept
    {
        m_pending = SceneIntent::None;
        m_pendingPath.clear();
    }
}
