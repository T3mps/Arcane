#pragma once

// The global/viewport keyboard reads of EditorAppFrame's input phase, as pure
// functions of the action map (settings arc S4, spec s7.2), so the routing is
// unit-tested without EditorApp.

#include "Input/EditorActions.hpp"

#include <glm/glm.hpp>

#include <cstdint>

namespace Arcane::Editor
{
    struct GlobalShortcutPresses
    {
        bool undo = false, redo = false;
        bool newScene = false, openScene = false, saveScene = false;
        bool cut = false, copy = false, paste = false, duplicate = false;
        bool closeDocument = false;
        bool perspective = false, ortho2D = false;
        bool frameSelected = false, frameAll = false;
    };
    [[nodiscard]] GlobalShortcutPresses ReadGlobalShortcuts(const EditorActions& actions);

    enum class ViewportTool : std::uint8_t { None, Select, Translate, Rotate, Scale };
    // The last tool pressed this frame (Q, W, E, R order, as before); None while RMB is held (the fly keys share them).
    [[nodiscard]] ViewportTool ReadViewportTool(const EditorActions& actions, bool rmbHeld);
    // The held fly keys as a camera-space axis (+Z forward, +X right, +Y up), not normalized.
    [[nodiscard]] glm::vec3 ReadFlyAxis(const EditorActions& actions);
}
