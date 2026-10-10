#pragma once

// Game-facing 2D physics events. No Manifold2D include. Read through
// Physics2D::World::StepEvents / FrameEvents.
//
// `entity` is valid for the frame it is read in unless that entity was
// destroyed after the step. Never store `entity` past the frame. `normal`
// points from a to b. `fixture` is the index into Collider::fixtures.

#include <cstdint>
#include <span>
#include <glm/vec2.hpp>
#include <Arcane/Ecs.hpp>
#include <Arcane/Guid.hpp>

namespace Arcane::Physics2D
{
    struct ContactSide  { Arcane::Entity entity = Arcane::Entity::Invalid(); Guid guid{}; std::uint32_t fixture = 0; };
    struct ContactBegin { ContactSide a, b; };
    struct ContactEnd   { ContactSide a, b; };
    struct ContactHit   { ContactSide a, b; glm::vec2 point{ 0.0f }; glm::vec2 normal{ 0.0f }; float approachSpeed = 0.0f; };
    struct SensorBegin  { ContactSide sensor, visitor; };
    struct SensorEnd    { ContactSide sensor, visitor; };
    struct ContactPoint { ContactSide self, other; glm::vec2 normal{ 0.0f }; std::uint32_t pointCount = 0; };

    struct Events
    {
        std::span<const ContactBegin> contactBegin;
        std::span<const ContactEnd>   contactEnd;
        std::span<const ContactHit>   contactHit;
        std::span<const SensorBegin>  sensorBegin;
        std::span<const SensorEnd>    sensorEnd;
    };

    struct BodyMotion
    {
        float velocityX = 0.0f;
        float velocityY = 0.0f;
        bool bodyReady = false;
        bool supported = false;
    };
}
