#pragma once

// Physics ECS components: authored data for Arcane's 2D physics. RigidBody2D,
// Collider2D (a fixture list) and PhysicsBodyRef2D (the runtime handle). PhysicsSettings2D,
// the per-scene gravity override, lives in Components.hpp.

#include <Arcane/Ecs.hpp>
#include <Arcane/Reflection.hpp>
#include <Arcane/Scene/Physics2DHandles.hpp>

#include <glm/vec2.hpp>

#include <cstdint>
#include <vector>

namespace Arcane
{
    // Same enumerator names and values as the solver. Polygon is part of the
    // value set so a hand-built kind round-trips, and it is not reflected:
    // Fixture2D carries no vertex array, so an Inspector pick would assert.
    enum class BodyType2D : std::uint8_t
    {
        Static    = 0,
        Kinematic = 1,
        Dynamic   = 2,
    };

    enum class ShapeKind2D : std::uint8_t
    {
        Circle  = 0,
        Capsule = 1,
        Aabb    = 2,
        Polygon = 3,
    };

    ARC_REFLECT_ENUM(BodyType2D)
        ARC_REFLECT_ENUM_VALUE(BodyType2D, Static)
        ARC_REFLECT_ENUM_VALUE(BodyType2D, Kinematic)
        ARC_REFLECT_ENUM_VALUE(BodyType2D, Dynamic)
    ARC_END_REFLECT_ENUM()

    ARC_REFLECT_ENUM(ShapeKind2D)
        ARC_REFLECT_ENUM_VALUE(ShapeKind2D, Circle)
        ARC_REFLECT_ENUM_VALUE(ShapeKind2D, Capsule)
        ARC_REFLECT_ENUM_VALUE(ShapeKind2D, Aabb)
    ARC_END_REFLECT_ENUM()

    struct RigidBody2D
    {
        ARC_CHANGE_TRACKED

        BodyType2D type          = BodyType2D::Kinematic;
        glm::vec2 velocity     {0.0f, 0.0f};
        float     mass          = 0.0f;
        float     linearDamping = 0.0f;
        bool      fixedRotation = false;
        bool      bullet        = false;
    };

    struct Fixture2D
    {
        ShapeKind2D kind      = ShapeKind2D::Circle;
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

    struct Collider2D
    {
        ARC_CHANGE_TRACKED

        std::vector<Fixture2D> fixtures;

        template<typename Archive>
        void Serialize(Archive& ar)
        {
            ar(fixtures);
        }
    };

    // Runtime handle. Hidden, not serialized. Re-established by System on load.
    struct PhysicsBodyRef2D
    {
        Detail::Physics2D::BodyHandle handle{};
        glm::vec2          appliedScale{1.0f, 1.0f};
    };

    ARC_REFLECT_TYPE(RigidBody2D)
        ARC_REFLECT_FIELD(RigidBody2D, type)
        ARC_REFLECT_FIELD(RigidBody2D, velocity)
        ARC_REFLECT_FIELD(RigidBody2D, mass)
        ARC_REFLECT_FIELD(RigidBody2D, linearDamping)
        ARC_REFLECT_FIELD(RigidBody2D, fixedRotation)
        ARC_REFLECT_FIELD(RigidBody2D, bullet)
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(Fixture2D)
        ARC_REFLECT_FIELD(Fixture2D, kind)
        ARC_REFLECT_FIELD(Fixture2D, radius)
        ARC_REFLECT_FIELD(Fixture2D, halfLen)
        ARC_REFLECT_FIELD(Fixture2D, halfW)
        ARC_REFLECT_FIELD(Fixture2D, halfH)
        ARC_REFLECT_FIELD(Fixture2D, localPos)
        ARC_REFLECT_FIELD(Fixture2D, localAngle)
        ARC_REFLECT_FIELD(Fixture2D, density)
        ARC_REFLECT_FIELD(Fixture2D, friction)
        ARC_REFLECT_FIELD(Fixture2D, restitution)
        ARC_REFLECT_FIELD(Fixture2D, categoryBits)
        ARC_REFLECT_FIELD(Fixture2D, maskBits)
        ARC_REFLECT_FIELD(Fixture2D, isSensor)
        ARC_REFLECT_FIELD(Fixture2D, contactEvents)
            ARC_REFLECT_ATTR(Tooltip, "Report contact begin/end for this fixture. Both fixtures of a pair must allow it.")
        ARC_REFLECT_FIELD(Fixture2D, sensorEvents)
            ARC_REFLECT_ATTR(Tooltip, "Report sensor enter/exit, as the sensor or as the visitor. Both must allow it.")
        ARC_REFLECT_FIELD(Fixture2D, hitEvents)
            ARC_REFLECT_ATTR(Tooltip, "Report impacts faster than physics.events.hitThreshold. Either fixture suffices.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(Collider2D)
        ARC_REFLECT_FIELD(Collider2D, fixtures)
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(PhysicsBodyRef2D)
        ARC_REFLECT_FIELD(PhysicsBodyRef2D, handle)
            ARC_REFLECT_ATTR(Serializable, false)
            ARC_REFLECT_ATTR(Hidden)
        ARC_REFLECT_FIELD(PhysicsBodyRef2D, appliedScale)
            ARC_REFLECT_ATTR(Serializable, false)
            ARC_REFLECT_ATTR(Hidden)
    ARC_END_REFLECT_TYPE()

    inline void RegisterPhysicsComponents2D(Arcane::ComponentRegistry& creg)
    {
        creg.RegisterComponent<RigidBody2D>();
        creg.RegisterComponent<Collider2D>();
        creg.RegisterComponent<PhysicsBodyRef2D>();
    }

    inline void RegisterPhysicsComponents2D(Arcane::Registry& reg)
    {
        RegisterPhysicsComponents2D(*reg.GetComponentRegistry());
    }
}
