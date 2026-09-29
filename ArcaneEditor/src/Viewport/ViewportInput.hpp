#pragma once

// Scene-in-a-panel input gating: pure predicates with no ImGui dependency, so
// they are unit-testable headlessly (see ArcaneTests/src/EditorViewportInputTest.cpp,
// source-compiled straight into ArcaneTests). Consumed by EditorApp to decide
// whether the plugin sees live scene input this frame and, if so, where the
// cursor lands in viewport-local pixels.

namespace Arcane::Editor
{
    struct ViewportRect { float x, y, w, h; };

    // Scene input (camera pan/zoom, click-pick) is live only when the Viewport
    // panel owns the cursor: hovered (mouse/wheel) or focused (keys).
    inline bool SceneInputActive(bool hovered, bool focused) noexcept { return hovered || focused; }

    // May an editor shortcut fire this frame? Not in Play, not while ImGui
    // owns the keyboard, not while an Input Actions rebind capture owns it
    // (its completing key would also fire the shortcut: ImGui's
    // WantCaptureKeyboard is false during a capture), and viewport tools
    // only while the Viewport is active.
    inline bool EditorShortcutsLive(bool playMode, bool wantCaptureKeyboard, bool rebindCaptureLive,
                                    bool requireViewportFocus, bool viewportActive) noexcept
    { return !playMode && !wantCaptureKeyboard && !rebindCaptureLive && (!requireViewportFocus || viewportActive); }

    // Map a window-global cursor (mx,my) to viewport-local pixels (lx,ly), origin
    // at the image's top-left. Returns false (and still writes lx/ly) if the cursor
    // is outside the rect. Right/bottom edges are exclusive.
    bool ToViewportLocal(ViewportRect r, float mx, float my, float& lx, float& ly) noexcept;
}
