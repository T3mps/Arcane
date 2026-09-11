// F2c Task 11: Arcane::ResolveMeshData -- the ONE entry point a host resolves a .arcmesh
// through (Mesh/MeshAsset.hpp). Pure, device-free, DECOUPLED from the Assets facade: every
// case here hand-builds its own MeshArtifactSupplyFn/CookPendingFn closures, exactly the way
// MeshBuilderTest.cpp drives the generators with no device and no facade. The facade-layer
// half of this task (Assets::MeshArtifactFor et al., and the real Request()-driven wiring
// through MeshCache) is AssetsTest.cpp's concern, not this file's.
//
// A generated source (any MeshSource other than Imported) delegates straight through to
// BuildMeshData + ComputeMeshBounds, unchanged -- proven here so a future edit to
// ResolveMeshData cannot quietly start requiring a supply for a primitive that never needed
// one. An Imported source resolves `data.importedSource` through `supply`, and the two ways
// that can come back empty -- PENDING (still cooking; QUIET, s7.1) vs MISSING/REFUSED (LOUD,
// names the guid) -- are the spec's own s7.1 split, and the reason this virtual exists at all.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Mesh/MeshAsset.hpp>

#include <string>
#include <vector>

using namespace Arcane;

TEST_CASE("mesh resolve: a generated source needs no supply at all", "[mesh]")
{
    MeshAssetData data; data.source = MeshSource::Cube;
    const MeshResolveResult r = ResolveMeshData(data, {}, {});
    CHECK(r.state == MeshResolveState::Ready);
    REQUIRE(r.mesh.has_value());
    CHECK(r.mesh->sections.size() == 1u);
    // The primitive path still computes bounds from vertices (s7.1's split).
    CHECK(r.bounds.max.x == Catch::Approx(0.5f));
}

TEST_CASE("mesh resolve: an imported source decodes sections and the STORED aabb",
          "[mesh]")
{
    // The artifact's AABB is taken, never recomputed -- it was cooked from these exact
    // vertices, and recomputing would be work that can only produce the same answer or
    // a different (wrong) one.
    LoadedClientMesh artifact;
    artifact.vertices = { /* 3 verts x 8 floats */
        0,0,0, 0,1,0, 0,0,
        1,0,0, 0,1,0, 1,0,
        1,0,1, 0,1,0, 1,1,
    };
    artifact.indices  = { 0, 1, 2 };
    artifact.sections = { { "Metal", 0, 3, 0 } };
    artifact.aabbMin[1] = -7.0f; artifact.aabbMax[1] = 9.0f;

    MeshAssetData data;
    data.source = MeshSource::Imported;
    data.importedSource = Guid::Generate();
    data.slots = { { "Metal", Guid::Generate() } };

    const MeshResolveResult r = ResolveMeshData(
        data, [&](const Guid& g) { return g == data.importedSource ? &artifact : nullptr; },
        [](const Guid&) { return false; });

    REQUIRE(r.state == MeshResolveState::Ready);
    REQUIRE(r.mesh.has_value());
    CHECK(r.mesh->vertices.size() == 3u);
    CHECK(r.mesh->indices == std::vector<std::uint32_t>{ 0, 1, 2 });
    REQUIRE(r.mesh->sections.size() == 1u);
    CHECK(r.mesh->sections[0].name == "Metal");
    CHECK(r.mesh->sections[0].slotIndex == 0u);
    CHECK(r.bounds.min.y == Catch::Approx(-7.0f));
    CHECK(r.bounds.max.y == Catch::Approx(9.0f));
}

TEST_CASE("mesh resolve: pending is QUIET, missing is LOUD (spec s7.1)", "[mesh]")
{
    // The distinction s7.1 turns on, and the reason CookPending is published at all:
    // both answer "no geometry", and only one of them is a problem.
    MeshAssetData data;
    data.source = MeshSource::Imported;
    data.importedSource = Guid::Generate();
    const auto noArtifact = [](const Guid&) -> const LoadedClientMesh* { return nullptr; };

    const MeshResolveResult pending =
        ResolveMeshData(data, noArtifact, [](const Guid&) { return true; });
    CHECK(pending.state == MeshResolveState::PendingCook);
    CHECK(pending.reason.empty());            // nothing to say -- it is still cooking
    CHECK_FALSE(pending.mesh.has_value());

    const MeshResolveResult missing =
        ResolveMeshData(data, noArtifact, [](const Guid&) { return false; });
    CHECK(missing.state == MeshResolveState::Failed);
    CHECK_FALSE(missing.reason.empty());      // and the reason must NAME the guid
    CHECK(missing.reason.find(data.importedSource.ToString()) != std::string::npos);
}

TEST_CASE("mesh resolve: a nil importedSource fails without consulting the supply",
          "[mesh]")
{
    // ValidateMeshAsset already refuses this (Task 10). Proven here too because the
    // resolve path is what a host actually calls, and a supply consulted with a nil
    // guid is a directory scan for nothing.
    bool consulted = false;
    MeshAssetData data; data.source = MeshSource::Imported;
    const MeshResolveResult r = ResolveMeshData(
        data, [&](const Guid&) { consulted = true; return nullptr; },
        [](const Guid&) { return false; });
    CHECK(r.state == MeshResolveState::Failed);
    CHECK_FALSE(consulted);
}
