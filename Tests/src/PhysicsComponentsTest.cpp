// M6 P3.2: Astra ECS physics component layer.
//
// Tests:
//   1. Components register + reflect: MetaRegistry has a non-null TypeMeta for
//      each of RigidBody2D, Collider2D, PhysicsBodyRef; GetFieldCount() > 0.
//   2. Binary round-trip: a Registry with RigidBody2D + Collider2D authored
//      values survives Save/Load with authored field values intact.
//      (PhysicsBodyRef.handle is Serializable(false) on the name-keyed path;
//       it harmlessly round-trips on the binary/trivially-copyable path.)

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>

#include <filesystem>
#include <memory>

using Catch::Approx;

// ---------------------------------------------------------------------------
// TEST 1 — components register + reflect
// ---------------------------------------------------------------------------

TEST_CASE("physics components are reflected (visitFields slot populated)", "[physics]")
{
    Astra::ComponentRegistry creg;
    Arcane::RegisterPhysicsComponents(creg);

    const auto* rb = creg.GetComponentDescriptor(Astra::TypeID<Arcane::RigidBody2D>::Value());
    const auto* col = creg.GetComponentDescriptor(Astra::TypeID<Arcane::Collider2D>::Value());
    const auto* ref = creg.GetComponentDescriptor(Astra::TypeID<Arcane::PhysicsBodyRef>::Value());

    REQUIRE(rb  != nullptr);
    REQUIRE(col != nullptr);
    REQUIRE(ref != nullptr);

    CHECK(rb->visitFields  != nullptr);
    CHECK(col->visitFields != nullptr);
    CHECK(ref->visitFields != nullptr);

    // MetaRegistry must have a non-null TypeMeta with at least one field for each.
    const auto* rbMeta  = Astra::GetMeta<Arcane::RigidBody2D>();
    const auto* colMeta = Astra::GetMeta<Arcane::Collider2D>();
    const auto* refMeta = Astra::GetMeta<Arcane::PhysicsBodyRef>();

    REQUIRE(rbMeta  != nullptr);
    REQUIRE(colMeta != nullptr);
    REQUIRE(refMeta != nullptr);

    CHECK(rbMeta->GetFieldCount()  > 0);
    CHECK(colMeta->GetFieldCount() > 0);
    CHECK(refMeta->GetFieldCount() > 0);
}

// ---------------------------------------------------------------------------
// TEST 2 — binary round-trip through whole-registry snapshot
// ---------------------------------------------------------------------------

TEST_CASE("physics components binary round-trip preserves authored field values", "[physics]")
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "arcane_physics_components_roundtrip.bin";

    // Authored values to assert after load.
    constexpr float kVelX      = 3.5f;
    constexpr float kVelY      = -1.2f;
    constexpr float kMass      = 2.0f;
    constexpr float kDamping   = 0.15f;
    constexpr float kRestitution = 0.4f;
    constexpr float kFriction   = 0.6f;
    constexpr float kDensity    = 1.5f;
    constexpr float kRadius     = 0.5f;
    constexpr uint32_t kCatBits = 0x02u;
    constexpr uint32_t kMaskBits = 0x01u;

    Astra::Entity savedEntity{};

    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg(components);
        Arcane::RegisterSceneComponents(reg);
        Arcane::RegisterPhysicsComponents(reg);

        savedEntity = reg.CreateEntity();

        Arcane::RigidBody2D rb;
        rb.type           = Arcane::Physics::BodyType::Dynamic;
        rb.velocity       = glm::vec2(kVelX, kVelY);
        rb.mass           = kMass;
        rb.linearDamping  = kDamping;
        rb.fixedRotation  = true;
        rb.bullet         = false;
        reg.AddComponent<Arcane::RigidBody2D>(savedEntity, rb);

        Arcane::Collider2D col;
        col.kind         = Arcane::Physics::ShapeKind::Circle;
        col.radius       = kRadius;
        col.restitution  = kRestitution;
        col.friction     = kFriction;
        col.density      = kDensity;
        col.categoryBits = kCatBits;
        col.maskBits     = kMaskBits;
        col.isSensor     = true;
        reg.AddComponent<Arcane::Collider2D>(savedEntity, col);

        // PhysicsBodyRef holds runtime state; we add it with a non-default
        // handle to ensure the binary trivially-copyable path round-trips it.
        Arcane::PhysicsBodyRef bref;
        bref.handle = Arcane::Physics::BodyHandle{ 7u, 3u };
        reg.AddComponent<Arcane::PhysicsBodyRef>(savedEntity, bref);

        auto saved = reg.Save(path);
        REQUIRE(saved.IsOk());
    }

    // Load into a fresh registry.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Arcane::RegisterSceneComponents(*components);
    Arcane::RegisterPhysicsComponents(*components);
    auto loaded = Astra::Registry::Load(path, components);
    REQUIRE(loaded.IsOk());
    std::unique_ptr<Astra::Registry> reg = std::move(*loaded.GetValue());

    // Assert RigidBody2D values survived.
    const auto* rb = reg->GetComponent<Arcane::RigidBody2D>(savedEntity);
    REQUIRE(rb != nullptr);
    CHECK(rb->type           == Arcane::Physics::BodyType::Dynamic);
    CHECK(rb->velocity.x     == Approx(kVelX));
    CHECK(rb->velocity.y     == Approx(kVelY));
    CHECK(rb->mass           == Approx(kMass));
    CHECK(rb->linearDamping  == Approx(kDamping));
    CHECK(rb->fixedRotation  == true);
    CHECK(rb->bullet         == false);

    // Assert Collider2D values survived.
    const auto* col = reg->GetComponent<Arcane::Collider2D>(savedEntity);
    REQUIRE(col != nullptr);
    CHECK(col->kind         == Arcane::Physics::ShapeKind::Circle);
    CHECK(col->radius       == Approx(kRadius));
    CHECK(col->restitution  == Approx(kRestitution));
    CHECK(col->friction     == Approx(kFriction));
    CHECK(col->density      == Approx(kDensity));
    CHECK(col->categoryBits == kCatBits);
    CHECK(col->maskBits     == kMaskBits);
    CHECK(col->isSensor     == true);

    // PhysicsBodyRef present on the entity.
    const auto* bref = reg->GetComponent<Arcane::PhysicsBodyRef>(savedEntity);
    REQUIRE(bref != nullptr);
    // Empirically verify what the binary (trivially-copyable) path does with the
    // Serializable(false) handle field.  We wrote {7u, 3u} before Save — assert
    // the actual post-Load values so the comment in PhysicsComponents.hpp matches
    // reality rather than being an untested claim.
    CHECK(bref->handle.index      == 7u);
    CHECK(bref->handle.generation == 3u);

    std::filesystem::remove(path);
}
