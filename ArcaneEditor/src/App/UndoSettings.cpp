#include "App/UndoSettings.hpp"

#include <Arcane/Config/CVarDecl.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Arcane::Editor
{
    namespace
    {
        ARC_CVAR(cvar_undoMaxSteps, "editor.undo.maxSteps", std::int32_t, 100,
                 .min = 1, .max = 10000, .flags = ::Arcane::CVarFlags::Archive,
                 .audience = ::Arcane::Audience::Editor, .scope = ::Arcane::SettingScope::PreferencesProject,
                 .help = "Undo history depth in steps; the oldest step drops past it.");
        ARC_CVAR(cvar_undoByteBudgetMB, "editor.undo.byteBudgetMB", std::int32_t, 512,
                 .min = 16, .max = 65536, .flags = ::Arcane::CVarFlags::Archive,
                 .audience = ::Arcane::Audience::Editor, .scope = ::Arcane::SettingScope::PreferencesProject,
                 .help = "Undo history byte budget (MB), memory plus Saved/UndoCache; the oldest step drops past it.");
        ARC_CVAR(cvar_undoSpillThresholdKB, "editor.undo.spillThresholdKB", std::int32_t, 256,
                 .min = 16, .max = 1048576, .flags = ::Arcane::CVarFlags::Archive,
                 .audience = ::Arcane::Audience::Editor, .scope = ::Arcane::SettingScope::PreferencesProject,
                 .help = "Undo payloads above this size (KB) spill to <project>/Saved/UndoCache.");

        std::int32_t ReadInt(const Arcane::CVarRegistry& cvars, std::string_view name, std::int32_t fallback)
        {
            const auto v = cvars.Get(cvars.Find(name));
            return v ? v->AsInt32() : fallback;
        }
    }

    Arcane::UndoLimits ReadUndoLimits(const Arcane::CVarRegistry& cvars)
    {
        Arcane::UndoLimits l;
        l.maxSteps       = static_cast<std::size_t>(ReadInt(cvars, "editor.undo.maxSteps", 100));
        l.byteBudget     = static_cast<std::uint64_t>(ReadInt(cvars, "editor.undo.byteBudgetMB", 512)) << 20;
        l.spillThreshold = static_cast<std::uint64_t>(ReadInt(cvars, "editor.undo.spillThresholdKB", 256)) << 10;
        return l;
    }
}
