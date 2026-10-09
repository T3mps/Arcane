// Fuzz regressions for ReadClientArtifact's size contract (fuzz/artifact_fuzz.cpp,
// inputs in fuzz/regressions/artifact). The header's dims must agree with the bytes
// the file actually carries -- the thumbnail upload reads thumbWidth*thumbHeight*4
// bytes and the mip upload reads each mip's slicePitch, both trusting these fields --
// so a disagreeing artifact is refused (Missing) instead of handed to them.
//
// Self-contained: the artifacts are built byte-for-byte from ArtifactReader.hpp's
// documented layout here, with no ArcaneAssetPipeline writer involved, so the hand
// layout below IS the contract under test.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ArtifactReader.hpp>
#include <Arcane/Guid.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr Arcane::Guid kGuid{ 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull };
    constexpr std::uint64_t kEmptySourceHash = 14695981039346656037ULL;   // FNV-1a 64 of no bytes

    struct Bytes
    {
        std::vector<std::byte> b;
        void U8(std::uint8_t v) { b.push_back(static_cast<std::byte>(v)); }
        void U32(std::uint32_t v) { for (int i = 0; i < 4; ++i) U8(static_cast<std::uint8_t>(v >> (8 * i))); }
        void U64(std::uint64_t v) { for (int i = 0; i < 8; ++i) U8(static_cast<std::uint8_t>(v >> (8 * i))); }
        void Append(const std::vector<std::byte>& o) { b.insert(b.end(), o.begin(), o.end()); }
    };

    struct Mip { std::uint64_t offset, size; std::uint32_t width, height; };

    struct TextureSpec
    {
        std::uint8_t format = 0;   // RGBA8
        std::uint32_t declaredMipCount = 1;
        std::vector<Mip> mips;
        std::size_t payloadBytes = 0;
        std::uint32_t thumbWidth = 0, thumbHeight = 0;
        std::size_t thumbBytes = 0;
    };

    std::vector<std::byte> BuildTexture(const TextureSpec& s)
    {
        Bytes h;
        for (char c : std::string("ARCA")) h.U8(static_cast<std::uint8_t>(c));
        h.U32(1);                       // artifactVersion
        h.U8(1);                        // contentKind: Texture
        h.U64(kGuid.hi); h.U64(kGuid.lo);
        h.U64(kEmptySourceHash);
        h.U32(1);                       // importerVersion
        h.U8(s.format); h.U8(0);        // format, dimension
        h.U32(1);                       // arrayOrDepth
        h.U32(s.mips.empty() ? 1 : s.mips[0].width);
        h.U32(s.mips.empty() ? 1 : s.mips[0].height);
        h.U32(s.declaredMipCount);
        h.U8(1);                        // srgb
        h.U32(s.thumbWidth); h.U32(s.thumbHeight);

        Bytes mipTable;
        mipTable.U32(static_cast<std::uint32_t>(s.mips.size()));
        for (const Mip& m : s.mips) { mipTable.U64(m.offset); mipTable.U64(m.size); mipTable.U32(m.width); mipTable.U32(m.height); }
        const std::vector<std::byte> payload(s.payloadBytes, std::byte{ 0x5A });
        const std::vector<std::byte> thumb(s.thumbBytes, std::byte{ 0x7F });

        const std::vector<std::pair<std::uint32_t, const std::vector<std::byte>*>> sections{
            { 1, &mipTable.b }, { 2, &payload }, { 3, &thumb } };
        std::uint64_t at = h.b.size() + 4 + 20 * sections.size();
        h.U32(static_cast<std::uint32_t>(sections.size()));
        for (const auto& [tag, body] : sections) { h.U32(tag); h.U64(at); h.U64(body->size()); at += body->size(); }
        for (const auto& [tag, body] : sections) h.Append(*body);
        return h.b;
    }

    Arcane::ArtifactRefusal Read(const std::vector<std::byte>& file)
    {
        const std::filesystem::path p = std::filesystem::temp_directory_path() / "arcane-artifact-size-contract.arcart";
        {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        }
        const Arcane::ArtifactReadResult r = Arcane::ReadClientArtifact(p, std::span<const std::byte>{}, kGuid);
        std::error_code ec;
        std::filesystem::remove(p, ec);
        CHECK((r.refusal == Arcane::ArtifactRefusal::None) == r.artifact.has_value());
        return r.refusal;
    }

    TextureSpec ValidRgba8()
    {
        TextureSpec s;
        s.declaredMipCount = 2;
        s.mips = { { 0, 4 * 4 * 4, 4, 4 }, { 64, 2 * 2 * 4, 2, 2 } };
        s.payloadBytes = 64 + 16;
        s.thumbWidth = 4; s.thumbHeight = 4; s.thumbBytes = 64;
        return s;
    }
}

TEST_CASE("artifact size contract: a consistent texture is accepted", "[artifact][fuzz]")
{
    CHECK(Read(BuildTexture(ValidRgba8())) == Arcane::ArtifactRefusal::None);

    TextureSpec bc7;   // NPOT BC7: 5x3 -> 2x1 blocks of 16 bytes; 2x1 -> 1 block
    bc7.format = 1;
    bc7.declaredMipCount = 2;
    bc7.mips = { { 0, 32, 5, 3 }, { 32, 16, 2, 1 } };
    bc7.payloadBytes = 48;
    CHECK(Read(BuildTexture(bc7)) == Arcane::ArtifactRefusal::None);
}

TEST_CASE("artifact size contract: thumbnail dims larger than its bytes are refused", "[artifact][fuzz]")
{
    TextureSpec s = ValidRgba8();
    s.thumbWidth = 64; s.thumbHeight = 64;   // 16 KiB promised, 64 bytes present
    CHECK(Read(BuildTexture(s)) == Arcane::ArtifactRefusal::Missing);

    s.thumbWidth = 0; s.thumbHeight = 0;     // and the reverse: bytes with no dims
    CHECK(Read(BuildTexture(s)) == Arcane::ArtifactRefusal::Missing);
}

TEST_CASE("artifact size contract: a mip smaller than its dims imply is refused", "[artifact][fuzz]")
{
    TextureSpec s = ValidRgba8();
    s.mips[0] = { 0, 16, 256, 256 };   // 256 KiB promised, 16 bytes present
    CHECK(Read(BuildTexture(s)) == Arcane::ArtifactRefusal::Missing);
}

TEST_CASE("artifact size contract: a mip whose byte count wraps to zero is refused", "[artifact][fuzz]")
{
    TextureSpec s = ValidRgba8();
    s.mips[1] = { 80, 0, 1u << 31, 1u << 31 };   // 2^31 * 2^31 * 4 == 2^64 -> 0
    CHECK(Read(BuildTexture(s)) == Arcane::ArtifactRefusal::Missing);
}

TEST_CASE("artifact size contract: declared mipCount must match the mip table", "[artifact][fuzz]")
{
    TextureSpec s = ValidRgba8();
    s.declaredMipCount = 7;
    CHECK(Read(BuildTexture(s)) == Arcane::ArtifactRefusal::Missing);
}
