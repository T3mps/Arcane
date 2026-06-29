// SpatialGrid: a per-shape fixed-tile spatial hash. Contract: after the caller's
// tight AABB filter, QueryAABB returns EXACTLY the brute-force overlap set,
// sorted + unique. The grid only narrows; correctness is gated by the oracle.
#include <algorithm>
#include <random>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/Broadphase/SpatialGrid.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp> // AabbOverlap

using namespace Arcane::Physics;

namespace
{
    Aabb2 Box(Real x0, Real y0, Real x1, Real y1)
    { Aabb2 a; a.min = Vec2(x0,y0); a.max = Vec2(x1,y1); return a; }
}

TEST_CASE("SpatialGrid query == brute-force after tight filter", "[physics][grid]")
{
    std::mt19937 rng(0xC0FFEE);
    std::uniform_real_distribution<float> pos(-500.0f, 500.0f);
    std::uniform_real_distribution<float> ext(2.0f, 60.0f);

    SpatialGrid grid(/*tileSize=*/32.0f);
    std::vector<Aabb2> boxes;
    for (std::uint32_t i = 0; i < 400; ++i)
    {
        const float x = pos(rng), y = pos(rng), w = ext(rng), h = ext(rng);
        Aabb2 b = Box(x, y, x + w, y + h);
        boxes.push_back(b);
        grid.Insert(i, b);
    }

    auto tightFilter = [&](const Aabb2& q, std::vector<std::uint32_t> cand) {
        std::vector<std::uint32_t> out;
        for (std::uint32_t id : cand) if (AabbOverlap(boxes[id], q)) out.push_back(id);
        std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    };
    auto brute = [&](const Aabb2& q) {
        std::vector<std::uint32_t> out;
        for (std::uint32_t i = 0; i < boxes.size(); ++i) if (AabbOverlap(boxes[i], q)) out.push_back(i);
        return out; // already sorted ascending
    };

    for (int t = 0; t < 200; ++t)
    {
        const float x = pos(rng), y = pos(rng), w = ext(rng), h = ext(rng);
        Aabb2 q = Box(x, y, x + w, y + h);
        std::vector<std::uint32_t> cand;
        grid.QueryAABB(q, cand);
        REQUIRE(tightFilter(q, cand) == brute(q));
    }

    SECTION("Move + Remove keep the invariant")
    {
        grid.Move(0, Box(900.0f, 900.0f, 920.0f, 920.0f)); boxes[0] = Box(900,900,920,920);
        grid.Remove(1); boxes[1] = Box(1e9f, 1e9f, 1e9f, 1e9f); // unreachable
        Aabb2 q = Box(890.0f, 890.0f, 930.0f, 930.0f);
        std::vector<std::uint32_t> cand; grid.QueryAABB(q, cand);
        std::vector<std::uint32_t> got = tightFilter(q, cand);
        REQUIRE(std::find(got.begin(), got.end(), 0u) != got.end());
        REQUIRE(std::find(got.begin(), got.end(), 1u) == got.end());
    }

    SECTION("Empty-grid query returns 0 and empty out")
    {
        SpatialGrid empty(32.0f);
        std::vector<std::uint32_t> out;
        int n = empty.QueryAABB(Box(-100.0f, -100.0f, 100.0f, 100.0f), out);
        REQUIRE(n == 0);
        REQUIRE(out.empty());
    }

    SECTION("Double-insert self-heals: id moves from A to B without explicit Remove")
    {
        SpatialGrid g(32.0f);
        // Insert id 7 at box A (well within query range of origin)
        Aabb2 boxA = Box(0.0f, 0.0f, 10.0f, 10.0f);
        // Box B is far away so no AABB overlap between A and B
        Aabb2 boxB = Box(5000.0f, 5000.0f, 5010.0f, 5010.0f);
        g.Insert(7u, boxA);
        // Re-insert at B without an explicit Remove -- self-healing fix must handle this
        g.Insert(7u, boxB);

        std::vector<std::uint32_t> cand;
        // Query near A: id 7 must NOT be there (it moved to B)
        g.QueryAABB(Box(-5.0f, -5.0f, 15.0f, 15.0f), cand);
        REQUIRE(std::find(cand.begin(), cand.end(), 7u) == cand.end());

        // Query near B: id 7 MUST be there
        cand.clear();
        g.QueryAABB(Box(4995.0f, 4995.0f, 5015.0f, 5015.0f), cand);
        REQUIRE(std::find(cand.begin(), cand.end(), 7u) != cand.end());
    }
}

#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Shapes.hpp>

TEST_CASE("StaticCandidates static-body set unchanged by grid reroute", "[physics][grid]")
{
    PhysicsWorld w;
    auto addStatic = [&](Real cx, Real cy, Real hw, Real hh) {
        BodyDef d; d.type = BodyType::Static; d.position = Vec2(cx, cy);
        d.shape = MakeAabb(hw, hh); return w.AddBody(d);
    };
    addStatic(0,   0,   50, 10);
    addStatic(120, 0,   50, 10);
    addStatic(0,   200, 10, 80);

    // Reference: brute-force overlap of the query box against every static slot.
    Aabb2 q; q.min = Vec2(-60, -20); q.max = Vec2(60, 20);
    std::vector<Aabb2> spans; std::vector<std::uint32_t> statics;
    std::vector<std::uint32_t> gridScratch;
    w.StaticCandidates(q, spans, statics, gridScratch);
    // Only the first static (centered at 0,0, half 50x10) overlaps q.
    REQUIRE(statics.size() == 1);
}

TEST_CASE("Residents(region) returns bodies in a tile region", "[physics][grid][residency]")
{
    PhysicsWorld w;
    BodyDef d; d.type = BodyType::Dynamic; d.shape = MakeCircle(Real(8));
    d.position = Vec2(10, 10);   BodyHandle a = w.AddBody(d);
    d.position = Vec2(300, 300); BodyHandle b = w.AddBody(d);
    w.Step(Real(1)/Real(60));    // commit positions -> residency updated

    // Region covering ~(0,0)..(64,64) in world space should contain `a`, not `b`.
    Aabb2 region; region.min = Vec2(0, 0); region.max = Vec2(64, 64);
    std::vector<std::uint32_t> residents;
    w.Residents(region, residents);
    REQUIRE(std::find(residents.begin(), residents.end(), a.index) != residents.end());
    REQUIRE(std::find(residents.begin(), residents.end(), b.index) == residents.end());
}

TEST_CASE("Residency tracks a body across a per-step position commit", "[physics][grid][residency]")
{
    PhysicsWorld w;
    BodyDef d; d.type = BodyType::Kinematic; d.shape = MakeCircle(Real(8));
    d.position = Vec2(10, 10);
    BodyHandle a = w.AddBody(d);
    w.SetVelocity(a, Vec2(1000, 0));   // moves +x deterministically

    w.Step(Real(1));                   // kinematic integrate: a -> ~(1010, 10)

    // It must have LEFT the origin region (proves Move-on-commit ran -- an
    // Insert-only residency would wrongly still report it here).
    Aabb2 origin; origin.min = Vec2(0, 0);   origin.max = Vec2(64, 64);
    std::vector<std::uint32_t> residents;
    w.Residents(origin, residents);
    REQUIRE(std::find(residents.begin(), residents.end(), a.index) == residents.end());

    // ...and ENTERED the destination region.
    Aabb2 dest; dest.min = Vec2(950, 0); dest.max = Vec2(1100, 64);
    w.Residents(dest, residents);
    REQUIRE(std::find(residents.begin(), residents.end(), a.index) != residents.end());
}

// G1: RemoveBody must evict a static from the grid (StaticCandidates stops
// returning it). Guards the m_staticGrid.Remove lockstep in RemoveBody.
TEST_CASE("StaticCandidates drops a static after RemoveBody", "[physics][grid]")
{
    PhysicsWorld w;
    BodyDef d; d.type = BodyType::Static; d.position = Vec2(0, 0);
    d.shape = MakeAabb(Real(50), Real(10));
    BodyHandle s = w.AddBody(d);

    Aabb2 q; q.min = Vec2(-60, -20); q.max = Vec2(60, 20);
    std::vector<Aabb2> spans; std::vector<std::uint32_t> statics;
    std::vector<std::uint32_t> gridScratch;
    w.StaticCandidates(q, spans, statics, gridScratch);
    REQUIRE(statics.size() == 1);          // present before removal

    w.RemoveBody(s);
    statics.clear(); spans.clear();
    w.StaticCandidates(q, spans, statics, gridScratch);
    REQUIRE(statics.empty());              // gone after removal
}

// G2: AddFixture to a static grows its grid AABB so it becomes findable at the
// new (wider) region. Guards the AddFixture static-grid Move.
TEST_CASE("AddFixture grows a static's grid AABB", "[physics][grid]")
{
    PhysicsWorld w;
    BodyDef d; d.type = BodyType::Static; d.position = Vec2(0, 0);
    d.shape = MakeAabb(Real(10), Real(10));   // small primary fixture near origin
    BodyHandle s = w.AddBody(d);

    // A region far to the +x side, NOT covered by the small primary fixture.
    Aabb2 farRegion; farRegion.min = Vec2(180, -10); farRegion.max = Vec2(220, 10);
    std::vector<Aabb2> spans; std::vector<std::uint32_t> statics;
    std::vector<std::uint32_t> gridScratch;
    w.StaticCandidates(farRegion, spans, statics, gridScratch);
    REQUIRE(statics.empty());              // not reachable yet

    // Add a second fixture offset far to +x so the body's union AABB now covers farRegion.
    FixtureDef fd; fd.shape = MakeAabb(Real(10), Real(10));
    fd.localPos = Vec2(200, 0);
    w.AddFixture(s, fd);

    statics.clear(); spans.clear();
    w.StaticCandidates(farRegion, spans, statics, gridScratch);
    REQUIRE(statics.size() == 1);          // now findable at the grown AABB
}

// G4: a DYNAMIC body's residency is refreshed via the solver commit path
// (CommitSlotPosition), not just the kinematic integrate. Drive it with velocity.
// WorldDef defaults: gravity=(0,0), linearDamping=0 -- the body moves freely.
// With v=600 and 30 steps of dt=1/60: displacement ~ 600*(30/60) = 300 units.
// Starting at x=10, final x ~ 310, which is well outside the (0..64) origin tile.
TEST_CASE("Residency tracks a dynamic body via the solver commit", "[physics][grid][residency]")
{
    PhysicsWorld w;
    BodyDef d; d.type = BodyType::Dynamic; d.shape = MakeCircle(Real(8));
    d.fixedRotation = true; d.position = Vec2(10, 10);
    BodyHandle a = w.AddBody(d);
    w.SetVelocity(a, Vec2(600, 0));        // dynamics integrate + commit via the solver

    for (int i = 0; i < 30; ++i) w.Step(Real(1)/Real(60)); // ~ +300 x over 0.5s (no damping/gravity)

    Aabb2 origin; origin.min = Vec2(0, 0); origin.max = Vec2(64, 64);
    std::vector<std::uint32_t> residents;
    w.Residents(origin, residents);
    // It should have left the origin tile region (residency followed the solver commit).
    REQUIRE(std::find(residents.begin(), residents.end(), a.index) == residents.end());
}

// G5: Residents on an unoccupied region returns empty.
TEST_CASE("Residents empty region returns no bodies", "[physics][grid][residency]")
{
    PhysicsWorld w;
    BodyDef d; d.type = BodyType::Dynamic; d.shape = MakeCircle(Real(8));
    d.position = Vec2(10, 10); w.AddBody(d);

    Aabb2 empty; empty.min = Vec2(5000, 5000); empty.max = Vec2(5100, 5100);
    std::vector<std::uint32_t> residents;
    const int n = w.Residents(empty, residents);
    REQUIRE(n == 0);
    REQUIRE(residents.empty());
}
