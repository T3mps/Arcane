#pragma once

// Camera: the sandbox's 2D pan + zoom view. Pure, dependency-free, header-only
// (glm::vec2 only) so it can be shared verbatim by the engine render bridge and
// included directly by the CPU camera test.
//
// CANONICAL TRANSFORM (the SandboxCameraTest pins this exact form, and both
// RenderSubmissionSystem and DrawPhysicsDebug apply it identically so sprites +
// the physics-debug overlay pan/zoom TOGETHER):
//
//     screen = world * (kPixelsPerMeter * zoom) + offset
//     world  = (screen - offset) / (kPixelsPerMeter * zoom)   (inverse)
//
// World is METERS (MKS); the canvas is pixels. kPixelsPerMeter folds the unit
// conversion into the single transform pair everything routes through -- so a
// 12.8 x 7.2 m layout fills a 1280x720 canvas at zoom 1, framing like the px
// original did. Default offset (0,0) + zoom 1 maps meters -> px at 100 px/m.

#include <glm/vec2.hpp>

namespace Arcane::Sandbox
{
    struct Camera
    {
        // World is METERS (MKS); the canvas is pixels. This is the single conversion
        // seam between the two unit systems -- 100 px per meter.
        static constexpr float kPixelsPerMeter = 100.0f;

        glm::vec2 offset{0.0f, 0.0f};   // screen-space translation (canvas px)
        float     zoom = 1.0f;          // additional world->screen scale (1 == 100 px/m)

        // world -> screen: screen = world * (kPixelsPerMeter * zoom) + offset.
        glm::vec2 WorldToScreen(glm::vec2 world) const noexcept
        {
            return world * (kPixelsPerMeter * zoom) + offset;
        }

        // screen -> world: world = (screen - offset) / (kPixelsPerMeter * zoom).
        glm::vec2 ScreenToWorld(glm::vec2 screen) const noexcept
        {
            return (screen - offset) / (kPixelsPerMeter * zoom);
        }
    };
}
