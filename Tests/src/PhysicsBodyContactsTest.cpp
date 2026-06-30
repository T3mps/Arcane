// Per-body contact adjacency (m_bodyContacts) invariant guard.
//
// Builds a churning mixed-shape pile so contacts are created, destroyed, and
// updated every step, and asserts DebugValidateBodyContacts() holds throughout:
// m_bodyContacts mirrors the live dyn-dyn body contacts exactly. This guards the
// create/destroy/recycle maintenance independently of SplitIsland.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <cstdint>
#include <vector>

using namespace Arcane::Physics;

namespace
{
    constexpr Real kStep = Real(1) / Real(60);
}

TEST_CASE("Body-contact adjacency mirrors live dyn-dyn contacts", "[physics][island]")
{
    WorldDef wd;
    wd.gravityY = Real(400);
    PhysicsWorld w(wd);

    // Wide static floor (gravity +y -> bodies fall down onto it).
    {
        BodyDef fd;
        fd.type     = BodyType::Static;
        fd.position = Vec2(Real(0), Real(200));
        fd.shape    = MakeAabb(Real(300), Real(20));
        w.AddBody(fd);
    }

    // 90 dynamic mixed-shape bodies raining into a pile (heavy contact churn).
    std::uint32_t seed = 1234567u;
    auto rnd = [&](Real a, Real b) -> Real {
        seed = seed * 1664525u + 1013904223u;
        return a + (b - a) * Real((seed >> 8) & 0xFFFF) / Real(65535);
    };
    for (int i = 0; i < 90; ++i)
    {
        BodyDef d;
        d.type     = BodyType::Dynamic;
        d.density  = Real(1);
        d.friction = Real(0.4);
        d.position = Vec2(rnd(Real(-200), Real(200)), rnd(Real(-150), Real(150)));
        if (i % 3 == 0)      { d.shape = MakeCircle(rnd(Real(6), Real(12))); }
        else if (i % 3 == 1) { d.shape = MakeAabb(Real(8), Real(8)); d.fixedRotation = true; }
        else                 { d.shape = MakeCapsule(Real(10), Real(5)); }
        w.AddBody(d);
    }

    REQUIRE(w.DebugValidateBodyContacts()); // holds before any step
    for (int k = 0; k < 200; ++k)
    {
        w.Step(kStep);
        REQUIRE(w.DebugValidateBodyContacts()); // and after every step
    }
}
