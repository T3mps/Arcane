#pragma once

// Scene-in-a-panel input gating: pure predicates with no ImGui dependency, so
// they are unit-testable headlessly (see Tests/src/EditorViewportInputTest.cpp,
// source-compiled straight into ArcaneTests). Consumed by EditorApp to decide
// whether the plugin sees live scene input this frame and, if so, where the
// cursor lands in viewport-local pixels.

namespace Arcane::Editor
{
    struct ViewportRect { float x, y, w, h; };

    // Scene input (camera pan/zoom, click-pick) is live only when the Viewport
    // panel owns the cursor: hovered (mouse/wheel) or focused (keys).
    inline bool SceneInputActive(bool hovered, bool focused) noexcept { return hovered || focused; }

    // Map a window-global cursor (mx,my) to viewport-local pixels (lx,ly), origin
    // at the image's top-left. Returns false (and still writes lx/ly) if the cursor
    // is outside the rect. Right/bottom edges are exclusive.
    bool ToViewportLocal(ViewportRect r, float mx, float my, float& lx, float& ly) noexcept;
}
