// PhysicsEvents2DTest.cpp -- [physics][events]: the Arcane event surface (spec
// 2026-10-08 s7). Bare registry + PhysicsSystem, +Y DOWN, g = 10 (the
// PhysicsSystemTest convention).
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <string>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsEvents2D.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>
#include "Helpers/ReferenceProjectDir.hpp"
#include "Helpers/SettingsSweep.hpp"

namespace
{
    constexpr float kDt = 1.0f / 60.0f;

    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ components };
        World()
        {
            Arcane::RegisterSceneComponents(reg);
            Arcane::RegisterPhysicsComponents(reg);
            Manifold2D::Physics::WorldDef wd; wd.gravityY = 10.0f;
            reg.SetResource(Arcane::PhysicsResource{ std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd), {} });
        }
        Astra::Entity Body(const char* name, Manifold2D::Physics::BodyType type, glm::vec2 pos,
                           std::vector<Arcane::Fixture> fixtures, glm::vec3 scale = glm::vec3(1.0f))
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Identity id; id.id = Arcane::Guid::Generate(); id.name = name;
            reg.AddComponent<Arcane::Identity>(e, id);
            Arcane::Transform t; t.position = glm::vec3(pos, 0.0f); t.scale = scale;
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
            Arcane::RigidBody2D rb; rb.type = type;
            // R8: a dynamic AABB asserts fixedRotation. Every dynamic body here is an Aabb.
            if (type == Manifold2D::Physics::BodyType::Dynamic)
                rb.fixedRotation = true;
            reg.AddComponent<Arcane::RigidBody2D>(e, rb);
            Arcane::Collider2D col; col.fixtures = std::move(fixtures);
            reg.AddComponent<Arcane::Collider2D>(e, col);
            reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});
            return e;
        }
        Arcane::PhysicsResource& Res() { return *reg.GetResource<Arcane::PhysicsResource>(); }
        Arcane::Guid GuidOf(Astra::Entity e) { return std::as_const(reg).GetComponent<Arcane::Identity>(e)->id; }
    };

    Arcane::Fixture Box(float hw, float hh)
    {
        Arcane::Fixture f; f.kind = Manifold2D::Physics::ShapeKind::Aabb; f.halfW = hw; f.halfH = hh; return f;
    }

    int LandingHits()
    {
        World w;
        Arcane::Fixture hitty = Box(0.5f, 0.5f); hitty.hitEvents = true;
        w.Body("Ground", Manifold2D::Physics::BodyType::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
        w.Body("Crate", Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -2.5f }, { hitty });
        Arcane::PhysicsSystem physics(kDt);
        int hits = 0;
        for (int i = 0; i < 90; ++i) { physics(w.reg); hits += static_cast<int>(w.Res().StepEvents().contactHit.size()); }
        return hits;
    }
}

TEST_CASE("Fixture event flags default contact+sensor on, hit off", "[physics][events]")
{
    Arcane::Fixture f;
    CHECK(f.contactEvents);
    CHECK(f.sensorEvents);
    CHECK_FALSE(f.hitEvents);
}

TEST_CASE("StepEvents reports a crate landing on static ground, by entity + GUID + fixture", "[physics][events]")
{
    World w;
    const Astra::Entity ground = w.Body("Ground", Manifold2D::Physics::BodyType::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    const Astra::Entity crate  = w.Body("Crate",  Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -2.0f }, { Box(0.5f, 0.5f) });
    Arcane::PhysicsSystem physics(kDt);
    int begins = 0;
    for (int i = 0; i < 120; ++i)
    {
        physics(w.reg);
        for (const Arcane::ContactBegin2D& e : w.Res().StepEvents().contactBegin)
        {
            ++begins;
            CHECK(e.a.entity == crate);           // A = the dynamic side
            CHECK(e.a.guid == w.GuidOf(crate));
            CHECK(e.a.fixture == 0u);
            CHECK(e.b.entity == ground);
            CHECK(e.b.guid == w.GuidOf(ground));
        }
    }
    CHECK(begins == 1);
}

TEST_CASE("fixture indices map through the auto-fixture and AddFixture paths", "[physics][events]")
{
    World w;
    Arcane::Fixture sensor = Box(2.0f, 2.0f); sensor.isSensor = true;    // fixture 0: the auto-fixture path
    Arcane::Fixture solid  = Box(0.3f, 0.3f); solid.localPos = { 0.0f, 5.0f };   // fixture 1: AddFixture
    const Astra::Entity zone = w.Body("Zone", Manifold2D::Physics::BodyType::Static, { 0.0f, -6.0f }, { sensor, solid });
    const Astra::Entity crate = w.Body("Crate", Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -12.0f }, { Box(0.5f, 0.5f) });
    Arcane::PhysicsSystem physics(kDt);
    int sensorBegins = 0;
    for (int i = 0; i < 90; ++i)
    {
        physics(w.reg);
        for (const Arcane::SensorBegin2D& e : w.Res().StepEvents().sensorBegin)
        {
            ++sensorBegins;
            CHECK(e.sensor.entity == zone);
            CHECK(e.sensor.fixture == 0u);
            CHECK(e.visitor.entity == crate);
        }
    }
    CHECK(sensorBegins == 1);
}

TEST_CASE("an AddFixture surface reports fixture index 1", "[physics][events]")
{
    World w;
    Arcane::Fixture away = Box(0.2f, 0.2f); away.localPos = { 30.0f, 0.0f };   // fixture 0: auto-fixture, off the fall
    Arcane::Fixture surface = Box(10.0f, 0.5f);                                 // fixture 1: AddFixture, the landing
    const Astra::Entity ground = w.Body("Ground", Manifold2D::Physics::BodyType::Static, { 0.0f, 0.5f }, { away, surface });
    const Astra::Entity crate  = w.Body("Crate",  Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -2.0f }, { Box(0.5f, 0.5f) });
    Arcane::PhysicsSystem physics(kDt);
    int begins = 0;
    for (int i = 0; i < 120; ++i)
    {
        physics(w.reg);
        for (const Arcane::ContactBegin2D& e : w.Res().StepEvents().contactBegin)
        {
            ++begins;
            CHECK(e.a.entity == crate);
            CHECK(e.b.entity == ground);
            CHECK(e.b.guid == w.GuidOf(ground));
            CHECK(e.b.fixture == 1u);
        }
    }
    CHECK(begins == 1);
}

TEST_CASE("Opting one fixture out silences the pair (both-fixtures rule)", "[physics][events]")
{
    World w;
    Arcane::Fixture quiet = Box(10.0f, 0.5f); quiet.contactEvents = false;
    w.Body("Ground", Manifold2D::Physics::BodyType::Static, { 0.0f, 0.5f }, { quiet });
    w.Body("Crate", Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -2.0f }, { Box(0.5f, 0.5f) });   // default on
    Arcane::PhysicsSystem physics(kDt);
    int begins = 0;
    for (int i = 0; i < 120; ++i) { physics(w.reg); begins += static_cast<int>(w.Res().StepEvents().contactBegin.size()); }
    CHECK(begins == 0);
}

TEST_CASE("A destroyed entity's End still carries its GUID", "[physics][events]")
{
    World w;
    w.Body("Ground", Manifold2D::Physics::BodyType::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    const Astra::Entity crate = w.Body("Crate", Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -0.49f }, { Box(0.5f, 0.5f) });
    const Arcane::Guid guid = w.GuidOf(crate);
    Arcane::PhysicsSystem physics(kDt);
    for (int i = 0; i < 10; ++i) physics(w.reg);
    w.reg.DestroyEntity(crate);
    physics(w.reg);                                // PASS 1 removes + retires; the step delivers the End
    const auto ends = w.Res().StepEvents().contactEnd;
    REQUIRE(ends.size() == 1);
    CHECK(ends[0].a.guid == guid);
    CHECK_FALSE(w.reg.IsValid(ends[0].a.entity));
    physics(w.reg);
    CHECK(w.Res().bodyRecords.size() == 1);        // retired record erased after its delivery
}

TEST_CASE("a recycled body slot never resolves to the retired record", "[physics][events]")
{
    World w;
    w.Body("Ground", Manifold2D::Physics::BodyType::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    const Astra::Entity first = w.Body("First", Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -0.49f }, { Box(0.5f, 0.5f) });
    Arcane::PhysicsSystem physics(kDt);
    for (int i = 0; i < 5; ++i) physics(w.reg);
    const Arcane::Guid firstGuid = w.GuidOf(first);
    w.reg.DestroyEntity(first);
    const Astra::Entity second = w.Body("Second", Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -0.49f }, { Box(0.5f, 0.5f) });
    physics(w.reg);                                // PASS 1 retires First, PASS 2 mints Second (same slot)
    bool sawFirstEnd = false, sawSecondBegin = false;
    for (const auto& e : w.Res().StepEvents().contactEnd)   if (e.a.guid == firstGuid) sawFirstEnd = true;
    for (const auto& e : w.Res().StepEvents().contactBegin) if (e.a.entity == second && e.a.guid == w.GuidOf(second)) sawSecondBegin = true;
    CHECK(sawFirstEnd);
    CHECK(sawSecondBegin);
}

TEST_CASE("a paused rescale retires the old fixture until the next capture", "[physics][events]")
{
    World w;
    w.Body("Ground", Manifold2D::Physics::BodyType::Static, { 0.0f, 0.5f }, { Box(10.0f, 0.5f) });
    const Astra::Entity crate = w.Body("Crate", Manifold2D::Physics::BodyType::Dynamic, { 0.0f, -0.49f }, { Box(0.5f, 0.5f) });
    const Arcane::Guid guid = w.GuidOf(crate);
    Arcane::PhysicsSystem physics(kDt);
    for (int i = 0; i < 10; ++i) physics(w.reg);   // resting contact, Begin already delivered

    Arcane::PhysicsSystem paused(kDt, /*stepWorld*/ false);
    w.reg.GetComponent<Arcane::Transform>(crate)->scale = glm::vec3(2.0f, 2.0f, 1.0f);
    paused(w.reg);                                 // rebuild: old handle retired, new handle current

    bool sawEnd = false, sawBegin = false;
    for (int i = 0; i < 8; ++i)
    {
        physics(w.reg);
        for (const Arcane::ContactEnd2D& e : w.Res().StepEvents().contactEnd)
            if (e.a.entity == crate && e.a.fixture == 0u && e.a.guid == guid) sawEnd = true;
        for (const Arcane::ContactBegin2D& e : w.Res().StepEvents().contactBegin)
            if (e.a.entity == crate && e.a.fixture == 0u && e.a.guid == guid) sawBegin = true;
    }
    CHECK(sawEnd);                                 // destroy-time End of the dropped fixture
    CHECK(sawBegin);                               // Begin of the rebuilt fixture
}

TEST_CASE("physics.events.hitThreshold reaches the world", "[physics][events]")
{
    {
        // Above the ~6 m/s landing. The guard publishes, and clears the Code
        // rung on the way out -- including when a REQUIRE fails.
        const Arcane::Test::ScopedCodeRung high("physics.events.hitThreshold", Arcane::CVarValue::Float32(50.0f));
        CHECK(LandingHits() == 0);
    }
    CHECK(LandingHits() >= 1);                     // default 1 m/s, restored
}

TEST_CASE("PhysicsEvents2D.hpp includes no Manifold2D header", "[physics][events][guard]")
{
    const auto path = Arcane::Test::FindReferenceProjectDir().parent_path() / "ArcaneCore/src/Arcane/Scene/PhysicsEvents2D.hpp";
    std::ifstream in(path);
    REQUIRE(in.good());
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("#include", 0) == 0) CHECK(line.find("Manifold2D") == std::string::npos);
}

TEST_CASE("Fixture event flags round-trip through scene JSON; absent keys keep the defaults", "[physics][events][json]")
{
    nlohmann::json doc;
    {
        World w;
        Arcane::Fixture f = Box(0.5f, 0.5f); f.contactEvents = false; f.hitEvents = true;
        const Astra::Entity root = w.reg.CreateEntity();
        w.reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
        Arcane::Collider2D col; col.fixtures.push_back(f);
        w.reg.AddComponent<Arcane::Collider2D>(root, col);
        w.reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });
        doc = Arcane::Scene::SaveJson(w.reg);
    }
    auto& fx = doc["entities"][0]["components"]["Arcane::Collider2D"]["fixtures"][0];
    CHECK(fx["contactEvents"] == false);
    CHECK(fx["hitEvents"] == true);
    fx.erase("sensorEvents");                      // a pre-spec scene has no key
    World loaded;
    REQUIRE(Arcane::Scene::LoadJson(loaded.reg, doc));
    loaded.reg.CreateView<Arcane::Collider2D>().ForEach([&](Astra::Entity, Arcane::Collider2D& c)
    {
        REQUIRE(c.fixtures.size() == 1);
        CHECK_FALSE(c.fixtures[0].contactEvents);
        CHECK(c.fixtures[0].hitEvents);
        CHECK(c.fixtures[0].sensorEvents);         // default kept
    });
}
