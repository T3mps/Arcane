// Persistent contacts: survive across steps, recompute the manifold at most once/step,
// destroy on fat-box separation + on body removal. (Pool populated each Step; not yet
// feeding the solver -- that is Task 4.)
#include <algorithm>
#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/Solver/Solver.hpp> // ContactConstraint

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

// ---- oracle-gate helpers (Phase 3, Task 3) ----------------------------------

namespace
{
    // A body-only resting scene: a static floor, a 3-box dynamic stack resting on
    // it, and a dynamic circle resting on the top box. NO tile geometry (spans are
    // Task 4). Convention (matches the other tests in this TU + PhysicsWorld.lua):
    // SMALLER y is HIGHER and gravity pulls toward +y (downward), so a body ABOVE
    // the floor has a smaller y and falls onto it. The world is constructed with
    // gravity by the caller. The boxes are square (hw == hh) so a flat box-on-box
    // manifold has 2 points; the circle-on-box manifold has 1.
    void BuildOracleScene(PhysicsWorld& w)
    {
        // Static floor (wide, thin slab) centered at y = 0; top surface at y = -5.
        BodyDef floor;
        floor.type     = BodyType::Static;
        floor.shape    = MakeAabb(Real(50), Real(5));
        floor.position = Vec2(Real(0), Real(0));      // top at y = -5
        floor.friction = Real(0.4);
        w.AddBody(floor);

        // 3 dynamic boxes stacked ABOVE the floor (hw=hh=5 -> 10x10 each). They
        // start lightly OVERLAPPING (centers 10 apart minus a 0.2 bite) so they are
        // already touching at step 0 and only settle a hair under gravity into a
        // stable resting stack -- a clean multi-point manifold per pair.
        // NOTE: a dynamic AABB shape must be fixedRotation (the engine asserts an
        // axis-aligned box can't rotate); the boxes stack flat so this is natural.
        const Real boxHalf = Real(5);
        BodyDef box;
        box.type          = BodyType::Dynamic;
        box.shape         = MakeAabb(boxHalf, boxHalf);
        box.density       = Real(1);
        box.friction      = Real(0.4);
        box.fixedRotation = true;

        box.position = Vec2(Real(0), Real(-9.8));  // bottom box (rests on floor top at -5)
        w.AddBody(box);
        box.position = Vec2(Real(0), Real(-19.6)); // middle box
        w.AddBody(box);
        box.position = Vec2(Real(0), Real(-29.4)); // top box
        w.AddBody(box);

        // A dynamic circle (r=5) resting on the top box. Center 9.8 above the top
        // box center so it sits just on top (light overlap, settles into contact).
        BodyDef ball;
        ball.type     = BodyType::Dynamic;
        ball.shape    = MakeCircle(Real(5));
        ball.density  = Real(1);
        ball.friction = Real(0.4);
        ball.position = Vec2(Real(0), Real(-39.2));
        w.AddBody(ball);
    }

    // Sort BOTH sets by (bodyA, bodyB, points[0].id), then require equal size +
    // per-element equality of bodyA, bodyB, pointCount, normal (within 1e-4),
    // bodyBIsBody + kind (EXACT), friction/restitution/invMassA/invMassB/
    // invInertiaA/invInertiaB (within 1e-4), and each point's id (exact) +
    // baseSeparation (within 1e-4). Anchors are NOT compared (they are a function
    // of post-step COM, which the oracle does not key on -- the manifold fields
    // below are all sourced from the same cached manifold).
    bool SameConstraintSet(std::vector<ContactConstraint> a,
                           std::vector<ContactConstraint> b)
    {
        auto less = [](const ContactConstraint& x, const ContactConstraint& y)
        {
            if (x.bodyA != y.bodyA) { return x.bodyA < y.bodyA; }
            if (x.bodyB != y.bodyB) { return x.bodyB < y.bodyB; }
            return x.points[0].id < y.points[0].id;
        };
        std::sort(a.begin(), a.end(), less);
        std::sort(b.begin(), b.end(), less);

        if (a.size() != b.size())
        {
            return false;
        }
        const Real kEps = Real(1e-4);
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            const ContactConstraint& x = a[i];
            const ContactConstraint& y = b[i];
            if (x.bodyA != y.bodyA || x.bodyB != y.bodyB ||
                x.pointCount != y.pointCount)
            {
                return false;
            }
            // EXACT enum/bool fields the emit lambda assigns.
            if (x.kind != y.kind || x.bodyBIsBody != y.bodyBIsBody)
            {
                return false;
            }
            if (std::fabs(x.normal.x - y.normal.x) > kEps ||
                std::fabs(x.normal.y - y.normal.y) > kEps)
            {
                return false;
            }
            // Combined material coefficients + cached inverse mass/inertia.
            if (std::fabs(x.friction - y.friction) > kEps ||
                std::fabs(x.restitution - y.restitution) > kEps ||
                std::fabs(x.invMassA - y.invMassA) > kEps ||
                std::fabs(x.invMassB - y.invMassB) > kEps ||
                std::fabs(x.invInertiaA - y.invInertiaA) > kEps ||
                std::fabs(x.invInertiaB - y.invInertiaB) > kEps)
            {
                return false;
            }
            for (int p = 0; p < x.pointCount; ++p)
            {
                if (x.points[p].id != y.points[p].id)
                {
                    return false;
                }
                if (std::fabs(x.points[p].baseSeparation -
                              y.points[p].baseSeparation) > kEps)
                {
                    return false;
                }
            }
        }
        return true;
    }
} // namespace

// The persistent-contact solver feed must produce the SAME constraint set as the legacy
// GenerateContacts path (same pairs/ids/normals/points) -- the pre-swap oracle. Sleeping
// is DISABLED so the awake-gate timing can't make the two diverge spuriously; the scene is
// body-only (no tile spans -- spans are Task 4).
TEST_CASE("Persistent contact walk == GenerateContacts constraint set", "[physics]")
{
    WorldDef wd;
    wd.gravityY = Real(400);                             // downward (+y), settles the stack onto the floor
    PhysicsWorld w(wd);
    BuildOracleScene(w);                                 // static floor + a 3-box stack + a circle (body-only)

    // No WorldDef sleep toggle exists in this engine; sleeping is governed by the
    // Island module's kSleepTime (0.5s) threshold. "Disable sleeping" for the oracle
    // by clearing each body's awake flag + sleep timer every step so an idle island
    // never crosses the threshold (the stage-5 island pass accumulates at most one
    // dt before the next reset). Both GenerateContacts AND the pool walk then see
    // every contact awake, so the awake-gate can't gate one path but not the other.
    const std::uint32_t count = w.Count();
    auto keepAwake = [&]()
    {
        for (std::uint32_t s = 0; s < count; ++s)
        {
            if (w.Alive(s) && w.TypeSlot(s) == BodyType::Dynamic)
            {
                w.SetAwakeSlot(s, true);
                w.SetSleepTimerSlot(s, Real(0));
            }
        }
    };
    for (int i = 0; i < 40; ++i)
    {
        keepAwake();                                     // awake BEFORE Step's stage-2 GenerateContacts
        w.Step(Real(1) / Real(60));
    }
    // The final Step's stage-5 island pass runs AFTER GenerateContacts, so if it
    // slept anything the post-step awake flags would no longer match what
    // GenerateContacts saw. Re-assert awake so EmitContactConstraints' awake-gate
    // observes the same awake state GenerateContacts did this frame.
    keepAwake();

    std::vector<ContactConstraint> fromPool;
    w.DebugEmitPoolConstraints(fromPool);                // EmitContactConstraints into 'out'
    std::vector<ContactConstraint> fromGen;
    w.DebugCopyActiveConstraints(fromGen);               // copy of m_contactConstraints (GenerateContacts)

    REQUIRE(!fromGen.empty());                            // the scene actually has contacts
    REQUIRE(SameConstraintSet(fromPool, fromGen));       // sorted set-equality on key fields
}
