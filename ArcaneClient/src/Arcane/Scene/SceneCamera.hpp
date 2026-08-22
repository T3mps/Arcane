#pragma once

// The scene-camera sweep: resolves the ACTIVE Camera entity into the
// screen = world * zoom + offset transform every host already applies
// (RenderContext2D, SceneResources.hpp).
//
// Deliberately a PURE function of (registry, viewport size) rather than a system
// or a Runtime method. Three callers have to agree about the view or the feature
// is pointless -- ArcaneRuntime, play-in-editor, and the editor's camera-rect
// overlay -- and a system would have to be registered by the GAME module (the
// plugin owns Schedulers), which would make a camera in a scene depend on the
// game rebuilding. Being pure also means it is unit-testable with no host at all.

#include <Arcane/Scene/Components.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>   // perspectiveRH_ZO, lookAtRH

#include <optional>

namespace Arcane
{
    struct SceneCameraView
    {
        // The authored facts, for callers that need to draw the camera rather than
        // look through it (the editor's camera-rect overlay).
        glm::vec2 worldCenter{0.0f, 0.0f};   // the camera entity's world position
        float     halfHeight = 0.0f;         // Camera::orthographicSize, meters

        // Derived for the viewport that was asked about.
        glm::vec2 offset{0.0f, 0.0f};        // screen-space translation, canvas px
        float     zoom = 1.0f;               // px per world-meter
    };

    // The active camera's view for a viewW x viewH viewport, or nullopt when the
    // scene has no usable active Camera.
    //
    // A nullopt means "leave the stored camera ALONE" -- NOT "use identity". Every
    // scene authored before cameras existed has no Camera entity, and substituting
    // an identity view would draw those scenes at 1 px per meter in the corner: a
    // black-looking window with no explanation, which is the exact failure this
    // seam exists to end. Unity is explicit for the same reason ("No cameras
    // rendering") instead of silently inventing a viewpoint.
    //
    // outCount, when given, receives the number of ACTIVE cameras found, so a host
    // can warn once that a scene is ambiguous -- the contract the PostProcess sweep
    // already set.
    inline std::optional<SceneCameraView> ActiveSceneCamera(Astra::Registry& reg,
                                                            float viewW, float viewH,
                                                            int* outCount = nullptr)
    {
        int       found = 0;
        glm::vec2 center{0.0f, 0.0f};
        float     halfH = 0.0f;

        // Viewed on Camera ALONE, with the pose fetched per hit, rather than
        // CreateView<Camera, WorldTransform>: WorldTransform is DERIVED, and on the
        // frame an entity first gets a Camera it may not have one yet (the
        // propagation pass that materialises it runs at a different point in the
        // frame for each host). A view over both would silently skip the camera for
        // that frame and the scene would flicker to the previous view.
        reg.CreateView<Camera>().ForEach(
            [&](Astra::Entity e, Camera& cam)
        {
            // Task 5 (Phase 4): the projection guard is a no-op for every scene
            // that predates the field (Camera::projection defaults to
            // Orthographic), which is what keeps this function's output
            // byte-identical for all of those. It exists so a Perspective camera
            // -- whose orthographicSize is just whatever default it never
            // touched -- cannot be swept up here and turned into a bogus 2D
            // offset+zoom view; it is ActivePerspectiveSceneCamera's below.
            if (!cam.active || cam.projection != CameraProjection::Orthographic || cam.orthographicSize <= 0.0f)
                return;
            if (found++ != 0)
                return;   // first active wins; keep counting for the caller's warning

            halfH = cam.orthographicSize;
            // Task 3 (F1): the world matrix is a mat4, so the translation is
            // column 3 (it was column 2). The ORTHOGRAPHIC path is otherwise
            // untouched and stays glm::vec2-shaped end to end -- an ortho camera
            // frames the XY plane and has no use for the entity's Z.
            if (const WorldTransform* wt = reg.GetComponent<WorldTransform>(e))
                center = glm::vec2(wt->matrix[3].x, wt->matrix[3].y);   // translation column
            else if (const Transform* lt = reg.GetComponent<Transform>(e))
                center = glm::vec2(lt->position);   // not propagated yet: local IS world for a root
        });

        if (outCount)
            *outCount = found;
        if (found == 0 || viewH <= 0.0f || halfH <= 0.0f)
            return std::nullopt;

        SceneCameraView v;
        v.worldCenter = center;
        v.halfHeight  = halfH;
        v.zoom        = (viewH * 0.5f) / halfH;
        // The camera's world position lands at the viewport CENTRE, which is what
        // makes zoom resolution-independent: screen = world * zoom + offset.
        v.offset      = glm::vec2(viewW, viewH) * 0.5f - center * v.zoom;
        return v;
    }

    // Task 5 (Phase 4): the perspective sibling. SceneCameraView above is
    // glm::vec2-shaped end to end (a 2D screen = world * zoom + offset
    // transform) and none of that math means anything to a lens, so a
    // Perspective camera gets its own return type -- a 4x4 view and a 4x4
    // projection -- rather than growing matrix fields no ortho caller reads.
    //
    // DEPTH CONVENTION: [0,1], not [-1,1] and not reverse-Z. glm defaults to
    // OpenGL's [-1,1] convention, and GLM_FORCE_DEPTH_ZERO_TO_ONE is not
    // defined anywhere in this tree, so PerspectiveProjection states [0,1] at
    // the call site via glm's explicit *_ZO entry point instead -- a tree-wide
    // define would also silently change every OTHER glm projection call,
    // ortho included, the moment one is added. Reverse-Z is a distinct,
    // separately-decided-against choice (it would additionally flip which of
    // near/far maps to NDC z=0 and swap the depth compare op); PerspectiveCameraTest.cpp
    // pins today's [0,1], forward-Z mapping explicitly so a future reverse-Z
    // switch is a deliberate decision, not a silent drift.
    //
    // HANDEDNESS: right-handed (the camera looks down -Z; +X is right, +Y is
    // up), matching glm's own RH family (perspectiveRH_*/lookAtRH). When this
    // was written there was no PRE-EXISTING 3D handedness to inherit from the
    // ortho path (Transform/WorldTransform were 2D-only) and sprite.hlsl
    // hardcodes clip.z = 0.0, so nothing upstream fixed a convention. Task 3
    // (F1) made Transform 3D, and it adopts THIS convention rather than
    // competing with it: +Z is toward the viewer, so a planar scene sits at
    // z = 0 and a camera looking down -Z sees it. RH is
    // instead FORCED by this task's own pinned property (PerspectiveCameraTest.cpp,
    // the 90-degree-fov case): a view-space point (z, 0, -z) lands exactly on
    // the RIGHT clip plane at fovY=90 deg/aspect=1 only if points in front of the
    // camera have NEGATIVE view-space Z, i.e. the camera looks down -Z (RH).
    // Under a LH camera (looking down +Z), that same point -- Z = -z < 0 --
    // would be BEHIND the camera and could not land on any clip plane of the
    // visible frustum at all.
    struct PerspectiveCameraView
    {
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
    };

    // The projection half in isolation: a pure numeric function of the lens
    // parameters alone (no Registry, no Camera, no WorldTransform), so its
    // properties can be pinned directly -- aspect ratio's effect on x only,
    // the near/far -> NDC-z mapping, and the 90-degree-fov clip-plane identity
    // above -- without any ECS scaffolding.
    [[nodiscard]] inline glm::mat4 PerspectiveProjection(float fovYDegrees, float aspectRatio,
                                                          float nearZ, float farZ) noexcept
    {
        return glm::perspectiveRH_ZO(glm::radians(fovYDegrees), aspectRatio, nearZ, farZ);
    }

    // The active PERSPECTIVE camera's view for an aspectRatio viewport, or
    // nullopt under the same "leave the stored camera ALONE" contract
    // ActiveSceneCamera documents above. Perspective cameras are swept
    // SEPARATELY from ActiveSceneCamera (which now skips any camera whose
    // projection is not Orthographic -- see its guard above), so a scene never
    // has to answer "which mode is the ambiguous first camera" -- each mode
    // resolves its own first-active-wins independently.
    //
    // VIEW: eye is the entity's world position, resolved by the exact same
    // WorldTransform-then-Transform-fallback rule ActiveSceneCamera uses
    // above (so moving a camera entity moves both lenses the same way),
    // extended into 3D at Z=0. Orientation is fixed at forward=(0,0,-1),
    // up=(0,1,0).
    //
    // Task 3 (F1) NOTE -- Transform now DOES carry a 3D position and a
    // quaternion, so the "no 3D orientation to read" reason this contract was
    // originally written for is gone. It is kept UNCHANGED here anyway, and
    // deliberately: Task 3 is a type widening, and pointing this lens at the
    // camera entity's real pose is a behaviour change that belongs with the
    // task that gives a user a way to AIM it (F4's camera work). Wiring it
    // early would silently re-frame every scene the moment one gained a
    // non-zero Z or a tilt. Today it still looks straight down -Z from the
    // authored XY position, byte-identically.
    inline std::optional<PerspectiveCameraView> ActivePerspectiveSceneCamera(Astra::Registry& reg,
                                                                              float aspectRatio,
                                                                              int* outCount = nullptr)
    {
        int       found = 0;
        glm::vec2 center{0.0f, 0.0f};
        float     fovYDegrees = 60.0f;
        float     nearZ = 0.1f;
        float     farZ  = 1000.0f;

        reg.CreateView<Camera>().ForEach(
            [&](Astra::Entity e, Camera& cam)
        {
            if (!cam.active || cam.projection != CameraProjection::Perspective)
                return;
            if (found++ != 0)
                return;   // first active wins; keep counting for the caller's warning

            fovYDegrees = cam.fovYDegrees;
            nearZ       = cam.nearZ;
            farZ        = cam.farZ;
            if (const WorldTransform* wt = reg.GetComponent<WorldTransform>(e))
                center = glm::vec2(wt->matrix[3].x, wt->matrix[3].y);   // translation column
            else if (const Transform* lt = reg.GetComponent<Transform>(e))
                center = glm::vec2(lt->position);   // not propagated yet: local IS world for a root
        });

        if (outCount)
            *outCount = found;
        if (found == 0 || aspectRatio <= 0.0f || nearZ <= 0.0f || farZ <= nearZ)
            return std::nullopt;

        PerspectiveCameraView v;
        const glm::vec3 eye(center, 0.0f);
        v.view       = glm::lookAtRH(eye, eye + glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        v.projection = PerspectiveProjection(fovYDegrees, aspectRatio, nearZ, farZ);
        return v;
    }
}
