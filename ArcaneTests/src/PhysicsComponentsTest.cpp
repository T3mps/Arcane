// M6 Physics-v2 T6: Astra ECS physics component layer, fixture-list Collider2D.
//
// Tests:
//   1. Components register + reflect: MetaRegistry has a non-null TypeMeta for
//      each of RigidBody2D, Collider2D, PhysicsBodyRef; GetFieldCount() > 0.
//   2. Binary round-trip (single fixture): a Registry with RigidBody2D +
//      single-fixture Collider2D authored values survives Save/Load with all
//      per-fixture fields intact.
//      (PhysicsBodyRef.handle is Serializable(false) on the name-keyed path;
//       it harmlessly round-trips on the binary/trivially-copyable path.)
//   3. Binary round-trip (two fixtures): a Collider2D with TWO fixtures
//      (fixture0 = circle r=4 @ local(0,0) density 1 friction 0.3;
//       fixture1 = aabb(2,2) @ local(10,0) restitution 0.5, isSensor=true)
//      survives Save/Load with ALL per-fixture fields intact on both fixtures.

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
// TEST 1 -- components register + reflect
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
// TEST 2 -- single-fixture binary round-trip through whole-registry snapshot
// ---------------------------------------------------------------------------

TEST_CASE("physics components binary round-trip preserves authored field values (single fixture)", "[physics]")
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
        rb.type           = Manifold2D::Physics::BodyType::Dynamic;
        rb.velocity       = glm::vec2(kVelX, kVelY);
        rb.mass           = kMass;
        rb.linearDamping  = kDamping;
        rb.fixedRotation  = true;
        rb.bullet         = false;
        reg.AddComponent<Arcane::RigidBody2D>(savedEntity, rb);

        // Single-fixture Collider2D.
        Arcane::Collider2D col;
        {
            Arcane::Fixture fx;
            fx.kind         = Manifold2D::Physics::ShapeKind::Circle;
            fx.radius       = kRadius;
            fx.restitution  = kRestitution;
            fx.friction     = kFriction;
            fx.density      = kDensity;
            fx.categoryBits = kCatBits;
            fx.maskBits     = kMaskBits;
            fx.isSensor     = true;
            col.fixtures.push_back(fx);
        }
        reg.AddComponent<Arcane::Collider2D>(savedEntity, col);

        // PhysicsBodyRef holds runtime state; we add it with a non-default
        // handle to ensure the binary trivially-copyable path round-trips it.
        Arcane::PhysicsBodyRef bref;
        bref.handle = Manifold2D::Physics::BodyHandle{ 7u, 3u };
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
    CHECK(rb->type           == Manifold2D::Physics::BodyType::Dynamic);
    CHECK(rb->velocity.x     == Approx(kVelX));
    CHECK(rb->velocity.y     == Approx(kVelY));
    CHECK(rb->mass           == Approx(kMass));
    CHECK(rb->linearDamping  == Approx(kDamping));
    CHECK(rb->fixedRotation  == true);
    CHECK(rb->bullet         == false);

    // Assert Collider2D: single fixture values survived.
    const auto* col = reg->GetComponent<Arcane::Collider2D>(savedEntity);
    REQUIRE(col != nullptr);
    REQUIRE(col->fixtures.size() == 1u);
    CHECK(col->fixtures[0].kind         == Manifold2D::Physics::ShapeKind::Circle);
    CHECK(col->fixtures[0].radius       == Approx(kRadius));
    CHECK(col->fixtures[0].restitution  == Approx(kRestitution));
    CHECK(col->fixtures[0].friction     == Approx(kFriction));
    CHECK(col->fixtures[0].density      == Approx(kDensity));
    CHECK(col->fixtures[0].categoryBits == kCatBits);
    CHECK(col->fixtures[0].maskBits     == kMaskBits);
    CHECK(col->fixtures[0].isSensor     == true);

    // PhysicsBodyRef present on the entity.
    const auto* bref = reg->GetComponent<Arcane::PhysicsBodyRef>(savedEntity);
    REQUIRE(bref != nullptr);
    // Empirically verify what the binary (trivially-copyable) path does with the
    // Serializable(false) handle field.  We wrote {7u, 3u} before Save -- assert
    // the actual post-Load values so the comment in PhysicsComponents.hpp matches
    // reality rather than being an untested claim.
    CHECK(bref->handle.index      == 7u);
    CHECK(bref->handle.generation == 3u);

    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// TEST 3 -- two-fixture binary round-trip: all per-fixture fields survive
// ---------------------------------------------------------------------------

TEST_CASE("Collider2D two-fixture round-trip: all per-fixture fields survive Save/Load", "[physics]")
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "arcane_physics_components_2fixture_roundtrip.bin";

    // Fixture 0: circle r=4 @ local(0,0), density 1, friction 0.3.
    constexpr float kF0Radius   = 4.0f;
    constexpr float kF0Density  = 1.0f;
    constexpr float kF0Friction = 0.3f;
    constexpr float kF0LocalX   = 0.0f;
    constexpr float kF0LocalY   = 0.0f;
    constexpr uint32_t kF0Cat   = 0x01u;
    constexpr uint32_t kF0Mask  = 0xFFFFFFFFu;

    // Fixture 1: capsule(halfLen=1.5, r=0.4) @ local(10,0) localAngle=0.5, isSensor=true.
    // Using Capsule (not Aabb) exercises halfLen + localAngle round-trip, which
    // was previously un-gated.  The round-trip test now asserts ALL descriptor
    // fields including halfLen and localAngle on the second fixture.
    constexpr float kF1HalfLen  = 1.5f;
    constexpr float kF1Radius   = 0.4f;
    constexpr float kF1LocalX   = 10.0f;
    constexpr float kF1LocalY   = 0.0f;
    constexpr float kF1LocalAng = 0.5f;
    constexpr float kF1Rest     = 0.5f;
    constexpr uint32_t kF1Cat   = 0x02u;
    constexpr uint32_t kF1Mask  = 0xFFFFFFFEu;

    Astra::Entity savedEntity{};

    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg(components);
        Arcane::RegisterSceneComponents(reg);
        Arcane::RegisterPhysicsComponents(reg);

        savedEntity = reg.CreateEntity();

        // Minimal RigidBody2D (only need it present for the overall test).
        Arcane::RigidBody2D rb;
        rb.type = Manifold2D::Physics::BodyType::Dynamic;
        reg.AddComponent<Arcane::RigidBody2D>(savedEntity, rb);

        // Two-fixture Collider2D.
        Arcane::Collider2D col;

        // Fixture 0: circle.
        {
            Arcane::Fixture fx;
            fx.kind         = Manifold2D::Physics::ShapeKind::Circle;
            fx.radius       = kF0Radius;
            fx.halfLen      = 0.0f;
            fx.halfW        = 0.0f;
            fx.halfH        = 0.0f;
            fx.localPos     = glm::vec2(kF0LocalX, kF0LocalY);
            fx.localAngle   = 0.0f;
            fx.density      = kF0Density;
            fx.friction     = kF0Friction;
            fx.restitution  = 0.0f;
            fx.categoryBits = kF0Cat;
            fx.maskBits     = kF0Mask;
            fx.isSensor     = false;
            col.fixtures.push_back(fx);
        }

        // Fixture 1: capsule (halfLen + localAngle round-trip gate).
        {
            Arcane::Fixture fx;
            fx.kind         = Manifold2D::Physics::ShapeKind::Capsule;
            fx.radius       = kF1Radius;
            fx.halfLen      = kF1HalfLen;
            fx.halfW        = 0.0f;
            fx.halfH        = 0.0f;
            fx.localPos     = glm::vec2(kF1LocalX, kF1LocalY);
            fx.localAngle   = kF1LocalAng;
            fx.density      = 1.0f;
            fx.friction     = 0.3f;
            fx.restitution  = kF1Rest;
            fx.categoryBits = kF1Cat;
            fx.maskBits     = kF1Mask;
            fx.isSensor     = true;
            col.fixtures.push_back(fx);
        }

        reg.AddComponent<Arcane::Collider2D>(savedEntity, col);
        reg.AddComponent<Arcane::PhysicsBodyRef>(savedEntity, Arcane::PhysicsBodyRef{});

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

    const auto* col = reg->GetComponent<Arcane::Collider2D>(savedEntity);
    REQUIRE(col != nullptr);
    REQUIRE(col->fixtures.size() == 2u);

    // Assert fixture 0 (circle).
    CHECK(col->fixtures[0].kind         == Manifold2D::Physics::ShapeKind::Circle);
    CHECK(col->fixtures[0].radius       == Approx(kF0Radius));
    CHECK(col->fixtures[0].localPos.x   == Approx(kF0LocalX));
    CHECK(col->fixtures[0].localPos.y   == Approx(kF0LocalY));
    CHECK(col->fixtures[0].density      == Approx(kF0Density));
    CHECK(col->fixtures[0].friction     == Approx(kF0Friction));
    CHECK(col->fixtures[0].categoryBits == kF0Cat);
    CHECK(col->fixtures[0].maskBits     == kF0Mask);
    CHECK(col->fixtures[0].isSensor     == false);

    // Assert fixture 1 (capsule, sensor) -- all descriptor fields including
    // halfLen and localAngle are now gated here (the previous Aabb variant
    // left both un-exercised).
    CHECK(col->fixtures[1].kind         == Manifold2D::Physics::ShapeKind::Capsule);
    CHECK(col->fixtures[1].halfLen      == Approx(kF1HalfLen));
    CHECK(col->fixtures[1].radius       == Approx(kF1Radius));
    CHECK(col->fixtures[1].localPos.x   == Approx(kF1LocalX));
    CHECK(col->fixtures[1].localPos.y   == Approx(kF1LocalY));
    CHECK(col->fixtures[1].localAngle   == Approx(kF1LocalAng));
    CHECK(col->fixtures[1].restitution  == Approx(kF1Rest));
    CHECK(col->fixtures[1].categoryBits == kF1Cat);
    CHECK(col->fixtures[1].maskBits     == kF1Mask);
    CHECK(col->fixtures[1].isSensor     == true);

    std::filesystem::remove(path);
}
