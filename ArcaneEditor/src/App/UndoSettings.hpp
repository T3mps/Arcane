#pragma once

// The editor's undo tunables (spec 2026-09-30 s2.4; settings S6-33): the
// EditorUndoSettings struct (editor.undo.*, Editor, Pref-P, Live), read here
// and pushed into the CommandStack -- the stack itself never reads a cvar.

#include <Arcane/Edit/CommandStack.hpp>

#include <cstdint>

namespace Arcane::Editor
{
    struct EditorUndoSettings
    {
        std::int32_t maxSteps         = 100;
        std::int32_t byteBudgetMB     = 512;
        std::int32_t spillThresholdKB = 256;
    };

    // Pure: steps as-is, MB and KB to bytes.
    [[nodiscard]] Arcane::UndoLimits ToUndoLimits(const EditorUndoSettings& s);

    // ToUndoLimits of the current published snapshot's block.
    [[nodiscard]] Arcane::UndoLimits ReadUndoLimits();
}
