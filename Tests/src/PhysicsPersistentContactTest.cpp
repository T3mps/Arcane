// Persistent contacts: survive across steps, recompute the manifold at most once/step,
// destroy on fat-box separation + on body removal. (Pool populated each Step; not yet
// feeding the solver -- that is Task 4.)
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Body.hpp>

using namespace Arcane::Physics;

TEST_CASE("Persistent contact survives across steps + destroys on separation", "[physics]")
{
    PhysicsWorld w;
    BodyDef d; d.shape = MakeAabb(Real(10), Real(10)); d.fixedRotation = true;
    d.type = BodyType::Static;  d.position = Vec2(0, 100);  w.AddBody(d);
    d.type = BodyType::Dynamic; d.position = Vec2(0, 81);   BodyHandle dyn = w.AddBody(d); // overlapping
    w.Step(Real(1) / Real(60));

    REQUIRE(w.DebugContactCount() >= 1);                 // a contact exists for the overlapping pair
    const std::size_t firstStepContacts = w.DebugContactCount();

    w.Step(Real(1) / Real(60));
    REQUIRE(w.DebugContactCount() == firstStepContacts); // persists (NOT recreated from scratch)

    w.SetPosition(dyn, Vec2(10000, 10000));              // teleport far -> fat boxes separate
    w.Step(Real(1) / Real(60));
    w.Step(Real(1) / Real(60));
    REQUIRE(w.DebugContactCount() == 0);                 // contact destroyed
}

TEST_CASE("Persistent contact destroyed when a body is removed", "[physics]")
{
    PhysicsWorld w;
    BodyDef d; d.shape = MakeAabb(Real(10), Real(10)); d.fixedRotation = true;
    d.type = BodyType::Static;  d.position = Vec2(0, 100);  BodyHandle s = w.AddBody(d);
    d.type = BodyType::Dynamic; d.position = Vec2(0, 81);   w.AddBody(d);
    w.Step(Real(1) / Real(60));
    REQUIRE(w.DebugContactCount() >= 1);

    w.RemoveBody(s);
    w.Step(Real(1) / Real(60));
    REQUIRE(w.DebugContactCount() == 0);                 // no stale contact to the dead body
}
