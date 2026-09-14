// MeshResidencyBudgetTest.cpp -- F2c Plan 2 Task 1. Device-free LRU policy
// (spec s7.2 / R2). No NRI, no GPU, no files.

#include <Arcane/Render/Nri/MeshResidencyBudget.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace
{
    using Arcane::Guid;
    using Arcane::MeshResidencyEntry;
    using Arcane::SelectEvictions;
    using Arcane::MeshResidencyBytes;
    using Arcane::kMeshResidencyBudgetBytes;

    Guid GuidA() { return Guid{1, 0}; }
    Guid GuidB() { return Guid{2, 0}; }
    Guid GuidC() { return Guid{3, 0}; }
}

TEST_CASE("mesh residency: nothing is evicted while under budget", "[render]")
{
    const std::vector<MeshResidencyEntry> entries = {
        { GuidA(), 100, 5 }, { GuidB(), 100, 7 },
    };
    CHECK(SelectEvictions(entries, 1000, /*currentFrame*/ 9).empty());
}

TEST_CASE("mesh residency: least-recently-DRAWN goes first", "[render]")
{
    const std::vector<MeshResidencyEntry> entries = {
        { GuidA(), 400, 2 },   // oldest
        { GuidB(), 400, 8 },
        { GuidC(), 400, 5 },
    };
    // Budget 1000, resident 1200 -> shed 200+, so exactly one entry goes: the oldest.
    const std::vector<Guid> evicted = SelectEvictions(entries, 1000, /*currentFrame*/ 9);
    REQUIRE(evicted.size() == 1u);
    CHECK(evicted[0] == GuidA());
}

TEST_CASE("mesh residency: eviction stops as soon as the budget is met", "[render]")
{
    // Not "evict until comfortable" -- evict the MINIMUM. A cache that over-sheds
    // re-uploads next frame, which is the cliff s7.2 exists to remove.
    const std::vector<MeshResidencyEntry> entries = {
        { GuidA(), 300, 1 }, { GuidB(), 300, 2 }, { GuidC(), 300, 3 },
    };
    const std::vector<Guid> evicted = SelectEvictions(entries, 700, 9);
    REQUIRE(evicted.size() == 1u);      // 900 - 300 = 600 <= 700; a second is waste
    CHECK(evicted[0] == GuidA());
}

TEST_CASE("mesh residency: an entry drawn THIS frame is never evicted", "[render]")
{
    // THE SAFETY RULE. Dropping geometry the in-flight frame still references is a
    // use-after-free wearing a policy's clothes.
    const std::vector<MeshResidencyEntry> entries = {
        { GuidA(), 800, /*lastDrawn*/ 9 },   // this frame -- PROTECTED
        { GuidB(), 800, /*lastDrawn*/ 3 },
    };
    const std::vector<Guid> evicted = SelectEvictions(entries, 500, /*currentFrame*/ 9);
    REQUIRE(evicted.size() == 1u);
    CHECK(evicted[0] == GuidB());
    // Still over budget after evicting everything evictable -- and that is correct.
    // The caller reports it; the cache does not corrupt the frame to satisfy a number.
}

TEST_CASE("mesh residency: a single mesh larger than the whole budget stays resident",
          "[render]")
{
    const std::vector<MeshResidencyEntry> entries = { { GuidA(), 2000, 9 } };
    CHECK(SelectEvictions(entries, 500, 9).empty());
}

TEST_CASE("mesh residency: the CPU copy is counted, on purpose", "[render]")
{
    // vertices 320 + indices 96 kept on BOTH sides, plus 40 bytes of CPU-only section
    // strings: 2*(320+96) + 40.
    CHECK(MeshResidencyBytes(320, 96, 40) == 872ull);
    // And the constant is what s7.2 pins, spelled so a typo is visible.
    CHECK(kMeshResidencyBudgetBytes == 536870912ull);
}
