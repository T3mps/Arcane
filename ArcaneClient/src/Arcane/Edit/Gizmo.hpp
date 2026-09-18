#pragma once

// Arcane/Edit: THE transform gizmo (ARCANE_API, editor-free, STATELESS, ONE
// code path for every view). Pure functions over value inputs: HitTest (which
// handle is under the cursor -- in PIXELS on the projected handle geometry, so
// a projection change cannot change what is grabbable), ApplyDrag (new
// transform from the drag start -- RAY-based through ViewTransform::ScreenToRay,
// so orthographic and perspective share the math and differ only in the ray
// constructor: Unreal's FViewportCursorLocation split), Draw (overlay pixels,
// top layer, no depth: ImGuizmo's posture, through a host-owned GizmoDrawSink
// since ABI 34). The editor owns all interaction state. (F4 spec s7.2, R9;
// landed 2026-09-17, ABI 33.)

#include <Arcane/Base/Api.hpp>
#include <Arcane/Scene/ViewTransform.hpp>   // ViewTransform, Ray

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>

namespace Arcane
{
    // Where Draw's pixels go. A PIXEL sink, deliberately not Batcher2D: the
    // scene batch is painted BEFORE the mesh pass (F5's compositing contract),
    // so a gizmo drawn into it disappears inside any mesh whose silhouette
    // covers the handles -- the desk finding of 2026-09-18. Unreal draws its
    // widget in SDPG_Foreground (UnrealWidgetRender.cpp: every primitive),
    // i.e. after the world with depth cleared; ImGuizmo draws into ImGui's
    // draw list. This interface is that posture: the host owns a sink that
    // paints over the finished frame (the editor's viewport chrome), and the
    // gizmo core stays editor-free and device-free. Coordinates are viewport
    // pixels (y down), colours linear RGBA in [0,1].
    struct ARCANE_API GizmoDrawSink
    {
        virtual ~GizmoDrawSink() = default;
        virtual void Line(glm::vec2 a, glm::vec2 b, float thickness, glm::vec4 rgba) = 0;
        virtual void Triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec4 rgba) = 0;
        virtual void Rect(glm::vec2 pos, glm::vec2 size, glm::vec4 rgba) = 0;   // axis-aligned, filled
    };

    enum class GizmoMode  { Translate, Rotate, Scale };
    enum class GizmoSpace { World, Local };

    // Translate: X/Y/Z arrows, XY/YZ/XZ plane squares, Center = camera-plane
    // free move. Rotate: X/Y/Z rings + Screen (the camera-facing ring).
    // Scale: X/Y/Z boxes + Center = uniform.
    enum class GizmoAxis : std::uint8_t { None, X, Y, Z, XY, YZ, XZ, Center, Screen };

    // Decoupled from Components so Edit/Gizmo has no Components/Transform
    // dependency (ViewTransform is the one Scene header it takes); Arcane Editor
    // maps Transform <-> this (DecomposeTRS / ComposeTRS below).
    struct GizmoTransform
    {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};   // identity; glm's ctor is (w, x, y, z)
        glm::vec3 scale{1.0f};
    };

    // Which handles exist this frame -- HOST-SIDE. The 2D view is NOT a gizmo
    // mode. It is the host hiding the handles that leave the XY plane --
    // Translate: the Z arrow and the YZ/XZ squares; Rotate: the X and Y rings
    // and the screen ring; Scale: the Z box -- and nothing else changes: a
    // masked-out handle cannot be hit, is not drawn, and the drags that remain
    // leave Z exactly where it was (an axis drag along X or Y has no Z
    // component; a plane drag in XY zeroes its normal component explicitly).
    // The user's ruling 2026-09-17: the old 2D path is deleted, not kept as a
    // fast path.
    struct ARCANE_API GizmoHandleMask   // exported: the members live in Arcane.dll
    {
        std::uint16_t bits = 0xFFFF;   // bit i <=> GizmoAxis(i) exists

        static GizmoHandleMask All() noexcept;
        static GizmoHandleMask Planar(GizmoMode mode) noexcept;   // Translate: X Y XY Center; Rotate: Z; Scale: X Y Center

        bool Has(GizmoAxis a) const noexcept;
        void Set(GizmoAxis a, bool on) noexcept;
    };

    struct GizmoSnap
    {
        bool  enabled = false;   // Ctrl held during the drag
        float translate = 0.5f;  // metres
        float rotationDeg = 15.0f;
        float scale = 0.1f;
    };

    // Screen-constant sizing, Unreal's rule (UnrealWidget.cpp): the handle
    // radius in world units is kAxisLenPx x the gizmo-size setting x this --
    // the clip-space w of the point over the projection's vertical scale and
    // the viewport height. Collapses to the orthographic zoom in 2D (w = 1),
    // grows with distance in perspective, so a handle is always the same size
    // on screen and an axis pointing at the camera foreshortens the way a real
    // object would.
    ARCANE_API float WorldUnitsPerPixel(const ViewTransform& view, glm::vec3 worldPoint) noexcept;

    // The parameter t along the LINE (origin + t * dir, dir unit) closest to
    // the ray; the parallel case projects the ray origin onto the line. The
    // axis drag: the difference of this before and after is the slide.
    ARCANE_API float ClosestLineParam(glm::vec3 lineOrigin, glm::vec3 lineDir, const Ray& ray) noexcept;

    // Where the ray meets the plane; nullopt when grazing or when the plane is
    // behind the ray. The plane and rotate drags.
    ARCANE_API std::optional<glm::vec3> RayPlane(const Ray& ray, glm::vec3 planePoint, glm::vec3 planeNormal) noexcept;

    // Which handle is under mouseScreen (None if off-gizmo), in pixels on the
    // projected geometry. Centre wins on overlap; rings are tried most
    // camera-facing first (an edge-on ring is a line through the pivot).
    ARCANE_API GizmoAxis HitTest(GizmoMode mode, GizmoSpace space,
                                 const GizmoTransform& t, const ViewTransform& view,
                                 GizmoHandleMask handles, float sizeScale,
                                 glm::vec2 mouseScreen);

    // Screen-constant gizmo geometry for the current state; hovered/active
    // brighten. Pixels into the host's foreground sink (see GizmoDrawSink):
    // over everything, no depth. The plane handles are Unreal's L-corners
    // (two bars along the two spanning axes, each in that axis's colour);
    // the filled square between them is the hit region and is painted only
    // while hovered/active.
    ARCANE_API void Draw(GizmoDrawSink& sink, GizmoMode mode, GizmoSpace space,
                         const GizmoTransform& t, const ViewTransform& view,
                         GizmoHandleMask handles, float sizeScale,
                         GizmoAxis hovered, GizmoAxis active);

    // New transform, computed from `start` (no accumulation drift). Ray-based:
    // the same math in every projection.
    ARCANE_API GizmoTransform ApplyDrag(GizmoMode mode, GizmoSpace space, GizmoAxis axis,
                                        const GizmoTransform& start, const ViewTransform& view,
                                        glm::vec2 mouseStartScreen, glm::vec2 mouseCurScreen,
                                        const GizmoSnap& snap);

    // A drag's effect on the PRIMARY, expressed so it can be replayed onto the
    // rest of a multi-selection. `translate` is a shared world delta;
    // `rotate`/`scale` act about `pivot`, so a group rotate ORBITS the other
    // members rather than spinning each in place.
    struct GizmoGroupDelta
    {
        glm::vec3 translate{0.0f};
        glm::quat rotate{1.0f, 0.0f, 0.0f, 0.0f};   // the WORLD turn
        glm::vec3 scale{1.0f};                       // ratio, component-wise
        glm::vec3 pivot{0.0f};                       // the primary's PRE-drag position
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

    // Scale from the column lengths, rotation from the normalised basis
    // (polar-decomposition-free; the same read ActivePerspectiveSceneCamera
    // does). A NEGATIVE determinant is a mirror: one scale component is
    // negative, and the decomposition cannot know which the author chose -- it
    // negates X, UE's convention (FMatrix::ExtractScaling / GetScaleVector).
    // The editor re-homes it with WithMirrorOn onto the axis the entity's
    // authored scale carries, so a Y-mirrored sprite dragged once does not come
    // back as an X-mirror with a half turn in its Inspector. Assumes no shear
    // (any product of TRS matrices). A zero-length column yields scale 0 on
    // that axis and the identity direction for it.
    //
    // For a MIRRORED entity, the half turn above folds into `rotation`, so a
    // GizmoSpace::Local draw/hit-test's arrows point along the DECOMPOSED
    // basis, not the authored one -- the X arrow of a Y-mirrored sprite points
    // the "other" way. That is Unreal's own behaviour for a mirrored actor's
    // local gizmo, not a defect here.
    ARCANE_API GizmoTransform DecomposeTRS(const glm::mat4& m);

    // translate * rotate * scale: identical to Transform::ToMatrix for the
    // equivalent position/rotation/scale (pinned in GizmoTest.cpp).
    ARCANE_API glm::mat4 ComposeTRS(const GizmoTransform& t);

    // Moves a negative X scale onto `axis` (1 = Y, 2 = Z) WITHOUT changing the
    // matrix: S' = S.D and R' = R.D with D the diagonal flipping X and `axis`
    // -- a proper rotation (a half turn about the third axis), so the pose is
    // identical. Identity when scale.x >= 0 or axis is 0.
    ARCANE_API GizmoTransform WithMirrorOn(const GizmoTransform& t, int axis);
}
