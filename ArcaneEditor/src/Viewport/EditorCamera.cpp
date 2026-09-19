#include "Viewport/EditorCamera.hpp"

#include <Arcane/Scene/Components.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Arcane::Editor
{
    // ---- camera math ------------------------------------------------------

    namespace
    {
        glm::vec3 DirFrom(float yawDeg, float pitchDeg) noexcept   // pivot -> eye direction
        {
            const float y = glm::radians(yawDeg), p = glm::radians(pitchDeg);
            return { std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y) };
        }
        float PixelsPerMetreAtPivot(const Orbit3D& o, glm::uvec2 vp) noexcept
        {
            const float halfH = o.distance * std::tan(glm::radians(o.fovYDeg) * 0.5f);
            return halfH > 0.0f ? float(vp.y) * 0.5f / halfH : 0.0f;
        }
        // UE's shape (bUseDistanceScaledCameraSpeed: min(distance / 1000 uu, 1000),
        // NO floor, and OFF by default). Ours is ON by default -- it is what makes
        // "F, then fly" feel right at every zoom -- with a 0.1 floor so a camera
        // parked on its pivot can still move. The floor is ours, not UE's.
        float DistanceScale(float distance) noexcept
        {
            return std::clamp(distance / 10.0f, 0.1f, 1000.0f);
        }
    }

    ViewTransform EditorCamera::Resolve(glm::uvec2 viewport) const noexcept
    {
        if (mode == ViewMode::TwoD)
            return ViewTransform::Orthographic(ortho.center, ortho.halfHeight, viewport);
        return ViewTransform::Perspective(Eye(), orbit.pivot, glm::vec3(0, 1, 0), orbit.fovYDeg, viewport, kNearZ, kFarZ);
    }
    glm::vec3 EditorCamera::Eye() const noexcept { return orbit.pivot + DirFrom(orbit.yawDeg, orbit.pitchDeg) * orbit.distance; }
    glm::vec3 EditorCamera::Forward() const noexcept { return -DirFrom(orbit.yawDeg, orbit.pitchDeg); }
    glm::vec3 EditorCamera::Right() const noexcept { return glm::normalize(glm::cross(Forward(), glm::vec3(0, 1, 0))); }
    glm::vec3 EditorCamera::Up() const noexcept { return glm::cross(Right(), Forward()); }

    void EditorCamera::Pan2D(glm::vec2 d, glm::uvec2 vp) noexcept
    {
        if (!(vp.y > 0u)) return;
        const float ppm = float(vp.y) * 0.5f / ortho.halfHeight;   // px per metre
        ortho.center += glm::vec2(-d.x, d.y) / ppm;                  // screen y down -> world y up
    }
    void EditorCamera::ZoomAt2D(glm::vec2 screenPos, float ticks, glm::uvec2 vp) noexcept
    {
        if (!(vp.x > 0u) || !(vp.y > 0u)) return;
        const float next = std::clamp(ortho.halfHeight / std::pow(kWheelStep, ticks), kMinHalfHeight, kMaxHalfHeight);
        if (!(next > 0.0f) || next == ortho.halfHeight) return;
        const glm::vec2 anchored = glm::vec2(Resolve(vp).ScreenToRay(screenPos).origin);   // world under the cursor (ortho: z ignored)
        ortho.halfHeight = next;
        const float ppm = float(vp.y) * 0.5f / next;
        ortho.center.x = anchored.x - (screenPos.x - float(vp.x) * 0.5f) / ppm;
        ortho.center.y = anchored.y + (screenPos.y - float(vp.y) * 0.5f) / ppm;
    }
    void EditorCamera::Look(glm::vec2 d) noexcept
    {
        const glm::vec3 eye = Eye();
        orbit.yawDeg   -= d.x * 0.2f;
        orbit.pitchDeg  = std::clamp(orbit.pitchDeg + d.y * 0.2f, -90.0f + 1e-3f, 90.0f - 1e-3f);
        orbit.pivot = eye - DirFrom(orbit.yawDeg, orbit.pitchDeg) * orbit.distance;   // the eye stays put
    }
    // 0.2 deg per pixel for BOTH Look and Orbit: UE's one MouseSensitivty
    // setting (default .2) feeds free-look and orbit alike
    // (EditorViewportClient.cpp ConvertMovementToDragRot / ...OrbitDragRot).
    void EditorCamera::Orbit(glm::vec2 d) noexcept
    {
        orbit.yawDeg   -= d.x * 0.2f;
        orbit.pitchDeg  = std::clamp(orbit.pitchDeg + d.y * 0.2f, -90.0f + 1e-3f, 90.0f - 1e-3f);
    }
    void EditorCamera::Fly(glm::vec3 local, float dt, bool boost) noexcept
    {
        if (!(dt > 0.0f)) return;
        const float speed = kBaseFlySpeed * speedScalar * DistanceScale(orbit.distance) * (boost ? 2.0f : 1.0f);
        const glm::vec3 delta = (Right() * local.x + glm::vec3(0, 1, 0) * local.y + Forward() * local.z) * (speed * dt);
        orbit.pivot += delta;   // eye = pivot + dir*distance, so the eye moves by the same delta
    }
    // GRAB-style: the world follows the cursor, in the camera's plane. UE's
    // default is the opposite sign (camera-relative, bInvertMiddleMousePan
    // false: the camera moves with the cursor); the grab sense matches our 2D
    // pan, which is why it is chosen.
    void EditorCamera::Pan3D(glm::vec2 d, glm::uvec2 vp) noexcept
    {
        const float ppm = PixelsPerMetreAtPivot(orbit, vp);
        if (!(ppm > 0.0f)) return;
        orbit.pivot += (Right() * -d.x + Up() * d.y) / ppm;
    }
    // MULTIPLICATIVE about the pivot, so the eye can never reach or pass it.
    // UE's OnDollyPerspectiveCamera is ADDITIVE along the view vector (~0.96 m
    // per notch at its default scroll speed) and does not move the LookAt, so
    // a UE dolly can cross the pivot; the pivot-relative form is Unity's, and
    // it is what keeps a later orbit sane after a dolly. Deliberate divergence.
    void EditorCamera::Dolly(float ticks) noexcept
    {
        orbit.distance = std::clamp(orbit.distance / std::pow(kWheelStep, ticks), kMinDistance, kMaxDistance);
    }
    // UE's wheel-while-flying step is additive +-10 % (down = x0.9); a symmetric
    // x1.1 / /1.1 keeps up-then-down a no-op. Limits are ours.
    void EditorCamera::AdjustSpeed(float ticks) noexcept
    {
        speedScalar = std::clamp(speedScalar * std::pow(1.1f, ticks), 0.01f, 100.0f);
    }
    void EditorCamera::Frame(const FramingBounds& b, glm::uvec2 vp) noexcept
    {
        if (!b.Valid() || !(vp.x > 0u) || !(vp.y > 0u)) return;
        const glm::vec3 lo = glm::min(b.min, b.max), hi = glm::max(b.min, b.max);
        const glm::vec3 centre = (lo + hi) * 0.5f, extent = hi - lo;
        if (mode == ViewMode::TwoD)
        {
            ortho.center = glm::vec2(centre);
            float halfH = 0.0f;
            if (extent.y > 0.0f) halfH = extent.y * 0.5f / kFrameFill;
            if (extent.x > 0.0f) { const float aspect = float(vp.x) / float(vp.y); halfH = std::max(halfH, extent.x * 0.5f / kFrameFill / aspect); }
            if (halfH > 0.0f) ortho.halfHeight = std::clamp(halfH, kMinHalfHeight, kMaxHalfHeight);
            return;
        }
        orbit.pivot = centre;
        const float radius = std::max(glm::length(extent) * 0.5f, 0.05f);
        const float aspect = float(vp.x) / float(vp.y);
        const float tanHalf = std::tan(glm::radians(orbit.fovYDeg) * 0.5f) * std::min(aspect, 1.0f);
        orbit.distance = std::clamp(radius / tanHalf / kFrameFill, kMinDistance, kMaxDistance);
    }
    void EditorCamera::CentreOrigin() noexcept { ortho.center = glm::vec2(0.0f); }
    glm::vec3 EditorCamera::FocusPoint() const noexcept { return mode == ViewMode::TwoD ? glm::vec3(ortho.center, 0.0f) : orbit.pivot; }

    // ---- framing bounds ---------------------------------------------------

    namespace
    {
        void Grow(FramingBounds& b, const Aabb& box) noexcept
        {
            if (b.count == 0) { b.min = box.min; b.max = box.max; }
            else              { b.min = glm::min(b.min, box.min); b.max = glm::max(b.max, box.max); }
            ++b.count;
        }
    }

    // F3 (spec s2.3): framing is a CONSUMER of WorldBounds -- the same box the
    // renderer culls with and the pick pass emits, so the three can never
    // disagree. A bare node (no WorldBounds) frames as its position.
    FramingBounds SelectionFramingBounds(Astra::Registry& reg,
                                         std::span<const Astra::Entity> entities)
    {
        FramingBounds b;
        for (Astra::Entity e : entities)
        {
            // No WorldTransform => a dead handle or a non-spatial node: there is
            // no position to frame, so it contributes nothing (not even a count).
            const WorldTransform* world = std::as_const(reg).GetComponent<WorldTransform>(e);
            if (!world)
                continue;
            if (const WorldBounds* wb = std::as_const(reg).GetComponent<WorldBounds>(e))
                Grow(b, wb->box);
            else
            {
                const glm::vec3 p(world->matrix[3]);   // a non-drawn node frames as its bare position
                Grow(b, Aabb{ p, p });
            }
        }
        return b;
    }

    FramingBounds SceneFramingBounds(Astra::Registry& reg)
    {
        FramingBounds b;
        // The SAME box the CPU visible set and the GPU scene rows read, so
        // "frame everything" frames exactly what is on screen.
        reg.CreateView<const WorldBounds, Astra::Not<Hidden>>().ForEach(
            [&](Astra::Entity, const WorldBounds& wb) { Grow(b, wb.box); });
        return b;
    }
}
