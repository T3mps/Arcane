// M6 Physics-v2 T6: Astra ECS physics component layer, fixture-list Arcane::Physics2D::Collider.
//
// Tests:
//   1. Components register + reflect: MetaRegistry has a non-null TypeMeta for
//      each of Arcane::Physics2D::RigidBody, Arcane::Physics2D::Collider, Arcane::Physics2D::BodyRef; GetFieldCount() > 0.
//   2. Binary round-trip (single fixture): a Registry with Arcane::Physics2D::RigidBody +
//      single-fixture Arcane::Physics2D::Collider authored values survives Save/Load with all
//      per-fixture fields intact.
//      (Arcane::Physics2D::BodyRef.handle is Serializable(false) on the name-keyed path;
//       it harmlessly round-trips on the binary/trivially-copyable path.)
//   3. Binary round-trip (two fixtures): a Arcane::Physics2D::Collider with TWO fixtures
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
    Arcane::Physics2D::RegisterComponents(creg);

    const auto* rb = creg.GetComponentDescriptor(Astra::TypeID<Arcane::Physics2D::RigidBody>::Value());
    const auto* col = creg.GetComponentDescriptor(Astra::TypeID<Arcane::Physics2D::Collider>::Value());
    const auto* ref = creg.GetComponentDescriptor(Astra::TypeID<Arcane::Physics2D::BodyRef>::Value());

    REQUIRE(rb  != nullptr);
    REQUIRE(col != nullptr);
    REQUIRE(ref != nullptr);

    CHECK(rb->visitFields  != nullptr);
    CHECK(col->visitFields != nullptr);
    CHECK(ref->visitFields != nullptr);

    // MetaRegistry must have a non-null TypeMeta with at least one field for each.
    const auto* rbMeta  = Astra::GetMeta<Arcane::Physics2D::RigidBody>();
    const auto* colMeta = Astra::GetMeta<Arcane::Physics2D::Collider>();
    const auto* refMeta = Astra::GetMeta<Arcane::Physics2D::BodyRef>();

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
        Arcane::Physics2D::RegisterComponents(reg);

        savedEntity = reg.CreateEntity();

        Arcane::Physics2D::RigidBody rb;
        rb.type           = Arcane::Physics2D::BodyType::Dynamic;
        rb.velocity       = glm::vec2(kVelX, kVelY);
        rb.mass           = kMass;
        rb.linearDamping  = kDamping;
        rb.fixedRotation  = true;
        rb.bullet         = false;
        reg.AddComponent<Arcane::Physics2D::RigidBody>(savedEntity, rb);

        // Single-fixture Arcane::Physics2D::Collider.
        Arcane::Physics2D::Collider col;
        {
            Arcane::Physics2D::Fixture fx;
            fx.kind         = Arcane::Physics2D::ShapeKind::Circle;
            fx.radius       = kRadius;
            fx.restitution  = kRestitution;
            fx.friction     = kFriction;
            fx.density      = kDensity;
            fx.categoryBits = kCatBits;
            fx.maskBits     = kMaskBits;
            fx.isSensor     = true;
            col.fixtures.push_back(fx);
        }
        reg.AddComponent<Arcane::Physics2D::Collider>(savedEntity, col);

        // Arcane::Physics2D::BodyRef holds runtime state; we add it with a non-default
        // handle to ensure the binary trivially-copyable path round-trips it.
        Arcane::Physics2D::BodyRef bref;
        bref.handle = Manifold2D::Physics::BodyHandle{ 7u, 3u };
        reg.AddComponent<Arcane::Physics2D::BodyRef>(savedEntity, bref);

        auto saved = reg.Save(path);
        REQUIRE(saved.IsOk());
    }

    // Load into a fresh registry.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Arcane::RegisterSceneComponents(*components);
    Arcane::Physics2D::RegisterComponents(*components);
    auto loaded = Astra::Registry::Load(path, components);
    REQUIRE(loaded.IsOk());
    std::unique_ptr<Astra::Registry> reg = std::move(*loaded.GetValue());

    // Assert Arcane::Physics2D::RigidBody values survived.
    const auto* rb = reg->GetComponent<Arcane::Physics2D::RigidBody>(savedEntity);
    REQUIRE(rb != nullptr);
    CHECK(rb->type           == Arcane::Physics2D::BodyType::Dynamic);
    CHECK(rb->velocity.x     == Approx(kVelX));
    CHECK(rb->velocity.y     == Approx(kVelY));
    CHECK(rb->mass           == Approx(kMass));
    CHECK(rb->linearDamping  == Approx(kDamping));
    CHECK(rb->fixedRotation  == true);
    CHECK(rb->bullet         == false);

    // Assert Arcane::Physics2D::Collider: single fixture values survived.
    const auto* col = reg->GetComponent<Arcane::Physics2D::Collider>(savedEntity);
    REQUIRE(col != nullptr);
    REQUIRE(col->fixtures.size() == 1u);
    CHECK(col->fixtures[0].kind         == Arcane::Physics2D::ShapeKind::Circle);
    CHECK(col->fixtures[0].radius       == Approx(kRadius));
    CHECK(col->fixtures[0].restitution  == Approx(kRestitution));
    CHECK(col->fixtures[0].friction     == Approx(kFriction));
    CHECK(col->fixtures[0].density      == Approx(kDensity));
    CHECK(col->fixtures[0].categoryBits == kCatBits);
    CHECK(col->fixtures[0].maskBits     == kMaskBits);
    CHECK(col->fixtures[0].isSensor     == true);

    // Arcane::Physics2D::BodyRef present on the entity.
    const auto* bref = reg->GetComponent<Arcane::Physics2D::BodyRef>(savedEntity);
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

TEST_CASE("Arcane::Physics2D::Collider two-fixture round-trip: all per-fixture fields survive Save/Load", "[physics]")
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "arcane_physics_components_2fixture_roundtrip.bin";

    // Arcane::Physics2D::Fixture 0: circle r=4 @ local(0,0), density 1, friction 0.3.
    constexpr float kF0Radius   = 4.0f;
    constexpr float kF0Density  = 1.0f;
    constexpr float kF0Friction = 0.3f;
    constexpr float kF0LocalX   = 0.0f;
    constexpr float kF0LocalY   = 0.0f;
    constexpr uint32_t kF0Cat   = 0x01u;
    constexpr uint32_t kF0Mask  = 0xFFFFFFFFu;

    // Arcane::Physics2D::Fixture 1: capsule(halfLen=1.5, r=0.4) @ local(10,0) localAngle=0.5, isSensor=true.
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
        Arcane::Physics2D::RegisterComponents(reg);

        savedEntity = reg.CreateEntity();

        // Minimal Arcane::Physics2D::RigidBody (only need it present for the overall test).
        Arcane::Physics2D::RigidBody rb;
        rb.type = Arcane::Physics2D::BodyType::Dynamic;
        reg.AddComponent<Arcane::Physics2D::RigidBody>(savedEntity, rb);

        // Two-fixture Arcane::Physics2D::Collider.
        Arcane::Physics2D::Collider col;

        // Arcane::Physics2D::Fixture 0: circle.
        {
            Arcane::Physics2D::Fixture fx;
            fx.kind         = Arcane::Physics2D::ShapeKind::Circle;
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

        // Arcane::Physics2D::Fixture 1: capsule (halfLen + localAngle round-trip gate).
        {
            Arcane::Physics2D::Fixture fx;
            fx.kind         = Arcane::Physics2D::ShapeKind::Capsule;
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

        reg.AddComponent<Arcane::Physics2D::Collider>(savedEntity, col);
        reg.AddComponent<Arcane::Physics2D::BodyRef>(savedEntity, Arcane::Physics2D::BodyRef{});

        auto saved = reg.Save(path);
        REQUIRE(saved.IsOk());
    }

    // Load into a fresh registry.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Arcane::RegisterSceneComponents(*components);
    Arcane::Physics2D::RegisterComponents(*components);
    auto loaded = Astra::Registry::Load(path, components);
    REQUIRE(loaded.IsOk());
    std::unique_ptr<Astra::Registry> reg = std::move(*loaded.GetValue());

    const auto* col = reg->GetComponent<Arcane::Physics2D::Collider>(savedEntity);
    REQUIRE(col != nullptr);
    REQUIRE(col->fixtures.size() == 2u);

    // Assert fixture 0 (circle).
    CHECK(col->fixtures[0].kind         == Arcane::Physics2D::ShapeKind::Circle);
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
    CHECK(col->fixtures[1].kind         == Arcane::Physics2D::ShapeKind::Capsule);
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
