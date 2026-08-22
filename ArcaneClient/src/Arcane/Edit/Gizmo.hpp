#pragma once

// Arcane/Edit: 2D transform gizmo core (ARCANE_API, editor-free, STATELESS).
// Pure functions over value inputs: HitTest (which handle is under the cursor),
// ApplyDrag (new transform from the drag start), Draw (screen-constant geometry
// into a Batcher2D). Arcane Editor owns all interaction state and consumes these.

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

    // Decoupled from Scene so Edit/Gizmo has no Scene dependency; Arcane Editor maps
    // Transform <-> this.
    struct GizmoTransform
    {
        glm::vec2 position{0.0f, 0.0f};
        float     rotation = 0.0f;      // radians
        glm::vec2 scale{1.0f, 1.0f};
    };

    // World<->screen for the viewport. MIRRORS Arcane::PickView (Render/PickEmit.hpp)
    // and the engine's single canonical transform: screen_px = world * worldToScreenScale
    // + cameraOffset (NO Y-flip, NO centering). Fill from the viewport camera:
    // cameraOffset = Runtime::CameraOffset(), worldToScreenScale = Runtime::CameraZoom().
    struct GizmoView
    {
        glm::vec2 cameraOffset{0.0f, 0.0f};       // screen-space translation, canvas px
        float     worldToScreenScale = 1.0f;      // px per world-meter (== Runtime::CameraZoom())
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

    // A drag's effect on the PRIMARY, expressed so it can be replayed onto the
    // rest of a multi-selection. `translate` is a shared world delta;
    // `rotate`/`scale` act about `pivot`, so a group rotate ORBITS the other
    // members rather than spinning each in place.
    struct GizmoGroupDelta
    {
        glm::vec2 translate{0.0f, 0.0f};
        float     rotate = 0.0f;        // radians
        glm::vec2 scale{1.0f, 1.0f};    // ratio, component-wise
        glm::vec2 pivot{0.0f, 0.0f};    // the primary's PRE-drag position
    };

    // Delta from the primary's pre-drag pose to its post-drag pose. A start
    // scale component under 1e-6 yields a ratio of 1 on that axis instead of
    // infinity.
    ARCANE_API GizmoGroupDelta MakeGroupDelta(const GizmoTransform& start,
                                              const GizmoTransform& end);

    // Replay a group delta onto a member's PRE-drag pose. Replaying onto the
    // primary's own start reproduces ApplyDrag's result, so callers may apply
    // this uniformly across the whole selection without special-casing.
    ARCANE_API GizmoTransform ApplyGroupDelta(const GizmoTransform& t,
                                              const GizmoGroupDelta& d);

    // The PLANAR slice of a TRS mat4 (the shape Transform::ToMatrix produces)
    // split into position/rotation/scale. Assumes no shear -- true for any
    // product of TRS matrices with non-negative scale, which is what the scene
    // graph builds. A zero-length basis axis yields scale 0 on that axis and
    // leaves the rotation taken from the other axis.
    //
    // Task 3 (F1) widened the matrix to a mat4; GizmoTransform stays 2D because
    // THE GIZMO STAYS 2D (making it 3D is F4). So this reads only the XY block
    // and the Z-axis turn, and everything out of that plane -- position.z,
    // scale.z, any tilt -- is DROPPED. Round-tripping a pose through
    // Decompose->Compose is therefore lossy for a non-planar entity, which is
    // why EditorApp's write-back re-merges the components the gizmo cannot
    // express instead of assigning the decomposed pose wholesale.
    ARCANE_API GizmoTransform DecomposeTRS(const glm::mat4& m);

    // Inverse of DecomposeTRS: the planar pose embedded in a mat4 at z = 0 with
    // unit Z scale. Identical to Transform::ToMatrix for the equivalent
    // position/rotation/scale (pinned in GizmoTest.cpp).
    ARCANE_API glm::mat4 ComposeTRS(const GizmoTransform& t);
}
