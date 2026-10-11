#pragma once

// The EDITOR's own viewport camera (F4 spec s4): pure state + pure math, no
// ImGui and no engine calls, so the [editor][camera] units drive it headlessly
// (same split as ViewportInput.hpp / SceneSession.hpp; the host performs every
// effect). It is NEVER a scene entity.
//
// WHY the editor owns a camera at all: ClientRuntime::SetView is the PLUGIN's
// seam (ArcaneRuntime: "plugin drives via ClientRuntime::SetView, default
// identity if it never does"), and a project whose game module never calls it
// gets the identity view. An authoring tool cannot depend on the game
// implementing navigation, so EditorApp drives this camera from viewport input
// and pushes it in Edit mode; in Play the scene camera wins so the game looks
// like the game.
//
// Unreal's model -- TWO PERSISTED TRANSFORMS SELECTED BY VIEW MODE
// (FEditorViewportClient::GetViewTransform() returns ViewTransformPerspective
// or ViewTransformOrthographic), so switching restores the other mode's
// framing instead of reprojecting one camera. Resolve(viewport) is the ONE
// producer the host pushes through ClientRuntime::SetView.
//
// 2D mode = the XY-plane orthographic view down -Z, near/far symmetric about
// z = 0 (+-1000 m, ViewTransform::Orthographic's defaults) so 2D content at any
// authored Z is visible. Ortho2D is { center, halfHeight } in WORLD units and
// NOTHING ELSE -- no roll: ViewTransform::AsAffine2D() is nullopt for any
// rotated orthographic view, and the gizmo / pick / physics overlays are gated
// on it. Right-drag pans (grab-style, the world follows the cursor), the wheel
// zooms multiplicatively about the cursor (editor.camera.wheelZoomStep; UE:
// bCenterZoomAroundCursor), halfHeight clamps to
// editor.camera.ortho{Min,Max}HalfHeight (UE's MIN_/MAX_ORTHOZOOM). F frames
// the selection, Home the scene.
//
// Perspective mode follows Unreal's input model: right-drag mouselooks about
// the EYE (Look; the pivot moves with it), Alt+left-drag orbits the stored
// PIVOT (Orbit), WASD/QE fly along the camera axes (Fly; Shift x2 boost is
// ours, Unity's), middle-drag pans in the view plane (Pan3D), the wheel
// dollies along the view vector (Dolly), and the wheel WHILE flying steps the
// speed scalar (AdjustSpeed). Fly and pan speed scale with the distance to the
// pivot (UE's bUseDistanceScaledCameraSpeed shape, ON here with a floor)
// times the persisted speedScalar (UE's CameraSpeedScalar). Pitch clamps to
// +-90 deg minus an epsilon (UE's pitch lock). F sets the pivot to the bounds
// centre and solves the distance from the AABB's half-diagonal and the fov
// (UE's FocusViewportOnBox). The pivot is always the STORED one F sets
// (Unity's model), never "orbit around selection".
//
// Persistence, the 3D input block and the --view-mode flag are Task 7's; this
// header carries only the state and the math. Every tunable is an
// editor.camera.* cvar (EditorCameraSettings, settings S6-30): each method
// reads the published snapshot once per call.

#include "Settings/EditorViewportSettings.hpp"   // EditorCameraSettings

#include <Arcane/Scene/ViewTransform.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

#include <Astra/Entity/Entity.hpp>

namespace Astra { class Registry; }

namespace Arcane::Editor
{
    // Persisted as int; append only.
    enum class ViewMode : std::uint8_t
    {
        TwoD        = 0,
        Perspective = 1,
    };

    // The 2D transform: the XY-plane orthographic view down -Z. `halfHeight`
    // is half the viewport's vertical extent in world METRES (MKS), so 5 m
    // shows a 10 m tall slice of the scene on first open. No roll (see the
    // file comment).
    struct Ortho2D
    {
        glm::vec2 center{0.0f, 0.0f};
        float     halfHeight = EditorCameraSettings{}.default2DHalfHeight;
    };

    // The perspective transform: the eye sits `distance` from `pivot` along
    // the (yaw, pitch) direction and looks at it. yaw 0 / pitch 0 puts the eye
    // on +Z looking down -Z (the 2D view's axis); the default 3/4 view is a
    // -30 deg yaw and a 30 deg pitch above the plane.
    struct Orbit3D
    {
        glm::vec3 pivot{0.0f, 0.0f, 0.0f};
        float     yawDeg   = EditorCameraSettings{}.default3DYaw;
        float     pitchDeg = EditorCameraSettings{}.default3DPitch;
        float     distance = EditorCameraSettings{}.default3DDistance;
        float     fovYDeg  = EditorCameraSettings{}.fovYDeg;
    };

    // World-space AABB for framing (3D; the 2D camera fits its XY projection).
    // `count` is the number of entities that actually contributed, which is
    // what separates "nothing framable here" (count 0 -- the caller must do
    // nothing) from "a real but zero-extent box" (count > 0 with min == max,
    // e.g. one transform-only node).
    struct FramingBounds
    {
        glm::vec3   min{0.0f, 0.0f, 0.0f};
        glm::vec3   max{0.0f, 0.0f, 0.0f};
        std::size_t count = 0;

        [[nodiscard]] bool Valid() const noexcept { return count > 0; }
    };

    struct EditorCamera
    {
        ViewMode mode = ViewMode::TwoD;
        Ortho2D  ortho;
        Orbit3D  orbit;
        float    speedScalar = EditorCameraSettings{}.speedScalar;   // editor.camera.speedScalar, synced per frame; the wheel steps it

        // The ONE view every consumer reads, for the current mode.
        [[nodiscard]] ViewTransform Resolve(glm::uvec2 viewport) const noexcept;

        // ---- 2D ------------------------------------------------------------
        // RMB drag: the world follows the cursor 1:1 (grab-style), so a
        // screen-px delta becomes a world-metre shift at the current scale.
        void Pan2D(glm::vec2 screenDelta, glm::uvec2 viewport) noexcept;

        // Wheel zoom ANCHORED at screenPos: the world point under the cursor is
        // still under the cursor afterwards. wheelTicks is the accumulated
        // per-frame wheel delta (InputSnapshot::wheelY; +y = in). A tick count
        // that would leave the zoom unchanged (0, or more of the same at a
        // clamp) is a no-op, so a pinned camera cannot drift.
        void ZoomAt2D(glm::vec2 screenPos, float wheelTicks, glm::uvec2 viewport) noexcept;

        // ---- 3D ------------------------------------------------------------
        // Right-drag: yaw/pitch about the EYE (the pivot moves so the eye stays
        // put); editor.camera.lookSensitivity deg per pixel.
        void Look(glm::vec2 mouseDeltaPx) noexcept;
        // Alt+left-drag: yaw/pitch about the PIVOT; editor.camera.orbitSensitivity
        // deg per pixel.
        void Orbit(glm::vec2 mouseDeltaPx) noexcept;
        // WASD/QE: move eye AND pivot along the camera's axes (x right, y
        // world up, z forward) at baseFlySpeed * speedScalar * distance scale,
        // x boostMultiplier when boosted.
        void Fly(glm::vec3 localAxis, float dtSeconds, bool boost) noexcept;
        // Middle-drag: grab-style pan in the camera plane (pivot and eye move).
        void Pan3D(glm::vec2 screenDelta, glm::uvec2 viewport) noexcept;
        // Wheel: multiplicative distance change about the pivot.
        void Dolly(float wheelTicks) noexcept;
        // Wheel while flying: speedScalar x speedWheelStep per tick, clamped to the
        // editor.camera.speedScalar cvar's range (the caller writes the cvar).
        void AdjustSpeed(float wheelTicks);

        [[nodiscard]] glm::vec3 Eye() const noexcept;
        [[nodiscard]] glm::vec3 Forward() const noexcept;
        [[nodiscard]] glm::vec3 Right() const noexcept;
        [[nodiscard]] glm::vec3 Up() const noexcept;

        // ---- both ----------------------------------------------------------
        // Mode-aware framing. 2D: centre on the XY box and fit the tighter axis
        // with frameFill (a zero-extent axis cannot imply a scale and is
        // ignored; a point only re-centres). Perspective: pivot = centre,
        // distance = half-diagonal / (tan(fovY/2) * min(aspect, 1)) / frameFill.
        // An invalid bounds or a zero viewport leaves the camera untouched.
        void Frame(const FramingBounds& bounds, glm::uvec2 viewport) noexcept;

        // An empty scene just opened: put the world origin at the 2D centre.
        void CentreOrigin() noexcept;

        // 2D: (center, 0); 3D: the pivot.
        [[nodiscard]] glm::vec3 FocusPoint() const noexcept;
    };

    // A FRESH camera's pose from the published editor.camera.default* cvars
    // (NextWorld: the editor's boot camera and a windowed project switch's
    // reset; a restored ini pose then overrides it). Sets ortho.halfHeight and
    // orbit yaw / pitch / distance only -- mode, centre, pivot, fov and speed
    // are untouched (the --view-mode seed and the per-frame sync own those).
    void ApplyFreshPose(EditorCamera& cam);

    // Bounds over an explicit entity set (Frame Selected). A sprite contributes
    // the AABB of the quad RenderSubmissionSystem draws -- the SAME four world
    // corners, SpriteWorldQuad (its sprite asset's base size, 1x1 m when
    // unresolved, through the full world basis about the asset's pivot; F4 plan
    // 1 T5) -- so framing and rendering cannot disagree. A MeshRenderer whose
    // mesh resolves in the MeshTable contributes its local AABB's eight corners
    // through the world matrix. An entity with a WorldTransform but nothing
    // drawable (no renderer, or a mesh that does not resolve) contributes its
    // position as a zero-extent point, so framing a bare node centres on it
    // instead of doing nothing. Entities with no WorldTransform (and dead
    // handles) are skipped.
    [[nodiscard]] FramingBounds SelectionFramingBounds(Astra::Registry& reg,
                                                       std::span<const Astra::Entity> entities);

    // Bounds over everything VISIBLE in the scene (Frame All / Home): sprites
    // and resolved meshes, Hidden excluded, i.e. exactly what the renderer
    // draws. Bare transform nodes (the SceneRoot at the origin among them)
    // would otherwise drag the box back toward (0,0), and hidden entities
    // would stretch it out to things the user cannot see.
    [[nodiscard]] FramingBounds SceneFramingBounds(Astra::Registry& reg);
}
