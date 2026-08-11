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
            if (!cam.active || cam.orthographicSize <= 0.0f)
                return;
            if (found++ != 0)
                return;   // first active wins; keep counting for the caller's warning

            halfH = cam.orthographicSize;
            if (const WorldTransform* wt = reg.GetComponent<WorldTransform>(e))
                center = glm::vec2(wt->matrix[2].x, wt->matrix[2].y);   // translation column
            else if (const Transform* lt = reg.GetComponent<Transform>(e))
                center = lt->position;   // not propagated yet: local IS world for a root
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
}
