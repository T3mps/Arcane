#include <Arcane/Config/PlayerSettings.hpp>

#include <algorithm>
#include <atomic>

namespace Arcane
{
    namespace
    {
        std::atomic<NetMode> g_sessionMode{ NetMode::Standalone };

        bool MatchesCategory(std::string_view name, std::string_view prefix)
        {
            if (prefix.empty()) return true;
            if (!name.starts_with(prefix)) return false;
            if (prefix.back() == '.' || name.size() == prefix.size()) return true;
            return name[prefix.size()] == '.';
        }
    }

    CVarContext CVarContextFor(NetMode mode) noexcept
    {
        switch (mode)
        {
            case NetMode::Standalone:      return CVarContext::LocalHost;
            case NetMode::ListenServer:    return CVarContext::LocalHost;
            case NetMode::Client:          return CVarContext::Client;
            case NetMode::DedicatedServer: return CVarContext::ServerAdmin;
        }
        return CVarContext::LocalHost;
    }

    namespace PlayerSettings
    {
        void SetSessionMode(NetMode mode) noexcept { g_sessionMode.store(mode, std::memory_order_release); }
        NetMode SessionMode() noexcept { return g_sessionMode.load(std::memory_order_acquire); }
        CVarContext SessionContext() noexcept { return CVarContextFor(SessionMode()); }

        std::vector<CVarListEntryEx> List(const CVarRegistry& registry, std::string_view categoryPrefix)
        {
            std::vector<CVarListEntryEx> out;
            for (CVarListEntryEx& e : registry.ListEx())
            {
                if (e.audience != Audience::PlayerSafe) continue;
                if (!MatchesCategory(e.name, categoryPrefix)) continue;
                out.push_back(std::move(e));
            }
            std::sort(out.begin(), out.end(), [](const CVarListEntryEx& a, const CVarListEntryEx& b) {
                if (a.categoryPath != b.categoryPath) return a.categoryPath < b.categoryPath;
                if (a.order != b.order) return a.order < b.order;
                return a.name < b.name;
            });
            return out;
        }

        SetResult Set(CVarRegistry& registry, std::string_view name, const CVarValue& value, CVarContext context)
        {
            if (context != CVarContext::LocalHost && context != CVarContext::Client)
                return SetResult::Denied;
            const CVarHandle handle = registry.Find(name);
            if (handle.IsStale()) return SetResult::Stale;
            return registry.Set(handle, value, SetBy::User, "user", context, nullptr);
        }

        std::vector<CVarListEntryEx> List(std::string_view categoryPrefix)
        {
            return List(CVarRegistry::Get(), categoryPrefix);
        }

        SetResult Set(std::string_view name, const CVarValue& value)
        {
            return Set(CVarRegistry::Get(), name, value, SessionContext());
        }
    }
}
