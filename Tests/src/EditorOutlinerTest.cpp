// Outliner slice 2: the headless row builder behind the Outliner panel.
// Cross-DLL note: BuildOutlinerRows calls Edit::DisplayName (Arcane.dll),
// so the fixture pins Arcane.dll's TypeContext slot to the shared test
// context -- same idiom as EntityOpsTest.

#include <catch2/catch_test_macros.hpp>

#include "EntityList.hpp"
#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

using namespace Arcane;
using Arcane::Editor::BuildOutlinerRows;
using Arcane::Editor::OutlinerRow;
using Arcane::Editor::OutlinerSort;
using Arcane::Editor::RowRange;

namespace
{
    struct World
    {
        std::shared_ptr<Astra::ComponentRegistry> creg = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{ creg };
        World()
        {
            // Belt-and-braces: test_main pins Arcane.dll's TypeContext slot once
            // before any test runs, which is the real guarantee (per-type IDs are
            // cached in per-module magic statics and never re-resolve, so a late
            // pin cannot repair an already-cached id). Re-pinning here only keeps
            // the slot pointed at the shared context; never install an unshared
            // one anywhere in this suite.
            Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
            RegisterSceneComponents(reg);
        }
        Astra::Entity Make(const char* name, Astra::Entity parent = Astra::Entity::Invalid())
        {
            Astra::Entity e = Edit::CreateEntity(reg, parent);
            Edit::RenameEntity(reg, e, name);
            return e;
        }
    };

    const std::unordered_set<std::uint64_t> kNoneCollapsed;
    const OutlinerSort kNoSort;
}

TEST_CASE("Rows come out in depth-first tree order with depths", "[editor][outliner]")
{
    World w;
    Astra::Entity a = w.Make("A");
    Astra::Entity b = w.Make("B", a);
    Astra::Entity c = w.Make("C", a);
    Astra::Entity d = w.Make("D", c);

    auto rows = BuildOutlinerRows(w.reg, "", kNoSort, kNoneCollapsed);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].entity == a); CHECK(rows[0].depth == 0);
    CHECK(rows[1].entity == b); CHECK(rows[1].depth == 1);
    CHECK(rows[2].entity == c); CHECK(rows[2].depth == 1);
    CHECK(rows[3].entity == d); CHECK(rows[3].depth == 2);
    CHECK(rows[0].hasChildren); CHECK(rows[0].childCount == 2);
    CHECK_FALSE(rows[1].hasChildren);
}

TEST_CASE("Labels use DisplayName; hidden flag rides Arcane::Hidden", "[editor][outliner]")
{
    World w;
    Astra::Entity named = w.Make("Player");
    Astra::Entity anon = w.reg.CreateEntity();            // no Identity at all
    w.reg.AddComponent<Hidden>(named, Hidden{});

    auto rows = BuildOutlinerRows(w.reg, "", kNoSort, kNoneCollapsed);
    REQUIRE(rows.size() == 2);
    auto find = [&](Astra::Entity e) -> const OutlinerRow&
    {
        for (const auto& r : rows) if (r.entity == e) return r;
        FAIL("row missing"); return rows[0];
    };
    CHECK(find(named).label == "Player");
    CHECK(find(named).hidden);
    CHECK(find(anon).label == Edit::DisplayName(w.reg, anon));   // "Entity <id>" fallback
    CHECK_FALSE(find(anon).hidden);
}

TEST_CASE("Filter keeps matches and their ancestors; ancestors dim", "[editor][outliner]")
{
    World w;
    Astra::Entity root = w.Make("Root");
    Astra::Entity mid = w.Make("Middle", root);
    Astra::Entity hit = w.Make("Treasure", mid);
    w.Make("Noise", root);
    w.Make("Static");

    auto rows = BuildOutlinerRows(w.reg, "TREAS", kNoSort, kNoneCollapsed);   // case-insensitive
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].entity == root); CHECK(rows[0].dimmed);
    CHECK(rows[1].entity == mid);  CHECK(rows[1].dimmed);
    CHECK(rows[2].entity == hit);  CHECK_FALSE(rows[2].dimmed);
}

TEST_CASE("Collapsed rows keep themselves, drop descendants; filter overrides", "[editor][outliner]")
{
    World w;
    Astra::Entity a = w.Make("A");
    Astra::Entity b = w.Make("B", a);
    w.Make("C", b);

    std::unordered_set<std::uint64_t> collapsed{ static_cast<std::uint64_t>(a.GetValue()) };
    auto rows = BuildOutlinerRows(w.reg, "", kNoSort, collapsed);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].entity == a);
    CHECK(rows[0].hasChildren);                 // arrow still drawable

    rows = BuildOutlinerRows(w.reg, "C", kNoSort, collapsed);   // search auto-expands
    REQUIRE(rows.size() == 3);
}

TEST_CASE("Sort reorders sibling groups without breaking the tree", "[editor][outliner]")
{
    World w;
    Astra::Entity zeta = w.Make("Zeta");
    Astra::Entity alpha = w.Make("alpha");      // case-insensitive: sorts before Zeta
    w.Make("z-child", alpha);
    w.Make("a-child", alpha);

    OutlinerSort byLabel{ OutlinerSort::Column::Label, true };
    auto rows = BuildOutlinerRows(w.reg, "", byLabel, kNoneCollapsed);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].entity == alpha);
    CHECK(rows[1].label == "a-child"); CHECK(rows[1].depth == 1);
    CHECK(rows[2].label == "z-child"); CHECK(rows[2].depth == 1);
    CHECK(rows[3].entity == zeta);

    OutlinerSort desc{ OutlinerSort::Column::Label, false };
    rows = BuildOutlinerRows(w.reg, "", desc, kNoneCollapsed);
    CHECK(rows[0].entity == zeta);
}

TEST_CASE("A matching root needs no ancestors and is not dimmed", "[editor][outliner]")
{
    // The ancestor-chain walk has an empty chain at depth 0; a root that
    // matches must survive on its own and stay undimmed. Descendants of a
    // match are NOT auto-kept -- only matches and their ancestors.
    World w;
    Astra::Entity root = w.Make("Beacon");
    w.Make("Child", root);
    w.Make("Other");

    auto rows = BuildOutlinerRows(w.reg, "beacon", kNoSort, kNoneCollapsed);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].entity == root);
    CHECK(rows[0].depth == 0);
    CHECK_FALSE(rows[0].dimmed);
}

TEST_CASE("RowRange spans visible rows inclusively, either direction", "[editor][outliner]")
{
    World w;
    Astra::Entity a = w.Make("A");
    Astra::Entity b = w.Make("B");
    Astra::Entity c = w.Make("C");
    auto rows = BuildOutlinerRows(w.reg, "", kNoSort, kNoneCollapsed);

    auto fwd = RowRange(rows, a, c);
    REQUIRE(fwd.size() == 3);
    CHECK(fwd.front() == a); CHECK(fwd.back() == c);

    auto rev = RowRange(rows, c, a);
    REQUIRE(rev.size() == 3);
    CHECK(rev.front() == a); CHECK(rev.back() == c);

    CHECK(RowRange(rows, a, Astra::Entity::Invalid()).empty());
    CHECK(RowRange(rows, b, b).size() == 1);
}
