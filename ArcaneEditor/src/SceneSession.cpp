#include "SceneSession.hpp"

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

        if (!IsDirty(stack))
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
