// Arcane::BitSet (b2BitSet equivalent) unit tests.
#include <cstdint>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Util/BitSet.hpp>
using Arcane::BitSet;

TEST_CASE("BitSet: set + ForEachSetBit walks ascending", "[bitset]")
{
    BitSet b; b.Resize(200);
    b.Set(5); b.Set(63); b.Set(64); b.Set(199);
    std::vector<std::uint32_t> got;
    b.ForEachSetBit([&](std::uint32_t i){ got.push_back(i); });
    REQUIRE(got == std::vector<std::uint32_t>{5u, 63u, 64u, 199u});
}

TEST_CASE("BitSet: ClearAll empties", "[bitset]")
{
    BitSet b; b.Resize(100); b.Set(10); b.Set(70);
    b.ClearAll();
    int n = 0; b.ForEachSetBit([&](std::uint32_t){ ++n; });
    REQUIRE(n == 0);
}

TEST_CASE("BitSet: InPlaceUnion ORs, walks ascending across blocks", "[bitset]")
{
    BitSet a; a.Resize(130); BitSet b; b.Resize(130);
    a.Set(1); a.Set(129); b.Set(1); b.Set(64);
    a.InPlaceUnion(b);
    std::vector<std::uint32_t> got;
    a.ForEachSetBit([&](std::uint32_t i){ got.push_back(i); });
    REQUIRE(got == std::vector<std::uint32_t>{1u, 64u, 129u}); // 1 deduped
}
