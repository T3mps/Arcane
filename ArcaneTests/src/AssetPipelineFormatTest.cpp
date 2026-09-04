// F2b Task 1: the .arcart texture artifact container (Arcane::AssetPipeline::ArtifactFormat).
// Pins the on-disk contract directly -- every TextureArtifactDesc field round-trips byte-
// exactly (including contentKind/dimension/arrayOrDepth, which are fixed-for-this-slice but
// still part of the serialized header), the mip table preserves insertion order, and the
// reader REFUSES (nullopt) on bad magic / unknown artifactVersion / a truncated section, while
// SKIPPING a section tag it doesn't recognise (forward-compat: F2c's mesh artifacts add new
// tags without breaking old readers).
//
// The wrong-magic/bad-version/unknown-section cases hand-roll their OWN encoder against the
// documented header layout (magic + artifactVersion + desc fields, all little-endian, exact
// byte widths from the interface's own field types) rather than reusing WriteTextureArtifact,
// so a bug that made the writer and reader agree with each other but disagree with the spec
// would still be caught.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/Guid.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

    std::vector<std::byte> PatternBytes(std::size_t n, std::uint8_t seed)
    {
        std::vector<std::byte> out(n);
        for (std::size_t i = 0; i < n; ++i)
            out[i] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + i * 7));
        return out;
    }

    // A fully-populated desc with every field distinct from every other same-width field, so a
    // transposed pair (e.g. width/height, thumbWidth/thumbHeight) fails the round-trip.
    TextureArtifactDesc MakeDesc()
    {
        TextureArtifactDesc desc{};
        desc.contentKind = ContentKind::Texture;
        desc.sourceGuid = Guid::Generate();
        desc.sourceHash = 0x1122334455667788ULL;
        desc.importerVersion = 7;
        desc.format = ArtifactPixelFormat::BC7;
        desc.dimension = ArtifactDimension::Tex2D;
        desc.arrayOrDepth = 1;
        desc.width = 256;
        desc.height = 128;
        desc.mipCount = 3;
        desc.srgb = true;
        desc.mips = {
            MipDesc{ /*offset*/ 0,   /*size*/ 100, /*width*/ 256, /*height*/ 128 },
            MipDesc{ /*offset*/ 100, /*size*/ 40,  /*width*/ 128, /*height*/ 64  },
            MipDesc{ /*offset*/ 140, /*size*/ 16,  /*width*/ 64,  /*height*/ 32  },
        };
        desc.thumbWidth = 8;
        desc.thumbHeight = 4;
        return desc;
    }

    // ---- Independent raw encoder for the negative-path tests -------------------------------
    // Mirrors the CONTRACT (spec s4 / task brief), not the implementation: magic 'A','R','C','A',
    // artifactVersion (u32), then the desc's fixed fields in declaration order, THEN
    // sectionCount (u32) + {tag u32, offset u64, size u64} per section, then section bodies.

    void PutU8(std::vector<std::byte>& b, std::uint8_t v) { b.push_back(static_cast<std::byte>(v)); }

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

    void PutBytes(std::vector<std::byte>& b, const std::vector<std::byte>& data)
    {
        b.insert(b.end(), data.begin(), data.end());
    }

    // Builds a minimal-but-complete valid header (magic..thumbHeight) for a desc with NO mips,
    // matching the byte widths pinned by the interface: 1+16+8+4+1+1+4+4+4+4+1+4+4 = 56 desc
    // bytes, +4 magic +4 version = 64 header bytes total.
    std::vector<std::byte> EncodeHeader(std::uint32_t artifactVersion)
    {
        std::vector<std::byte> h;
        PutU8(h, static_cast<std::uint8_t>('A')); PutU8(h, static_cast<std::uint8_t>('R'));
        PutU8(h, static_cast<std::uint8_t>('C')); PutU8(h, static_cast<std::uint8_t>('A'));
        PutU32(h, artifactVersion);
        PutU8(h, static_cast<std::uint8_t>(ContentKind::Texture));
        PutU64(h, 0xAAAABBBBCCCCDDDDULL);   // sourceGuid.hi
        PutU64(h, 0x1111222233334444ULL);   // sourceGuid.lo
        PutU64(h, 0xDEADBEEFCAFEBABEULL);   // sourceHash
        PutU32(h, 1);                       // importerVersion
        PutU8(h, static_cast<std::uint8_t>(ArtifactPixelFormat::RGBA8));
        PutU8(h, static_cast<std::uint8_t>(ArtifactDimension::Tex2D));
        PutU32(h, 1);                       // arrayOrDepth
        PutU32(h, 4);                       // width
        PutU32(h, 4);                       // height
        PutU32(h, 0);                       // mipCount
        PutU8(h, 0);                        // srgb
        PutU32(h, 2);                       // thumbWidth
        PutU32(h, 1);                       // thumbHeight
        return h;
    }

    void WriteFile(const fs::path& path, const std::vector<std::byte>& bytes)
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        REQUIRE(ofs.good());
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(ofs.good());
    }

    std::vector<std::byte> ReadFile(const fs::path& path)
    {
        std::ifstream ifs(path, std::ios::binary);
        REQUIRE(ifs.good());
        ifs.seekg(0, std::ios::end);
        const auto len = static_cast<std::size_t>(ifs.tellg());
        ifs.seekg(0, std::ios::beg);
        std::vector<std::byte> out(len);
        if (len > 0)
            ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
        REQUIRE(ifs.good());
        return out;
    }
}

// ---- round trip -------------------------------------------------------------------------

TEST_CASE("pipeline: WriteTextureArtifact/ReadTextureArtifact round-trip every desc field", "[pipeline]")
{
    const fs::path dir = TempDir("roundtrip");
    const fs::path path = dir / "roundtrip.arcart";

    const TextureArtifactDesc desc = MakeDesc();
    const std::vector<std::byte> payload = PatternBytes(156, 0x10);   // matches the 3 mips' total (100+40+16)
    const std::vector<std::byte> thumb   = PatternBytes(8 * 4 * 4, 0x40);   // 8x4 RGBA8

    REQUIRE(WriteTextureArtifact(path, desc, payload, thumb));

    auto loaded = ReadTextureArtifact(path);
    REQUIRE(loaded.has_value());
    const TextureArtifactDesc& out = loaded->desc;

    CHECK(out.contentKind == ContentKind::Texture);
    CHECK(out.sourceGuid == desc.sourceGuid);
    CHECK(out.sourceHash == desc.sourceHash);
    CHECK(out.importerVersion == desc.importerVersion);
    CHECK(out.format == desc.format);
    CHECK(out.dimension == ArtifactDimension::Tex2D);
    CHECK(out.arrayOrDepth == 1u);
    CHECK(out.width == desc.width);
    CHECK(out.height == desc.height);
    CHECK(out.mipCount == desc.mipCount);
    CHECK(out.srgb == desc.srgb);
    CHECK(out.thumbWidth == desc.thumbWidth);
    CHECK(out.thumbHeight == desc.thumbHeight);

    REQUIRE(out.mips.size() == desc.mips.size());
    for (std::size_t i = 0; i < desc.mips.size(); ++i)
    {
        INFO("mip index " << i);
        CHECK(out.mips[i].offset == desc.mips[i].offset);
        CHECK(out.mips[i].size == desc.mips[i].size);
        CHECK(out.mips[i].width == desc.mips[i].width);
        CHECK(out.mips[i].height == desc.mips[i].height);
    }

    CHECK(loaded->payload == payload);
    CHECK(loaded->thumbRgba == thumb);
}

TEST_CASE("pipeline: srgb=false and an empty mip table round-trip too", "[pipeline]")
{
    const fs::path dir = TempDir("roundtrip_empty");
    const fs::path path = dir / "empty.arcart";

    TextureArtifactDesc desc = MakeDesc();
    desc.srgb = false;
    desc.mipCount = 0;
    desc.mips.clear();

    const std::vector<std::byte> payload{};   // legitimately empty -- no mips, no texel data
    const std::vector<std::byte> thumb = PatternBytes(4, 0x99);

    REQUIRE(WriteTextureArtifact(path, desc, payload, thumb));

    auto loaded = ReadTextureArtifact(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->desc.srgb == false);
    CHECK(loaded->desc.mips.empty());
    CHECK(loaded->payload.empty());
    CHECK(loaded->thumbRgba == thumb);
}

// ---- refusals -----------------------------------------------------------------------------

TEST_CASE("pipeline: ReadTextureArtifact refuses a file with the wrong magic", "[pipeline]")
{
    const fs::path dir = TempDir("bad_magic");
    const fs::path path = dir / "bad_magic.arcart";

    const TextureArtifactDesc desc = MakeDesc();
    const std::vector<std::byte> payload = PatternBytes(16, 1);
    const std::vector<std::byte> thumb = PatternBytes(4, 2);
    REQUIRE(WriteTextureArtifact(path, desc, payload, thumb));

    // Baseline: the untouched file parses -- proves the refusal below fires BECAUSE of the
    // corruption, not because reading is broken in general.
    REQUIRE(ReadTextureArtifact(path).has_value());

    std::vector<std::byte> bytes = ReadFile(path);
    REQUIRE(bytes.size() >= 4);
    bytes[0] = static_cast<std::byte>('X');   // 'ARCA' -> 'XRCA'
    WriteFile(path, bytes);

    REQUIRE_FALSE(ReadTextureArtifact(path).has_value());
}

TEST_CASE("pipeline: ReadTextureArtifact refuses artifactVersion + 1", "[pipeline]")
{
    const fs::path dir = TempDir("bad_version");
    const fs::path path = dir / "bad_version.arcart";

    const TextureArtifactDesc desc = MakeDesc();
    const std::vector<std::byte> payload = PatternBytes(16, 3);
    const std::vector<std::byte> thumb = PatternBytes(4, 4);
    REQUIRE(WriteTextureArtifact(path, desc, payload, thumb));
    REQUIRE(ReadTextureArtifact(path).has_value());   // baseline

    std::vector<std::byte> bytes = ReadFile(path);
    REQUIRE(bytes.size() >= 8);

    // artifactVersion is the little-endian u32 immediately after the 4-byte magic.
    std::uint32_t version = 0;
    for (int i = 0; i < 4; ++i)
        version |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[4 + i])) << (8 * i);
    ++version;
    for (int i = 0; i < 4; ++i)
        bytes[4 + i] = static_cast<std::byte>(static_cast<std::uint8_t>(version >> (8 * i)));

    WriteFile(path, bytes);

    REQUIRE_FALSE(ReadTextureArtifact(path).has_value());
}

TEST_CASE("pipeline: ReadTextureArtifact refuses a file truncated mid-payload", "[pipeline]")
{
    const fs::path dir = TempDir("truncated");
    const fs::path path = dir / "truncated.arcart";

    TextureArtifactDesc desc = MakeDesc();
    desc.mipCount = 0;
    desc.mips.clear();

    // Payload is the section written immediately before Thumbnail (MipTable, Payload,
    // Thumbnail order per the spec) -- cutting off the whole Thumbnail plus a few more bytes
    // is guaranteed to land inside Payload, regardless of the exact header/table byte widths.
    const std::vector<std::byte> payload = PatternBytes(64, 5);
    const std::vector<std::byte> thumb = PatternBytes(8, 6);
    REQUIRE(WriteTextureArtifact(path, desc, payload, thumb));
    REQUIRE(ReadTextureArtifact(path).has_value());   // baseline

    const auto fullSize = fs::file_size(path);
    REQUIRE(fullSize > thumb.size() + 10);
    std::error_code ec;
    fs::resize_file(path, fullSize - thumb.size() - 10, ec);
    REQUIRE_FALSE(ec);

    REQUIRE_FALSE(ReadTextureArtifact(path).has_value());
}

// ---- forward-compat ------------------------------------------------------------------------

TEST_CASE("pipeline: an unrecognised section tag is skipped, not a refusal", "[pipeline]")
{
    const fs::path dir = TempDir("unknown_section");
    const fs::path path = dir / "unknown_section.arcart";

    const std::vector<std::byte> mipTableBody = { std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0} };   // count=0
    const std::vector<std::byte> payloadBody = PatternBytes(8, 0x70);
    const std::vector<std::byte> thumbBody = PatternBytes(4, 0x80);
    const std::vector<std::byte> unknownBody = PatternBytes(6, 0x90);   // a FUTURE section this reader has never heard of

    std::vector<std::byte> file = EncodeHeader(/*artifactVersion*/ 1);

    constexpr std::uint32_t kSectionCount = 4;
    const std::uint64_t headerSize = file.size();               // 64
    const std::uint64_t tableSize = 4 + kSectionCount * 20ULL;   // count + 4 * {tag,offset,size}
    std::uint64_t offset = headerSize + tableSize;

    const std::uint64_t mipTableOffset = offset; offset += mipTableBody.size();
    const std::uint64_t payloadOffset  = offset; offset += payloadBody.size();
    const std::uint64_t thumbOffset    = offset; offset += thumbBody.size();
    const std::uint64_t unknownOffset  = offset; offset += unknownBody.size();

    PutU32(file, kSectionCount);
    PutU32(file, static_cast<std::uint32_t>(SectionTag::MipTable));  PutU64(file, mipTableOffset); PutU64(file, mipTableBody.size());
    PutU32(file, static_cast<std::uint32_t>(SectionTag::Payload));   PutU64(file, payloadOffset);  PutU64(file, payloadBody.size());
    PutU32(file, static_cast<std::uint32_t>(SectionTag::Thumbnail)); PutU64(file, thumbOffset);    PutU64(file, thumbBody.size());
    PutU32(file, 0xFEEDFACEu);   /* unknown tag */                   PutU64(file, unknownOffset);  PutU64(file, unknownBody.size());

    REQUIRE(file.size() == headerSize + tableSize);

    PutBytes(file, mipTableBody);
    PutBytes(file, payloadBody);
    PutBytes(file, thumbBody);
    PutBytes(file, unknownBody);

    WriteFile(path, file);

    auto loaded = ReadTextureArtifact(path);
    REQUIRE(loaded.has_value());   // the unknown tag must NOT cause a refusal
    CHECK(loaded->desc.mips.empty());
    CHECK(loaded->payload == payloadBody);
    CHECK(loaded->thumbRgba == thumbBody);
}
