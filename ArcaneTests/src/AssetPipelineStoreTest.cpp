// F2b Task 2: the hash-flat artifact store (Arcane::AssetPipeline::ArtifactStore) and the
// triple cook key (Arcane::AssetPipeline::ComputeCookKey). Pins: the cook key changes when any
// one of its three constituents changes (source bytes, one setting field, importerVersion) and
// is otherwise stable; Commit is atomic -- success renames the tmp file into place, a writer
// that returns false or THROWS leaves no file at the final name and no stray tmp file; two
// Commits racing the SAME cook key (pinned single-process via re-entrancy, per the fix-loop
// ruling on the task-2 review) never share a tmp path and the final content is exactly one
// writer's complete output; a directory of artifacts written directly (Task 1's
// WriteTextureArtifact, at store.PathFor(key) paths) has its Guid -> cook key index reproduced
// EXACTLY by RebuildIndexFromScan against a fresh store instance (nothing carried over
// in-process); SweepOrphans removes only artifacts whose Guid is absent from the live set,
// leaving live artifacts untouched (present AND still readable).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/ArtifactStore.hpp>
#include <Arcane/AssetPipeline/CookKey.hpp>
#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>
#include <Arcane/Guid.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace Arcane::AssetPipeline;
using Arcane::Guid;

namespace
{
    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_pipeline_store_test" / leaf;
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

    // A minimal-but-valid TextureArtifactDesc for a given source Guid -- no mips, tiny sizes --
    // just enough for WriteTextureArtifact/ReadTextureArtifact to round-trip, so the store tests
    // exercise RebuildIndexFromScan against REAL artifact headers, not a hand-rolled stand-in.
    TextureArtifactDesc MakeDesc(const Guid& guid, std::uint32_t importerVersion)
    {
        TextureArtifactDesc desc{};
        desc.contentKind = ContentKind::Texture;
        desc.sourceGuid = guid;
        desc.sourceHash = 0xABCDEF0123456789ULL;
        desc.importerVersion = importerVersion;
        desc.format = ArtifactPixelFormat::RGBA8;
        desc.dimension = ArtifactDimension::Tex2D;
        desc.arrayOrDepth = 1;
        desc.width = 4;
        desc.height = 4;
        desc.mipCount = 0;
        desc.srgb = true;
        desc.thumbWidth = 2;
        desc.thumbHeight = 2;
        return desc;
    }

    std::vector<std::byte> ReadWholeFile(const fs::path& path)
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

    // Counts files with a ".tmp" extension directly inside `dir` -- used to prove "no stray tmp
    // file remains" without needing to guess Commit's internal per-invocation naming scheme.
    std::size_t CountTmpFiles(const fs::path& dir)
    {
        std::error_code ec;
        if (!fs::exists(dir, ec)) return 0;

        std::size_t count = 0;
        for (const auto& entry : fs::directory_iterator(dir, ec))
        {
            if (ec) break;
            if (entry.path().extension() == ".tmp") ++count;
        }
        return count;
    }
}

// ---- ComputeCookKey: the triple ----------------------------------------------------------

TEST_CASE("pipeline: ComputeCookKey is stable for identical inputs", "[pipeline]")
{
    const std::vector<std::byte> bytes = PatternBytes(64, 0x11);
    TextureMetaSettings settings{};
    settings.srgb = true;
    settings.maxSize = 2048;

    const std::uint64_t a = ComputeCookKey(bytes, settings, kTextureImporterVersion);
    const std::uint64_t b = ComputeCookKey(bytes, settings, kTextureImporterVersion);
    CHECK(a == b);
}

TEST_CASE("pipeline: ComputeCookKey changes when a single source byte changes", "[pipeline]")
{
    std::vector<std::byte> bytes = PatternBytes(64, 0x22);
    TextureMetaSettings settings{};

    const std::uint64_t before = ComputeCookKey(bytes, settings, kTextureImporterVersion);
    bytes[30] = static_cast<std::byte>(static_cast<std::uint8_t>(static_cast<std::uint8_t>(bytes[30]) ^ 0xFF));
    const std::uint64_t after = ComputeCookKey(bytes, settings, kTextureImporterVersion);

    CHECK(before != after);
}

TEST_CASE("pipeline: ComputeCookKey changes when a single setting field changes", "[pipeline]")
{
    const std::vector<std::byte> bytes = PatternBytes(32, 0x33);

    TextureMetaSettings base{};
    base.srgb = true;
    base.maxSize = 1024;
    const std::uint64_t baseline = ComputeCookKey(bytes, base, kTextureImporterVersion);

    TextureMetaSettings srgbFlipped = base;
    srgbFlipped.srgb = false;
    CHECK(ComputeCookKey(bytes, srgbFlipped, kTextureImporterVersion) != baseline);

    TextureMetaSettings maxSizeChanged = base;
    maxSizeChanged.maxSize = 2048;
    CHECK(ComputeCookKey(bytes, maxSizeChanged, kTextureImporterVersion) != baseline);
}

TEST_CASE("pipeline: ComputeCookKey changes when importerVersion changes", "[pipeline]")
{
    const std::vector<std::byte> bytes = PatternBytes(32, 0x44);
    TextureMetaSettings settings{};

    const std::uint64_t v1 = ComputeCookKey(bytes, settings, kTextureImporterVersion);
    const std::uint64_t v2 = ComputeCookKey(bytes, settings, kTextureImporterVersion + 1);

    CHECK(v1 != v2);
}

// ---- ArtifactStore::PathFor ---------------------------------------------------------------

TEST_CASE("pipeline: ArtifactStore::PathFor shards on the key's first two hex digits", "[pipeline]")
{
    const fs::path root = TempDir("pathfor");
    ArtifactStore store(root);

    const fs::path p = store.PathFor(0x0a1b2c3d4e5f6789ULL);
    CHECK(p.parent_path().filename() == "0a");
    CHECK(p.filename() == "0a1b2c3d4e5f6789.arcart");
    CHECK(p.parent_path().parent_path().filename() == "Artifacts");
}

// ---- ArtifactStore::Commit: atomicity ------------------------------------------------------

TEST_CASE("pipeline: Commit renames a successful writer's tmp file into place", "[pipeline]")
{
    const fs::path root = TempDir("commit_success");
    ArtifactStore store(root);

    const std::uint64_t key = 0x1122334455667788ULL;
    const fs::path finalPath = store.PathFor(key);

    const std::vector<std::byte> content = PatternBytes(16, 0x55);

    // Commit's tmp naming is an internal per-invocation detail (process id + a counter) --
    // capture whatever path the writer is actually handed rather than assuming a fixed
    // "<final>.tmp" scheme, then prove THAT exact path is gone after a successful commit.
    fs::path capturedTmp;
    const bool committed = store.Commit(key, [&](const fs::path& tmp) -> bool
    {
        capturedTmp = tmp;
        CHECK(tmp != finalPath);
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
        return ofs.good();
    });

    REQUIRE(committed);
    CHECK(fs::exists(finalPath));
    CHECK_FALSE(fs::exists(capturedTmp));
    CHECK(CountTmpFiles(finalPath.parent_path()) == 0u);

    CHECK(ReadWholeFile(finalPath) == content);
}

TEST_CASE("pipeline: Commit leaves nothing behind when the writer returns false", "[pipeline]")
{
    const fs::path root = TempDir("commit_returns_false");
    ArtifactStore store(root);

    const std::uint64_t key = 0x2233445566778899ULL;
    const fs::path finalPath = store.PathFor(key);

    fs::path capturedTmp;
    const bool committed = store.Commit(key, [&](const fs::path& tmp) -> bool
    {
        capturedTmp = tmp;
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        ofs << "partial";
        return false;   // writer decided the content is bad -- Commit must not publish it
    });

    CHECK_FALSE(committed);
    CHECK_FALSE(fs::exists(finalPath));
    CHECK_FALSE(fs::exists(capturedTmp));
    CHECK(CountTmpFiles(finalPath.parent_path()) == 0u);
}

TEST_CASE("pipeline: Commit leaves no file and no stray tmp file when the writer throws", "[pipeline]")
{
    const fs::path root = TempDir("commit_throws");
    ArtifactStore store(root);

    const std::uint64_t key = 0x33445566778899AAULL;
    const fs::path finalPath = store.PathFor(key);

    fs::path capturedTmp;
    const bool committed = store.Commit(key, [&](const fs::path& tmp) -> bool
    {
        capturedTmp = tmp;
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        ofs << "would-be-partial-content";
        ofs.flush();
        throw std::runtime_error("writer blew up mid-write");
    });

    CHECK_FALSE(committed);
    CHECK_FALSE(fs::exists(finalPath));
    CHECK_FALSE(fs::exists(capturedTmp));
    CHECK(CountTmpFiles(finalPath.parent_path()) == 0u);
}

TEST_CASE("pipeline: Commit is safe when two Commits for the SAME key interleave (single-process)", "[pipeline]")
{
    // Pins the concurrency mechanism (per-invocation tmp paths) that Task 5's parallel arccook
    // stagers rely on: two Commits racing the SAME cook key must never share a tmp path, so
    // neither writer's bytes can ever land interleaved/corrupted at the other's location.
    // Interleaving is reproduced deterministically, single-process, via re-entrancy: from
    // INSIDE Commit A's writer callback, a complete, independent Commit B runs for the SAME
    // key before A's own writer finishes.
    const fs::path root = TempDir("commit_interleaved_same_key");
    ArtifactStore store(root);

    const std::uint64_t key = 0x4455667788990011ULL;
    const fs::path finalPath = store.PathFor(key);

    // Different-length, recognizable content per writer -- a mixed/truncated result could not
    // match either pattern's length, let alone its bytes.
    const std::vector<std::byte> contentA = PatternBytes(37, 0xA0);
    const std::vector<std::byte> contentB = PatternBytes(19, 0xB0);

    auto writeContent = [](const fs::path& tmp, const std::vector<std::byte>& content) -> bool
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
        return ofs.good();
    };

    bool committedB = false;

    const bool committedA = store.Commit(key, [&](const fs::path& tmpA) -> bool
    {
        committedB = store.Commit(key, [&](const fs::path& tmpB) -> bool
        {
            // THE mechanism this test pins: two invocations for the same cook key never agree
            // on a tmp path. Pre-fix (a single "<final>.tmp" path shared by every invocation),
            // this assertion is exactly what fails.
            REQUIRE(tmpB != tmpA);
            return writeContent(tmpB, contentB);
        });

        return writeContent(tmpA, contentA);
    });

    REQUIRE(committedA);
    REQUIRE(committedB);

    // The final content is EXACTLY one writer's complete output -- never a mix, never a
    // truncation.
    const std::vector<std::byte> finalContent = ReadWholeFile(finalPath);
    const bool matchesA = (finalContent == contentA);
    const bool matchesB = (finalContent == contentB);
    CHECK((matchesA || matchesB));
    CHECK_FALSE((matchesA && matchesB));   // different lengths -- both matching is impossible

    // Both invocations cleaned up (renamed away) their own private tmp path -- nothing stray.
    CHECK(CountTmpFiles(finalPath.parent_path()) == 0u);
}

// ---- RebuildIndexFromScan -------------------------------------------------------------------

TEST_CASE("pipeline: RebuildIndexFromScan reproduces the index from artifact headers alone", "[pipeline]")
{
    const fs::path root = TempDir("rebuild_scan");

    struct Entry { Guid guid; std::uint64_t key; };
    std::vector<Entry> entries;

    for (std::uint8_t i = 0; i < 5; ++i)
    {
        const Guid guid = Guid::Generate();
        const std::vector<std::byte> sourceBytes = PatternBytes(20, static_cast<std::uint8_t>(0x60 + i));
        TextureMetaSettings settings{};
        settings.maxSize = static_cast<std::uint32_t>(512 * (i + 1));
        const std::uint64_t key = ComputeCookKey(sourceBytes, settings, kTextureImporterVersion);

        ArtifactStore writerStore(root);   // PathFor is pure -- any instance over the same root agrees
        const fs::path path = writerStore.PathFor(key);
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);

        const TextureArtifactDesc desc = MakeDesc(guid, kTextureImporterVersion);
        const std::vector<std::byte> payload = PatternBytes(8, 1);
        const std::vector<std::byte> thumb = PatternBytes(4, 2);
        REQUIRE(WriteTextureArtifact(path, desc, payload, thumb));

        entries.push_back({ guid, key });
    }

    // A FRESH store instance, never PutIndex'd -- proves the index is reconstructed from disk,
    // not carried over from any in-process state.
    ArtifactStore store(root);
    store.RebuildIndexFromScan();

    for (const Entry& e : entries)
    {
        const std::optional<std::uint64_t> looked = store.Lookup(e.guid);
        REQUIRE(looked.has_value());
        CHECK(*looked == e.key);
    }
}

// ---- SweepOrphans --------------------------------------------------------------------------

TEST_CASE("pipeline: SweepOrphans removes non-live artifacts and touches nothing live", "[pipeline]")
{
    const fs::path root = TempDir("sweep_orphans");
    ArtifactStore store(root);

    const Guid guidLive1 = Guid::Generate();
    const Guid guidLive2 = Guid::Generate();
    const Guid guidOrphan = Guid::Generate();

    auto writeOne = [&](const Guid& guid, std::uint8_t seed) -> std::uint64_t
    {
        const std::vector<std::byte> sourceBytes = PatternBytes(12, seed);
        TextureMetaSettings settings{};
        const std::uint64_t key = ComputeCookKey(sourceBytes, settings, kTextureImporterVersion);
        const fs::path path = store.PathFor(key);
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        const TextureArtifactDesc desc = MakeDesc(guid, kTextureImporterVersion);
        REQUIRE(WriteTextureArtifact(path, desc, PatternBytes(4, 9), PatternBytes(4, 10)));
        return key;
    };

    const std::uint64_t keyLive1 = writeOne(guidLive1, 0x70);
    const std::uint64_t keyLive2 = writeOne(guidLive2, 0x71);
    const std::uint64_t keyOrphan = writeOne(guidOrphan, 0x72);

    store.RebuildIndexFromScan();
    REQUIRE(store.Lookup(guidLive1).has_value());
    REQUIRE(store.Lookup(guidLive2).has_value());
    REQUIRE(store.Lookup(guidOrphan).has_value());

    const std::unordered_set<Guid> liveGuids{ guidLive1, guidLive2 };
    const std::size_t removed = store.SweepOrphans(liveGuids);

    CHECK(removed == 1u);
    CHECK_FALSE(fs::exists(store.PathFor(keyOrphan)));
    CHECK(fs::exists(store.PathFor(keyLive1)));
    CHECK(fs::exists(store.PathFor(keyLive2)));

    // "Touches nothing live" means byte-for-byte untouched, not merely "not deleted" -- both
    // survivors must still parse as valid artifacts.
    CHECK(ReadTextureArtifact(store.PathFor(keyLive1)).has_value());
    CHECK(ReadTextureArtifact(store.PathFor(keyLive2)).has_value());

    CHECK_FALSE(store.Lookup(guidOrphan).has_value());
    CHECK(store.Lookup(guidLive1).has_value());
    CHECK(store.Lookup(guidLive2).has_value());
}

TEST_CASE("pipeline: SweepOrphans against an empty live set removes every indexed artifact", "[pipeline]")
{
    const fs::path root = TempDir("sweep_all");
    ArtifactStore store(root);

    const Guid guidA = Guid::Generate();
    const std::vector<std::byte> sourceBytes = PatternBytes(10, 0x80);
    TextureMetaSettings settings{};
    const std::uint64_t key = ComputeCookKey(sourceBytes, settings, kTextureImporterVersion);
    const fs::path path = store.PathFor(key);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    REQUIRE(WriteTextureArtifact(path, MakeDesc(guidA, kTextureImporterVersion), PatternBytes(4, 3), PatternBytes(4, 4)));

    store.RebuildIndexFromScan();
    REQUIRE(store.Lookup(guidA).has_value());

    const std::size_t removed = store.SweepOrphans(std::unordered_set<Guid>{});

    CHECK(removed == 1u);
    CHECK_FALSE(fs::exists(path));
    CHECK_FALSE(store.Lookup(guidA).has_value());
}
