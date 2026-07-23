// Arcane::AssetId: opaque, GUID-backed asset handle (Slice 2 swapped the backing from
// a logical mount path to a Guid). CPU-only.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Guid.hpp>
#include <Arcane/Project/AssetId.hpp>

#include <unordered_set>

TEST_CASE("AssetId default-constructs invalid; FromGuid is valid", "[project]")
{
    Arcane::AssetId nil;
    CHECK_FALSE(nil.IsValid());

    const Arcane::Guid g = Arcane::Guid::Generate();
    Arcane::AssetId id = Arcane::AssetId::FromGuid(g);
    CHECK(id.IsValid());
    CHECK(id.Value() == g);
}

TEST_CASE("AssetId equality and hashing key off the same identity", "[project]")
{
    const Arcane::Guid ga = Arcane::Guid::Generate();
    const Arcane::Guid gc = Arcane::Guid::Generate();
    auto a = Arcane::AssetId::FromGuid(ga);
    auto b = Arcane::AssetId::FromGuid(ga);
    auto c = Arcane::AssetId::FromGuid(gc);

    CHECK(a == b);
    CHECK(a != c);

    std::unordered_set<Arcane::AssetId> set;
    set.insert(a);
    set.insert(b);   // same identity as a
    set.insert(c);
    CHECK(set.size() == 2);
}
