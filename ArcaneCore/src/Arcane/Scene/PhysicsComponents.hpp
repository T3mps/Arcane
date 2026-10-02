#pragma once

// Physics ECS components: reflected authored data for the Arcane 2D physics
// engine (M6 Physics-v2 T6: fixture-list Collider2D). These live in the
// Arcane.dll scene layer alongside the other scene components -- NOT in
// Core/Physics (Core is Astra-free).
//
// Three components:
//   RigidBody2D    -- body type, velocity, mass params, bullet flag. Defaults
//                     to Kinematic to match the overworld character-controller
//                     idiom (kinematic bodies are script-driven, not force-driven).
//   Collider2D     -- a fixture LIST. Each Fixture carries its own shape
//                     descriptor, local transform, material, and collision filter.
//                     The list is a std::vector<Fixture> (see REFLECTION DECISION
//                     below). For the create pass, PhysicsSystem calls AddBody
//                     with fixture[0] as the primary shape then AddFixture for
//                     fixtures[1..].
//   PhysicsBodyRef -- runtime BodyHandle re-established by PhysicsSystem on
//                     load, exactly as WorldTransform::matrix is derived. The
//                     handle field is Serializable(false) so name-keyed JSON
//                     skips it; it round-trips harmlessly on the binary path.
//
// REFLECTION DECISION -- vector<Fixture> vs fixed array:
//   Astra DOES support std::vector<T> fields via FieldInfo::isVector, and the
//   Astra BinaryWriter/BinaryReader have explicit vector<T> operator() overloads.
//   The Fixture descriptor struct is fully trivially copyable (all POD members),
//   so the writer takes the fast bulk-memcpy path (WriteBytes(data, N*sizeof(T))).
//   Collider2D itself is no longer trivially copyable (it now contains a
//   std::vector), so it provides a Serialize(Archive&) method (the Astra contract
//   for non-trivial components) that calls ar(fixtures) to delegate to the
//   archive's vector overload. ARCANE_REFLECT_FIELD reflects the vector field for
//   the visitFields / editor / JSON-schema path.
//   CONCLUSION: std::vector<Fixture> is used. No fixed-array fallback needed.
//
// Registration: RegisterPhysicsComponents(ComponentRegistry&) and the overload
// taking Registry& mirror the RegisterSceneComponents pattern in SceneModule.hpp.

#include <Manifold2D/Physics/PhysicsTypes.hpp>
#include <Manifold2D/Physics/Shapes.hpp>

#include <Arcane/Ecs.hpp>
#include <Arcane/Reflection.hpp>

#include <glm/vec2.hpp>

#include <cstdint>
#include <vector>

namespace Arcane
{
    // Physics types were lifted to the standalone Manifold2D library (Phase 2).
    // Alias so the authored-component code below reads Phys:: for the engine's
    // Manifold2D::Physics types. This alias IS the facade for Manifold2D:
    // games spell Arcane::Phys::BodyType.
    // ARCANE_INTERNAL_BEGIN: the Manifold2D facade alias names the library once, here
    namespace Phys = Manifold2D::Physics;
    // ARCANE_INTERNAL_END

    // -------------------------------------------------------------------------
    // RigidBody2D
    // -------------------------------------------------------------------------
    // Authored dynamics parameters. Mirrors Core::BodyDef so P3.3 maps 1:1.
    // Default type = Kinematic (overworld default: character controllers are
    // script-driven and must not be pushed by dynamic bodies).
    struct RigidBody2D
    {
        // Astra change tracking (2026-09-11 physics wiring, spec s4.1a): a
        // paused PhysicsSystem pass re-mints a body whose RigidBody2D or
        // Collider2D changed since the last reconcile, so an Inspector edit
        // (or its undo) reaches the world without an exact compare per body.
        ARCANE_CHANGE_TRACKED

        Phys::BodyType type          = Phys::BodyType::Kinematic;
        glm::vec2         velocity      {0.0f, 0.0f};
        float             mass          = 0.0f;      // 0 => density-derived
        float             linearDamping = 0.0f;
        bool              fixedRotation = false;
        bool              bullet        = false;     // enable GJK-TOI CCD clamp
    };

    // -------------------------------------------------------------------------
    // Fixture
    // -------------------------------------------------------------------------
    // Per-fixture authored descriptor: shape kind + scalars for the three
    // primitive families (Circle/Capsule/Aabb), local transform in the body
    // frame, material, and collision filter.
    //
    // Deliberately trivially copyable (all POD members). This makes
    // std::vector<Fixture> use the bulk WriteBytes path in BinaryWriter and the
    // bulk ReadBytes path in BinaryReader -- no per-element Serialize overhead.
    //
    // LOCAL TRANSFORM fields:
    //   localPos   -- offset from the body's origin to this fixture's center,
    //                 in the body's local frame. Used by PhysicsWorld::AddFixture.
    //   localAngle -- rotation of this fixture relative to the body (radians).
    //
    // Polygon authored verts remain out of scope (no vertex array in this struct;
    // ShapeKind::Polygon asserts/skips in PhysicsSystem as in P3.3).
    struct Fixture
    {
        // Shape discriminator + scalar params (three shapes, matching P3.2 style).
        Phys::ShapeKind kind      = Phys::ShapeKind::Circle;
        float              radius    = 0.5f;   // Circle/Capsule radius
        float              halfLen   = 0.0f;   // Capsule half-length
        float              halfW     = 0.5f;   // Aabb half-width
        float              halfH     = 0.5f;   // Aabb half-height

        // Local transform (body frame).
        glm::vec2          localPos  {0.0f, 0.0f};
        float              localAngle = 0.0f;

        // Material.
        float density     = 1.0f;
        float friction    = 0.3f;
        float restitution = 0.0f;

        // Collision filter.
        uint32_t categoryBits = 0x00000001u;
        uint32_t maskBits     = 0xFFFFFFFFu;

        bool isSensor = false;
    };

    // -------------------------------------------------------------------------
    // Collider2D
    // -------------------------------------------------------------------------
    // A fixture list. Each Fixture is a complete shape+material descriptor;
    // PhysicsSystem iterates fixtures to build the body and attach extra
    // fixtures via PhysicsWorld::AddFixture.
    //
    // SERIALIZATION: Collider2D is no longer trivially copyable (std::vector
    // member). It provides Serialize(Archive&) which delegates to ar(fixtures)
    // -- the archive's vector<Fixture> overload does bulk POD memcpy because
    // Fixture is trivially copyable.
    struct Collider2D
    {
        // Astra change tracking (2026-09-11 physics wiring, spec s4.1a): a
        // paused PhysicsSystem pass re-mints a body whose RigidBody2D or
        // Collider2D changed since the last reconcile, so an Inspector edit
        // (or its undo) reaches the world without an exact compare per body.
        ARCANE_CHANGE_TRACKED

        std::vector<Fixture> fixtures;

        template<typename Archive>
        void Serialize(Archive& ar)
        {
            ar(fixtures);
        }
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
        Phys::BodyHandle handle{};   // default = kInvalidBody (index=0, gen=0)

        // Runtime baseline: the lt.scale most recently baked into this body's
        // fixtures. The paused reconcile pass rebuilds fixtures when lt.scale
        // diverges from this. Serializable(false), re-seeded on load exactly as
        // `handle` is; the physics world exposes no fixture-dims read-back, so
        // this cached baseline is how a scale change is detected (Unity/Unreal
        // "baked scale" model). Still trivially copyable (glm::vec2 is POD) ->
        // the binary snapshot path memcpies it harmlessly.
        glm::vec2 appliedScale{1.0f, 1.0f};
    };

} // namespace Arcane

// Enum reflection blocks -- at namespace scope (Manifold2D::Physics, where
// BodyType/ShapeKind now live after the Phase 2 lift), so the macro argument is
// a simple identifier (no :: in the token-paste).
// BodyType and ShapeKind are reflected here (in the Arcane.dll module that
// owns physics components) so the visitFields seam can serialize them by name
// on the JSON path and the MetaRegistry knows their string->value mappings.
// ARCANE_INTERNAL_BEGIN: the enum reflection must sit in the enums' own (Manifold2D) namespace
namespace Manifold2D::Physics
{
// ARCANE_INTERNAL_END
    ARCANE_REFLECT_ENUM(BodyType)
        ARCANE_REFLECT_ENUM_VALUE(BodyType, Static)
        ARCANE_REFLECT_ENUM_VALUE(BodyType, Kinematic)
        ARCANE_REFLECT_ENUM_VALUE(BodyType, Dynamic)
    ARCANE_END_REFLECT_ENUM()

    // The reflected set is EXACTLY the buildable set. Polygon is part of the
    // Core type but Fixture carries no authored vertex array, so
    // MakeScaledShape (PhysicsSystem.hpp) asserts on it -- and everything
    // reflected here is one Inspector combo pick away (FieldKind::Enum, one
    // level down inside the Collider2D fixture list since the Vector editor),
    // with a paused pass re-minting the body on the resulting
    // Changed<Collider2D>. Reflecting Polygon made that assert a Debug-editor
    // abort on a menu selection (2026-09-12 review). It comes back with the
    // verts field (or a PolygonCollider2D component) that makes it buildable;
    // PhysicsSystemTest's "every reflected ShapeKind value is one
    // MakeScaledShape can build" holds the line. A hand-edited scene naming
    // "Polygon" now loads with that fixture's kind left at its default
    // (Circle) -- the JSON reader's existing unknown-enum-name rule
    // (ReflectionJson.hpp, EnumFromString -> nullopt -> no write) -- instead
    // of asserting at mint.
    ARCANE_REFLECT_ENUM(ShapeKind)
        ARCANE_REFLECT_ENUM_VALUE(ShapeKind, Circle)
        ARCANE_REFLECT_ENUM_VALUE(ShapeKind, Capsule)
        ARCANE_REFLECT_ENUM_VALUE(ShapeKind, Aabb)
    ARCANE_END_REFLECT_ENUM()
} // namespace Manifold2D::Physics

// Component type reflection blocks -- at namespace scope (design rule: every
// engine component is ARCANE_REFLECT-annotated from day one).
namespace Arcane
{
    ARCANE_REFLECT_TYPE(RigidBody2D)
        ARCANE_REFLECT_FIELD(RigidBody2D, type)
        ARCANE_REFLECT_FIELD(RigidBody2D, velocity)
        ARCANE_REFLECT_FIELD(RigidBody2D, mass)
        ARCANE_REFLECT_FIELD(RigidBody2D, linearDamping)
        ARCANE_REFLECT_FIELD(RigidBody2D, fixedRotation)
        ARCANE_REFLECT_FIELD(RigidBody2D, bullet)
    ARCANE_END_REFLECT_TYPE()

    // Fixture: reflected so editors / JSON schema can walk per-fixture fields.
    // Each Fixture is a standalone reflected struct (no enclosing component);
    // the visitFields consumer descends into the vector elements by hand.
    ARCANE_REFLECT_TYPE(Fixture)
        ARCANE_REFLECT_FIELD(Fixture, kind)
        ARCANE_REFLECT_FIELD(Fixture, radius)
        ARCANE_REFLECT_FIELD(Fixture, halfLen)
        ARCANE_REFLECT_FIELD(Fixture, halfW)
        ARCANE_REFLECT_FIELD(Fixture, halfH)
        ARCANE_REFLECT_FIELD(Fixture, localPos)
        ARCANE_REFLECT_FIELD(Fixture, localAngle)
        ARCANE_REFLECT_FIELD(Fixture, density)
        ARCANE_REFLECT_FIELD(Fixture, friction)
        ARCANE_REFLECT_FIELD(Fixture, restitution)
        ARCANE_REFLECT_FIELD(Fixture, categoryBits)
        ARCANE_REFLECT_FIELD(Fixture, maskBits)
        ARCANE_REFLECT_FIELD(Fixture, isSensor)
    ARCANE_END_REFLECT_TYPE()

    // Collider2D: reflects the fixture list. Serializable on BOTH paths since
    // 2026-09-11 (2D physics wiring): the reflection->JSON bridge grew a
    // container branch (ReflectionJson.hpp, WriteVector/ReadVector) that walks
    // each Fixture's reflected fields, so a fixture list authored in the
    // Inspector or by hand in an .arcscene round-trips -- scene schema v5.
    // Before that the field was Serializable(false) because the bridge could
    // neither write nor read a container and a saved Collider2D could never be
    // opened again (SceneJsonTest pins the fix). The BINARY path was always
    // fine: Collider2D::Serialize(Archive&) carries the vector directly.
    ARCANE_REFLECT_TYPE(Collider2D)
        ARCANE_REFLECT_FIELD(Collider2D, fixtures)
    ARCANE_END_REFLECT_TYPE()

    // Hidden as well as Serializable(false): runtime plumbing nobody authors
    // (the handle is re-established by PhysicsSystem on load), which used to
    // render as "unsupported" rows in the Inspector.
    ARCANE_REFLECT_TYPE(PhysicsBodyRef)
        ARCANE_REFLECT_FIELD(PhysicsBodyRef, handle)
            ARCANE_REFLECT_ATTR(Serializable, false)
            ARCANE_REFLECT_ATTR(Hidden)
        ARCANE_REFLECT_FIELD(PhysicsBodyRef, appliedScale)
            ARCANE_REFLECT_ATTR(Serializable, false)
            ARCANE_REFLECT_ATTR(Hidden)
    ARCANE_END_REFLECT_TYPE()
} // namespace Arcane

// Registration functions -- header-only, mirroring SceneModule.hpp.
namespace Arcane
{
    inline void RegisterPhysicsComponents(Arcane::ComponentRegistry& creg)
    {
        creg.RegisterComponent<RigidBody2D>();
        creg.RegisterComponent<Collider2D>();
        creg.RegisterComponent<PhysicsBodyRef>();
        // NOTE: Fixture is a data descriptor embedded inside Collider2D::fixtures,
        // not a standalone ECS component. It is reflected via ARCANE_REFLECT_TYPE
        // (static-init path) so editors/JSON-schema can walk per-fixture fields,
        // but it is NOT registered with ComponentRegistry (no entity slot needed).
    }

    inline void RegisterPhysicsComponents(Arcane::Registry& reg)
    {
        RegisterPhysicsComponents(*reg.GetComponentRegistry());
    }
} // namespace Arcane
