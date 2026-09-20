// Device-free contracts for the F3 GPU cull node.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Nri/nodes/MeshCullNode.hpp>

TEST_CASE("mesh cull: dispatch covers every row in 64-thread groups", "[meshcull][render]")
{
    CHECK(Arcane::MeshCullDispatchGroups(0u) == 0u);
    CHECK(Arcane::MeshCullDispatchGroups(1u) == 1u);
    CHECK(Arcane::MeshCullDispatchGroups(63u) == 1u);
    CHECK(Arcane::MeshCullDispatchGroups(64u) == 1u);
    CHECK(Arcane::MeshCullDispatchGroups(65u) == 2u);
}

TEST_CASE("mesh cull: metadata keeps stable batch ids including transparent and non-emitted keys", "[meshcull][render]")
{
    const Arcane::GpuCullBatch noRows{ 0u, 0u, 0u, 0u };
    const Arcane::GpuCullBatch emitted{ 3u, 5u, 1u, 1u };
    const Arcane::GpuCullBatch transparent{ 8u, 2u, 0u, 0u };
    CHECK(sizeof(Arcane::GpuCullBatch) == 16u);
    CHECK(noRows.emitted == 0u);
    CHECK(emitted.argIndex == 1u);
    CHECK(transparent.capacity == 2u);
    CHECK(transparent.emitted == 0u);
}
