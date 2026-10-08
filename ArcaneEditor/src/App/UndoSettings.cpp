#include "App/UndoSettings.hpp"

#include <Arcane/Config/Settings.hpp>

#include <cstddef>
#include <cstdint>

namespace Arcane::Editor
{
    ARC_REFLECT_TYPE(EditorUndoSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.undo", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(EditorUndoSettings, maxSteps)
            ARC_REFLECT_ATTR(Range, 1.0, 10000.0)
            ARC_REFLECT_ATTR(Tooltip, "Undo history depth in steps; the oldest step drops past it.")
        ARC_REFLECT_FIELD(EditorUndoSettings, byteBudgetMB)
            ARC_REFLECT_ATTR(Range, 16.0, 65536.0)
            ARC_REFLECT_ATTR(Tooltip, "Undo history byte budget (MB), memory plus Saved/UndoCache; the oldest step drops past it.")
        ARC_REFLECT_FIELD(EditorUndoSettings, spillThresholdKB)
            ARC_REFLECT_ATTR(Range, 16.0, 1048576.0)
            ARC_REFLECT_ATTR(Tooltip, "Undo payloads above this size (KB) spill to <project>/Saved/UndoCache.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(EditorUndoSettings);

    Arcane::UndoLimits ToUndoLimits(const EditorUndoSettings& s)
    {
        Arcane::UndoLimits l;
        l.maxSteps       = static_cast<std::size_t>(s.maxSteps);
        l.byteBudget     = static_cast<std::uint64_t>(s.byteBudgetMB) << 20;
        l.spillThreshold = static_cast<std::uint64_t>(s.spillThresholdKB) << 10;
        return l;
    }

    Arcane::UndoLimits ReadUndoLimits() { return ToUndoLimits(Arcane::Settings<EditorUndoSettings>()); }
}
