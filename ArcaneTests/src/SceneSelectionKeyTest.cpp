#include <catch2/catch_test_macros.hpp>
#include "Panels/SceneSelectionKey.hpp"
#include <set>

using namespace Arcane::Editor;
namespace K = SceneSelectionKey;

TEST_CASE("SceneSelectionKey: the key is the whole set; the alive subset keeps the live members and drops the dead ones", "[editor][inspector]")
{
    using Astra::Entity;
    const Entity a(static_cast<Entity::StorageType>(7)), b(static_cast<Entity::StorageType>(9)), c(static_cast<Entity::StorageType>(11));
    SelectionContext sel;
    sel.Select(a); sel.Toggle(b); sel.Toggle(c);        // primary = c (last toggled)
    const std::string key = K::Encode(sel);
    CHECK(key == "11;7,9,11");
    K::KeySet set = K::Decode(key);
    REQUIRE(set.members.size() == 3);
    CHECK(set.primary == c);
    // Round trip through Clear + AddRange re-reports the identical key.
    SelectionContext again; again.Clear(); again.AddRange(set.members, set.primary);
    CHECK(K::Encode(again) == key);
    // The PRIMARY dies: the survivors stay, the last survivor becomes primary.
    std::set<Entity> dead{ c };
    K::KeySet alive = K::AliveSubset(set, [&](Entity e) { return !dead.count(e); });
    REQUIRE(alive.members.size() == 2);
    CHECK(alive.primary == b);
    dead = { a, b, c };
    CHECK(K::AliveSubset(set, [&](Entity e) { return !dead.count(e); }).members.empty());
    // Malformed keys decode to nothing.
    CHECK(K::Decode("").members.empty());
    CHECK(K::Decode("7").members.empty());
    CHECK(K::Decode("bogus;1,2").members.empty());
    CHECK(K::Decode("5;1,2").members.empty());          // primary not in the list
    CHECK(K::Decode("1;1,x").members.empty());
    CHECK(K::Encode(SelectionContext{}).empty());
}
