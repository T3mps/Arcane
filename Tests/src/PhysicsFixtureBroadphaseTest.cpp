// Per-fixture broadphase oracle: the fixture-pair candidate set the world's
// per-fixture broadphase produces must EQUAL the brute-force O(n^2) set over all
// live MOVER fixture world-AABBs (a<b by fixture slot). Compound bodies (the whisk
// case) are the point: each fixture is its own proxy.
#include <algorithm>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp> // BroadphasePair, AabbOverlap

using namespace Arcane::Physics;

namespace
{
    std::vector<BroadphasePair> BruteFixturePairs(const PhysicsWorld& w)
    {
        std::vector<std::uint32_t> fx; std::vector<Aabb2> box;
        w.LiveFixtureAabbs(fx, box);
        std::vector<BroadphasePair> out;
        for (std::size_t i = 0; i < fx.size(); ++i)
            for (std::size_t j = i + 1; j < fx.size(); ++j)
                if (AabbOverlap(box[i], box[j]))
                {
                    std::uint32_t a = fx[i], b = fx[j];
                    out.push_back(a < b ? BroadphasePair{a, b} : BroadphasePair{b, a});
                }
        std::sort(out.begin(), out.end());
        return out;
    }

    // Returns sorted Pairs() from the fixture broadphase.
    std::vector<BroadphasePair> FxBpPairs(const PhysicsWorld& w)
    {
        std::vector<BroadphasePair> got;
        w.FixtureBroadphase().Pairs(got);
        std::sort(got.begin(), got.end());
        return got;
    }

    // Returns the current live mover-fixture count.
    std::size_t LiveFxCount(const PhysicsWorld& w)
    {
        std::vector<std::uint32_t> fx; std::vector<Aabb2> box;
        w.LiveFixtureAabbs(fx, box);
        return fx.size();
    }
}

TEST_CASE("Per-fixture broadphase pairs == brute-force (compound scene)", "[physics][fxbroadphase]")
{
    PhysicsWorld w;
    auto addBox = [&](Real x, Real y, Real hw, Real hh, BodyType t) {
        BodyDef d; d.type = t; d.position = Vec2(x, y); d.fixedRotation = true;
        d.shape = MakeAabb(hw, hh); return w.AddBody(d);
    };
    BodyHandle c = addBox(0, 0, 10, 10, BodyType::Dynamic);
    FixtureDef f; f.shape = MakeAabb(Real(10), Real(10));
    f.localPos = Vec2(40, 0);  w.AddFixture(c, f);
    f.localPos = Vec2(80, 0);  w.AddFixture(c, f);
    addBox(35,  0, 12, 12, BodyType::Dynamic);
    addBox(82,  0, 12, 12, BodyType::Dynamic);
    addBox(400, 0, 12, 12, BodyType::Dynamic);
    addBox(40, -3, 14, 14, BodyType::Static);   // static fixtures excluded from the tree

    w.Step(Real(1) / Real(60));

    std::vector<BroadphasePair> got;
    w.FixtureBroadphase().Pairs(got);
    std::sort(got.begin(), got.end());
    REQUIRE(got == BruteFixturePairs(w));
}

// ----------------------------------------------------------------
// Lifecycle case 1: DropFixture removes the proxy
// ----------------------------------------------------------------
// Build a 3-fixture compound body c (fixtures at x=0, x=40, x=80)
// with a neighbor that overlaps the 2nd and 3rd fixtures.  After an
// initial Step + oracle check, drop the 2nd fixture and verify:
//   (a) LiveFixtureAabbs count drops by exactly 1 (immediately, before Step), and
//   (b) Pairs()==brute (the dropped fixture's pairs are GONE) after the Step.
TEST_CASE("DropFixture removes proxy from fixture broadphase", "[physics][fxbroadphase]")
{
    PhysicsWorld w;
    auto addBox = [&](Real x, Real y, Real hw, Real hh, BodyType t) {
        BodyDef d; d.type = t; d.position = Vec2(x, y); d.fixedRotation = true;
        d.shape = MakeAabb(hw, hh); return w.AddBody(d);
    };

    // Compound body: primary fixture at origin, plus two extra fixtures.
    BodyHandle c = addBox(0, 0, 10, 10, BodyType::Dynamic);
    FixtureDef f; f.shape = MakeAabb(Real(10), Real(10));
    f.localPos = Vec2(40, 0); FixtureHandle fx2 = w.AddFixture(c, f); // 2nd fixture
    f.localPos = Vec2(80, 0);               w.AddFixture(c, f);       // 3rd fixture

    // Neighbor overlapping the 2nd fixture (at x=40) and the 3rd (at x=80).
    addBox(45, 0, 12, 12, BodyType::Dynamic);

    w.Step(Real(1) / Real(60));

    // Initial oracle check: broadphase must match brute.
    REQUIRE(FxBpPairs(w) == BruteFixturePairs(w));
    // Pairs must be non-empty (the neighbor overlaps at least the 2nd fixture).
    REQUIRE(!FxBpPairs(w).empty());

    // Record compound body's fixture count and live mover-fixture count before drop.
    std::uint32_t cFxBefore = w.FixtureCount(c);
    std::size_t liveBefore = LiveFxCount(w);
    REQUIRE(cFxBefore == 3u); // primary + fx2 + 3rd

    // Drop the 2nd fixture (fx2) from the compound body.
    w.DropFixture(fx2);
    REQUIRE_FALSE(w.IsValid(fx2)); // handle must be stale

    // Both FixtureCount(c) and LiveFixtureAabbs reflect the drop immediately:
    // DropFixture unlinks the slot from m_bodyFixtures and removes its proxy.
    REQUIRE(w.FixtureCount(c) == cFxBefore - 1u);
    REQUIRE(LiveFxCount(w) == liveBefore - 1u);

    w.Step(Real(1) / Real(60));

    // After drop + Step: broadphase must still match brute.
    // The dropped fixture's proxy was removed by DropFixture, and the fixed
    // LiveFixtureAabbs now enumerates via m_bodyFixtures (the authoritative
    // live set), so both sides of the oracle agree exactly.
    REQUIRE(FxBpPairs(w) == BruteFixturePairs(w));
}

// ----------------------------------------------------------------
// Lifecycle case 2: RemoveBody removes all its proxies
// ----------------------------------------------------------------
// Build 2 overlapping dynamic bodies (A and B), each single-fixture.
// After initial oracle check, remove body A and verify:
//   (a) Pairs()==brute (no pairs reference A's fixture), and
//   (b) live mover-fixture count dropped by A's fixture count (1).
TEST_CASE("RemoveBody removes all fixture proxies from broadphase", "[physics][fxbroadphase]")
{
    PhysicsWorld w;
    auto addBox = [&](Real x, Real y, Real hw, Real hh, BodyType t) {
        BodyDef d; d.type = t; d.position = Vec2(x, y); d.fixedRotation = true;
        d.shape = MakeAabb(hw, hh); return w.AddBody(d);
    };

    // Two overlapping dynamic bodies.
    BodyHandle a = addBox(  0, 0, 15, 15, BodyType::Dynamic);
    BodyHandle b = addBox( 20, 0, 15, 15, BodyType::Dynamic);
    // A third body far away so the pair set stays non-trivial.
    addBox(200, 0, 15, 15, BodyType::Dynamic);

    w.Step(Real(1) / Real(60));

    // Initial oracle check.
    REQUIRE(FxBpPairs(w) == BruteFixturePairs(w));
    // a and b overlap, so at least one pair must exist.
    REQUIRE(!FxBpPairs(w).empty());

    std::uint32_t aFxCount = w.FixtureCount(a); // should be 1 for a plain AddBody
    std::size_t countBefore = LiveFxCount(w);

    w.RemoveBody(a);
    REQUIRE_FALSE(w.IsValid(a)); // handle must be stale

    w.Step(Real(1) / Real(60));

    // After removal: broadphase must still match brute.
    REQUIRE(FxBpPairs(w) == BruteFixturePairs(w));

    // Live count must have dropped by exactly aFxCount.
    REQUIRE(LiveFxCount(w) == countBefore - aFxCount);
}

// ----------------------------------------------------------------
// Lifecycle case 3: Move across steps -- separated pair is removed
// ----------------------------------------------------------------
// Two dynamic bodies start overlapping.  Body B is then teleported far
// away (SetPosition).  After a Step the pair must be gone and
// Pairs()==brute must still hold.
//
// Gravity default is (0,0) per WorldDef, so a body with zero velocity
// stays put without an external gravity pull.  SetPosition is the
// deterministic teleport that also snaps prev, avoiding lerp smear.
TEST_CASE("Fixture broadphase pair removed after bodies separate (move)", "[physics][fxbroadphase]")
{
    PhysicsWorld w;
    auto addBox = [&](Real x, Real y, Real hw, Real hh, BodyType t) {
        BodyDef d; d.type = t; d.position = Vec2(x, y); d.fixedRotation = true;
        d.shape = MakeAabb(hw, hh); return w.AddBody(d);
    };

    // Two overlapping bodies.
    BodyHandle a = addBox(  0, 0, 20, 20, BodyType::Dynamic);
    BodyHandle b = addBox( 10, 0, 20, 20, BodyType::Dynamic);

    w.Step(Real(1) / Real(60));

    // Initial oracle check: must overlap, pair must be present.
    REQUIRE(FxBpPairs(w) == BruteFixturePairs(w));
    REQUIRE(!FxBpPairs(w).empty());

    // Teleport body B far away so its fixture no longer overlaps A's.
    w.SetPosition(b, Vec2(Real(1000), Real(0)));

    w.Step(Real(1) / Real(60));

    // After separation: oracle still holds AND the pair is gone.
    REQUIRE(FxBpPairs(w) == BruteFixturePairs(w));
    REQUIRE(FxBpPairs(w).empty());
}
