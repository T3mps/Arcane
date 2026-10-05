#pragma once

// Settings arc S3 (spec s6.3, s6.5, s12): the debounced archive queue both
// settings windows share -- every edit marks its (rung, key) dirty; the
// queue writes once the edits go quiet for editor.settings.saveDebounceMs,
// and at once on window close and editor exit -- and the tracker behind
// the Restart bar (a Restart setting whose value differs from boot) and
// the next-world note (a NextWorld setting changed since the last world
// load). Pure: no ImGui.

#include "Settings/SettingsEdit.hpp"   // RungChange, SettingsEditSink

#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Config/CVarRef.hpp>
#include <Arcane/Config/CVarRegistry.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Arcane::Editor
{
    ARC_CVAR_EXTERN(cvar_settingsSaveDebounceMs, std::int32_t);   // editor.settings.saveDebounceMs

    // Brings `names` up to date in `rung`'s config folder (SettingsHost: WriteCVarRungArchive).
    using RungWriter = std::function<void(SetBy rung, const std::vector<std::string>& names)>;

    class SettingsArchiveQueue
    {
    public:
        void MarkDirty(SetBy rung, const std::string& name, double nowSeconds);
        // Writes every dirty rung once `debounceMs` passed since the last
        // MarkDirty. True when it wrote.
        bool Tick(double nowSeconds, std::int32_t debounceMs, const RungWriter& write);
        void Flush(const RungWriter& write);
        [[nodiscard]] bool Dirty() const noexcept { return !m_dirty.empty(); }
    private:
        std::map<SetBy, std::set<std::string>> m_dirty;
        double m_lastChange = 0.0;
    };

    // The sink every settings edit reports to: marks the edit's (rung, name) dirty at now().
    [[nodiscard]] SettingsEditSink ArchiveSink(SettingsArchiveQueue* queue, std::function<double()> now);

    class SettingsApplyTracker
    {
    public:
        // Baselines every Restart / NextWorld cvar not seen before at its
        // current (pending) value. Boot = the first call (SettingsHost); a
        // module's cvars are first seen when a window rebuilds after its load.
        void Observe(const CVarRegistry& registry);
        // A world was created (Play, scene open/new): NextWorld baselines := now.
        void WorldCreated(const CVarRegistry& registry);
        [[nodiscard]] std::vector<std::string> PendingRestart(const CVarRegistry& registry) const;
        [[nodiscard]] std::vector<std::string> PendingNextWorld(const CVarRegistry& registry) const;
    private:
        std::map<std::string, CVarValue> m_restart, m_nextWorld;
    };
}
