#pragma once

// Pure predicates for arbitrating viewport pointer input between the editor
// (gizmo/pick), the game's in-viewport debug ImGui, and the plugin's gameplay.
// No ImGui/GPU dependency -> unit-testable with neither (see
// ArcaneTests/src/ViewportImGuiInputTest.cpp). Consumed by EditorApp.

namespace Arcane::Editor
{
    // True when the game's viewport debug UI owns the pointer this frame, so the
    // editor gizmo/pick AND the plugin's gameplay input must be suppressed for it.
    // Only in Play (the game UI runs only then), only when the cursor is inside the
    // viewport, and only when the game context reported WantCaptureMouse.
    inline bool GameUiClaimsPointer(bool isPlaying, bool inViewport,
                                    bool gameWantCaptureMouse) noexcept
    {
        return isPlaying && inViewport && gameWantCaptureMouse;
    }
}
