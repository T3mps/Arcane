// Read-only debug accessors: enumerate exactly the live structures, and tag each
// contact with the narrowphase that produced it. Determinism/behavior unchanged.
//
// These accessors back the Slice A physics debug-visualization overlay. They are
// pure read paths over the broadphase trees + the static/residency grids + the
// solver's ContactConstraint pool. Nothing here feeds back into the Step path:
// iteration order is for display only.
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp>
#include <Arcane/Physics/Broadphase/DynamicTree.hpp>
#include <Arcane/Physics/Broadphase/SpatialGrid.hpp>
#include <Arcane/Physics/Narrowphase/NarrowphaseTrace.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Solver/Solver.hpp>

using namespace Arcane::Physics;

TEST_CASE("Debug accessors enumerate broadphase + contacts", "[physics][debugviz]")
{
    PhysicsWorld w;
    auto addBox = [&](Real x, Real y, BodyType t)
    {
        BodyDef d;
        d.type          = t;
        d.position      = Vec2(x, y);
        d.fixedRotation = true;
        d.shape         = MakeAabb(Real(10), Real(10));
        return w.AddBody(d);
    };
    addBox(0, 100, BodyType::Static);  // a static -> static grid
    addBox(0, 79, BodyType::Dynamic);  // overlaps the static -> a contact
    w.Step(Real(1) / Real(60));

    // (a) ForEachLeaf yields live mover fixtures, tight inside fat.
    // FixtureBroadphaseTree() is a POINTER (the broadphase is selectable; it is
    // non-null only for the default DynamicTree mover broadphase, which this
    // default-constructed world uses).
    const DynamicTree* tree = w.FixtureBroadphaseTree();
    REQUIRE(tree != nullptr);
    std::size_t leaves = 0;
    tree->ForEachLeaf(
        [&](std::uint32_t, const Aabb2& tight, const Aabb2& fat)
        {
            ++leaves;
            REQUIRE(fat.min.x <= tight.min.x);
            REQUIRE(fat.max.x >= tight.max.x);
            REQUIRE(fat.min.y <= tight.min.y);
            REQUIRE(fat.max.y >= tight.max.y);
        });
    REQUIRE(leaves >= 1);

    // (b) static grid has >=1 occupied cell.
    std::size_t cells = 0;
    w.StaticGrid().ForEachCell(
        [&](int, int, const std::vector<std::uint32_t>& ids)
        {
            REQUIRE(!ids.empty());
            ++cells;
        });
    REQUIRE(cells >= 1);

    // (c) ForEachContactConstraint count == ActiveContactCount, and kind is set.
    std::size_t n = 0;
    bool kindSet  = false;
    w.ForEachContactConstraint(
        [&](const ContactConstraint& cc)
        {
            ++n;
            if (cc.kind != NarrowphaseKind::Separated) kindSet = true;
        });
    REQUIRE(n == w.ActiveContactCount());
    if (n > 0) REQUIRE(kindSet);
}
