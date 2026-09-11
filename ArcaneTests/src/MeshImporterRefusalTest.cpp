// F2c Task 6: MeshImporter's front half -- parse, MANDATORY validate, and every
// refusal it owes -- landed before any geometry so the refusal postures are pinned
// by the fixture corpus rather than retro-fitted around a working happy path (spec
// s4.5, s5.3 first half). `ImportMesh` already takes `externalBuffers` in its
// signature (Task 7 fills in `MeshImportResult::mesh`); this task's cases assert
// only refusal/warning behavior. Tagged "[pipeline]", same tag MeshCookKeyTest.cpp
// and the rest of this suite's AssetPipeline*Test.cpp siblings use.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/AssetPipeline/MeshImporter.hpp>
#include <Arcane/AssetPipeline/MeshMetaSettings.hpp>
#include <Arcane/Guid.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using Arcane::Guid;
using Arcane::AssetPipeline::ImportMesh;
using Arcane::AssetPipeline::MeshImportResult;
using Arcane::AssetPipeline::MeshMetaSettings;
using Arcane::AssetPipeline::ReadExternalBuffers;

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

    // Import a fixture exactly the way CookSession will: read the source, read its
    // external buffers, hand BOTH to the importer.
    MeshImportResult ImportFixture(const char* name)
    {
        const std::vector<std::byte> bytes = ReadFixture(name);
        const auto buffers = ReadExternalBuffers(bytes, Fixture(name));
        std::vector<std::span<const std::byte>> spans;
        if (buffers) for (const auto& b : *buffers) spans.emplace_back(b);
        return ImportMesh(bytes, spans, Fixture(name), Guid::Generate(), MeshMetaSettings{});
    }
}

TEST_CASE("mesh import: a file with no meshes refuses loudly (spec s4.5)", "[pipeline]")
{
    const MeshImportResult r = ImportFixture("empty.glb");
    CHECK_FALSE(r.mesh.has_value());
    REQUIRE_FALSE(r.refusal.empty());
    // Actionable, not "import failed": the reason must name the file.
    CHECK(r.refusal.find("empty.glb") != std::string::npos);
}

TEST_CASE("mesh import: an unsupported required extension refuses, naming it (A2)",
          "[pipeline]")
{
    const MeshImportResult r = ImportFixture("requires_draco.gltf");
    CHECK_FALSE(r.mesh.has_value());
    REQUIRE_FALSE(r.refusal.empty());
    // The GENERAL rule, not a hand-list: the entry's own name appears because the
    // importer walked extensionsRequired, which is what covers KHR_texture_basisu,
    // EXT_meshopt_compression and every future extension by construction.
    CHECK(r.refusal.find("KHR_draco_mesh_compression") != std::string::npos);
    CHECK(r.refusal.find("extensionsRequired") != std::string::npos);
}

TEST_CASE("mesh import: a malformed sparse accessor is refused by validate", "[pipeline]")
{
    // The CVE-hardened path. The fixture is a truncated/overflowing index bound, not
    // a working exploit (spec s9) -- what is asserted is that cgltf_validate RAN and
    // its verdict was a GATE, never that a specific overflow was survived.
    const MeshImportResult r = ImportFixture("bad_sparse.glb");
    CHECK_FALSE(r.mesh.has_value());
    CHECK_FALSE(r.refusal.empty());
}

TEST_CASE("mesh import: degenerate triangles are DROPPED with a warning, not refused",
          "[pipeline]")
{
    // A2 part 2, the sharpest edge in s4.5: this fixture has 1 good triangle and 2
    // degenerate ones. Refusing it is the bug the amendment exists to prevent.
    const MeshImportResult r = ImportFixture("degenerate.glb");
    REQUIRE(r.refusal.empty());
    REQUIRE_FALSE(r.warnings.empty());
    // "one WARN naming the primitive" -- the diagnostic must locate the damage.
    CHECK(r.warnings[0].find("degenerate") != std::string::npos);
}

TEST_CASE("mesh import: a mesh whose triangles are ALL degenerate refuses", "[pipeline]")
{
    // The boundary between the two tiers, written down so nobody collapses them:
    // partial damage warns, TOTAL damage refuses ("nothing drawable"). Patched from
    // degenerate.glb's own bytes so the corpus needs no tenth entry: the GOOD
    // triangle's index triple (0,1,2, three little-endian u16s) lives at byte offset
    // 676 within the .glb file (make-mesh-fixtures.ps1's own
    // $DegenerateGoodTriangleByteOffset, asserted against on every corpus
    // regeneration) -- rewritten here to {0,0,0}, which repeats the same corner three
    // times and is degenerate exactly like the other two triangles in this fixture.
    std::vector<std::byte> bytes = ReadFixture("degenerate.glb");
    constexpr std::size_t kGoodTriangleByteOffset = 676;
    REQUIRE(bytes.size() >= kGoodTriangleByteOffset + 6);
    for (std::size_t i = 0; i < 6; ++i)
        bytes[kGoodTriangleByteOffset + i] = std::byte{ 0 };

    const auto buffers = ReadExternalBuffers(bytes, Fixture("degenerate.glb"));
    std::vector<std::span<const std::byte>> spans;
    if (buffers) for (const auto& b : *buffers) spans.emplace_back(b);
    const MeshImportResult r = ImportMesh(bytes, spans, Fixture("degenerate.glb"),
                                           Guid::Generate(), MeshMetaSettings{});
    CHECK_FALSE(r.mesh.has_value());
    CHECK_FALSE(r.refusal.empty());
}

TEST_CASE("mesh import: external buffers come back in declaration order", "[pipeline]")
{
    const std::vector<std::byte> nested = ReadFixture("nested.gltf");
    const auto buffers = ReadExternalBuffers(nested, Fixture("nested.gltf"));
    REQUIRE(buffers.has_value());
    REQUIRE(buffers->size() == 1u);            // nested.bin
    CHECK_FALSE(buffers->front().empty());

    // A GLB's buffer is EMBEDDED -- it contributes nothing, because its bytes are
    // already inside the source. Asserting it keeps "source bytes" meaning exactly
    // one thing across the cook key, the artifact hash and the client reader.
    const std::vector<std::byte> glb = ReadFixture("single.glb");
    const auto glbBuffers = ReadExternalBuffers(glb, Fixture("single.glb"));
    REQUIRE(glbBuffers.has_value());
    CHECK(glbBuffers->empty());
}
