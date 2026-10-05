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
        case SettingScope::PreferencesProject:
            return target == PreferenceTarget::ThisProject ? SetBy::User : SetBy::EditorUser;
        case SettingScope::Project:
            return SetBy::Project;
        }
        return SetBy::Project;
    }

    PreferenceTarget PreferenceTargetOf(const CVarRegistry& registry, std::string_view name)
    {
        const auto e = registry.Explain(name);
        if (!e || e->scope == SettingScope::Project) return PreferenceTarget::AllProjects;
        if (HoldsRung(*e, SetBy::User)) return PreferenceTarget::ThisProject;
        if (HoldsRung(*e, SetBy::EditorUser)) return PreferenceTarget::AllProjects;
        return e->scope == SettingScope::PreferencesProject
                   ? PreferenceTarget::ThisProject
                   : PreferenceTarget::AllProjects;
    }

    SetResult SetPreferenceTarget(CVarRegistry& registry, std::string_view name, PreferenceTarget target)
    {
        const CVarHandle handle = registry.Find(name);
        const auto e = registry.Explain(name);
        if (handle.IsStale() || !e) return SetResult::Stale;
        if (e->scope == SettingScope::Project) return SetResult::Denied;
        if (target == PreferenceTarget::AllProjects)
        {
            // Promote first while User may still win: Set records the weaker
            // EditorUser rung and reports RefusedWeaker until User is cleared.
            if (!HoldsRung(*e, SetBy::EditorUser) || e->scope == SettingScope::PreferencesProject)
            {
                const SetResult promoted = registry.Set(handle, e->pending, SetBy::EditorUser,
                                                        SourceFor(SetBy::EditorUser), CVarContext::Editor);
                if (promoted != SetResult::Applied && promoted != SetResult::RefusedWeaker)
                    return promoted;
            }
            (void)registry.ClearRung(handle, SetBy::User);
            return SetResult::Applied;
        }
        if (!HoldsRung(*e, SetBy::User))
        {
            const SetResult copied = registry.Set(handle, e->pending, SetBy::User,
                                                  SourceFor(SetBy::User), CVarContext::Editor);
            if (copied != SetResult::Applied && copied != SetResult::RefusedWeaker)
                return copied;
        }
        if (e->scope == SettingScope::PreferencesProject)
            (void)registry.ClearRung(handle, SetBy::EditorUser);
        return SetResult::Applied;
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
        {
            const auto e = registry.Explain(entry.name);
            if (!e || e->scope == SettingScope::Project) continue;
            if (HoldsRung(*e, SetBy::User))
                out.push_back(entry.name);
        }
        std::sort(out.begin(), out.end());
        return out;
    }
}
