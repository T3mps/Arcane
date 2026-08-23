// MeshSubmissionTest.cpp -- Tasks 4 and 5 of the F2a arc (3D vocabulary in
// the scene). Three groups, all CPU-only (no device, no compiler, no
// batcher, no Runtime):
//   [1] MeshCache resolves .arcmesh Guids into owned geometry (MeshEntry).
//   [2] MeshMaterialCache resolves "mesh"-kind .arcmat Guids into constants
//       (ResolvedMeshMaterial).
//   [3] CollectMeshInstances (Scene/MeshSubmissionSystem.hpp) is the
//       consumer that sweeps the scene and resolves each MeshRenderer
//       through a real MeshTable/MeshMaterialTable -- built directly here
//       (real maps, real MeshData from BuildCube/ComputeMeshBounds) rather
//       than through the caches above, since the sweep itself never touches
//       MeshCache/MeshMaterialCache (see MeshSubmissionSystem.hpp's own
//       NO Request() CALL note); neither is wired into SceneRenderResolver
//       yet, so [1] and [2] drive each cache directly, mirroring
//       SceneRenderResolverTest.cpp's SpriteCache group: a temp asset on
//       disk plus a lambda resolver, no mocks of the asset layer.
//
// Both caches follow SpriteMaterialCache's failure discipline, not
// SpriteCache's: a broken Guid stays OUT of the published table (memoized in
// a private `failed` set) rather than getting a visible placeholder entry --
// there is no meaningful "placeholder mesh" or "placeholder material", so
// Resolve() returning null is the correct outcome for CollectMeshInstances to
// act on.

// Include order: NRI headers first, ALWAYS -- MeshSubmissionSystem.hpp pulls
// MeshNode.hpp (for MeshInstance), which pulls <NRI.h> and, transitively,
// <Extensions/NRIDeviceCreation.h> (declares nri::Message::ERROR) ahead of
// anything below that could drag in <windows.h> (wingdi.h #defines ERROR).
// See MeshSubmissionSystem.hpp's own header comment.
#include <Arcane/Scene/MeshSubmissionSystem.hpp>

#include <Arcane/Guid.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialTypes.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>
#include <Arcane/Render/MeshBuilder.hpp>
#include <Arcane/Render/MeshCache.hpp>
#include <Arcane/Render/MeshMaterialCache.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

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
    // .arcmesh on disk. `material` rides along so MeshEntry::material (the
    // loaded asset's OWN default material Guid, the second link in the
    // submission sweep's resolution chain) has something non-nil to assert on.
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

    // MeshEntry::material is what the submission sweep reads the mesh's OWN
    // default material Guid from -- copied off the loaded .arcmesh at Request
    // time so the chain never needs a second file read nor a cache pointer.
    CHECK(entry.material == material);

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

    cache.Request(id);
    CHECK(cache.Table().at(id).data.vertices.size() == 16);

    // Clear is the project-switch path: a Guid resolves through the CURRENT
    // project's registry, so nothing may survive the switch.
    cache.Clear();
    CHECK(cache.Table().empty());

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

// ------------------------------------------------- [3] CollectMeshInstances

namespace
{
    // A resolvable MeshEntry with REAL geometry (BuildCube/ComputeMeshBounds,
    // not a zeroed-out stand-in) and the given default material Guid --
    // matching exactly what MeshCache::Request now produces
    // (MeshCache.cpp: `entry.material = data->material`). The file header's
    // "build them honestly" note is why this goes through the real
    // generator/bounds functions rather than a fabricated MeshData.
    Arcane::MeshEntry MakeMeshEntry(const Arcane::Guid& defaultMaterial)
    {
        Arcane::MeshEntry entry;
        entry.data     = Arcane::BuildCube(1.0f);
        entry.bounds   = Arcane::ComputeMeshBounds(entry.data);
        entry.material = defaultMaterial;
        return entry;
    }

    // Spawns one entity carrying WorldTransform{matrix} + MeshRenderer{mesh,
    // materialOverride} and returns it, so a case that needs to add a THIRD
    // component (Hidden) still has the handle.
    Astra::Entity SpawnMeshEntity(Astra::Registry& reg, const glm::mat4& matrix,
                                   const Arcane::Guid& mesh,
                                   const Arcane::Guid& materialOverride)
    {
        Astra::Entity e = reg.CreateEntity();
        Arcane::WorldTransform wt; wt.matrix = matrix;
        reg.AddComponent<Arcane::WorldTransform>(e, wt);
        Arcane::MeshRenderer mr;
        mr.mesh            = mesh;
        mr.materialOverride = materialOverride;
        reg.AddComponent<Arcane::MeshRenderer>(e, mr);
        return e;
    }
}

TEST_CASE("CollectMeshInstances uses a resolvable materialOverride's baseColor over the mesh default",
          "[mesh][submission]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    const Arcane::Guid meshId      = Arcane::Guid::Generate();
    const Arcane::Guid defaultMat  = Arcane::Guid::Generate();
    const Arcane::Guid overrideMat = Arcane::Guid::Generate();

    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
    meshes.emplace(meshId, MakeMeshEntry(defaultMat));
    std::unordered_map<Arcane::Guid, Arcane::ResolvedMeshMaterial> materials;
    // Deliberately distinct colours so a case that reads the wrong link in
    // the chain fails loudly rather than by coincidence.
    materials.emplace(defaultMat,  Arcane::ResolvedMeshMaterial{ glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) }); // red: must lose
    materials.emplace(overrideMat, Arcane::ResolvedMeshMaterial{ glm::vec4(0.0f, 1.0f, 0.0f, 1.0f) }); // green: must win
    reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });
    reg.SetResource<Arcane::MeshMaterialTable>(Arcane::MeshMaterialTable{ &materials });

    Arcane::Transform t; t.position = glm::vec3(1.0f, 2.0f, 3.0f);
    const glm::mat4 matrix = t.ToMatrix();
    SpawnMeshEntity(reg, matrix, meshId, overrideMat);

    std::vector<Arcane::MeshInstance> out;
    Arcane::CollectMeshInstances(reg, out);

    REQUIRE(out.size() == 1);
    // BORROWED: the instance must point INTO the published table's entry,
    // never own a copy (MeshInstance::mesh's own borrowing contract).
    CHECK(out[0].mesh == &meshes.at(meshId).data);
    CHECK(out[0].model == matrix);
    CHECK(out[0].baseColor == glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
}

TEST_CASE("CollectMeshInstances falls to the mesh asset's default material when materialOverride is nil",
          "[mesh][submission]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    const Arcane::Guid meshId     = Arcane::Guid::Generate();
    const Arcane::Guid defaultMat = Arcane::Guid::Generate();

    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
    meshes.emplace(meshId, MakeMeshEntry(defaultMat));
    std::unordered_map<Arcane::Guid, Arcane::ResolvedMeshMaterial> materials;
    materials.emplace(defaultMat, Arcane::ResolvedMeshMaterial{ glm::vec4(0.0f, 0.0f, 1.0f, 1.0f) }); // blue
    reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });
    reg.SetResource<Arcane::MeshMaterialTable>(Arcane::MeshMaterialTable{ &materials });

    SpawnMeshEntity(reg, glm::mat4(1.0f), meshId, Arcane::Guid{});   // materialOverride nil (default)

    std::vector<Arcane::MeshInstance> out;
    Arcane::CollectMeshInstances(reg, out);

    REQUIRE(out.size() == 1);
    CHECK(out[0].baseColor == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
}

TEST_CASE("CollectMeshInstances resolves to white when materialOverride and the mesh default are both nil",
          "[mesh][submission]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    const Arcane::Guid meshId = Arcane::Guid::Generate();
    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
    meshes.emplace(meshId, MakeMeshEntry(Arcane::Guid{}));   // nil default material
    std::unordered_map<Arcane::Guid, Arcane::ResolvedMeshMaterial> materials;   // nothing to resolve to regardless
    reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });
    reg.SetResource<Arcane::MeshMaterialTable>(Arcane::MeshMaterialTable{ &materials });

    SpawnMeshEntity(reg, glm::mat4(1.0f), meshId, Arcane::Guid{});   // materialOverride nil too

    std::vector<Arcane::MeshInstance> out;
    Arcane::CollectMeshInstances(reg, out);

    REQUIRE(out.size() == 1);
    CHECK(out[0].baseColor == glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
}

TEST_CASE("CollectMeshInstances falls through an unresolvable materialOverride to the mesh default, not white",
          "[mesh][submission]")
{
    // THE LANDMINE CASE. `overrideMat` is a VALID (non-nil) Guid that is
    // deliberately never inserted into `materials` below -- exactly the
    // state MeshMaterialCache::Request leaves behind for a broken Guid
    // after its ONE ARC_WARN already fired (MeshMaterialCache.cpp's `fail`
    // lambda, memoized into a private `failed` set). CollectMeshInstances
    // never calls Request itself (see MeshSubmissionSystem.hpp's own NO
    // Request() CALL / WARN-ONCE notes), so it never warns here either --
    // this case is what proves the FALL-THROUGH happens (the instance still
    // draws, with the mesh's own colour) without needing to observe a warn
    // that this function was never going to emit in the first place.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    const Arcane::Guid meshId      = Arcane::Guid::Generate();
    const Arcane::Guid defaultMat  = Arcane::Guid::Generate();
    const Arcane::Guid overrideMat = Arcane::Guid::Generate();   // never inserted into `materials`

    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
    meshes.emplace(meshId, MakeMeshEntry(defaultMat));
    std::unordered_map<Arcane::Guid, Arcane::ResolvedMeshMaterial> materials;
    // A colour that is neither white nor ever assigned to overrideMat, so a
    // read of the wrong link (silently-white, or a stale overrideMat entry)
    // cannot pass by coincidence.
    materials.emplace(defaultMat, Arcane::ResolvedMeshMaterial{ glm::vec4(0.2f, 0.4f, 0.6f, 1.0f) });
    reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });
    reg.SetResource<Arcane::MeshMaterialTable>(Arcane::MeshMaterialTable{ &materials });

    SpawnMeshEntity(reg, glm::mat4(1.0f), meshId, overrideMat);

    std::vector<Arcane::MeshInstance> out;
    Arcane::CollectMeshInstances(reg, out);

    REQUIRE(out.size() == 1);   // NOT skipped -- a broken override still draws
    CHECK(out[0].baseColor == glm::vec4(0.2f, 0.4f, 0.6f, 1.0f));   // the mesh default, never white
}

TEST_CASE("CollectMeshInstances skips a Hidden entity", "[mesh][submission]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    const Arcane::Guid meshId = Arcane::Guid::Generate();
    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
    meshes.emplace(meshId, MakeMeshEntry(Arcane::Guid{}));
    reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });

    Astra::Entity e = SpawnMeshEntity(reg, glm::mat4(1.0f), meshId, Arcane::Guid{});
    reg.AddComponent<Arcane::Hidden>(e, Arcane::Hidden{});

    std::vector<Arcane::MeshInstance> out;
    Arcane::CollectMeshInstances(reg, out);
    CHECK(out.empty());
}

TEST_CASE("CollectMeshInstances skips an entity missing WorldTransform", "[mesh][submission]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    const Arcane::Guid meshId = Arcane::Guid::Generate();
    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
    meshes.emplace(meshId, MakeMeshEntry(Arcane::Guid{}));
    reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });

    // e1: MeshRenderer only -- no WorldTransform. CreateView<WorldTransform,
    // MeshRenderer, ...> excludes it by construction, the same way it would
    // exclude any entity missing a required component.
    Astra::Entity e1 = reg.CreateEntity();
    Arcane::MeshRenderer mr1; mr1.mesh = meshId;
    reg.AddComponent<Arcane::MeshRenderer>(e1, mr1);

    // e2: the control -- carries both, so it must be the one instance that
    // survives (proves e1's absence isn't just an empty-registry accident).
    Arcane::Transform t; t.position = glm::vec3(5.0f, 6.0f, 7.0f);
    const glm::mat4 matrix = t.ToMatrix();
    SpawnMeshEntity(reg, matrix, meshId, Arcane::Guid{});

    std::vector<Arcane::MeshInstance> out;
    Arcane::CollectMeshInstances(reg, out);

    REQUIRE(out.size() == 1);
    CHECK(out[0].model == matrix);
}

TEST_CASE("CollectMeshInstances skips an entity whose MeshRenderer::mesh is nil", "[mesh][submission]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    // No MeshTable/MeshMaterialTable resource published at all -- a nil mesh
    // Guid must be skipped before either resource is even consulted, so this
    // case doubles as coverage for Resolve()'s null-table safety too.
    SpawnMeshEntity(reg, glm::mat4(1.0f), Arcane::Guid{}, Arcane::Guid{});

    std::vector<Arcane::MeshInstance> out;
    Arcane::CollectMeshInstances(reg, out);
    CHECK(out.empty());
}

TEST_CASE("CollectMeshInstances skips an entity whose mesh Guid is not in the table",
          "[mesh][submission]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    const Arcane::Guid presentMeshId = Arcane::Guid::Generate();
    const Arcane::Guid missingMeshId = Arcane::Guid::Generate();   // valid, but never published
    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
    meshes.emplace(presentMeshId, MakeMeshEntry(Arcane::Guid{}));
    reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });

    SpawnMeshEntity(reg, glm::mat4(1.0f), missingMeshId, Arcane::Guid{});

    std::vector<Arcane::MeshInstance> out;
    Arcane::CollectMeshInstances(reg, out);
    CHECK(out.empty());
}

TEST_CASE("CollectMeshInstances::model is byte-identical to WorldTransform::matrix",
          "[mesh][submission]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    const Arcane::Guid meshId = Arcane::Guid::Generate();
    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
    meshes.emplace(meshId, MakeMeshEntry(Arcane::Guid{}));
    reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });

    // Rotation + non-uniform scale + translation, not just a translation --
    // a matrix with real trigonometric entries is what would expose the
    // sweep re-deriving the pose (e.g. from position/rotation/scale fields)
    // instead of copying WorldTransform::matrix verbatim.
    Arcane::Transform t;
    t.position = glm::vec3(3.0f, -2.0f, 9.0f);
    t.rotation = Arcane::RotationAboutZ(0.7f);
    t.scale    = glm::vec3(2.0f, 3.0f, 1.5f);
    const glm::mat4 matrix = t.ToMatrix();

    SpawnMeshEntity(reg, matrix, meshId, Arcane::Guid{});

    std::vector<Arcane::MeshInstance> out;
    Arcane::CollectMeshInstances(reg, out);

    REQUIRE(out.size() == 1);
    // Exact equality, not Approx: a mismatch here would mean the sweep is
    // re-deriving the pose rather than copying it straight across.
    CHECK(out[0].model == matrix);
}

TEST_CASE("CollectMeshInstances clears `out` on entry", "[mesh][submission]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);

    const Arcane::Guid meshId = Arcane::Guid::Generate();
    std::unordered_map<Arcane::Guid, Arcane::MeshEntry> meshes;
    meshes.emplace(meshId, MakeMeshEntry(Arcane::Guid{}));
    reg.SetResource<Arcane::MeshTable>(Arcane::MeshTable{ &meshes });

    SpawnMeshEntity(reg, glm::mat4(1.0f), meshId, Arcane::Guid{});

    std::vector<Arcane::MeshInstance> out;
    out.push_back(Arcane::MeshInstance{});   // a stray pre-existing entry
    Arcane::CollectMeshInstances(reg, out);
    REQUIRE(out.size() == 1);   // the stray entry is gone, not appended to

    Arcane::CollectMeshInstances(reg, out);   // second call, same unchanged scene
    REQUIRE(out.size() == 1);   // still 1, not 2 -- a rebuild, not an accumulation
}
