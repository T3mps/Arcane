#pragma once

// Game-facing 2D physics events. No Manifold2D include. Read through
// PhysicsWorld2D::StepEvents / FrameEvents.
//
// `entity` is valid for the frame it is read in unless that entity was
// destroyed after the step. Never store `entity` past the frame. `normal`
// points from a to b. `fixture` is the index into Collider::fixtures.

#include <cstdint>
#include <span>
#include <glm/vec2.hpp>
#include <Arcane/Ecs.hpp>
#include <Arcane/Guid.hpp>

namespace Arcane
{
    struct ContactSide2D  { Arcane::Entity entity = Arcane::Entity::Invalid(); Guid guid{}; std::uint32_t fixture = 0; };
    struct ContactBegin2D { ContactSide2D a, b; };
    struct ContactEnd2D   { ContactSide2D a, b; };
    struct ContactHit2D   { ContactSide2D a, b; glm::vec2 point{ 0.0f }; glm::vec2 normal{ 0.0f }; float approachSpeed = 0.0f; };
    struct SensorBegin2D  { ContactSide2D sensor, visitor; };
    struct SensorEnd2D    { ContactSide2D sensor, visitor; };
    struct ContactPoint2D { ContactSide2D self, other; glm::vec2 normal{ 0.0f }; std::uint32_t pointCount = 0; };

    struct PhysicsEvents2D
    {
        std::span<const ContactBegin2D> contactBegin;
        std::span<const ContactEnd2D>   contactEnd;
        std::span<const ContactHit2D>   contactHit;
        std::span<const SensorBegin2D>  sensorBegin;
        std::span<const SensorEnd2D>    sensorEnd;
    };

    struct BodyMotion2D
    {
        float velocityX = 0.0f;
        float velocityY = 0.0f;
        bool bodyReady = false;
        bool supported = false;
    };
}
