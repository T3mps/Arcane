// MeshSubmissionTest.cpp -- Task 4 of the F2a arc (3D vocabulary in the
// scene). Two groups, both CPU-only (no device, no compiler, no batcher,
// no Runtime): MeshCache resolves .arcmesh Guids into owned geometry
// (MeshEntry); MeshMaterialCache resolves "mesh"-kind .arcmat Guids into
// constants (ResolvedMeshMaterial). Task 5's MeshSubmissionSystem is the
// consumer that borrows out of the first and reads the second -- neither is
// wired into SceneRenderResolver yet, so these cases drive each cache
// directly, mirroring SceneRenderResolverTest.cpp's SpriteCache group: a temp
// asset on disk plus a lambda resolver, no mocks of the asset layer.
//
// Both caches follow SpriteMaterialCache's failure discipline, not
// SpriteCache's: a broken Guid stays OUT of the published table (memoized in
// a private `failed` set) rather than getting a visible placeholder entry --
// there is no meaningful "placeholder mesh" or "placeholder material", so
// Resolve() returning null is the correct outcome for MeshSubmissionSystem to
// act on.

#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialTypes.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>
#include <Arcane/Render/MeshBuilder.hpp>
#include <Arcane/Render/MeshCache.hpp>
#include <Arcane/Render/MeshMaterialCache.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <glm/glm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>

namespace
{
    namespace fs = std::filesystem;

    fs::path MakeTempDir(const char* tag)
    {
        std::error_code ec;
        fs::path d = fs::temp_directory_path() / (std::string("arcane_mesh_submission_") + tag);
        fs::remove_all(d, ec);
        fs::create_directories(d, ec);
        return d;
    }

    // A valid, minimal mesh asset -- Cube reads none of the topology fields
    // (MeshAssetTest.cpp: "a cube reads nothing -- valid under every
    // parameter combination"), so it is the cheapest way to get a resolvable
    // .arcmesh on disk. `material` rides along so AssetFor's contract (the
    // loaded asset's OWN default material Guid) has something non-nil to
    // assert on.
    Arcane::Guid WriteCubeMesh(const fs::path& file, const Arcane::Guid& material)
    {
        Arcane::MeshAssetData data;
        data.id       = Arcane::Guid::Generate();
        data.name     = "probe-cube";
        data.source   = Arcane::MeshSource::Cube;
        data.material = material;
        REQUIRE(Arcane::SaveMeshAsset(file, data));
        return data.id;
    }

    // A Plane, whose vertex count (subdivisions+1)^2 -- MeshBuilder.cpp's
    // BuildPlane -- makes an edit OBSERVABLE without touching bytes, which is
    // what the Invalidate case needs: subdivisions 1 -> 4 vertices, 3 -> 16.
    Arcane::Guid WritePlaneMesh(const fs::path& file, std::uint32_t subdivisions)
    {
        Arcane::MeshAssetData data;
        data.id           = Arcane::Guid::Generate();
        data.name         = "probe-plane";
        data.source       = Arcane::MeshSource::Plane;
        data.subdivisions = subdivisions;
        REQUIRE(Arcane::SaveMeshAsset(file, data));
        return data.id;
    }

    // A structurally well-formed .arcmesh whose CONTENT ValidateMeshAsset
    // refuses (UvSphere needs rings >= 3; MeshAssetTest.cpp pins the same
    // threshold) -- SaveMeshAsset itself never validates, so this writes
    // cleanly and only BuildMeshData's internal validate call rejects it.
    Arcane::Guid WriteInvalidUvSphereMesh(const fs::path& file)
    {
        Arcane::MeshAssetData data;
        data.id       = Arcane::Guid::Generate();
        data.name     = "probe-bad-sphere";
        data.source   = Arcane::MeshSource::UvSphere;
        data.rings    = 1;    // < 3: refused
        data.segments = 32;
        REQUIRE(Arcane::SaveMeshAsset(file, data));
        return data.id;
    }

    // A BASE "mesh"-kind material carrying one saved "baseColor" Color param.
    // No snippet, matching the F2a design (a mesh material carries two
    // params -- baseColor consumed, albedo declared-not-bound -- and no
    // snippet at all).
    Arcane::Guid WriteMeshMaterial(const fs::path& file, const glm::vec4& baseColor)
    {
        Arcane::MaterialAssetData data;
        data.id   = Arcane::Guid::Generate();
        data.name = "probe-material";
        data.kind = "mesh";
        data.params.emplace_back(
            "baseColor",
            Arcane::MatParamValue::MakeColor(baseColor.r, baseColor.g, baseColor.b, baseColor.a));
        REQUIRE(Arcane::SaveMaterialAsset(file, data));
        return data.id;
    }

    // An INSTANCE of `parent` -- no kind, no snippet (both come from the base
    // at the end of the chain, MaterialAsset.hpp:5-9) -- with an OPTIONAL
    // sparse "baseColor" override.
    Arcane::Guid WriteMeshMaterialInstance(const fs::path& file, const Arcane::Guid& parent,
                                           std::optional<glm::vec4> baseColorOverride)
    {
        Arcane::MaterialAssetData data;
        data.id     = Arcane::Guid::Generate();
        data.name   = "probe-instance";
        data.parent = parent;
        if (baseColorOverride)
            data.params.emplace_back(
                "baseColor",
                Arcane::MatParamValue::MakeColor(baseColorOverride->r, baseColorOverride->g,
                                                 baseColorOverride->b, baseColorOverride->a));
        REQUIRE(Arcane::SaveMaterialAsset(file, data));
        return data.id;
    }
}

// -------------------------------------------------------------- [1] MeshCache

TEST_CASE("MeshCache resolves a Guid once and keeps serving that entry", "[mesh]")
{
    const fs::path dir      = MakeTempDir("mesh_resolve_once");
    const fs::path file     = dir / "probe.arcmesh";
    const Arcane::Guid material = Arcane::Guid::Generate();
    const Arcane::Guid id       = WriteCubeMesh(file, material);

    int resolveCalls = 0;
    Arcane::MeshCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path>
    {
        ++resolveCalls;
        return file;
    };
    Arcane::MeshCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(cache.Table().size() == 1);
    REQUIRE(resolveCalls == 1);

    const Arcane::MeshEntry& entry = cache.Table().at(id);
    CHECK(entry.data.vertices.size() == Arcane::BuildCube(1.0f).vertices.size());
    CHECK(entry.bounds.min == glm::vec3(-0.5f, -0.5f, -0.5f));
    CHECK(entry.bounds.max == glm::vec3(0.5f, 0.5f, 0.5f));

    // AssetFor is what Task 5's material chain reads the mesh's OWN default
    // material Guid from, without a second file read.
    const Arcane::MeshAssetData* asset = cache.AssetFor(id);
    REQUIRE(asset != nullptr);
    CHECK(asset->id == id);
    CHECK(asset->material == material);

    // Per-frame sweeps call Request for every referenced Guid every frame;
    // the whole point is that this is free after the first one. Pinned BY
    // ADDRESS, not just by resolveCalls: the same MeshData's address staying
    // put is exactly what proves no rebuild happened (the brief's own
    // instrument for this case) -- a std::unordered_map never relocates an
    // existing element's storage on insertion of OTHER keys, and Request's
    // table.contains(id) guard means THIS key is never re-inserted at all.
    const Arcane::MeshData* firstAddr = &cache.Table().at(id).data;
    for (int i = 0; i < 5; ++i)
        cache.Request(id);
    const Arcane::MeshData* secondAddr = &cache.Table().at(id).data;
    CHECK(firstAddr == secondAddr);
    CHECK(resolveCalls == 1);

    // A nil Guid is not an error and must not create an entry (MeshRenderer's
    // default `mesh` is nil -- an entity with no geometry assigned yet).
    cache.Request(Arcane::Guid::Nil());
    CHECK(cache.Table().size() == 1);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshCache keeps an unresolvable Guid out of the table and warns once", "[mesh]")
{
    const Arcane::Guid id = Arcane::Guid::Generate();

    int resolveCalls = 0;
    Arcane::MeshCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path>
    {
        ++resolveCalls;
        return std::nullopt;   // not in the asset registry
    };
    Arcane::MeshCache cache(std::move(s));

    cache.Request(id);
    // Unlike SpriteCache, a failure must NOT land in the published table --
    // there is no placeholder mesh, so a broken Guid must resolve to nullptr
    // through MeshTable::Resolve so MeshSubmissionSystem can skip the entity.
    CHECK_FALSE(cache.Table().contains(id));
    CHECK(cache.AssetFor(id) == nullptr);

    // Memoized: a per-frame sweep must not re-hit the filesystem (nor warn
    // again) for a Guid already known to be unresolvable.
    cache.Request(id);
    cache.Request(id);
    CHECK(resolveCalls == 1);
}

TEST_CASE("MeshCache keeps an .arcmesh that fails validation out of the table", "[mesh]")
{
    const fs::path dir  = MakeTempDir("mesh_invalid");
    const fs::path file = dir / "bad.arcmesh";
    const Arcane::Guid id = WriteInvalidUvSphereMesh(file);

    int resolveCalls = 0;
    Arcane::MeshCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path>
    {
        ++resolveCalls;
        return file;
    };
    Arcane::MeshCache cache(std::move(s));

    cache.Request(id);
    // The file loads fine (it is well-formed JSON) but BuildMeshData refuses
    // it -- nullopt exactly when ValidateMeshAsset does (MeshAsset.hpp's own
    // contract) -- so this is a DIFFERENT failure path than "not in the
    // registry" and must land the same way: out of the table, memoized.
    CHECK_FALSE(cache.Table().contains(id));
    CHECK(cache.AssetFor(id) == nullptr);

    cache.Request(id);
    CHECK(resolveCalls == 1);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshCache::Invalidate forces the next Request to re-read the file", "[mesh]")
{
    const fs::path dir  = MakeTempDir("mesh_invalidate");
    const fs::path file = dir / "probe.arcmesh";
    const Arcane::Guid id = WritePlaneMesh(file, /*subdivisions=*/1);   // 2x2 = 4 vertices

    Arcane::MeshCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path> { return file; };
    Arcane::MeshCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(cache.Table().at(id).data.vertices.size() == 4);

    // The mesh editor re-saves the asset with denser topology. Without the
    // invalidation the viewport keeps drawing the PRE-edit geometry forever --
    // Request is a once-per-Guid cache, so this hook IS the mechanism that
    // makes an edit show up.
    Arcane::MeshAssetData edited;
    edited.id           = id;
    edited.name         = "probe-plane";
    edited.source       = Arcane::MeshSource::Plane;
    edited.subdivisions = 3;   // 4x4 = 16 vertices
    REQUIRE(Arcane::SaveMeshAsset(file, edited));

    cache.Request(id);
    CHECK(cache.Table().at(id).data.vertices.size() == 4);   // still stale: cached

    cache.Invalidate(id);
    CHECK_FALSE(cache.Table().contains(id));
    CHECK(cache.AssetFor(id) == nullptr);

    cache.Request(id);
    CHECK(cache.Table().at(id).data.vertices.size() == 16);

    // Clear is the project-switch path: a Guid resolves through the CURRENT
    // project's registry, so nothing may survive the switch.
    cache.Clear();
    CHECK(cache.Table().empty());
    CHECK(cache.AssetFor(id) == nullptr);

    std::error_code ec; fs::remove_all(dir, ec);
}

// -------------------------------------------------------- [2] MeshMaterialCache

TEST_CASE("MeshMaterialCache resolves a Guid once and keeps serving that entry",
          "[mesh][material]")
{
    const fs::path dir  = MakeTempDir("matl_resolve_once");
    const fs::path file = dir / "probe.arcmat";
    const glm::vec4 color(0.25f, 0.5f, 0.75f, 1.0f);
    const Arcane::Guid id = WriteMeshMaterial(file, color);

    int resolveCalls = 0;
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path>
    {
        ++resolveCalls;
        return file;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(cache.Table().size() == 1);
    REQUIRE(resolveCalls == 1);
    CHECK(cache.Table().at(id).baseColor == color);

    // Per-frame sweeps call Request for every referenced Guid every frame;
    // this proves the second call touches neither the filesystem nor the
    // chain-walk logic again (the resolveCalls counter is this cache's stand-
    // in for MeshCache's address pin: there is no heap allocation inside
    // ResolvedMeshMaterial to pin an address on, so "the resolver was
    // consulted exactly once" is the property that proves no re-resolve
    // happened).
    for (int i = 0; i < 5; ++i)
        cache.Request(id);
    CHECK(resolveCalls == 1);

    // A nil Guid is not an error and must not create an entry (a nil
    // materialOverride/mesh-default is the documented "fall through" case,
    // not a broken reference).
    cache.Request(Arcane::Guid::Nil());
    CHECK(cache.Table().size() == 1);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshMaterialCache keeps an unresolvable Guid out of the table and warns once",
          "[mesh][material]")
{
    const Arcane::Guid id = Arcane::Guid::Generate();

    int resolveCalls = 0;
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path>
    {
        ++resolveCalls;
        return std::nullopt;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(id);
    CHECK_FALSE(cache.Table().contains(id));

    cache.Request(id);
    cache.Request(id);
    CHECK(resolveCalls == 1);
}

TEST_CASE("MeshMaterialCache keeps a malformed .arcmat out of the table", "[mesh][material]")
{
    const fs::path dir  = MakeTempDir("matl_malformed");
    const fs::path file = dir / "junk.arcmat";
    { std::ofstream f(file); f << "this is not json"; }
    const Arcane::Guid id = Arcane::Guid::Generate();

    int resolveCalls = 0;
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path>
    {
        ++resolveCalls;
        return file;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(id);
    CHECK_FALSE(cache.Table().contains(id));
    cache.Request(id);
    CHECK(resolveCalls == 1);

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshMaterialCache detects a parent-chain cycle and keeps it out of the table",
          "[mesh][material]")
{
    // A <-> B: neither ever reaches a base. WriteMeshMaterialInstance can't
    // express this directly (it needs the OTHER Guid before that file
    // exists), so this writes the pair by hand.
    const fs::path dir = MakeTempDir("matl_cycle");
    const fs::path fileA = dir / "a.arcmat";
    const fs::path fileB = dir / "b.arcmat";
    const Arcane::Guid idA = Arcane::Guid::Generate();
    const Arcane::Guid idB = Arcane::Guid::Generate();

    Arcane::MaterialAssetData a; a.id = idA; a.name = "a"; a.parent = idB;
    Arcane::MaterialAssetData b; b.id = idB; b.name = "b"; b.parent = idA;
    REQUIRE(Arcane::SaveMaterialAsset(fileA, a));
    REQUIRE(Arcane::SaveMaterialAsset(fileB, b));

    std::unordered_map<Arcane::Guid, fs::path> registry{ { idA, fileA }, { idB, fileB } };
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid& g) -> std::optional<fs::path>
    {
        auto it = registry.find(g);
        return it != registry.end() ? std::optional<fs::path>(it->second) : std::nullopt;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(idA);
    CHECK_FALSE(cache.Table().contains(idA));

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshMaterialCache resolves an instance's baseColor override over its base's",
          "[mesh][material]")
{
    const fs::path dir     = MakeTempDir("matl_instance_override");
    const fs::path baseFile = dir / "base.arcmat";
    const fs::path instFile = dir / "inst.arcmat";
    const Arcane::Guid baseId = WriteMeshMaterial(baseFile, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    const Arcane::Guid instId =
        WriteMeshMaterialInstance(instFile, baseId, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));

    std::unordered_map<Arcane::Guid, fs::path> registry{ { baseId, baseFile }, { instId, instFile } };
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid& g) -> std::optional<fs::path>
    {
        auto it = registry.find(g);
        return it != registry.end() ? std::optional<fs::path>(it->second) : std::nullopt;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(instId);
    REQUIRE(cache.Table().contains(instId));
    CHECK(cache.Table().at(instId).baseColor == glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshMaterialCache instance inherits its base's baseColor when it overrides nothing",
          "[mesh][material]")
{
    const fs::path dir      = MakeTempDir("matl_instance_inherit");
    const fs::path baseFile = dir / "base.arcmat";
    const fs::path instFile = dir / "inst.arcmat";
    const Arcane::Guid baseId = WriteMeshMaterial(baseFile, glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
    const Arcane::Guid instId = WriteMeshMaterialInstance(instFile, baseId, std::nullopt);

    std::unordered_map<Arcane::Guid, fs::path> registry{ { baseId, baseFile }, { instId, instFile } };
    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid& g) -> std::optional<fs::path>
    {
        auto it = registry.find(g);
        return it != registry.end() ? std::optional<fs::path>(it->second) : std::nullopt;
    };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(instId);
    REQUIRE(cache.Table().contains(instId));
    CHECK(cache.Table().at(instId).baseColor == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));

    std::error_code ec; fs::remove_all(dir, ec);
}

TEST_CASE("MeshMaterialCache::Invalidate forces the next Request to re-read the file",
          "[mesh][material]")
{
    const fs::path dir  = MakeTempDir("matl_invalidate");
    const fs::path file = dir / "probe.arcmat";
    const Arcane::Guid id = WriteMeshMaterial(file, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));

    Arcane::MeshMaterialCache::Services s;
    s.resolveAsset = [&](const Arcane::Guid&) -> std::optional<fs::path> { return file; };
    Arcane::MeshMaterialCache cache(std::move(s));

    cache.Request(id);
    REQUIRE(cache.Table().at(id).baseColor == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));

    // The material editor re-saves the asset with a new colour. Without the
    // invalidation the scene keeps drawing the PRE-edit colour forever.
    Arcane::MaterialAssetData edited;
    edited.id   = id;
    edited.name = "probe-material";
    edited.kind = "mesh";
    edited.params.emplace_back("baseColor", Arcane::MatParamValue::MakeColor(0.0f, 1.0f, 0.0f, 1.0f));
    REQUIRE(Arcane::SaveMaterialAsset(file, edited));

    cache.Request(id);
    CHECK(cache.Table().at(id).baseColor == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));   // still stale

    cache.Invalidate(id);
    CHECK_FALSE(cache.Table().contains(id));

    cache.Request(id);
    CHECK(cache.Table().at(id).baseColor == glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));

    cache.Clear();
    CHECK(cache.Table().empty());

    std::error_code ec; fs::remove_all(dir, ec);
}
