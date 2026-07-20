#pragma once

// Arcane/Edit: 2D transform gizmo core (ARCANE_API, editor-free, STATELESS).
// Pure functions over value inputs: HitTest (which handle is under the cursor),
// ApplyDrag (new transform from the drag start), Draw (screen-constant geometry
// into a Batcher2D). Grimoire owns all interaction state and consumes these.

#include <Arcane/Base/Api.hpp>

#include <glm/glm.hpp>

namespace Arcane
{
    class Batcher2D;

    enum class GizmoMode  { Translate, Rotate, Scale };
    enum class GizmoSpace { World, Local };
    // Center = free-move (Translate) / uniform (Scale); in Rotate the ring is the
    // sole handle and is reported as Center (ApplyDrag ignores the axis there).
    enum class GizmoAxis  { None, X, Y, Center };

    // Decoupled from Scene so Edit/Gizmo has no Scene dependency; Grimoire maps
    // LocalTransform <-> this.
    struct GizmoTransform
    {
        glm::vec2 position{0.0f, 0.0f};
        float     rotation = 0.0f;      // radians
        glm::vec2 scale{1.0f, 1.0f};
    };

    // World<->screen for the viewport. Fill from the viewport camera (Grimoire).
    struct GizmoView
    {
        glm::vec2 cameraOffset{0.0f, 0.0f};   // world point at the viewport center
        float     zoom = 1.0f;
        float     pixelsPerMeter = 100.0f;
        glm::vec2 viewportOriginPx{0.0f, 0.0f};
        glm::vec2 viewportSizePx{0.0f, 0.0f};
    };

    struct GizmoSnap
    {
        bool  enabled = false;   // Ctrl held during the drag
        float translate = 0.5f;  // world units grid
        float rotationDeg = 15.0f;
        float scale = 0.1f;
    };

    // Which handle is under mouseScreen (None if off-gizmo). Center-priority.
    ARCANE_API GizmoAxis HitTest(GizmoMode mode, GizmoSpace space,
                                 const GizmoTransform& t, const GizmoView& view,
                                 glm::vec2 mouseScreen);

    // Screen-constant gizmo geometry for the current state; hovered/active brighten.
    ARCANE_API void Draw(Batcher2D& batcher, GizmoMode mode, GizmoSpace space,
                         const GizmoTransform& t, const GizmoView& view,
                         GizmoAxis hovered, GizmoAxis active);

    // New transform, computed from `start` (no accumulation drift).
    ARCANE_API GizmoTransform ApplyDrag(GizmoMode mode, GizmoSpace space, GizmoAxis axis,
                                        const GizmoTransform& start, const GizmoView& view,
                                        glm::vec2 mouseStartScreen, glm::vec2 mouseCurScreen,
                                        const GizmoSnap& snap);
}
