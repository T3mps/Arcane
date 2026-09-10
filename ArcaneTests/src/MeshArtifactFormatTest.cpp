// F2c Task 3: the .arcart MESH artifact container (Arcane::AssetPipeline::ArtifactFormat).
// Pins the mesh pair's on-disk contract directly, the same way AssetPipelineFormatTest.cpp
// pins the texture pair's: every MeshArtifactDesc field round-trips byte-exactly, an empty
// section NAME round-trips as empty (not as an absent slot), the kind gate refuses a mesh
// file through the texture reader and vice versa, a declared indexWidth other than 4 is
// refused in v1, a header declaring nonzero vertexCount/indexCount with the VertexData
// section omitted outright is refused (the decoded arrays must agree with the DECLARED
// counts, not just each present section's own internal consistency), an unrecognised
// SectionTag is skipped (forward-compat -- the SAME tag space the texture kind uses, so a
// reader of one kind must tolerate the other's tags), and SlotNamesFromSections derives one
// name per slot from the section table.
//
// The unrecognised-section-tag and missing-VertexData cases hand-roll their OWN encoder
// against the DOCUMENTED layout (header fields in MeshArtifactDesc's declaration order, then
// the container's own section table, then the section bodies) rather than reusing
// WriteMeshArtifact, matching the texture test's own discipline: a bug that made the writer
// and reader agree with each other but disagree with the spec would still be caught.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/Guid.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace Arcane::AssetPipeline;
using Arcane::Guid;

namespace
{
    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_pipeline_format_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    // ---- Independent raw encoder for the negative-path test --------------------------------
    // Mirrors the CONTRACT (this task's brief / ArtifactFormat.hpp's own comments), not the
    // implementation: magic 'A','R','C','A', artifactVersion (u32), then MeshArtifactDesc's
    // fixed fields in DECLARATION order, THEN the container's own sectionCount (u32) +
    // {tag u32, offset u64, size u64} per section, then section bodies.

    void PutU8(std::vector<std::byte>& b, std::uint8_t v) { b.push_back(static_cast<std::byte>(v)); }

    void PutU16(std::vector<std::byte>& b, std::uint16_t v)
    {
        for (int i = 0; i < 2; ++i)
            PutU8(b, static_cast<std::uint8_t>(v >> (8 * i)));
    }

    void PutU32(std::vector<std::byte>& b, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            PutU8(b, static_cast<std::uint8_t>(v >> (8 * i)));
    }

    void PutU64(std::vector<std::byte>& b, std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            PutU8(b, static_cast<std::uint8_t>(v >> (8 * i)));
    }

    void PutF32(std::vector<std::byte>& b, float v) { PutU32(b, std::bit_cast<std::uint32_t>(v)); }

    void PutBytes(std::vector<std::byte>& b, const std::vector<std::byte>& data)
    {
        b.insert(b.end(), data.begin(), data.end());
    }

    // Builds a complete header (magic..aabbMax) for a mesh desc with the given counts, matching
    // the byte widths pinned by the interface: 1(contentKind)+16(guid)+8(sourceHash)+
    // 4(importerVersion)+4(vertexCount)+4(indexCount)+4(sectionCount)+1(indexWidth)+
    // 12(aabbMin)+12(aabbMax) = 66 desc bytes, +4 magic +4 version = 74 header bytes total.
    std::vector<std::byte> EncodeMeshHeader(std::uint32_t artifactVersion, std::uint32_t vertexCount,
                                             std::uint32_t indexCount, std::uint32_t sectionCount)
    {
        std::vector<std::byte> h;
        PutU8(h, static_cast<std::uint8_t>('A')); PutU8(h, static_cast<std::uint8_t>('R'));
        PutU8(h, static_cast<std::uint8_t>('C')); PutU8(h, static_cast<std::uint8_t>('A'));
        PutU32(h, artifactVersion);
        PutU8(h, static_cast<std::uint8_t>(ContentKind::Mesh));
        PutU64(h, 0xAAAABBBBCCCCDDDDULL);   // sourceGuid.hi
        PutU64(h, 0x1111222233334444ULL);   // sourceGuid.lo
        PutU64(h, 0xDEADBEEFCAFEBABEULL);   // sourceHash
        PutU32(h, 1);                       // importerVersion
        PutU32(h, vertexCount);
        PutU32(h, indexCount);
        PutU32(h, sectionCount);
        PutU8(h, 4);                        // indexWidth (v1's only legal value)
        PutF32(h, -1.0f); PutF32(h, -2.0f); PutF32(h, -3.0f);   // aabbMin
        PutF32(h, 1.0f);  PutF32(h, 2.0f);  PutF32(h, 3.0f);    // aabbMax
        return h;
    }

    void WriteFile(const fs::path& path, const std::vector<std::byte>& bytes)
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        REQUIRE(ofs.good());
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(ofs.good());
    }
}

// ---- round trip -----------------------------------------------------------------------------

TEST_CASE("mesh artifact: every header field round-trips byte-exactly", "[pipeline]")
{
    const fs::path dir = TempDir("mesh_roundtrip");
    MeshArtifactDesc desc{};
    desc.contentKind = ContentKind::Mesh;
    desc.sourceGuid = Guid::Generate();
    desc.sourceHash = 0x0123456789ABCDEFULL;
    desc.importerVersion = 3;
    desc.vertexCount = 4;
    desc.indexCount = 9;
    desc.sectionCount = 2;
    desc.indexWidth = 4;
    // Every component distinct, so a transposed pair fails the round-trip.
    desc.aabbMin[0] = -1.5f; desc.aabbMin[1] = -2.25f; desc.aabbMin[2] = -3.125f;
    desc.aabbMax[0] =  4.5f; desc.aabbMax[1] =  5.25f; desc.aabbMax[2] =  6.125f;
    desc.sections = {
        { "Metal", 0, 6, 0 },
        { "Metal", 6, 3, 0 },   // A1: two sections, ONE slot
    };

    const std::vector<MeshArtifactVertex> vertices = {
        { 0,0,0,  0,1,0,  0,0 }, { 1,0,0,  0,1,0,  1,0 },
        { 1,0,1,  0,1,0,  1,1 }, { 0,0,1,  0,1,0,  0,1 },
    };
    const std::vector<std::uint32_t> indices = { 0,1,2, 0,2,3, 1,2,3 };

    const fs::path path = dir / "mesh.arcart";
    REQUIRE(WriteMeshArtifact(path, desc, vertices, indices));

    const std::optional<LoadedMeshArtifact> loaded = ReadMeshArtifact(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->desc.contentKind == ContentKind::Mesh);
    CHECK(loaded->desc.sourceGuid == desc.sourceGuid);
    CHECK(loaded->desc.sourceHash == desc.sourceHash);
    CHECK(loaded->desc.importerVersion == 3u);
    CHECK(loaded->desc.vertexCount == 4u);
    CHECK(loaded->desc.indexCount == 9u);
    CHECK(loaded->desc.sectionCount == 2u);
    CHECK(loaded->desc.indexWidth == 4u);
    for (int i = 0; i < 3; ++i)
    {
        CHECK(loaded->desc.aabbMin[i] == desc.aabbMin[i]);
        CHECK(loaded->desc.aabbMax[i] == desc.aabbMax[i]);
    }
    REQUIRE(loaded->desc.sections.size() == 2u);
    CHECK(loaded->desc.sections[0].name == "Metal");
    CHECK(loaded->desc.sections[1].indexOffset == 6u);
    CHECK(loaded->desc.sections[1].indexCount == 3u);
    CHECK(loaded->desc.sections[0].slotIndex == loaded->desc.sections[1].slotIndex);
    REQUIRE(loaded->vertices.size() == 4u);
    CHECK(loaded->vertices[2].u == 1.0f);
    CHECK(loaded->vertices[2].v == 1.0f);
    CHECK(loaded->indices == indices);
}

TEST_CASE("mesh artifact: an empty section NAME round-trips as empty, not as absent",
          "[pipeline]")
{
    // The unnamed slot (a primitive with no material, s4.3) is a REAL slot whose
    // name is the empty string -- a writer that skipped a zero-length name, or a
    // reader that treated it as a truncated record, would lose the slot.
    const fs::path dir = TempDir("mesh_unnamed");
    MeshArtifactDesc desc{};
    desc.sourceGuid = Guid::Generate();
    desc.vertexCount = 3; desc.indexCount = 3; desc.sectionCount = 1;
    desc.sections = { { "", 0, 3, 0 } };
    const std::vector<MeshArtifactVertex> v(3);
    const std::vector<std::uint32_t> i = { 0, 1, 2 };
    REQUIRE(WriteMeshArtifact(dir / "m.arcart", desc, v, i));
    const auto loaded = ReadMeshArtifact(dir / "m.arcart");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->desc.sections.size() == 1u);
    CHECK(loaded->desc.sections[0].name.empty());
}

// ---- kind gate --------------------------------------------------------------------------

TEST_CASE("mesh artifact: the texture reader refuses it and vice versa", "[pipeline]")
{
    // The fail-closed contentKind check both readers already carry (ArtifactFormat.cpp's
    // own "spares F2c's mesh-artifact author a texture reader that happily misreads")
    // -- now that kind 2 exists, prove it BOTH ways rather than trusting the comment.
    const fs::path dir = TempDir("mesh_kind_gate");
    MeshArtifactDesc mesh{};
    mesh.sourceGuid = Guid::Generate();
    mesh.vertexCount = 3; mesh.indexCount = 3; mesh.sectionCount = 1;
    mesh.sections = { { "", 0, 3, 0 } };
    REQUIRE(WriteMeshArtifact(dir / "mesh.arcart", mesh,
                              std::vector<MeshArtifactVertex>(3),
                              std::vector<std::uint32_t>{ 0, 1, 2 }));
    CHECK_FALSE(ReadTextureArtifact(dir / "mesh.arcart").has_value());

    TextureArtifactDesc tex{};
    tex.contentKind = ContentKind::Texture;
    tex.sourceGuid = Guid::Generate();
    tex.format = ArtifactPixelFormat::RGBA8;
    tex.dimension = ArtifactDimension::Tex2D;
    tex.arrayOrDepth = 1; tex.width = 1; tex.height = 1; tex.mipCount = 1;
    tex.mips = { MipDesc{ 0, 4, 1, 1 } };
    tex.thumbWidth = 1; tex.thumbHeight = 1;
    const std::vector<std::byte> texel(4, std::byte{ 0xFF });
    REQUIRE(WriteTextureArtifact(dir / "tex.arcart", tex, texel, texel));
    CHECK_FALSE(ReadMeshArtifact(dir / "tex.arcart").has_value());
}

// ---- refusals -----------------------------------------------------------------------------

TEST_CASE("mesh artifact: a declared indexWidth other than 4 is refused in v1",
          "[pipeline]")
{
    // Comparison A5: the byte is DECLARED so a future 16-bit path is a branch. In v1
    // the only legal value is 4, and a file claiming otherwise is refused rather than
    // read as though its indices were u32 anyway.
    const fs::path dir = TempDir("mesh_indexwidth");
    MeshArtifactDesc desc{};
    desc.sourceGuid = Guid::Generate();
    desc.vertexCount = 3; desc.indexCount = 3; desc.sectionCount = 1;
    desc.indexWidth = 2;   // v1 writers never produce this; a hand-edited file might
    desc.sections = { { "", 0, 3, 0 } };
    REQUIRE(WriteMeshArtifact(dir / "m.arcart", desc,
                              std::vector<MeshArtifactVertex>(3),
                              std::vector<std::uint32_t>{ 0, 1, 2 }));
    CHECK_FALSE(ReadMeshArtifact(dir / "m.arcart").has_value());
}

TEST_CASE("mesh artifact: a header declaring nonzero counts with the VertexData section "
          "omitted is refused", "[pipeline]")
{
    // Hand-rolled against the DOCUMENTED layout, NOT through the writer: a header that
    // declares vertexCount=3/indexCount=3, but whose container-level section table only
    // carries IndexData and SectionTable (VertexData is entirely absent). Every per-section
    // check passes -- the present sections are internally consistent -- so without the
    // decoded-count-vs-declared-count check this would silently return a LoadedMeshArtifact
    // with vertexCount=3 paired with an EMPTY vertices array. An absent body disagrees with
    // its declared count maximally, the same class of corruption a truncated section is.
    const fs::path dir = TempDir("mesh_missing_vertexdata");
    const fs::path path = dir / "missing_vertexdata.arcart";

    constexpr std::uint32_t kVertexCount = 3;
    constexpr std::uint32_t kIndexCount = 3;
    constexpr std::uint32_t kSectionCount = 1;

    // IndexData body: {0, 1, 2} -- internally consistent with kIndexCount.
    std::vector<std::byte> indexDataBody;
    PutU32(indexDataBody, 0); PutU32(indexDataBody, 1); PutU32(indexDataBody, 2);

    // SectionTable body: 1 record, internally consistent with kSectionCount and covering all
    // 3 declared indices.
    std::vector<std::byte> sectionTableBody;
    PutU32(sectionTableBody, 1);   // 1 section record
    PutU16(sectionTableBody, 0);   // name length 0
    PutU32(sectionTableBody, 0);   // indexOffset
    PutU32(sectionTableBody, 3);   // indexCount
    PutU32(sectionTableBody, 0);   // slotIndex

    std::vector<std::byte> file = EncodeMeshHeader(/*artifactVersion*/ 1, kVertexCount, kIndexCount, kSectionCount);

    // Only 2 container-level sections -- VertexData is omitted outright, not merely resized.
    constexpr std::uint32_t kContainerSectionCount = 2;
    const std::uint64_t headerSize = file.size();
    const std::uint64_t tableSize = 4 + kContainerSectionCount * 20ULL;
    std::uint64_t offset = headerSize + tableSize;

    const std::uint64_t indexOffset = offset; offset += indexDataBody.size();
    const std::uint64_t sectionTableOffset = offset; offset += sectionTableBody.size();

    PutU32(file, kContainerSectionCount);
    PutU32(file, static_cast<std::uint32_t>(SectionTag::IndexData));    PutU64(file, indexOffset);        PutU64(file, indexDataBody.size());
    PutU32(file, static_cast<std::uint32_t>(SectionTag::SectionTable)); PutU64(file, sectionTableOffset); PutU64(file, sectionTableBody.size());

    REQUIRE(file.size() == headerSize + tableSize);

    PutBytes(file, indexDataBody);
    PutBytes(file, sectionTableBody);

    WriteFile(path, file);

    // Baseline sanity: the file is well-formed EXCEPT for the missing VertexData section, so
    // this pins the refusal to that specific omission rather than some other malformed byte.
    CHECK_FALSE(ReadMeshArtifact(path).has_value());
}

// ---- forward-compat ------------------------------------------------------------------------

TEST_CASE("mesh artifact: an unrecognised section tag is skipped, not fatal",
          "[pipeline]")
{
    // Forward-compat, hand-rolled against the DOCUMENTED layout rather than through
    // the writer -- append a section table entry with tag 999 pointing at a body the
    // reader has no case for, and require the mesh still loads. This is what keeps a
    // future Tangents/Thumbnail section additive.
    const fs::path dir = TempDir("mesh_unknown_section");
    const fs::path path = dir / "unknown_section.arcart";

    constexpr std::uint32_t kVertexCount = 3;
    constexpr std::uint32_t kIndexCount = 3;
    constexpr std::uint32_t kSectionCount = 1;   // the mesh's own SectionTable has 1 record

    // VertexData body: 3 zeroed vertices, 32 bytes each.
    std::vector<std::byte> vertexDataBody;
    for (std::uint32_t v = 0; v < kVertexCount; ++v)
        for (int f = 0; f < 8; ++f)
            PutF32(vertexDataBody, 0.0f);

    // IndexData body: {0, 1, 2}.
    std::vector<std::byte> indexDataBody;
    PutU32(indexDataBody, 0); PutU32(indexDataBody, 1); PutU32(indexDataBody, 2);

    // SectionTable body (the MESH's own per-section records, tag=SectionTable): count=1, then
    // one record with an empty name, covering all 3 indices, slot 0.
    std::vector<std::byte> sectionTableBody;
    PutU32(sectionTableBody, 1);          // 1 section record
    PutU16(sectionTableBody, 0);          // name length 0
    PutU32(sectionTableBody, 0);          // indexOffset
    PutU32(sectionTableBody, 3);          // indexCount
    PutU32(sectionTableBody, 0);          // slotIndex

    // A future section this reader has never heard of.
    std::vector<std::byte> unknownBody;
    for (int k = 0; k < 6; ++k)
        PutU8(unknownBody, static_cast<std::uint8_t>(0x90 + k));

    std::vector<std::byte> file = EncodeMeshHeader(/*artifactVersion*/ 1, kVertexCount, kIndexCount, kSectionCount);

    constexpr std::uint32_t kContainerSectionCount = 4;
    const std::uint64_t headerSize = file.size();                          // 74
    const std::uint64_t tableSize = 4 + kContainerSectionCount * 20ULL;    // count + 4 * {tag,offset,size}
    std::uint64_t offset = headerSize + tableSize;

    const std::uint64_t vertexOffset = offset; offset += vertexDataBody.size();
    const std::uint64_t indexOffset  = offset; offset += indexDataBody.size();
    const std::uint64_t sectionTableOffset = offset; offset += sectionTableBody.size();
    const std::uint64_t unknownOffset = offset; offset += unknownBody.size();

    PutU32(file, kContainerSectionCount);
    PutU32(file, static_cast<std::uint32_t>(SectionTag::VertexData));   PutU64(file, vertexOffset);       PutU64(file, vertexDataBody.size());
    PutU32(file, static_cast<std::uint32_t>(SectionTag::IndexData));    PutU64(file, indexOffset);        PutU64(file, indexDataBody.size());
    PutU32(file, static_cast<std::uint32_t>(SectionTag::SectionTable)); PutU64(file, sectionTableOffset); PutU64(file, sectionTableBody.size());
    PutU32(file, 999u);   /* unknown tag */                             PutU64(file, unknownOffset);      PutU64(file, unknownBody.size());

    REQUIRE(file.size() == headerSize + tableSize);

    PutBytes(file, vertexDataBody);
    PutBytes(file, indexDataBody);
    PutBytes(file, sectionTableBody);
    PutBytes(file, unknownBody);

    WriteFile(path, file);

    const auto loaded = ReadMeshArtifact(path);
    REQUIRE(loaded.has_value());   // the unknown tag must NOT cause a refusal
    CHECK(loaded->indices == std::vector<std::uint32_t>{ 0, 1, 2 });
    REQUIRE(loaded->desc.sections.size() == 1u);
    CHECK(loaded->desc.sections[0].name.empty());
}

// ---- SlotNamesFromSections ----------------------------------------------------------------

TEST_CASE("mesh artifact: SlotNamesFromSections derives one name per slot", "[pipeline]")
{
    const std::vector<MeshArtifactSection> sections = {
        { "Metal", 0, 6, 0 }, { "Paint", 6, 3, 1 }, { "Metal", 9, 3, 0 },
    };
    const std::vector<std::string> names = SlotNamesFromSections(sections);
    REQUIRE(names.size() == 2u);
    CHECK(names[0] == "Metal");
    CHECK(names[1] == "Paint");
    CHECK(SlotNamesFromSections({}).empty());
}
