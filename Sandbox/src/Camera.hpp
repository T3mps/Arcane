#pragma once

// Camera: the sandbox's 2D pan + zoom view. Pure, dependency-free, header-only
// (glm::vec2 only) so it can be shared verbatim by the engine render bridge and
// included directly by the CPU camera test.
//
// CANONICAL TRANSFORM (the SandboxCameraTest pins this exact form, and both
// RenderSubmissionSystem and DrawPhysicsDebug apply it identically so sprites +
// the physics-debug overlay pan/zoom TOGETHER):
//
//     screen = world * zoom + offset
//     world  = (screen - offset) / zoom   (inverse)
//
// world units == canvas pixels at zoom 1. Default offset (0,0) + zoom 1 is the
// identity transform (every pre-camera caller is unchanged).

#include <glm/vec2.hpp>

namespace Arcane::Sandbox
{
    struct Camera
    {
        glm::vec2 offset{0.0f, 0.0f};   // screen-space translation (canvas px)
        float     zoom = 1.0f;          // world->screen scale (1 == 1:1)

        // world -> screen: screen = world * zoom + offset.
        glm::vec2 WorldToScreen(glm::vec2 world) const noexcept
        {
            return world * zoom + offset;
        }

        // screen -> world: world = (screen - offset) / zoom. Inverse of the above.
        glm::vec2 ScreenToWorld(glm::vec2 screen) const noexcept
        {
            return (screen - offset) / zoom;
        }
    };
}
