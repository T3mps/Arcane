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
    REQUIRE(found.size() == 1u);
    CHECK(fs::exists(found.front()));

    // Round-trips back to the SAME guid through the reader itself -- proving the scan did not
    // just find A file, it found the RIGHT one.
    const std::vector<std::byte> targetSource = EncodePng(5, 5, GradientPixels(5, 5));
    const auto result = Arcane::ReadClientArtifact(found.front(), targetSource, targetGuid);
    CHECK(result.refusal == Arcane::ArtifactRefusal::None);

    const auto notFound = Arcane::FindArtifactForGuid(intermediateDir, Guid::Generate());
    CHECK(notFound.empty());
}

TEST_CASE("artifact: FindArtifactForGuid returns empty when the store has no Artifacts/ dir yet",
          "[artifact]")
{
    const fs::path dir = TempDir("find_guid_no_store");
    const auto found = Arcane::FindArtifactForGuid(dir / "Intermediate", Guid::Generate());
    CHECK(found.empty());
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
    REQUIRE(found.size() == 1u);
    CHECK(found.front() == artifactPath);

    // The full read (ReadClientArtifact, UNCHANGED by this fix) still succeeds too -- the
    // padding sits past every section the table names, so a correct parse ignores it
    // exactly as it always did.
    const auto result = Arcane::ReadClientArtifact(found.front(), fx.sourceBytes, guid);
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
    REQUIRE(found.size() == 1u);
    CHECK(found.front() == artifactPath);

    // The FULL read, by contrast, legitimately refuses this file -- it has no section table
    // at all (sectionCount cannot even be read), which is exactly the "unparseable" shape
    // ReadClientArtifact's own Missing refusal already covers. Stated here to draw the
    // line: the header-only probe's tolerance for a missing tail is scoped to itself, not a
    // general loosening of the format's rules.
    const auto result = Arcane::ReadClientArtifact(artifactPath, fx.sourceBytes, guid);
    CHECK(result.refusal == Arcane::ArtifactRefusal::Missing);
}

// ---- C1(b) fix (final-review wave, 2026-09-04): multi-candidate resolution -----------------
//
// The defect: FindArtifactForGuid used to return the FIRST guid match in unspecified
// directory-scan order. A same-guid recook under a new cook key can leave the SUPERSEDED
// old-key artifact still on disk (CookSession's own C1a self-heal is best-effort), and BOTH
// the stale and the fresh artifact carry the SAME sourceGuid header -- so a first-match
// resolve could pick the stale one and refuse HashMismatch forever, sticky, even though a
// valid artifact sits right next to it. The fix: FindArtifactForGuid now returns EVERY
// guid-matching candidate; the caller (mirrored below -- Assets.cpp's ResolveArtifact is
// the real, private production copy of this exact loop) validates each and accepts the
// first that reads back clean.
//
// APPROACH TO DIRECTORY-SCAN ORDER (stated per the finding's own request):
// std::filesystem::recursive_directory_iterator carries NO ordering guarantee, so this test
// does not gamble on the real platform's behaviour. It constructs BOTH physical orderings
// explicitly, each in its own SECTION, by naming the stale artifact's 256-way shard
// directory to sort lexicographically before the fresh one's in one SECTION and after it in
// the other -- so regardless of which real order this platform's std::filesystem actually
// visits directories in, at least one (and typically both, empirically, on this platform's
// NTFS/FindFirstFile-backed iteration) SECTION exercises "the stale candidate is visited
// before the fresh one", the exact case the pre-fix code always lost to. Both SECTIONs must
// resolve to the SAME correct (fresh) artifact.

TEST_CASE("artifact: the client resolves the FRESH artifact even with a stale same-guid "
          "artifact still physically on disk, in EITHER directory-scan order (C1b)", "[artifact]")
{
    const Guid guid = Guid::Generate();

    const std::vector<std::byte> currentSource = EncodePng(6, 6, GradientPixels(6, 6));
    const auto freshImported = Arcane::AssetPipeline::ImportTexture(currentSource, guid, DefaultRgba8Settings());
    REQUIRE(freshImported.has_value());

    // Deliberately a DIFFERENT size (and therefore different pixel bytes and a different
    // sourceHash) -- exactly what a genuine source edit produces, never a byte-identical
    // artifact that would "validate" by coincidence.
    const std::vector<std::byte> oldSource = EncodePng(3, 3, GradientPixels(3, 3));
    const auto staleImported = Arcane::AssetPipeline::ImportTexture(oldSource, guid, DefaultRgba8Settings());
    REQUIRE(staleImported.has_value());
    REQUIRE(freshImported->desc.sourceHash != staleImported->desc.sourceHash);

    // Mirrors Assets.cpp's ResolveArtifact loop exactly: iterate every FindArtifactForGuid
    // candidate, accept the first that validates clean against the CURRENT source bytes.
    // ResolveArtifact itself is a private, unexported implementation detail of Assets.cpp
    // (not reachable from here), so this reimplements its exact algorithm at the level this
    // suite already tests (ArtifactReader's own public surface) -- AssetsPixelsTest.cpp
    // carries the companion end-to-end proof through the REAL public Assets facade.
    auto resolveLikeAssetsCpp = [&](const std::vector<fs::path>& candidates) -> Arcane::ArtifactReadResult
    {
        Arcane::ArtifactReadResult best;
        best.refusal = Arcane::ArtifactRefusal::Missing;
        for (const fs::path& candidate : candidates)
        {
            Arcane::ArtifactReadResult r = Arcane::ReadClientArtifact(candidate, currentSource, guid);
            if (r.refusal == Arcane::ArtifactRefusal::None)
                return r;
            if (best.refusal == Arcane::ArtifactRefusal::Missing && r.refusal != Arcane::ArtifactRefusal::Missing)
                best.refusal = r.refusal;
        }
        return best;
    };

    SECTION("stale artifact's shard sorts BEFORE the fresh one's")
    {
        const fs::path dir = TempDir("multi_candidate_stale_first");
        const fs::path intermediateDir = dir / "Intermediate";
        fs::create_directories(intermediateDir / "Artifacts" / "0_stale_shard");
        fs::create_directories(intermediateDir / "Artifacts" / "9_fresh_shard");
        REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(
            intermediateDir / "Artifacts" / "0_stale_shard" / "stale.arcart",
            staleImported->desc, staleImported->payload, staleImported->thumbRgba));
        REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(
            intermediateDir / "Artifacts" / "9_fresh_shard" / "fresh.arcart",
            freshImported->desc, freshImported->payload, freshImported->thumbRgba));

        const auto candidates = Arcane::FindArtifactForGuid(intermediateDir, guid);
        REQUIRE(candidates.size() == 2u);   // both physically present -- the scan finds BOTH

        const Arcane::ArtifactReadResult resolved = resolveLikeAssetsCpp(candidates);
        REQUIRE(resolved.refusal == Arcane::ArtifactRefusal::None);
        REQUIRE(resolved.artifact.has_value());
        CHECK(resolved.artifact->info.width == freshImported->desc.width);
        // The stale file is still on disk -- proves resolution works DESPITE it.
        CHECK(fs::exists(intermediateDir / "Artifacts" / "0_stale_shard" / "stale.arcart"));
    }

    SECTION("stale artifact's shard sorts AFTER the fresh one's")
    {
        const fs::path dir = TempDir("multi_candidate_fresh_first");
        const fs::path intermediateDir = dir / "Intermediate";
        fs::create_directories(intermediateDir / "Artifacts" / "0_fresh_shard");
        fs::create_directories(intermediateDir / "Artifacts" / "9_stale_shard");
        REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(
            intermediateDir / "Artifacts" / "0_fresh_shard" / "fresh.arcart",
            freshImported->desc, freshImported->payload, freshImported->thumbRgba));
        REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(
            intermediateDir / "Artifacts" / "9_stale_shard" / "stale.arcart",
            staleImported->desc, staleImported->payload, staleImported->thumbRgba));

        const auto candidates = Arcane::FindArtifactForGuid(intermediateDir, guid);
        REQUIRE(candidates.size() == 2u);

        const Arcane::ArtifactReadResult resolved = resolveLikeAssetsCpp(candidates);
        REQUIRE(resolved.refusal == Arcane::ArtifactRefusal::None);
        REQUIRE(resolved.artifact.has_value());
        CHECK(resolved.artifact->info.width == freshImported->desc.width);
    }
}

TEST_CASE("artifact: candidates that ALL fail report the most informative refusal, "
          "HashMismatch over a bare Missing (C1b)", "[artifact]")
{
    // Neither candidate validates against the CURRENT source -- both are stale relative to
    // it. The pin: the caller's loop (mirrored here) must not report "Missing" just because
    // it iterated more than one candidate; it reports the more informative HashMismatch.
    const Guid guid = Guid::Generate();
    const std::vector<std::byte> currentSource = EncodePng(5, 5, GradientPixels(5, 5));

    const std::vector<std::byte> staleSourceA = EncodePng(2, 2, GradientPixels(2, 2));
    const auto importedA = Arcane::AssetPipeline::ImportTexture(staleSourceA, guid, DefaultRgba8Settings());
    REQUIRE(importedA.has_value());
    const std::vector<std::byte> staleSourceB = EncodePng(3, 3, GradientPixels(3, 3));
    const auto importedB = Arcane::AssetPipeline::ImportTexture(staleSourceB, guid, DefaultRgba8Settings());
    REQUIRE(importedB.has_value());

    const fs::path dir = TempDir("multi_candidate_all_stale");
    const fs::path intermediateDir = dir / "Intermediate";
    fs::create_directories(intermediateDir / "Artifacts" / "aa");
    REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(
        intermediateDir / "Artifacts" / "aa" / "a.arcart", importedA->desc, importedA->payload, importedA->thumbRgba));
    REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(
        intermediateDir / "Artifacts" / "aa" / "b.arcart", importedB->desc, importedB->payload, importedB->thumbRgba));

    const auto candidates = Arcane::FindArtifactForGuid(intermediateDir, guid);
    REQUIRE(candidates.size() == 2u);

    Arcane::ArtifactReadResult best;
    best.refusal = Arcane::ArtifactRefusal::Missing;
    for (const fs::path& candidate : candidates)
    {
        Arcane::ArtifactReadResult r = Arcane::ReadClientArtifact(candidate, currentSource, guid);
        REQUIRE(r.refusal != Arcane::ArtifactRefusal::None);   // neither should validate
        if (best.refusal == Arcane::ArtifactRefusal::Missing && r.refusal != Arcane::ArtifactRefusal::Missing)
            best.refusal = r.refusal;
    }
    CHECK(best.refusal == Arcane::ArtifactRefusal::HashMismatch);
}

// ---- I3 fix (final-review wave, 2026-09-04): unbounded mipCount reserve --------------------

TEST_CASE("artifact: ReadClientArtifact refuses a crafted huge mipCount instead of throwing "
          "bad_alloc (I3)", "[artifact]")
{
    const fs::path dir = TempDir("huge_mipcount");
    const Guid guid = Guid::Generate();
    const Fixture fx = CookFixture(dir / "a.arcart", guid, 8, 8);   // >=1 real mip, RGBA8 default

    // Fixed layout for this shape (see ArtifactFormat.cpp's WriteTextureArtifact): a 64-byte
    // header, then a 64-byte section table for exactly 3 sections (MipTable, Payload,
    // Thumbnail -- always in that order, MipTable first, per the writer's fixed `sections`
    // array), so the MipTable section's body -- and its own leading mipCount u32 -- starts
    // at a KNOWN absolute file offset: 128.
    constexpr std::size_t kMipCountOffset = 128;

    std::vector<std::byte> bytes;
    {
        std::ifstream ifs(fx.artifactPath, std::ios::binary);
        REQUIRE(ifs.good());
        ifs.seekg(0, std::ios::end);
        const auto len = static_cast<std::size_t>(ifs.tellg());
        ifs.seekg(0, std::ios::beg);
        bytes.resize(len);
        if (len > 0)
            ifs.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(len));
        REQUIRE(ifs.good());
    }
    REQUIRE(bytes.size() >= kMipCountOffset + 4);

    // Sanity: these 4 bytes really are the mip count this fixture wrote -- proves the
    // hand-derived offset above is right BEFORE corrupting it.
    std::uint32_t existingMipCount = 0;
    for (int i = 0; i < 4; ++i)
        existingMipCount |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[kMipCountOffset + i])) << (8 * i);
    REQUIRE(existingMipCount == fx.imported.desc.mipCount);
    REQUIRE(existingMipCount >= 1u);

    for (int i = 0; i < 4; ++i)
        bytes[kMipCountOffset + i] = static_cast<std::byte>(0xFFu);   // mipCount = 0xFFFFFFFF

    {
        std::ofstream ofs(fx.artifactPath, std::ios::binary | std::ios::trunc);
        REQUIRE(ofs.good());
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(ofs.good());
    }

    // THE pin: must refuse cleanly (Missing), never throw std::bad_alloc trying to
    // std::vector::reserve(0xFFFFFFFF) mip entries that could not possibly fit in this tiny
    // file.
    Arcane::ArtifactReadResult result;
    REQUIRE_NOTHROW(result = Arcane::ReadClientArtifact(fx.artifactPath, fx.sourceBytes, guid));
    CHECK(result.refusal == Arcane::ArtifactRefusal::Missing);
    CHECK_FALSE(result.artifact.has_value());
}
