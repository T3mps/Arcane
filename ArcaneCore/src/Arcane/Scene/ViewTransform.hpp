#pragma once

// ViewTransform: THE ONE camera type every consumer reads (F4, spec s3).
// view + projection + viewport. The orthographic scene camera and the editor's
// own camera both produce it; sprites, the mesh pass, picking, the gizmo and
// the physics overlay all consume it. The 2D affine mapping the engine used to
// carry (screen = world * zoom + offset) is its orthographic case, exposed as
// Affine2D for the overlays that still draw in pixels (AsAffine2D below).
//
// CONVENTIONS (pinned in SceneCamera.hpp and PerspectiveCameraTest): right-
// handed, +Y up, camera forward -Z, clip depth [0,1] forward-Z (the *_ZO glm
// entry points). Pixels are y-DOWN; the flip lives in WorldToScreen's NDC ->
// pixel step and nowhere in world space (spec s2: no Y flip in the world).

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>
#include <optional>

namespace Arcane
{
    struct Ray
    {
        glm::vec3 origin{0.0f};
        glm::vec3 direction{0.0f, 0.0f, -1.0f};   // unit
    };

    // The legacy per-pixel overlay mapping, now PER-AXIS so it can carry the
    // Y mirror: pixel = world.xy * scale + offset, with scale.y NEGATIVE for a
    // +Y-up world on a y-down canvas. Overlay code must project POINTS through
    // Point() (never "project the centre, then rotate in screen space with the
    // world angle"): a mirrored map reverses the sense of every angle, which
    // AngleSign() reports for the one place that needs it (a canvas-space
    // rotation in the id pass).
    struct Affine2D
    {
        glm::vec2 offset{0.0f, 0.0f};
        glm::vec2 scale{1.0f, 1.0f};

        [[nodiscard]] glm::vec2 Point(glm::vec2 world) const noexcept { return world * scale + offset; }
        [[nodiscard]] glm::vec2 Unpoint(glm::vec2 pixel) const noexcept { return (pixel - offset) / scale; }
        [[nodiscard]] float     Length(float metres) const noexcept { return std::abs(scale.x) * metres; }
        [[nodiscard]] float     AngleSign() const noexcept { return (scale.x * scale.y) < 0.0f ? -1.0f : 1.0f; }
    };

    struct ViewTransform
    {
        glm::mat4  view{1.0f};
        glm::mat4  projection{1.0f};
        glm::uvec2 viewport{0u, 0u};

        [[nodiscard]] glm::mat4 ViewProjection() const noexcept { return projection * view; }

        // An orthographic projection has no perspective divide: the w row is (0,0,0,1).
        [[nodiscard]] bool IsOrthographic() const noexcept
        {
            return projection[3][3] == 1.0f && projection[2][3] == 0.0f;
        }

        // .xy = pixels (top-left origin, y down), .z = NDC depth in [0,1].
        // A point at (or behind) the eye has w ~ 0 and returns NaN in every
        // component; callers that draw must std::isfinite-check.
        [[nodiscard]] glm::vec3 WorldToScreen(glm::vec3 world) const noexcept
        {
            const glm::vec4 clip = projection * (view * glm::vec4(world, 1.0f));
            if (!(std::abs(clip.w) > 1e-12f))
            {
                const float nan = std::numeric_limits<float>::quiet_NaN();
                return glm::vec3(nan, nan, nan);
            }
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            return glm::vec3((ndc.x * 0.5f + 0.5f) * float(viewport.x),
                             (0.5f - ndc.y * 0.5f) * float(viewport.y),
                             ndc.z);
        }

        // Perspective: origin = the eye, direction through the pixel.
        // Orthographic: origin = the pixel's point on the NEAR plane, direction =
        // the view axis (parallel rays). UE does the same under reversed-Z: its
        // DeprojectScreenToWorld starts the ray at projection-space z = 1 (its
        // near plane) and ends at 0.01 (SceneView.cpp:1510); forward-Z [0,1]
        // puts our near plane at z = 0, hence the 0 below.
        [[nodiscard]] Ray ScreenToRay(glm::vec2 pixel) const noexcept
        {
            const float nx = viewport.x ? (pixel.x / float(viewport.x)) * 2.0f - 1.0f : 0.0f;
            const float ny = viewport.y ? 1.0f - (pixel.y / float(viewport.y)) * 2.0f : 0.0f;
            const glm::mat4 inv = glm::inverse(projection * view);
            glm::vec4 nearP = inv * glm::vec4(nx, ny, 0.0f, 1.0f);
            glm::vec4 farP  = inv * glm::vec4(nx, ny, 1.0f, 1.0f);
            nearP /= nearP.w;
            farP  /= farP.w;
            Ray r;
            if (IsOrthographic())
            {
                r.origin    = glm::vec3(nearP);
                r.direction = glm::normalize(glm::vec3(farP) - glm::vec3(nearP));
            }
            else
            {
                r.origin    = glm::vec3(glm::inverse(view)[3]);
                r.direction = glm::normalize(glm::vec3(farP) - r.origin);
            }
            return r;
        }

        // The overlay mapping, when this view IS the orthographic XY case
        // (no rotation in the view's upper 2x2). nullopt otherwise -- a
        // perspective or tilted view has no per-axis affine.
        [[nodiscard]] std::optional<Affine2D> AsAffine2D() const noexcept
        {
            if (!IsOrthographic()) return std::nullopt;
            // A zero viewport (the default-constructed "no view pushed" state) has no
            // pixel map either: the scale would be 0 and every overlay would collapse.
            if (viewport.x == 0u || viewport.y == 0u) return std::nullopt;
            const glm::mat4 vp = ViewProjection();
            if (std::abs(vp[0][1]) > 1e-6f || std::abs(vp[1][0]) > 1e-6f) return std::nullopt;
            Affine2D a;
            a.scale.x  =  vp[0][0] * 0.5f * float(viewport.x);
            a.scale.y  = -vp[1][1] * 0.5f * float(viewport.y);
            a.offset.x = (vp[3][0] * 0.5f + 0.5f) * float(viewport.x);
            a.offset.y = (0.5f - vp[3][1] * 0.5f) * float(viewport.y);
            return a;
        }

        [[nodiscard]] static ViewTransform Orthographic(glm::vec2 center, float halfHeight, glm::uvec2 viewport,
                                                        float nearZ = -1000.0f, float farZ = 1000.0f) noexcept
        {
            ViewTransform v;
            v.viewport = viewport;
            const float aspect = viewport.y ? float(viewport.x) / float(viewport.y) : 1.0f;
            const float halfW  = halfHeight * aspect;
            v.view       = glm::translate(glm::mat4(1.0f), glm::vec3(-center, 0.0f));
            v.projection = glm::orthoRH_ZO(-halfW, halfW, -halfHeight, halfHeight, nearZ, farZ);
            return v;
        }

        [[nodiscard]] static ViewTransform Perspective(glm::vec3 eye, glm::vec3 target, glm::vec3 up, float fovYDegrees,
                                                       glm::uvec2 viewport, float nearZ, float farZ) noexcept
        {
            ViewTransform v;
            v.viewport = viewport;
            const float aspect = viewport.y ? float(viewport.x) / float(viewport.y) : 1.0f;
            v.view       = glm::lookAtRH(eye, target, up);
            v.projection = glm::perspectiveRH_ZO(glm::radians(fovYDegrees), aspect, nearZ, farZ);
            return v;
        }
    };
}
