// F2c Task 7: MeshImporter's geometry half -- node-tree flatten + bake, the winding flip for a
// negative-determinant (mirroring) node, one section per primitive with slots deduplicated by
// material NAME (A1), meshoptimizer remap/per-section-optimize, the artifact's AABB, and the
// SourceHash extraction that finally makes `externalBuffers` a USED parameter of ImportMesh
// (Task 6 accepted it but never read it). Task 6 landed the front half (parse/validate/refusal
// ladder) with `mesh` left unset; every case here asserts on the now-filled
// `MeshImportResult::mesh`. Tagged "[pipeline]", same tag MeshImporterRefusalTest.cpp and the
// rest of this suite's AssetPipeline*Test.cpp siblings use.
//
// Deliberately does NOT include MeshImporterRefusalTest.cpp -- ImportFixture is copied here
// rather than cross-included, so this TU builds and reads standalone (this suite's established
// convention: every *Test.cpp owns its own local helpers).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/MeshImporter.hpp>
#include <Arcane/AssetPipeline/MeshMetaSettings.hpp>
#include <Arcane/Guid.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
#include <vector>

using Arcane::Guid;
using Arcane::AssetPipeline::ContentKind;
using Arcane::AssetPipeline::ImportMesh;
using Arcane::AssetPipeline::MeshArtifactSection;
using Arcane::AssetPipeline::MeshImportResult;
using Arcane::AssetPipeline::MeshMetaSettings;
using Arcane::AssetPipeline::ReadExternalBuffers;
using Arcane::AssetPipeline::SlotNamesFromSections;
using Arcane::AssetPipeline::WriteMeshArtifact;

namespace fs = std::filesystem;

namespace
{
    fs::path Fixture(const char* name) { return fs::path("data") / "gltf" / name; }

    std::vector<std::byte> ReadFixture(const char* name)
    {
        std::ifstream in(Fixture(name), std::ios::binary);
        const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
        return std::vector<std::byte>(reinterpret_cast<const std::byte*>(raw.data()),
                                       reinterpret_cast<const std::byte*>(raw.data()) + raw.size());
    }

    // Import a fixture exactly the way CookSession will: read the source, read its external
    // buffers, hand BOTH to the importer. Copied from MeshImporterRefusalTest.cpp's own helper
    // of the same name/shape rather than shared across TUs.
    MeshImportResult ImportFixture(const char* name)
    {
        const std::vector<std::byte> bytes = ReadFixture(name);
        const auto buffers = ReadExternalBuffers(bytes, Fixture(name));
        std::vector<std::span<const std::byte>> spans;
        if (buffers) for (const auto& b : *buffers) spans.emplace_back(b);
        return ImportMesh(bytes, spans, Fixture(name), Guid::Generate(), MeshMetaSettings{});
    }

    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_mesh_importer_geometry_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    std::vector<std::byte> ReadAllBytes(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        REQUIRE(in.good());
        in.seekg(0, std::ios::end);
        const auto len = static_cast<std::size_t>(in.tellg());
        in.seekg(0, std::ios::beg);
        std::vector<std::byte> out(len);
        if (len > 0)
            in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
        REQUIRE(in.good());
        return out;
    }

    // multi.glb's own RAW (pre-remap) vertex count, as scripts/make-mesh-fixtures.ps1's header
    // declares it (Build-MultiGlb's comment: "3 primitives x 4 verts" = 12) -- restated here as
    // a local constexpr rather than imported, since the generator is a build-time PowerShell
    // script with nothing this TU could #include.
    constexpr std::uint32_t kMultiGlbRawVertexCount = 12;
}

TEST_CASE("mesh import: a single-primitive glb imports one named section, one slot",
          "[pipeline]")
{
    const MeshImportResult r = ImportFixture("single.glb");
    REQUIRE(r.refusal.empty());
    REQUIRE(r.mesh.has_value());
    CHECK(r.mesh->desc.contentKind == ContentKind::Mesh);
    CHECK(r.mesh->desc.indexWidth == 4u);
    CHECK(r.mesh->desc.vertexCount == r.mesh->vertices.size());
    CHECK(r.mesh->desc.indexCount == r.mesh->indices.size());
    REQUIRE(r.mesh->desc.sections.size() == 1u);
    CHECK(r.mesh->desc.sections[0].name == "SingleMat");
    CHECK(r.mesh->desc.sections[0].indexOffset == 0u);
    CHECK(r.mesh->desc.sections[0].slotIndex == 0u);
    CHECK(SlotNamesFromSections(r.mesh->desc.sections).size() == 1u);
}

TEST_CASE("mesh import: two primitives sharing one material share ONE slot (A1)",
          "[pipeline]")
{
    // THE AMENDMENT'S PIN, spelled as spec s9 words it: three sections, TWO slots,
    // and the two Metal sections' slotIndex EQUAL. Get this wrong and a user is asked
    // to assign one material twice while R3's name re-association goes ambiguous.
    const MeshImportResult r = ImportFixture("multi.glb");
    REQUIRE(r.mesh.has_value());
    REQUIRE(r.mesh->desc.sections.size() == 3u);
    CHECK(SlotNamesFromSections(r.mesh->desc.sections).size() == 2u);
    CHECK(r.mesh->desc.sections[0].slotIndex == r.mesh->desc.sections[1].slotIndex);
    CHECK(r.mesh->desc.sections[2].slotIndex != r.mesh->desc.sections[0].slotIndex);

    // Ranges tile the index buffer exactly -- no gap, no overlap, nothing lost. This
    // is also what Plan 2's per-section draws depend on being true.
    std::uint32_t cursor = 0;
    for (const MeshArtifactSection& s : r.mesh->desc.sections)
    {
        CHECK(s.indexOffset == cursor);
        cursor += s.indexCount;
    }
    CHECK(cursor == r.mesh->desc.indexCount);
}

TEST_CASE("mesh import: nested node transforms bake into vertex positions", "[pipeline]")
{
    // The assertion is on the AABB rather than a named vertex, because meshopt's
    // vertex-fetch permutation makes "vertex k" meaningless while the BOUNDS are
    // permutation-invariant -- derived from what survives the pipeline, not from what
    // the generator happened to emit first.
    const MeshImportResult r = ImportFixture("nested.gltf");
    REQUIRE(r.mesh.has_value());
    // The generator places the child quad so its BAKED box is x[1,2] z[3,4] (see
    // scripts/make-mesh-fixtures.ps1's nested tables, which state these four numbers
    // as the fixture's contract). An UNBAKED import leaves it at the origin, so this
    // case fails loudly rather than approximately if the walk is skipped.
    CHECK(r.mesh->desc.aabbMin[0] == Catch::Approx(1.0f));
    CHECK(r.mesh->desc.aabbMax[0] == Catch::Approx(2.0f));
    CHECK(r.mesh->desc.aabbMin[2] == Catch::Approx(3.0f));
    CHECK(r.mesh->desc.aabbMax[2] == Catch::Approx(4.0f));
}

TEST_CASE("mesh import: a negative-determinant node flips triangle winding", "[pipeline]")
{
    // s4.3's mirror rule, winding half -- DERIVED, not eyeballed. For every triangle,
    // cross(v1-v0, v2-v0) must point the SAME way as the averaged vertex normal, which
    // is MeshBuilder.hpp's WINDING contract applied to imported geometry. Under a
    // mirroring node an unflipped import fails this on every face.
    const MeshImportResult r = ImportFixture("mirrored.glb");
    REQUIRE(r.mesh.has_value());
    REQUIRE(r.mesh->indices.size() % 3 == 0);
    REQUIRE(r.mesh->indices.size() >= 3);
    for (std::size_t t = 0; t + 2 < r.mesh->indices.size(); t += 3)
    {
        const auto& a = r.mesh->vertices[r.mesh->indices[t]];
        const auto& b = r.mesh->vertices[r.mesh->indices[t + 1]];
        const auto& c = r.mesh->vertices[r.mesh->indices[t + 2]];
        const glm::vec3 pa(a.px, a.py, a.pz), pb(b.px, b.py, b.pz), pc(c.px, c.py, c.pz);
        const glm::vec3 geo = glm::cross(pb - pa, pc - pa);
        const glm::vec3 avg = glm::normalize(glm::vec3(a.nx, a.ny, a.nz)
                                           + glm::vec3(b.nx, b.ny, b.nz)
                                           + glm::vec3(c.nx, c.ny, c.nz));
        INFO("triangle " << (t / 3));
        CHECK(glm::dot(geo, avg) > 0.0f);
    }
}

TEST_CASE("mesh import: a mirrored NO-NORMAL primitive gets outward flat normals",
          "[pipeline]")
{
    // THE NON-VACUOUS HALF of the case above. mirrored.glb's second primitive carries
    // POSITION only, so its normals are GENERATED -- and the case above cannot fail on
    // that primitive no matter what, because the flat normal and the geometric normal
    // are computed from the same corners and agree by construction whichever order
    // they were taken in. What must be asserted instead is that the generated normal
    // points AWAY FROM the mesh's centroid, i.e. outward, which is false exactly when
    // the flat normal was taken from the PRE-flip order (T7 pipeline step 2's UE
    // divergence -- GLTFMeshFactory.cpp:515 runs before the mirrored loop at :597-605,
    // and UE ships that inversion).
    const MeshImportResult r = ImportFixture("mirrored.glb");
    REQUIRE(r.mesh.has_value());
    REQUIRE(r.mesh->desc.sections.size() == 2u);
    const MeshArtifactSection& gen = r.mesh->desc.sections[1];   // the no-NORMAL one

    glm::vec3 centroid(0.0f);
    for (const auto& v : r.mesh->vertices) centroid += glm::vec3(v.px, v.py, v.pz);
    centroid /= static_cast<float>(r.mesh->vertices.size());

    for (std::uint32_t i = gen.indexOffset; i < gen.indexOffset + gen.indexCount; ++i)
    {
        const auto& v = r.mesh->vertices[r.mesh->indices[i]];
        const glm::vec3 p(v.px, v.py, v.pz);
        const glm::vec3 n(v.nx, v.ny, v.nz);
        INFO("generated-normal vertex " << i);
        CHECK(glm::dot(n, p - centroid) > 0.0f);   // outward, not into the surface
    }
}

TEST_CASE("mesh import: an UNmirrored fixture passes the same winding predicate",
          "[pipeline]")
{
    // POSITIVE CONTROL for the case above. Without it, a predicate that is vacuously
    // true (or a flip applied unconditionally) would pass the mirrored case and nobody
    // would know -- the same control discipline the panel-split plan's digest-chip
    // case uses.
    const MeshImportResult r = ImportFixture("single.glb");
    REQUIRE(r.mesh.has_value());
    REQUIRE(r.mesh->indices.size() % 3 == 0);
    REQUIRE(r.mesh->indices.size() >= 3);
    for (std::size_t t = 0; t + 2 < r.mesh->indices.size(); t += 3)
    {
        const auto& a = r.mesh->vertices[r.mesh->indices[t]];
        const auto& b = r.mesh->vertices[r.mesh->indices[t + 1]];
        const auto& c = r.mesh->vertices[r.mesh->indices[t + 2]];
        const glm::vec3 pa(a.px, a.py, a.pz), pb(b.px, b.py, b.pz), pc(c.px, c.py, c.pz);
        const glm::vec3 geo = glm::cross(pb - pa, pc - pa);
        const glm::vec3 avg = glm::normalize(glm::vec3(a.nx, a.ny, a.nz)
                                           + glm::vec3(b.nx, b.ny, b.nz)
                                           + glm::vec3(c.nx, c.ny, c.nz));
        INFO("triangle " << (t / 3));
        CHECK(glm::dot(geo, avg) > 0.0f);
    }
}

TEST_CASE("mesh import: same input, byte-identical artifact, twice (spec s9)",
          "[pipeline]")
{
    // DETERMINISM proven where it matters -- through the WRITER, on BYTES. Comparing
    // in-memory structs would miss a nondeterministic float that still compares equal
    // and would not exercise the serializer at all.
    const fs::path dir = TempDir("mesh_determinism");
    const Guid guid = Guid::Generate();   // the SAME guid both times: it is a header field
    for (const char* name : { "single.glb", "multi.glb", "nested.gltf", "mirrored.glb" })
    {
        INFO(name);
        const std::vector<std::byte> bytes = ReadFixture(name);
        const auto buffers = ReadExternalBuffers(bytes, Fixture(name));
        std::vector<std::span<const std::byte>> spans;
        if (buffers) for (const auto& b : *buffers) spans.emplace_back(b);

        const MeshImportResult first =
            ImportMesh(bytes, spans, Fixture(name), guid, MeshMetaSettings{});
        const MeshImportResult second =
            ImportMesh(bytes, spans, Fixture(name), guid, MeshMetaSettings{});
        REQUIRE(first.mesh.has_value());
        REQUIRE(second.mesh.has_value());

        const fs::path pathA = dir / "a.arcart";
        const fs::path pathB = dir / "b.arcart";
        REQUIRE(WriteMeshArtifact(pathA, first.mesh->desc, first.mesh->vertices, first.mesh->indices));
        REQUIRE(WriteMeshArtifact(pathB, second.mesh->desc, second.mesh->vertices, second.mesh->indices));

        CHECK(ReadAllBytes(pathA) == ReadAllBytes(pathB));
    }
}

TEST_CASE("mesh import: the vertex stream is deduplicated", "[pipeline]")
{
    // Proof that meshopt_generateVertexRemap actually ran. multi.glb's generator emits
    // shared corners as separate vertices per primitive; kMultiGlbRawVertexCount is the
    // count the fixture DECLARES, stated in the generator's header, so a remapped
    // import must come back strictly under it.
    const MeshImportResult r = ImportFixture("multi.glb");
    REQUIRE(r.mesh.has_value());
    CHECK(r.mesh->desc.vertexCount < kMultiGlbRawVertexCount);
}

TEST_CASE("mesh import: a mesh no scene node references refuses, not a silent empty artifact",
          "[pipeline]")
{
    // Task review finding: rung 5's drawability gate counts triangles over data->meshes[]
    // FILE-WIDE, but the bake reaches primitives only through the scene's node graph
    // (pipeline step 1). A mesh nobody's node references still passes rung 5's gate while
    // the bake itself produces nothing for it -- spec S4.5's "parses but yields nothing
    // drawable" case, which must refuse loudly rather than hand back a "successful"
    // ImportedMesh with empty vertices/indices/sections.
    //
    // Patched in memory, same byte-patch technique MeshImporterRefusalTest.cpp's
    // all-degenerate case already uses: single.glb's JSON chunk contains the scene's own
    // "nodes":[0] exactly once (the top-level "nodes" array -- SingleNode itself -- is a
    // DIFFERENT, untouched substring: "nodes":[{"name":"SingleNode","mesh":0}]). Rewriting
    // the SCENE's own reference to "nodes":[ ] (same byte length, so the GLB chunk lengths
    // stay valid) leaves SingleNode and its mesh in the file but unreachable from the
    // scene's roots.
    std::vector<std::byte> bytes = ReadFixture("single.glb");

    const char* needle = "\"nodes\":[0]";
    const char* replacement = "\"nodes\":[ ]";
    const std::size_t needleLen = std::strlen(needle);
    REQUIRE(std::strlen(replacement) == needleLen);

    const auto it = std::search(bytes.begin(), bytes.end(),
                                 reinterpret_cast<const std::byte*>(needle),
                                 reinterpret_cast<const std::byte*>(needle) + needleLen);
    REQUIRE(it != bytes.end());
    std::copy(reinterpret_cast<const std::byte*>(replacement),
              reinterpret_cast<const std::byte*>(replacement) + needleLen, it);

    const auto buffers = ReadExternalBuffers(bytes, Fixture("single.glb"));
    std::vector<std::span<const std::byte>> spans;
    if (buffers) for (const auto& b : *buffers) spans.emplace_back(b);
    const MeshImportResult r =
        ImportMesh(bytes, spans, Fixture("single.glb"), Guid::Generate(), MeshMetaSettings{});

    CHECK_FALSE(r.mesh.has_value());
    REQUIRE_FALSE(r.refusal.empty());
    CHECK(r.refusal.find("single.glb") != std::string::npos);
}

// ---- Final-review fix I2 (2026-09-11): primitive admission -----------------------------------

TEST_CASE("mesh import: a NON-INDEXED triangle primitive imports (the implicit identity "
          "index buffer), not refused as 'every triangle is degenerate'",
          "[pipeline]")
{
    // nonindexed.glb: one mode-4 primitive, POSITION only, NO `indices` accessor, 6
    // vertices drawn in order as 2 triangles, material "Flat". Legal glTF and common.
    // Before the fix both rung 5 and the bake `continue`d on `prim.indices == nullptr`,
    // so this file refused with the WRONG reason (a mixed file lost the geometry silently).
    const MeshImportResult r = ImportFixture("nonindexed.glb");
    REQUIRE(r.refusal.empty());
    REQUIRE(r.mesh.has_value());
    CHECK(r.warnings.empty());                // no degenerate, no skip, no partial
    CHECK(r.mesh->indices.size() == 6u);      // 2 triangles, every corner emitted
    CHECK(r.mesh->desc.indexCount == 6u);
    REQUIRE(r.mesh->desc.sections.size() == 1u);
    CHECK(r.mesh->desc.sections[0].name == "Flat");
    CHECK(r.mesh->desc.sections[0].indexOffset == 0u);
    CHECK(r.mesh->desc.sections[0].indexCount == 6u);
    // No NORMAL -> the flat-normal branch duplicates per corner, then the remap dedupes
    // the two bit-identical shared corners ((0,0,0) and (1,1,0) appear in both triangles
    // with the same flat normal and the same (0,0) uv): 6 raw -> 4 unique.
    CHECK(r.mesh->vertices.size() == 4u);
    CHECK(r.mesh->desc.vertexCount == 4u);
}

TEST_CASE("mesh import: a non-triangle primitive mode is SKIPPED with one warning naming "
          "it, and the surviving primitives still import",
          "[pipeline]")
{
    // multi.glb's JSON chunk spells every primitive's mode explicitly ("mode":4, three
    // times, primitive-major). Patching the FIRST occurrence to "mode":5 (triangle
    // strip -- same byte length, so the GLB chunk lengths stay valid) turns primitive 0
    // (Metal) into an unsupported mode while primitives 1 (Metal) and 2 (Paint) stay
    // triangle lists. Before the fix primitive 0 was dropped SILENTLY; now it is skipped
    // with exactly one warning naming the primitive and its mode, and the file imports
    // with the two survivors.
    std::vector<std::byte> bytes = ReadFixture("multi.glb");

    const char* needle = "\"mode\":4";
    const char* replacement = "\"mode\":5";
    const std::size_t needleLen = std::strlen(needle);
    REQUIRE(std::strlen(replacement) == needleLen);

    const auto it = std::search(bytes.begin(), bytes.end(),
                                 reinterpret_cast<const std::byte*>(needle),
                                 reinterpret_cast<const std::byte*>(needle) + needleLen);
    REQUIRE(it != bytes.end());   // the FIRST occurrence == primitive 0's own mode
    std::copy(reinterpret_cast<const std::byte*>(replacement),
              reinterpret_cast<const std::byte*>(replacement) + needleLen, it);

    const auto buffers = ReadExternalBuffers(bytes, Fixture("multi.glb"));
    std::vector<std::span<const std::byte>> spans;
    if (buffers) for (const auto& b : *buffers) spans.emplace_back(b);
    const MeshImportResult r =
        ImportMesh(bytes, spans, Fixture("multi.glb"), Guid::Generate(), MeshMetaSettings{});

    REQUIRE(r.refusal.empty());
    REQUIRE(r.mesh.has_value());
    // ONE warning, naming the primitive (its material name, "Metal", is PrimitiveLabel's
    // first choice) and the mode it was skipped for.
    REQUIRE(r.warnings.size() == 1u);
    CHECK(r.warnings[0].find("Metal") != std::string::npos);
    CHECK(r.warnings[0].find("skipped") != std::string::npos);
    CHECK(r.warnings[0].find("mode 5") != std::string::npos);
    // The two survivors: sections for primitive 1 (Metal) and primitive 2 (Paint), two
    // slots, 12 indices (2 quads x 2 triangles x 3).
    REQUIRE(r.mesh->desc.sections.size() == 2u);
    CHECK(r.mesh->desc.sections[0].name == "Metal");
    CHECK(r.mesh->desc.sections[1].name == "Paint");
    CHECK(SlotNamesFromSections(r.mesh->desc.sections).size() == 2u);
    CHECK(r.mesh->indices.size() == 12u);

    // And the ALL-skipped refusal names the actual reason: patch the remaining two
    // "mode":4 occurrences too and the file must refuse for its modes, not for
    // "degenerate" triangles it never had.
    for (int k = 0; k < 2; ++k)
    {
        const auto next = std::search(bytes.begin(), bytes.end(),
                                       reinterpret_cast<const std::byte*>(needle),
                                       reinterpret_cast<const std::byte*>(needle) + needleLen);
        REQUIRE(next != bytes.end());
        std::copy(reinterpret_cast<const std::byte*>(replacement),
                  reinterpret_cast<const std::byte*>(replacement) + needleLen, next);
    }
    const MeshImportResult allSkipped =
        ImportMesh(bytes, spans, Fixture("multi.glb"), Guid::Generate(), MeshMetaSettings{});
    CHECK_FALSE(allSkipped.mesh.has_value());
    REQUIRE_FALSE(allSkipped.refusal.empty());
    CHECK(allSkipped.refusal.find("unsupported mode") != std::string::npos);
    CHECK(allSkipped.refusal.find("3 skipped") != std::string::npos);   // one per primitive
    CHECK(allSkipped.refusal.find("degenerate") == std::string::npos);
}
