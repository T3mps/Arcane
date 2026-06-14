#pragma once

// Physics ECS components: reflected authored data for the Arcane 2D physics
// engine (M6 P3.2). These live in the Arcane.dll scene layer alongside the
// other scene components — NOT in Core/Physics (Core is Astra-free).
//
// Three components:
//   RigidBody2D   — body type, velocity, mass params, bullet flag. Defaults to
//                   Kinematic to match the overworld character-controller idiom
//                   (kinematic bodies are script-driven, not force-driven).
//   Collider2D    — shape descriptor (primitive scalars, not a Core::Shape) plus
//                   material and collision filter. Polygon authored verts are out
//                   of scope for P3.2; the PhysicsSystem (P3.3) builds a
//                   Core::Shape from this descriptor via MakeCircle/MakeCapsule/
//                   MakeAabb. Design decision: avoid embedding Core::Shape directly
//                   (its std::vector<Vec2> is not ASTRA_REFLECT-able; yagni).
//   PhysicsBodyRef— runtime BodyHandle re-established by PhysicsSystem on load,
//                   exactly as WorldTransform::matrix is derived. The handle field
//                   is Serializable(false) so name-keyed JSON skips it; it
//                   round-trips harmlessly on the trivially-copyable binary path.
//
// Registration: RegisterPhysicsComponents(ComponentRegistry&) and the overload
// taking Registry& mirror the RegisterSceneComponents pattern in SceneModule.hpp.

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Reflection/Reflection.hpp>
#include <Astra/Registry/Registry.hpp>

#include <glm/vec2.hpp>

#include <cstdint>

namespace Arcane
{
    // -------------------------------------------------------------------------
    // RigidBody2D
    // -------------------------------------------------------------------------
    // Authored dynamics parameters. Mirrors Core::BodyDef so P3.3 maps 1:1.
    // Default type = Kinematic (overworld default: character controllers are
    // script-driven and must not be pushed by dynamic bodies).
    struct RigidBody2D
    {
        Physics::BodyType type          = Physics::BodyType::Kinematic;
        glm::vec2         velocity      {0.0f, 0.0f};
        float             mass          = 0.0f;      // 0 => density-derived
        float             linearDamping = 0.0f;
        bool              fixedRotation = false;
        bool              bullet        = false;     // enable GJK-TOI CCD clamp
    };

    // -------------------------------------------------------------------------
    // Collider2D
    // -------------------------------------------------------------------------
    // Reflectable shape descriptor: kind discriminator + scalar params for the
    // three primitive shape families (Circle/Capsule/Aabb). Polygon authored
    // verts are deferred (YAGNI for P3.2 — add later when the PhysicsSystem
    // needs authored polygon geometry). Material + collision filter live here
    // as plain floats/uint32s so the whole struct stays trivially copyable and
    // ASTRA_REFLECT-able without a custom visitor.
    struct Collider2D
    {
        // Shape discriminator + scalar params (three shapes for P3.2).
        Physics::ShapeKind kind      = Physics::ShapeKind::Circle;
        float              radius    = 0.5f;   // Circle/Capsule radius (ignored when kind != Circle/Capsule)
        float              halfLen   = 0.0f;   // Capsule half-length   (ignored when kind != Capsule)
        float              halfW     = 0.5f;   // Aabb half-width       (ignored when kind != Aabb)
        float              halfH     = 0.5f;   // Aabb half-height      (ignored when kind != Aabb)

        // Material.
        float restitution = 0.0f;
        float friction    = 0.3f;
        float density     = 1.0f;

        // Collision filter.
        uint32_t categoryBits = 0x00000001u;
        uint32_t maskBits     = 0xFFFFFFFFu;

        bool isSensor = false;
    };

    // -------------------------------------------------------------------------
    // PhysicsBodyRef
    // -------------------------------------------------------------------------
    // Runtime slot: the BodyHandle into the PhysicsWorld's free-list SoA arrays.
    // Re-established by PhysicsSystem on scene load; never authored.
    // The handle field is Serializable(false) so the name-keyed JSON/snapshot
    // path skips it (matches WorldTransform::matrix treatment in Components.hpp).
    struct PhysicsBodyRef
    {
        Physics::BodyHandle handle{};   // default = kInvalidBody (index=0, gen=0)
    };

} // namespace Arcane

// Enum reflection blocks — at namespace scope (Arcane::Physics), so the
// macro argument is a simple identifier (no :: in the token-paste).
// BodyType and ShapeKind are reflected here (in the Arcane.dll module that
// owns physics components) so the visitFields seam can serialize them by name
// on the JSON path and the MetaRegistry knows their string->value mappings.
namespace Arcane::Physics
{
    ASTRA_REFLECT_ENUM(BodyType)
        ASTRA_REFLECT_ENUM_VALUE(BodyType, Static)
        ASTRA_REFLECT_ENUM_VALUE(BodyType, Kinematic)
        ASTRA_REFLECT_ENUM_VALUE(BodyType, Dynamic)
    ASTRA_END_REFLECT_ENUM()

    ASTRA_REFLECT_ENUM(ShapeKind)
        ASTRA_REFLECT_ENUM_VALUE(ShapeKind, Circle)
        ASTRA_REFLECT_ENUM_VALUE(ShapeKind, Capsule)
        ASTRA_REFLECT_ENUM_VALUE(ShapeKind, Aabb)
        // Polygon is part of the Core type's full set but Collider2D carries no
        // authored vertex array — Polygon colliders are not buildable until a later
        // task adds a verts field (or a separate PolygonCollider2D component).
        ASTRA_REFLECT_ENUM_VALUE(ShapeKind, Polygon)
    ASTRA_END_REFLECT_ENUM()
} // namespace Arcane::Physics

// Component type reflection blocks — at namespace scope (design rule: every
// engine component is ASTRA_REFLECT-annotated from day one).
namespace Arcane
{
    ASTRA_REFLECT_TYPE(RigidBody2D)
        ASTRA_REFLECT_FIELD(RigidBody2D, type)
        ASTRA_REFLECT_FIELD(RigidBody2D, velocity)
        ASTRA_REFLECT_FIELD(RigidBody2D, mass)
        ASTRA_REFLECT_FIELD(RigidBody2D, linearDamping)
        ASTRA_REFLECT_FIELD(RigidBody2D, fixedRotation)
        ASTRA_REFLECT_FIELD(RigidBody2D, bullet)
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_TYPE(Collider2D)
        ASTRA_REFLECT_FIELD(Collider2D, kind)
        ASTRA_REFLECT_FIELD(Collider2D, radius)
        ASTRA_REFLECT_FIELD(Collider2D, halfLen)
        ASTRA_REFLECT_FIELD(Collider2D, halfW)
        ASTRA_REFLECT_FIELD(Collider2D, halfH)
        ASTRA_REFLECT_FIELD(Collider2D, restitution)
        ASTRA_REFLECT_FIELD(Collider2D, friction)
        ASTRA_REFLECT_FIELD(Collider2D, density)
        ASTRA_REFLECT_FIELD(Collider2D, categoryBits)
        ASTRA_REFLECT_FIELD(Collider2D, maskBits)
        ASTRA_REFLECT_FIELD(Collider2D, isSensor)
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_TYPE(PhysicsBodyRef)
        ASTRA_REFLECT_FIELD(PhysicsBodyRef, handle)
            ASTRA_REFLECT_ATTR(Serializable, false)
    ASTRA_END_REFLECT_TYPE()
} // namespace Arcane

// Registration functions — header-only, mirroring SceneModule.hpp.
namespace Arcane
{
    inline void RegisterPhysicsComponents(Astra::ComponentRegistry& creg)
    {
        creg.RegisterComponent<RigidBody2D>();
        creg.RegisterComponent<Collider2D>();
        creg.RegisterComponent<PhysicsBodyRef>();
    }

    inline void RegisterPhysicsComponents(Astra::Registry& reg)
    {
        RegisterPhysicsComponents(*reg.GetComponentRegistry());
    }
} // namespace Arcane
