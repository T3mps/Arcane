// Arcane::AssetId: opaque, GUID-ready asset handle. Slice 1 backing = a logical
// mount path; the swap to a Guid (Slice 2) must not change this interface. CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Project/AssetId.hpp>

#include <unordered_set>

TEST_CASE("AssetId default-constructs invalid; FromMountPath is valid", "[project]")
{
    Arcane::AssetId nil;
    CHECK_FALSE(nil.IsValid());

    Arcane::AssetId id = Arcane::AssetId::FromMountPath("game://characters/hero.png");
    CHECK(id.IsValid());
    CHECK(id.Key() == "game://characters/hero.png");
}

TEST_CASE("AssetId equality and hashing key off the same identity", "[project]")
{
    auto a = Arcane::AssetId::FromMountPath("game://a.png");
    auto b = Arcane::AssetId::FromMountPath("game://a.png");
    auto c = Arcane::AssetId::FromMountPath("game://b.png");

    CHECK(a == b);
    CHECK(a != c);

    std::unordered_set<Arcane::AssetId> set;
    set.insert(a);
    set.insert(b);   // same identity as a
    set.insert(c);
    CHECK(set.size() == 2);
}
