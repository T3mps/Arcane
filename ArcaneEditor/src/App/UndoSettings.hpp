#pragma once

// The editor's undo tunables (spec 2026-09-30 s2.4): Archive cvars, read here
// and pushed into the CommandStack -- the stack itself never reads a cvar.

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Edit/CommandStack.hpp>

namespace Arcane::Editor
{
    [[nodiscard]] Arcane::UndoLimits ReadUndoLimits(const Arcane::CVarRegistry& cvars = Arcane::CVarRegistry::Get());
}
