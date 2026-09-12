#pragma once
// The overlay decision (spec 2026-09-11-physics-2d-wiring s6.3), pure so it
// is testable device-less and the ImGui frame has nothing to decide:
//   * selected-body outline -- ALWAYS in Edit mode when the primary selection
//     carries a live body (the authoring feedback; Unity's collider gizmo);
//   * whole world (outlines + contacts) -- only behind View -> Physics Overlay,
//     Edit and Play alike; session state, never persisted (ruling R5).
namespace Arcane::Editor
{
    struct PhysicsOverlayPlan
    {
        bool draw       = false;
        bool wholeWorld = false;
    };

    [[nodiscard]] constexpr PhysicsOverlayPlan PlanPhysicsOverlay(bool inPlayMode, bool overlayToggled,
                                                                  bool hasSelectedBody) noexcept
    {
        PhysicsOverlayPlan p;
        p.wholeWorld = overlayToggled;
        p.draw       = overlayToggled || (!inPlayMode && hasSelectedBody);
        return p;
    }
}
