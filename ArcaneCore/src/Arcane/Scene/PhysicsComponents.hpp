#pragma once

// Physics ECS components: authored data for Arcane's 2D physics. RigidBody,
// Collider (a fixture list) and BodyRef (the runtime handle). SceneSettings,
// the per-scene gravity override, lives in Components.hpp.

#include <Arcane/Ecs.hpp>
#include <Arcane/Reflection.hpp>
#include <Arcane/Scene/Physics2DHandles.hpp>

#include <glm/vec2.hpp>

#include <cstdint>
#include <vector>

namespace Arcane::Physics2D
{
    // Same enumerator names and values as the solver. Polygon is part of the
    // value set so a hand-built kind round-trips, and it is not reflected:
    // Fixture carries no vertex array, so an Inspector pick would assert.
    enum class BodyType : std::uint8_t
    {
        Static    = 0,
        Kinematic = 1,
        Dynamic   = 2,
    };

    enum class ShapeKind : std::uint8_t
    {
        Circle  = 0,
        Capsule = 1,
        Aabb    = 2,
        Polygon = 3,
    };

    ARC_REFLECT_ENUM(BodyType)
        ARC_REFLECT_ENUM_VALUE(BodyType, Static)
        ARC_REFLECT_ENUM_VALUE(BodyType, Kinematic)
        ARC_REFLECT_ENUM_VALUE(BodyType, Dynamic)
    ARC_END_REFLECT_ENUM()

    ARC_REFLECT_ENUM(ShapeKind)
        ARC_REFLECT_ENUM_VALUE(ShapeKind, Circle)
        ARC_REFLECT_ENUM_VALUE(ShapeKind, Capsule)
        ARC_REFLECT_ENUM_VALUE(ShapeKind, Aabb)
    ARC_END_REFLECT_ENUM()

    struct RigidBody
    {
        ARC_CHANGE_TRACKED

        BodyType type          = BodyType::Kinematic;
        glm::vec2 velocity     {0.0f, 0.0f};
        float     mass          = 0.0f;
        float     linearDamping = 0.0f;
        bool      fixedRotation = false;
        bool      bullet        = false;
    };

    struct Fixture
    {
        ShapeKind kind      = ShapeKind::Circle;
        float     radius    = 0.5f;
        float     halfLen   = 0.0f;
        float     halfW     = 0.5f;
        float     halfH     = 0.5f;

        glm::vec2 localPos   {0.0f, 0.0f};
        float     localAngle = 0.0f;

        float density     = 1.0f;
        float friction    = 0.3f;
        float restitution = 0.0f;

        uint32_t categoryBits = 0x00000001u;
        uint32_t maskBits     = 0xFFFFFFFFu;

        bool isSensor = false;

        bool contactEvents = true;
        bool sensorEvents  = true;
        bool hitEvents     = false;
    };

    struct Collider
    {
        ARC_CHANGE_TRACKED

        std::vector<Fixture> fixtures;

        template<typename Archive>
        void Serialize(Archive& ar)
        {
            ar(fixtures);
        }
    };

    // Runtime handle. Hidden, not serialized. Re-established by System on load.
    struct BodyRef
    {
        Detail::BodyHandle handle{};
        glm::vec2          appliedScale{1.0f, 1.0f};
    };

    ARC_REFLECT_TYPE(RigidBody)
        ARC_REFLECT_FIELD(RigidBody, type)
        ARC_REFLECT_FIELD(RigidBody, velocity)
        ARC_REFLECT_FIELD(RigidBody, mass)
        ARC_REFLECT_FIELD(RigidBody, linearDamping)
        ARC_REFLECT_FIELD(RigidBody, fixedRotation)
        ARC_REFLECT_FIELD(RigidBody, bullet)
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(Fixture)
        ARC_REFLECT_FIELD(Fixture, kind)
        ARC_REFLECT_FIELD(Fixture, radius)
        ARC_REFLECT_FIELD(Fixture, halfLen)
        ARC_REFLECT_FIELD(Fixture, halfW)
        ARC_REFLECT_FIELD(Fixture, halfH)
        ARC_REFLECT_FIELD(Fixture, localPos)
        ARC_REFLECT_FIELD(Fixture, localAngle)
        ARC_REFLECT_FIELD(Fixture, density)
        ARC_REFLECT_FIELD(Fixture, friction)
        ARC_REFLECT_FIELD(Fixture, restitution)
        ARC_REFLECT_FIELD(Fixture, categoryBits)
        ARC_REFLECT_FIELD(Fixture, maskBits)
        ARC_REFLECT_FIELD(Fixture, isSensor)
        ARC_REFLECT_FIELD(Fixture, contactEvents)
            ARC_REFLECT_ATTR(Tooltip, "Report contact begin/end for this fixture. Both fixtures of a pair must allow it.")
        ARC_REFLECT_FIELD(Fixture, sensorEvents)
            ARC_REFLECT_ATTR(Tooltip, "Report sensor enter/exit, as the sensor or as the visitor. Both must allow it.")
        ARC_REFLECT_FIELD(Fixture, hitEvents)
            ARC_REFLECT_ATTR(Tooltip, "Report impacts faster than physics.events.hitThreshold. Either fixture suffices.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(Collider)
        ARC_REFLECT_FIELD(Collider, fixtures)
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(BodyRef)
        ARC_REFLECT_FIELD(BodyRef, handle)
            ARC_REFLECT_ATTR(Serializable, false)
            ARC_REFLECT_ATTR(Hidden)
        ARC_REFLECT_FIELD(BodyRef, appliedScale)
            ARC_REFLECT_ATTR(Serializable, false)
            ARC_REFLECT_ATTR(Hidden)
    ARC_END_REFLECT_TYPE()

    inline void RegisterComponents(Arcane::ComponentRegistry& creg)
    {
        creg.RegisterComponent<RigidBody>();
        creg.RegisterComponent<Collider>();
        creg.RegisterComponent<BodyRef>();
    }

    inline void RegisterComponents(Arcane::Registry& reg)
    {
        RegisterComponents(*reg.GetComponentRegistry());
    }
}
