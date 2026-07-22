#include "ViewportInput.hpp"

namespace Arcane::Editor
{
    bool ToViewportLocal(ViewportRect r, float mx, float my, float& lx, float& ly) noexcept
    {
        lx = mx - r.x;
        ly = my - r.y;
        return lx >= 0.0f && ly >= 0.0f && lx < r.w && ly < r.h;
    }
}
