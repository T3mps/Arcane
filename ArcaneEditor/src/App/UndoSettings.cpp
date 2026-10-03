#include "App/UndoSettings.hpp"

#include <Arcane/Config/CVarDecl.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Arcane::Editor
{
    namespace
    {
        ARC_CVAR_RANGED("editor.undo.maxSteps", "editor", Int32, CVarValue::Int32(100),
                        CVarValue::Int32(1), CVarValue::Int32(10000), CVarFlags::Archive,
                        "Undo history depth in steps; the oldest step drops past it.");
        ARC_CVAR_RANGED("editor.undo.byteBudgetMB", "editor", Int32, CVarValue::Int32(512),
                        CVarValue::Int32(16), CVarValue::Int32(65536), CVarFlags::Archive,
                        "Undo history byte budget (MB), memory plus Saved/UndoCache; the oldest step drops past it.");
        ARC_CVAR_RANGED("editor.undo.spillThresholdKB", "editor", Int32, CVarValue::Int32(256),
                        CVarValue::Int32(16), CVarValue::Int32(1048576), CVarFlags::Archive,
                        "Undo payloads above this size (KB) spill to <project>/Saved/UndoCache.");

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
