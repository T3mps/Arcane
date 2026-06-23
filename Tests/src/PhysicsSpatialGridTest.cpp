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
