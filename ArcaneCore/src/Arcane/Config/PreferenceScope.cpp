#include <Arcane/Config/PreferenceScope.hpp>

#include <algorithm>
#include <utility>

namespace Arcane
{
    namespace
    {
        bool HoldsRung(const CVarExplain& e, SetBy rung)
        {
            for (const CVarHistoryRecord& h : e.history)
                if (h.by == rung)
                    return true;
            return false;
        }

        // The source the file layer uses for the same rung, so an edit REPLACES
        // the file's record (one record per (rung, source)) instead of stacking.
        std::string_view SourceFor(SetBy rung)
        {
            switch (rung)
            {
            case SetBy::EditorUser: return "editor-user";
            case SetBy::User:       return "user";
            default:                return "project";
            }
        }
    }

    SetBy PreferenceRung(SettingScope scope, PreferenceTarget target) noexcept
    {
        switch (scope)
        {
        case SettingScope::PreferencesMachine:
            return target == PreferenceTarget::ThisProject ? SetBy::User : SetBy::EditorUser;
        case SettingScope::PreferencesProject:
            return SetBy::User;
        case SettingScope::Project:
            return SetBy::Project;
        }
        return SetBy::Project;
    }

    PreferenceTarget PreferenceTargetOf(const CVarRegistry& registry, std::string_view name)
    {
        const auto e = registry.Explain(name);
        if (!e || e->scope != SettingScope::PreferencesMachine) return PreferenceTarget::AllProjects;
        return HoldsRung(*e, SetBy::User) ? PreferenceTarget::ThisProject : PreferenceTarget::AllProjects;
    }

    SetResult SetPreferenceTarget(CVarRegistry& registry, std::string_view name, PreferenceTarget target)
    {
        const CVarHandle handle = registry.Find(name);
        const auto e = registry.Explain(name);
        if (handle.IsStale() || !e) return SetResult::Stale;
        if (e->scope != SettingScope::PreferencesMachine) return SetResult::Denied;
        if (target == PreferenceTarget::AllProjects)
        {
            (void)registry.ClearRung(handle, SetBy::User);
            return SetResult::Applied;
        }
        if (HoldsRung(*e, SetBy::User)) return SetResult::Applied;
        return registry.Set(handle, e->pending, SetBy::User, SourceFor(SetBy::User), CVarContext::Editor);
    }

    SetResult EditPreference(CVarRegistry& registry, std::string_view name, CVarValue value)
    {
        const CVarHandle handle = registry.Find(name);
        const auto e = registry.Explain(name);
        if (handle.IsStale() || !e) return SetResult::Stale;
        const SetBy rung = PreferenceRung(e->scope, PreferenceTargetOf(registry, name));
        return registry.Set(handle, std::move(value), rung, SourceFor(rung), CVarContext::Editor);
    }

    std::vector<std::string> ProjectOverrides(const CVarRegistry& registry)
    {
        std::vector<std::string> out;
        for (const CVarListEntry& entry : registry.List())
            if (PreferenceTargetOf(registry, entry.name) == PreferenceTarget::ThisProject)
                out.push_back(entry.name);
        std::sort(out.begin(), out.end());
        return out;
    }
}
