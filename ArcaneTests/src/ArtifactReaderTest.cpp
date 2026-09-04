// F2b Task 6: the runtime artifact route -- Arcane::ArtifactReader (ArcaneClient-local, the
// byte-contract PEER of Task 1's Arcane::AssetPipeline::ArtifactFormat writer; ArcaneClient
// itself never links/includes ArcaneAssetPipeline -- see ArtifactReader.hpp's own header
// banner). This suite is the CROSS-LIB byte-contract test: every fixture is written through the
// PIPELINE's real WriteTextureArtifact (via TextureImporter::ImportTexture, the real production
// cook path) and read back through THIS reader, so a drift between the two independent
// implementations of the SAME on-disk format shows up here, not at Task 7's consumer.
//
// Pins: the round trip (header + mips + payload + thumbnail, byte-exact); the three refusal
// states each watched firing on their own (Missing/HashMismatch/VersionNewerThanEngine); the
// subsumption clause (an artifact OLDER than the engine collapses into Missing, NOT a fourth
// refusal kind); and FindArtifactForGuid's directory-scan resolution over a real
// Arcane::AssetPipeline::ArtifactStore-shaped tree.
//
// Tagged [artifact] rather than [pipeline] (the pipeline lib's own tag, e.g.
// AssetPipelineFormatTest.cpp) -- this file is squarely an ArcaneClient-side concern (the
// reader half of the contract), so it earns its own tag even though it links the pipeline lib
// to build its fixtures. Swept by both `"~[gpu]"` (the required full-suite run) and any future
// `"[artifact]"`-scoped run.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ArtifactReader.hpp>

#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/ArtifactStore.hpp>
#include <Arcane/AssetPipeline/CookKey.hpp>
#include <Arcane/AssetPipeline/TextureImporter.hpp>
#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>
#include <Arcane/Guid.hpp>

#include <stb_image_write.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using Arcane::Guid;

namespace
{
    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_artifact_reader_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    void CollectPngBytes(void* ctx, void* data, int size)
    {
        auto* out = static_cast<std::vector<std::byte>*>(ctx);
        const auto* p = static_cast<std::byte*>(data);
        out->insert(out->end(), p, p + size);
    }

    // Encodes an in-memory RGBA8 buffer to PNG bytes -- same in-memory fixture mechanism
    // AssetPipelineImporterTest.cpp uses (stbi_write_png_to_func), so no binary .png files are
    // committed to the repo.
    std::vector<std::byte> EncodePng(int width, int height, const std::vector<unsigned char>& rgba)
    {
        std::vector<std::byte> out;
        REQUIRE(stbi_write_png_to_func(&CollectPngBytes, &out, width, height, 4, rgba.data(), width * 4) != 0);
        return out;
    }

    std::vector<unsigned char> GradientPixels(int width, int height)
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(width) * height * 4);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                unsigned char* p = px.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                p[0] = static_cast<unsigned char>((x * 255) / (width > 1 ? width - 1 : 1));
                p[1] = static_cast<unsigned char>((y * 255) / (height > 1 ? height - 1 : 1));
                p[2] = 128;
                p[3] = 255;
            }
        }
        return px;
    }

    // The real production cook path (TextureImporter::ImportTexture + WriteTextureArtifact),
    // exactly what arccook drives -- so a fixture written here is byte-identical in shape to a
    // real staged .arcart. `settings` defaults to Rgba8 (no BC7 compression) so payload bytes
    // stay directly inspectable without a block decoder.
    struct Fixture
    {
        std::vector<std::byte> sourceBytes;   // the "current staged source" ReadClientArtifact hashes
        Arcane::AssetPipeline::ImportedTexture imported;
        fs::path artifactPath;
    };

    // `settings` defaults to Rgba8 (no BC7 compression) so most tests' payload bytes stay
    // directly inspectable without a block decoder; a caller that wants BC7 (the format-mirror
    // pin below) passes its own settings with format explicitly set.
    Arcane::AssetPipeline::TextureMetaSettings DefaultRgba8Settings()
    {
        Arcane::AssetPipeline::TextureMetaSettings settings;
        settings.format = Arcane::AssetPipeline::TextureMetaSettings::Format::Rgba8;
        return settings;
    }

    Fixture CookFixture(const fs::path& artifactPath, const Guid& sourceGuid, int w, int h,
                        Arcane::AssetPipeline::TextureMetaSettings settings = DefaultRgba8Settings())
    {
        Fixture fx;
        fx.sourceBytes = EncodePng(w, h, GradientPixels(w, h));

        auto imported = Arcane::AssetPipeline::ImportTexture(fx.sourceBytes, sourceGuid, settings);
        REQUIRE(imported.has_value());
        fx.imported = std::move(*imported);
        fx.artifactPath = artifactPath;

        REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(
            artifactPath, fx.imported.desc, fx.imported.payload, fx.imported.thumbRgba));
        return fx;
    }
}

// ---- round trip (the cross-lib byte contract) ----------------------------------------------

TEST_CASE("artifact: ReadClientArtifact round-trips a real cooked artifact byte-exactly", "[artifact]")
{
    const fs::path dir = TempDir("roundtrip");
    const Guid guid = Guid::Generate();
    const Fixture fx = CookFixture(dir / "a.arcart", guid, 17, 9);   // NPOT, multi-mip

    const auto result = Arcane::ReadClientArtifact(fx.artifactPath, fx.sourceBytes, guid);
    REQUIRE(result.refusal == Arcane::ArtifactRefusal::None);
    REQUIRE(result.artifact.has_value());

    const Arcane::LoadedClientArtifact& art = *result.artifact;
    CHECK(art.info.width == fx.imported.desc.width);
    CHECK(art.info.height == fx.imported.desc.height);
    CHECK(art.info.mipCount == fx.imported.desc.mipCount);
    CHECK(art.info.srgb == fx.imported.desc.srgb);
    CHECK(art.format == Arcane::ArtifactPixelFormatValue::RGBA8);   // settings.format = Rgba8 above
    CHECK(art.thumbWidth == fx.imported.desc.thumbWidth);
    CHECK(art.thumbHeight == fx.imported.desc.thumbHeight);

    REQUIRE(art.mips.size() == fx.imported.desc.mips.size());
    for (std::size_t i = 0; i < art.mips.size(); ++i)
    {
        INFO("mip " << i);
        CHECK(art.mips[i].offset == fx.imported.desc.mips[i].offset);
        CHECK(art.mips[i].size == fx.imported.desc.mips[i].size);
        CHECK(art.mips[i].width == fx.imported.desc.mips[i].width);
        CHECK(art.mips[i].height == fx.imported.desc.mips[i].height);
    }

    CHECK(art.payload == fx.imported.payload);
    CHECK(art.thumbRgba == fx.imported.thumbRgba);
}

TEST_CASE("artifact: ArtifactPixelFormatValue mirrors the pipeline's BC7 byte too", "[artifact]")
{
    const fs::path dir = TempDir("bc7_format");
    const Guid guid = Guid::Generate();

    Arcane::AssetPipeline::TextureMetaSettings settings;
    settings.format = Arcane::AssetPipeline::TextureMetaSettings::Format::Bc7;
    const Fixture fx = CookFixture(dir / "a.arcart", guid, 8, 8, settings);
    REQUIRE(fx.imported.desc.format == Arcane::AssetPipeline::ArtifactPixelFormat::BC7);

    const auto result = Arcane::ReadClientArtifact(fx.artifactPath, fx.sourceBytes, guid);
    REQUIRE(result.refusal == Arcane::ArtifactRefusal::None);
    CHECK(result.artifact->format == Arcane::ArtifactPixelFormatValue::BC7);
}

// ---- refusals, each watched firing on its own ------------------------------------------------

TEST_CASE("artifact: ReadClientArtifact reports Missing for a path that does not exist", "[artifact]")
{
    const fs::path dir = TempDir("missing_no_file");
    const auto result = Arcane::ReadClientArtifact(dir / "does-not-exist.arcart", {}, Guid::Generate());
    CHECK(result.refusal == Arcane::ArtifactRefusal::Missing);
    CHECK_FALSE(result.artifact.has_value());
}

TEST_CASE("artifact: ReadClientArtifact reports Missing when the header sourceGuid does not match",
          "[artifact]")
{
    const fs::path dir = TempDir("missing_wrong_guid");
    const Guid cookedFor = Guid::Generate();
    const Fixture fx = CookFixture(dir / "a.arcart", cookedFor, 4, 4);

    const Guid someoneElse = Guid::Generate();
    const auto result = Arcane::ReadClientArtifact(fx.artifactPath, fx.sourceBytes, someoneElse);
    CHECK(result.refusal == Arcane::ArtifactRefusal::Missing);
}

TEST_CASE("artifact: ReadClientArtifact refuses HashMismatch when the staged source changed",
          "[artifact]")
{
    const fs::path dir = TempDir("hash_mismatch");
    const Guid guid = Guid::Generate();
    const Fixture fx = CookFixture(dir / "a.arcart", guid, 6, 6);

    // Baseline: the SAME bytes the artifact was cooked from still validate.
    REQUIRE(Arcane::ReadClientArtifact(fx.artifactPath, fx.sourceBytes, guid).refusal
            == Arcane::ArtifactRefusal::None);

    // The scratch scenario: a source edited WITHOUT recooking -- different bytes, same
    // artifact on disk (the exact shape Step 3's watched ArcaneRuntime refusal exercises).
    std::vector<std::byte> editedSource = fx.sourceBytes;
    REQUIRE(!editedSource.empty());
    editedSource[0] = static_cast<std::byte>(static_cast<unsigned char>(editedSource[0]) ^ 0xFF);

    const auto result = Arcane::ReadClientArtifact(fx.artifactPath, editedSource, guid);
    CHECK(result.refusal == Arcane::ArtifactRefusal::HashMismatch);
    CHECK_FALSE(result.artifact.has_value());
}

TEST_CASE("artifact: ReadClientArtifact refuses VersionNewerThanEngine", "[artifact]")
{
    const fs::path dir = TempDir("version_newer");
    const Guid guid = Guid::Generate();
    Fixture fx = CookFixture(dir / "a.arcart", guid, 4, 4);

    // Re-write the SAME artifact with importerVersion bumped past this engine's own mirror --
    // simulating a cook produced by a NEWER importer than this build knows how to read.
    Arcane::AssetPipeline::TextureArtifactDesc bumped = fx.imported.desc;
    bumped.importerVersion = Arcane::kClientTextureImporterVersionMirror + 1;
    REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(fx.artifactPath, bumped, fx.imported.payload,
                                                        fx.imported.thumbRgba));

    const auto result = Arcane::ReadClientArtifact(fx.artifactPath, fx.sourceBytes, guid);
    CHECK(result.refusal == Arcane::ArtifactRefusal::VersionNewerThanEngine);
    CHECK_FALSE(result.artifact.has_value());
}

TEST_CASE("artifact: an artifact OLDER than the engine is a key miss (Missing), not a refusal",
          "[artifact]")
{
    // The subsumption clause (spec s5, restated in ArtifactReader.hpp's own header banner):
    // importerVersion < the engine's mirror collapses into Missing. In production this
    // situation is actually unreachable at the CURRENT cook key (CookSession's key already
    // folds in importerVersion, so an old-importer artifact sits at a DIFFERENT key and this
    // guid's CURRENT key would simply have no artifact) -- this case proves the READER's own
    // rule directly, independent of that key-derivation argument.
    const fs::path dir = TempDir("version_older");
    const Guid guid = Guid::Generate();
    Fixture fx = CookFixture(dir / "a.arcart", guid, 4, 4);

    REQUIRE(Arcane::kClientTextureImporterVersionMirror >= 1);
    Arcane::AssetPipeline::TextureArtifactDesc older = fx.imported.desc;
    older.importerVersion = Arcane::kClientTextureImporterVersionMirror - 1;
    REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(fx.artifactPath, older, fx.imported.payload,
                                                        fx.imported.thumbRgba));

    const auto result = Arcane::ReadClientArtifact(fx.artifactPath, fx.sourceBytes, guid);
    CHECK(result.refusal == Arcane::ArtifactRefusal::Missing);
}

// ---- FindArtifactForGuid: the directory-scan resolution --------------------------------------

TEST_CASE("artifact: FindArtifactForGuid resolves the right file among several, over a real "
          "ArtifactStore-shaped tree", "[artifact]")
{
    const fs::path dir = TempDir("find_guid");
    const fs::path intermediateDir = dir / "Intermediate";

    Arcane::AssetPipeline::ArtifactStore store(intermediateDir);

    const Guid targetGuid = Guid::Generate();
    std::vector<Guid> otherGuids{ Guid::Generate(), Guid::Generate(), Guid::Generate() };

    // Commit several artifacts through the REAL store (hash-flat 256-way shard, exactly the
    // layout arccook produces) so this test proves the scan over that real layout, not a
    // hand-picked directory shape.
    auto commitOne = [&](const Guid& g, int w, int h)
    {
        Arcane::AssetPipeline::TextureMetaSettings settings;
        const std::vector<std::byte> src = EncodePng(w, h, GradientPixels(w, h));
        auto imported = Arcane::AssetPipeline::ImportTexture(src, g, settings);
        REQUIRE(imported.has_value());
        const std::uint64_t key = Arcane::AssetPipeline::ComputeCookKey(
            src, settings, Arcane::AssetPipeline::kTextureImporterVersion);
        REQUIRE(store.Commit(key, [&](const fs::path& tmp)
        {
            return Arcane::AssetPipeline::WriteTextureArtifact(tmp, imported->desc, imported->payload,
                                                               imported->thumbRgba);
        }));
    };

    commitOne(targetGuid, 5, 5);
    for (std::size_t i = 0; i < otherGuids.size(); ++i)
        commitOne(otherGuids[i], 6 + (int)i, 6 + (int)i);

    const auto found = Arcane::FindArtifactForGuid(intermediateDir, targetGuid);
    REQUIRE(found.has_value());
    CHECK(fs::exists(*found));

    // Round-trips back to the SAME guid through the reader itself -- proving the scan did not
    // just find A file, it found the RIGHT one.
    const std::vector<std::byte> targetSource = EncodePng(5, 5, GradientPixels(5, 5));
    const auto result = Arcane::ReadClientArtifact(*found, targetSource, targetGuid);
    CHECK(result.refusal == Arcane::ArtifactRefusal::None);

    const auto notFound = Arcane::FindArtifactForGuid(intermediateDir, Guid::Generate());
    CHECK_FALSE(notFound.has_value());
}

TEST_CASE("artifact: FindArtifactForGuid returns nullopt when the store has no Artifacts/ dir yet",
          "[artifact]")
{
    const fs::path dir = TempDir("find_guid_no_store");
    const auto found = Arcane::FindArtifactForGuid(dir / "Intermediate", Guid::Generate());
    CHECK_FALSE(found.has_value());
}

// ---- REVIEW FIX (post-Task-7): ReadHeaderOnly must never pay for anything past the header ----
//
// FindArtifactForGuid's directory scan calls ReadHeaderOnly once per candidate .arcart file.
// Before this fix, ReadHeaderOnly called ReadWholeFile -- reading and copying the candidate's
// ENTIRE contents (payload, thumbnail, everything) -- and then parsed only the fixed 64-byte
// header out of it. ArtifactReader.hpp's own header banner claimed the opposite ("reading only
// each candidate's FIXED HEADER... cheap"), which was false against the code underneath it. The
// two cases below prove the FIXED behaviour is correct regardless of what -- if anything -- sits
// after the header: an enormous trailing region the fix must not touch, and a truncated file with
// no trailing region at all.

TEST_CASE("artifact: FindArtifactForGuid resolves a candidate whose header is followed by an "
          "enormous payload region",
          "[artifact]")
{
    const fs::path dir = TempDir("header_only_enormous");
    const fs::path intermediateDir = dir / "Intermediate";
    const fs::path artifactsDir = intermediateDir / "Artifacts";
    fs::create_directories(artifactsDir);

    const Guid guid = Guid::Generate();
    const fs::path artifactPath = artifactsDir / "a.arcart";
    const Fixture fx = CookFixture(artifactPath, guid, 4, 4);

    // Pad the artifact file with 20 MiB of real trailing bytes, appended AFTER its own
    // legitimate section table/payload/thumbnail -- never addressed by any section table
    // entry, so a CORRECT parse (full or header-only) ignores it either way. The point of
    // padding this large is what a pre-fix, whole-file ReadHeaderOnly would have paid for
    // on EVERY directory-scan candidate; ReadFilePrefix reads a fixed ~256-byte prefix
    // regardless of what follows it, which is the property this case exercises.
    {
        std::ofstream ofs(artifactPath, std::ios::binary | std::ios::app);
        REQUIRE(ofs);
        constexpr std::size_t kPadding = 20ull * 1024 * 1024;
        const std::vector<char> zeros(kPadding, '\0');
        ofs.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        REQUIRE(ofs);
    }
    REQUIRE(fs::file_size(artifactPath) > 20ull * 1024 * 1024);

    const auto found = Arcane::FindArtifactForGuid(intermediateDir, guid);
    REQUIRE(found.has_value());
    CHECK(*found == artifactPath);

    // The full read (ReadClientArtifact, UNCHANGED by this fix) still succeeds too -- the
    // padding sits past every section the table names, so a correct parse ignores it
    // exactly as it always did.
    const auto result = Arcane::ReadClientArtifact(*found, fx.sourceBytes, guid);
    CHECK(result.refusal == Arcane::ArtifactRefusal::None);
}

TEST_CASE("artifact: FindArtifactForGuid resolves a candidate truncated to just its fixed "
          "64-byte header -- no section table, no payload, no thumbnail",
          "[artifact]")
{
    const fs::path dir = TempDir("header_only_truncated");
    const fs::path intermediateDir = dir / "Intermediate";
    const fs::path artifactsDir = intermediateDir / "Artifacts";
    fs::create_directories(artifactsDir);

    const Guid guid = Guid::Generate();
    const fs::path artifactPath = artifactsDir / "a.arcart";
    const Fixture fx = CookFixture(artifactPath, guid, 4, 4);

    // Truncate to EXACTLY the fixed header's own width (64 bytes: magic 4 + version 4 +
    // contentKind 1 + sourceGuid 16 + sourceHash 8 + importerVersion 4 + format 1 +
    // dimension 1 + arrayOrDepth 4 + width 4 + height 4 + mipCount 4 + srgb 1 +
    // thumbWidth 4 + thumbHeight 4) -- nothing of the section table, payload or thumbnail
    // survives. The header-only probe must still succeed: it never reads past the header
    // regardless of what (if anything) follows it on disk.
    {
        std::error_code ec;
        fs::resize_file(artifactPath, 64, ec);
        REQUIRE_FALSE(ec);
    }
    REQUIRE(fs::file_size(artifactPath) == 64);

    const auto found = Arcane::FindArtifactForGuid(intermediateDir, guid);
    REQUIRE(found.has_value());
    CHECK(*found == artifactPath);

    // The FULL read, by contrast, legitimately refuses this file -- it has no section table
    // at all (sectionCount cannot even be read), which is exactly the "unparseable" shape
    // ReadClientArtifact's own Missing refusal already covers. Stated here to draw the
    // line: the header-only probe's tolerance for a missing tail is scoped to itself, not a
    // general loosening of the format's rules.
    const auto result = Arcane::ReadClientArtifact(artifactPath, fx.sourceBytes, guid);
    CHECK(result.refusal == Arcane::ArtifactRefusal::Missing);
}
