// Selection outline: OutlineJfaStepCount reverse-lookup ([outline], CPU) and
// PickPassId/PickSampleTexel ([pick], CPU).
//
// THE GRAPH'S OutlineNode (Render/Nri/nodes/PickOutlineNodes.*) is the only
// selection-outline implementation, and OutlineJfaStepCount below is the sole
// implementation of its jump-schedule formula.
//
// A NAMED COVERAGE GAP: nothing pins the GPU-executed PIXEL correctness of the
// JFA algorithm -- nearest-edge distance field construction, multi-id selection
// membership, the touching-silhouette union (no spurious seam at a shared
// edge), or the amber/cyan anti-aliased straddle composite. RenderGraphTest.cpp
// covers OutlineNode entirely structurally (barriers, pool slots, frame
// composition against a null context -- see its own "GPU-free by construction"
// banner), never a real render + readback. What IS pinned is the jump SCHEDULE
// (RenderGraphTest.cpp's "outline jfa" case) and the node-graph shape.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Nri/nodes/PickOutlineNodes.hpp>
#include <Arcane/Render/PickEmit.hpp>

#include <Astra/Entity/Entity.hpp>

#include <glm/glm.hpp>

#include <vector>

TEST_CASE("OutlineJfaStepCount = ceil(log2(maxThickness)) + 2", "[outline]")
{
    // The sole surviving implementation of the schedule-length formula
    // SelectionOutline.cpp's JfaPassCount used to own (JfaPassCount(x) + 1,
    // since OutlineJfaStepCount counts the trailing repeat step JfaPassCount
    // did not). Same four thickness values JfaPassCount was pinned against.
    CHECK(Arcane::OutlineJfaStepCount(1)  == 2u);
    CHECK(Arcane::OutlineJfaStepCount(3)  == 4u);
    CHECK(Arcane::OutlineJfaStepCount(16) == 6u);
    CHECK(Arcane::OutlineJfaStepCount(32) == 7u);
}

TEST_CASE("PickPassId maps ordered entities to k+1, 0 for absent/invalid", "[pick]")
{
    const Astra::Entity a = Astra::Entity(1, 0);
    const Astra::Entity b = Astra::Entity(2, 0);
    const Astra::Entity c = Astra::Entity(3, 0);
    const std::vector<Astra::Entity> ordered{ a, b, c };

    CHECK(Arcane::PickPassId(ordered, a) == 1u);
    CHECK(Arcane::PickPassId(ordered, b) == 2u);
    CHECK(Arcane::PickPassId(ordered, c) == 3u);
    CHECK(Arcane::PickPassId(ordered, Astra::Entity(9, 0)) == 0u);   // absent
    CHECK(Arcane::PickPassId(ordered, Astra::Entity::Invalid()) == 0u);
    CHECK(Arcane::PickPassId({}, a) == 0u);                          // empty
}

TEST_CASE("PickSampleTexel maps a 1x click to the center subsample, clamped", "[pick]")
{
    // ss=2, id buffer 128x128 (1x 64x64). Click at 1x pixel (10,20) -> 2x texel (21,41).
    CHECK(Arcane::PickSampleTexel(glm::vec2(10.4f, 20.9f), 2u, 128u, 128u) == glm::ivec2(21, 41));
    // ss=1 is identity (floored), clamped to bounds.
    CHECK(Arcane::PickSampleTexel(glm::vec2(3.7f, 4.2f), 1u, 64u, 64u) == glm::ivec2(3, 4));
    // out-of-range clamps into the buffer.
    CHECK(Arcane::PickSampleTexel(glm::vec2(999.0f, -5.0f), 2u, 128u, 128u) == glm::ivec2(127, 0));
}
