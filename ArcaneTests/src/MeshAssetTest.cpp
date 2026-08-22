#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Arcane/Mesh/MeshAsset.hpp>
#include <Arcane/Render/MeshBuilder.hpp>

#include <filesystem>
#include <fstream>

using namespace Arcane;
using Catch::Matchers::WithinAbs;

namespace
{
    std::filesystem::path TempMeshPath(const char* stem)
    {
        std::filesystem::path p =
            std::filesystem::temp_directory_path() / "arcane_mesh_asset_test";
        std::filesystem::create_directories(p);
        return p / (std::string(stem) + ".arcmesh");
    }
}

TEST_CASE("a .arcmesh round-trips through save and load", "[mesh][asset]")
{
    MeshAssetData in;
    in.id                 = Guid::Generate();
    in.name               = "Test Capsule";
    in.source             = MeshSource::Capsule;
    in.rings              = 6;
    in.segments           = 20;
    in.subdivisions       = 4;
    in.capsuleLengthRatio = 3.0f;
    in.material           = Guid::Generate();

    const std::filesystem::path p = TempMeshPath("roundtrip");
    REQUIRE(SaveMeshAsset(p, in));

    const std::optional<MeshAssetData> out = LoadMeshAsset(p);
    REQUIRE(out.has_value());
    CHECK(*out == in);
}

TEST_CASE("a missing or malformed .arcmesh loads as nullopt, not as defaults", "[mesh][asset]")
{
    CHECK_FALSE(LoadMeshAsset(TempMeshPath("does-not-exist-at-all")).has_value());

    const std::filesystem::path junk = TempMeshPath("junk");
    { std::ofstream f(junk); f << "this is not json"; }
    CHECK_FALSE(LoadMeshAsset(junk).has_value());
}

TEST_CASE("validation is PER SOURCE, over the fields that source reads", "[mesh][asset]")
{
    // The flat tagged struct's one real hazard: validating every field
    // regardless of tag would refuse legal assets. A plane has no radial
    // segments, so segments == 0 is as meaningless to it as halfLen is to a
    // Manifold2D Circle -- and therefore must NOT refuse.
    MeshAssetData plane;
    plane.source       = MeshSource::Plane;
    plane.segments     = 0;
    plane.rings        = 0;
    plane.subdivisions = 1;
    CHECK_FALSE(ValidateMeshAsset(plane).has_value());

    plane.subdivisions = 0;
    CHECK(ValidateMeshAsset(plane).has_value());

    // A cube reads NOTHING -- it is valid under every parameter combination.
    MeshAssetData cube;
    cube.source             = MeshSource::Cube;
    cube.rings              = 0;
    cube.segments           = 0;
    cube.subdivisions       = 0;
    cube.capsuleLengthRatio = 0.0f;
    CHECK_FALSE(ValidateMeshAsset(cube).has_value());

    // Thresholds are UE's (SphereGenerator.h:200-201, CapsuleGenerator.h:265-267).
    MeshAssetData sphere;
    sphere.source = MeshSource::UvSphere;
    sphere.rings = 3; sphere.segments = 3;
    CHECK_FALSE(ValidateMeshAsset(sphere).has_value());
    sphere.rings = 2;
    CHECK(ValidateMeshAsset(sphere).has_value());
    sphere.rings = 3; sphere.segments = 2;
    CHECK(ValidateMeshAsset(sphere).has_value());

    // A capsule's CAP-RING floor is 2, not 3 -- an arc needs fewer steps than
    // a closed loop (UE's NumHemisphereArcSteps).
    MeshAssetData capsule;
    capsule.source = MeshSource::Capsule;
    capsule.rings = 2; capsule.segments = 3; capsule.capsuleLengthRatio = 1.0f;
    CHECK_FALSE(ValidateMeshAsset(capsule).has_value());
    capsule.rings = 1;
    CHECK(ValidateMeshAsset(capsule).has_value());
    capsule.rings = 2; capsule.capsuleLengthRatio = 0.99f;
    CHECK(ValidateMeshAsset(capsule).has_value());
}

TEST_CASE("the refusal reason names the offending field", "[mesh][asset]")
{
    MeshAssetData bad;
    bad.source = MeshSource::UvSphere;
    bad.rings = 1; bad.segments = 32;
    const std::optional<std::string> why = ValidateMeshAsset(bad);
    REQUIRE(why.has_value());
    // The message is what a user reads in the Problems pane; a reason that
    // does not name the field is not actionable.
    CHECK(why->find("rings") != std::string::npos);
}

TEST_CASE("BuildMeshData dispatches per source and refuses exactly what validation does",
          "[mesh][asset]")
{
    MeshAssetData d;

    d.source = MeshSource::Cube;
    REQUIRE(BuildMeshData(d).has_value());
    CHECK(BuildMeshData(d)->vertices.size() == BuildCube(1.0f).vertices.size());

    d.source = MeshSource::Plane; d.subdivisions = 1;
    REQUIRE(BuildMeshData(d).has_value());
    CHECK(BuildMeshData(d)->indices.size() == 6);

    d.subdivisions = 0;
    CHECK_FALSE(BuildMeshData(d).has_value());   // refuses, emits nothing
}

TEST_CASE("BuildMeshData is deterministic", "[mesh][asset]")
{
    // Same input, same bytes -- the artifact-determinism rule the cook contract
    // states, applied here so F2c inherits a builder that already obeys it.
    MeshAssetData d;
    d.source = MeshSource::UvSphere;
    d.rings = 12; d.segments = 24;

    const std::optional<MeshData> a = BuildMeshData(d);
    const std::optional<MeshData> b = BuildMeshData(d);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(a->vertices.size() == b->vertices.size());
    REQUIRE(a->indices == b->indices);
    for (std::size_t i = 0; i < a->vertices.size(); ++i)
    {
        CHECK_THAT(a->vertices[i].position.x, WithinAbs(b->vertices[i].position.x, 0.0f));
        CHECK_THAT(a->vertices[i].position.y, WithinAbs(b->vertices[i].position.y, 0.0f));
        CHECK_THAT(a->vertices[i].position.z, WithinAbs(b->vertices[i].position.z, 0.0f));
    }
}

TEST_CASE("the unit rule holds for every source", "[mesh][asset]")
{
    // The spec's load-bearing rule, pinned once per source: nothing a generator
    // emits exceeds unit extent in X or Z. (Capsule is the ONE exception in Y,
    // and only by its ratio -- which is why the ratio is the only size-ish
    // field that survived.)
    const MeshSource all[] = { MeshSource::Plane, MeshSource::Cube,
                               MeshSource::UvSphere, MeshSource::Cylinder,
                               MeshSource::Capsule };
    for (MeshSource s : all)
    {
        MeshAssetData d;
        d.source = s;
        const std::optional<MeshData> m = BuildMeshData(d);
        REQUIRE(m.has_value());
        const MeshBounds b = ComputeMeshBounds(*m);
        CHECK(b.max.x - b.min.x <= 1.0f + 1e-3f);
        CHECK(b.max.z - b.min.z <= 1.0f + 1e-3f);
    }
}
