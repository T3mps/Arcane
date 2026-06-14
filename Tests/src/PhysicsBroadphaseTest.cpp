// Physics M6 P1.6: Broadphase equivalence + oracle parity.
//
// The broadphase CONTRACT is the narrowed pair-set: candidate pairs filtered to
// true AABB overlap, emitted SORTED by (a,b). The Lua equivalence invariant
// (physics_harness "M4: broadphase equivalence") is that all three strategies
// produce the IDENTICAL set. The captured oracle
// Arcane/Tests/data/physics_oracle/broadphase.json holds:
//
//   equivalence_scene:
//     boxes[]        : 50 entries [id, x0, y0, x1, y1] in their FINAL state
//                      (the capture already applied the harness's "move every
//                      3rd box" pass; we insert these post-move boxes directly).
//     cellSize       : 48 (the SpatialHash cell size for this scene).
//     narrowed_pairs : the 9 canonical [i,j] (i<j) overlap pairs -- the
//                      contract every strategy must reproduce.
//     pair_count     : 9.
//   spatialhash_basic:
//     boxes[]        : 3 entries [id, x0,y0,x1,y1], cellSize 64.
//     query          : {x0,y0,x1,y1, ids[]} -- queryAABB expected ids.
//     pairs          : {count, flat[]} -- expected pair (1 pair: [1,2]).
//
// We build all three strategies (DynamicTree, SpatialHash, SweepAndPrune) from
// the same boxes (via Update), then assert each Pairs() equals the others AND
// equals the oracle's narrowed set. We also exercise Update (move) + Remove and
// re-assert equivalence, and assert QueryAABB id-set equivalence across
// strategies. Pair ids are integers -> exact comparisons.

#include <cstdint>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/Broadphase/Broadphase.hpp>
#include <Arcane/Physics/Broadphase/DynamicTree.hpp>
#include <Arcane/Physics/Broadphase/SpatialHash.hpp>
#include <Arcane/Physics/Broadphase/SweepAndPrune.hpp>

#include "Helpers/PhysicsOracle.hpp"

using namespace Arcane::Physics;

namespace
{
    // A scene box read from the oracle: id + tight AABB.
    struct SceneBox
    {
        std::uint32_t id = 0;
        Aabb2         box{};
    };

    // Parse an oracle box row [id, x0, y0, x1, y1] into a SceneBox.
    SceneBox ParseBox(const nlohmann::json& row)
    {
        SceneBox sb;
        sb.id      = row[0].get<std::uint32_t>();
        sb.box.min = Vec2(row[1].get<Real>(), row[2].get<Real>());
        sb.box.max = Vec2(row[3].get<Real>(), row[4].get<Real>());
        return sb;
    }

    // Build the three strategies for a scene's cellSize. DynamicTree is the
    // default mover broadphase; SpatialHash + SweepAndPrune are the alternates.
    std::vector<std::unique_ptr<IBroadphase>> MakeAll(Real cellSize)
    {
        std::vector<std::unique_ptr<IBroadphase>> v;
        v.emplace_back(std::make_unique<DynamicTree>());
        v.emplace_back(std::make_unique<SpatialHash>(cellSize));
        v.emplace_back(std::make_unique<SweepAndPrune>());
        return v;
    }

    // Collect Pairs() into a fresh sorted vector (the contract guarantees the
    // output is already sorted; this just snapshots it for comparison).
    std::vector<BroadphasePair> PairsOf(const IBroadphase& bp)
    {
        std::vector<BroadphasePair> out;
        bp.Pairs(out);
        return out;
    }

    std::vector<std::uint32_t> QueryOf(const IBroadphase& bp, const Aabb2& box)
    {
        std::vector<std::uint32_t> out;
        bp.QueryAABB(box, out);
        return out;
    }
} // namespace

// ====================================================================
// 1) spatialhash_basic: the 3-box scene (queryAABB + pairs) -- the direct
//    "== SpatialHash ==" harness block, all three strategies + oracle.
// ====================================================================
TEST_CASE("physics: broadphase spatialhash_basic matches the oracle", "[physics]")
{
    const nlohmann::json j  = Arcane::Test::LoadOracle("broadphase");
    const auto&          sc = j.at("spatialhash_basic");
    const Real           cs = sc.at("cellSize").get<Real>();

    std::vector<SceneBox> boxes;
    for (const auto& row : sc.at("boxes"))
    {
        boxes.push_back(ParseBox(row));
    }

    auto impls = MakeAll(cs);
    for (auto& bp : impls)
    {
        for (const SceneBox& b : boxes)
        {
            bp->Update(b.id, b.box);
        }
    }

    // Expected pairs from the oracle (flat [a,b,...], count pairs).
    const int expectCount = sc.at("pairs").at("count").get<int>();
    const auto& flat      = sc.at("pairs").at("flat");
    std::vector<BroadphasePair> expectPairs;
    for (int k = 0; k < expectCount; ++k)
    {
        expectPairs.push_back(BroadphasePair{
            flat[2 * k].get<std::uint32_t>(),
            flat[2 * k + 1].get<std::uint32_t>() });
    }

    // Expected query ids.
    const auto& q = sc.at("query");
    const Aabb2 qbox{ Vec2(q.at("x0").get<Real>(), q.at("y0").get<Real>()),
                      Vec2(q.at("x1").get<Real>(), q.at("y1").get<Real>()) };
    std::vector<std::uint32_t> expectIds;
    for (const auto& v : q.at("ids"))
    {
        expectIds.push_back(v.get<std::uint32_t>());
    }

    for (auto& bp : impls)
    {
        CHECK(PairsOf(*bp) == expectPairs);
        CHECK(QueryOf(*bp, qbox) == expectIds);
    }
}

// ====================================================================
// 2) equivalence_scene: the 50-box scene. All three strategies emit the
//    IDENTICAL sorted pair set == the oracle's 9 narrowed pairs.
// ====================================================================
TEST_CASE("physics: broadphase 50-box scene == oracle narrowed pairs", "[physics]")
{
    const nlohmann::json j  = Arcane::Test::LoadOracle("broadphase");
    const auto&          sc = j.at("equivalence_scene");
    const Real           cs = sc.at("cellSize").get<Real>();

    std::vector<SceneBox> boxes;
    for (const auto& row : sc.at("boxes"))
    {
        boxes.push_back(ParseBox(row));
    }
    REQUIRE(boxes.size() == 50u);

    // Oracle narrowed pair set (already sorted by (i,j) in the fixture).
    std::vector<BroadphasePair> oraclePairs;
    for (const auto& row : sc.at("narrowed_pairs"))
    {
        oraclePairs.push_back(BroadphasePair{ row[0].get<std::uint32_t>(),
                                              row[1].get<std::uint32_t>() });
    }
    REQUIRE(static_cast<int>(oraclePairs.size()) ==
            sc.at("pair_count").get<int>());
    REQUIRE(oraclePairs.size() == 9u);

    auto impls = MakeAll(cs);
    for (auto& bp : impls)
    {
        for (const SceneBox& b : boxes)
        {
            bp->Update(b.id, b.box);
        }
    }

    // Each strategy's pair set == the oracle (and therefore == each other).
    const std::vector<BroadphasePair> treePairs = PairsOf(*impls[0]);
    const std::vector<BroadphasePair> hashPairs = PairsOf(*impls[1]);
    const std::vector<BroadphasePair> sapPairs  = PairsOf(*impls[2]);

    CHECK(treePairs == oraclePairs);
    CHECK(hashPairs == oraclePairs);
    CHECK(sapPairs == oraclePairs);
    // Explicit cross-strategy identity (the Lua equivalence invariant).
    CHECK(treePairs == hashPairs);
    CHECK(hashPairs == sapPairs);

    // Determinism: calling Pairs() again yields the identical vector.
    CHECK(PairsOf(*impls[0]) == treePairs);
}

// ====================================================================
// 3) Update (move every 3rd) + Remove: equivalence holds after mutation.
//    Mirrors the harness's move-every-3rd + remove paths -- exercises the
//    DynamicTree fat-box reinsert, the SpatialHash re-bucketing, and SAP's
//    insertion-sort, then re-asserts all three agree.
// ====================================================================
TEST_CASE("physics: broadphase equivalence under Update + Remove", "[physics]")
{
    const nlohmann::json j  = Arcane::Test::LoadOracle("broadphase");
    const auto&          sc = j.at("equivalence_scene");
    const Real           cs = sc.at("cellSize").get<Real>();

    std::vector<SceneBox> boxes;
    for (const auto& row : sc.at("boxes"))
    {
        boxes.push_back(ParseBox(row));
    }

    auto impls = MakeAll(cs);
    for (auto& bp : impls)
    {
        for (const SceneBox& b : boxes)
        {
            bp->Update(b.id, b.box);
        }
    }

    // Move every 3rd body by a fixed offset (same offset to every strategy so
    // they stay in lockstep). Translation preserves the box extents.
    const Vec2 shift(37.0f, -23.0f);
    for (std::size_t i = 0; i < boxes.size(); i += 3)
    {
        Aabb2 moved;
        moved.min = boxes[i].box.min + shift;
        moved.max = boxes[i].box.max + shift;
        for (auto& bp : impls)
        {
            bp->Update(boxes[i].id, moved);
        }
    }

    // Remove a handful of bodies from every strategy.
    const std::uint32_t toRemove[] = { 7u, 13u, 29u, 44u };
    for (const std::uint32_t id : toRemove)
    {
        for (auto& bp : impls)
        {
            bp->Remove(id);
        }
    }
    // Removing an absent id is a no-op on every strategy.
    for (auto& bp : impls)
    {
        bp->Remove(9999u);
    }

    const std::vector<BroadphasePair> treePairs = PairsOf(*impls[0]);
    const std::vector<BroadphasePair> hashPairs = PairsOf(*impls[1]);
    const std::vector<BroadphasePair> sapPairs  = PairsOf(*impls[2]);

    CHECK(treePairs == hashPairs);
    CHECK(hashPairs == sapPairs);

    // The removed ids must not appear in any emitted pair.
    for (const BroadphasePair& p : treePairs)
    {
        for (const std::uint32_t id : toRemove)
        {
            CHECK(p.a != id);
            CHECK(p.b != id);
        }
    }
}

// ====================================================================
// 4) QueryAABB equivalence: a few query boxes return identical id sets across
//    all three strategies (each narrows against the TIGHT box, sorted asc).
// ====================================================================
TEST_CASE("physics: broadphase QueryAABB identical across strategies", "[physics]")
{
    const nlohmann::json j  = Arcane::Test::LoadOracle("broadphase");
    const auto&          sc = j.at("equivalence_scene");
    const Real           cs = sc.at("cellSize").get<Real>();

    std::vector<SceneBox> boxes;
    for (const auto& row : sc.at("boxes"))
    {
        boxes.push_back(ParseBox(row));
    }

    auto impls = MakeAll(cs);
    for (auto& bp : impls)
    {
        for (const SceneBox& b : boxes)
        {
            bp->Update(b.id, b.box);
        }
    }

    const Aabb2 queries[] = {
        { Vec2(0.0f, 0.0f), Vec2(200.0f, 200.0f) },      // top-left region
        { Vec2(400.0f, 300.0f), Vec2(900.0f, 700.0f) },  // bottom-right region
        { Vec2(100.0f, 100.0f), Vec2(150.0f, 150.0f) },  // tight window
        { Vec2(-1000.0f, -1000.0f), Vec2(-900.0f, -900.0f) }, // empty (offscreen)
        { Vec2(0.0f, 0.0f), Vec2(1000.0f, 800.0f) },     // whole scene
    };

    for (const Aabb2& qb : queries)
    {
        const std::vector<std::uint32_t> tree = QueryOf(*impls[0], qb);
        const std::vector<std::uint32_t> hash = QueryOf(*impls[1], qb);
        const std::vector<std::uint32_t> sap  = QueryOf(*impls[2], qb);
        CHECK(tree == hash);
        CHECK(hash == sap);
    }

    // The whole-scene query returns every present id (sorted 1..50).
    const std::vector<std::uint32_t> all =
        QueryOf(*impls[0], Aabb2{ Vec2(0.0f, 0.0f), Vec2(1000.0f, 800.0f) });
    CHECK(all.size() == 50u);
    CHECK(all.front() == 1u);
    CHECK(all.back() == 50u);
}
