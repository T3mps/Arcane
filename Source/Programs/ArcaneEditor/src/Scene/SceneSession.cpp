#include "Scene/SceneSession.hpp"

#include <cctype>

namespace Arcane::Editor
{
    namespace
    {
        // A comparable spelling for a path that may no longer exist (so no
        // std::filesystem::equivalent): lexically normal, generic separators,
        // case-folded on Windows where the filesystem is case-insensitive.
        std::string PathKey(const std::filesystem::path& p)
        {
            std::string s = p.lexically_normal().generic_string();
#if defined(_WIN32)
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
            return s;
        }
    }

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

    void SceneSession::NoteMoved(const std::filesystem::path& from, const std::filesystem::path& to)
    {
        if (!m_path.empty() && PathKey(m_path) == PathKey(from))
            m_path = to;
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
