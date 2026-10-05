#include "Settings/SettingsApply.hpp"

#include <iterator>
#include <optional>
#include <utility>

namespace Arcane::Editor
{
    ARC_CVAR(cvar_settingsSaveDebounceMs, "editor.settings.saveDebounceMs", std::int32_t, 500,
             .min = 0, .max = 10000,
             .audience = ::Arcane::Audience::Editor,
             .scope = ::Arcane::SettingScope::PreferencesMachine,
             .help = "Quiet time, in milliseconds, before a settings window writes its edits to the config files. Closing the window or the editor always writes at once.",
             .keywords = "save autosave delay debounce");

    namespace
    {
        std::optional<CVarValue> PendingOf(const CVarRegistry& registry, const std::string& name)
        {
            if (const std::optional<CVarExplain> e = registry.Explain(name)) return e->pending;
            return std::nullopt;
        }

        std::vector<std::string> Differing(const CVarRegistry& registry, const std::map<std::string, CVarValue>& base)
        {
            std::vector<std::string> out;
            for (const auto& [name, value] : base)
                if (const std::optional<CVarValue> now = PendingOf(registry, name); now && !(*now == value))
                    out.push_back(name);
            return out;
        }
    }

    void SettingsArchiveQueue::MarkDirty(SetBy rung, const std::string& name, double nowSeconds)
    {
        m_dirty[rung].insert(name);
        m_lastChange = nowSeconds;
    }

    bool SettingsArchiveQueue::Tick(double nowSeconds, std::int32_t debounceMs, const RungWriter& write)
    {
        if (m_dirty.empty()) return false;
        if ((nowSeconds - m_lastChange) * 1000.0 < static_cast<double>(debounceMs)) return false;
        if (Flush(write)) return true;
        m_lastChange = nowSeconds;   // retry after another quiet debounce, not every frame
        return false;
    }

    bool SettingsArchiveQueue::Flush(const RungWriter& write)
    {
        std::map<SetBy, std::set<std::string>> dirty = std::move(m_dirty);
        m_dirty.clear();
        for (auto& [rung, names] : dirty)
        {
            if (write && write(rung, std::vector<std::string>(names.begin(), names.end()))) continue;
            m_dirty[rung] = std::move(names);   // not persisted: keep the edits
        }
        return m_dirty.empty();
    }

    SettingsEditSink ArchiveSink(SettingsArchiveQueue* queue, std::function<double()> now)
    {
        return [queue, now = std::move(now)](const RungChange& change)
        {
            if (queue) queue->MarkDirty(change.rung, change.name, now ? now() : 0.0);
        };
    }

    void SettingsApplyTracker::Observe(const CVarRegistry& registry)
    {
        // A cvar that is no longer registered (module unloaded) loses its
        // baseline, so it is freshly baselined when it reappears.
        for (std::map<std::string, CVarValue>* base : { &m_restart, &m_nextWorld })
            for (auto it = base->begin(); it != base->end();)
                it = PendingOf(registry, it->first) ? std::next(it) : base->erase(it);
        for (const std::string& name : registry.Names(/*includeHidden=*/true))
        {
            const std::optional<CVarDescInfo> d = registry.Describe(name);
            if (!d || d->apply == ApplyMode::Live) continue;
            std::map<std::string, CVarValue>& base = d->apply == ApplyMode::Restart ? m_restart : m_nextWorld;
            if (base.contains(name)) continue;
            if (const std::optional<CVarValue> v = PendingOf(registry, name)) base.emplace(name, *v);
        }
    }

    void SettingsApplyTracker::WorldCreated(const CVarRegistry& registry)
    {
        for (auto it = m_nextWorld.begin(); it != m_nextWorld.end();)
        {
            if (const std::optional<CVarValue> v = PendingOf(registry, it->first)) { it->second = *v; ++it; }
            else it = m_nextWorld.erase(it);
        }
        Observe(registry);
    }

    std::vector<std::string> SettingsApplyTracker::PendingRestart(const CVarRegistry& registry) const
    {
        return Differing(registry, m_restart);
    }

    std::vector<std::string> SettingsApplyTracker::PendingNextWorld(const CVarRegistry& registry) const
    {
        return Differing(registry, m_nextWorld);
    }
}
