// PixelsFor: the device-free decoded-pixel supply, and the facade's ONLY image
// route. Retained, LRU-budgeted beside
// the bytes/json caches, keyed by the resolved Guid -> path: decode-once,
// cache-hit on repeat access, memoized failure on a bad/unresolvable id or a
// corrupt file. Device-less throughout -- Assets::Create() takes no device and
// PixelsFor never needed one (that is the entire point of this seam).

#include <catch2/catch_test_macros.hpp>

// stb_image_write: implementation is owned by VendorSmokeTest.cpp in this
// same exe (STB_IMAGE_WRITE_IMPLEMENTATION defined there). Include header
// only here. (AssetsTest.cpp used the same idiom until its own PNG writer
// went with the GetTexture case at ABI v15 -- this file is now its only user.)
#include <stb_image_write.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Project/AssetId.hpp>

// F2b Task 6: TextureInfoFor/PixelsFor's artifact-backed contract, and the header-dims-vs-
// thumb-dims pin, exercised against a REAL cooked artifact (TextureImporter + WriteTextureArtifact,
// the same production path arccook drives) -- these tests link ArcaneAssetPipeline to build
// their fixtures ONLY; ArcaneClient's own Assets.cpp never links it (see ArtifactReader.hpp).
#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/TextureImporter.hpp>
#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>

namespace
{
    // 8x4 RGBA8 checkerboard, 1x1 squares, black/white -- deterministic by
    // formula so the test can assert the EXACT decoded byte pattern rather
    // than merely round-tripping whatever it happened to write.
    constexpr std::uint32_t kCheckerW = 8;
    constexpr std::uint32_t kCheckerH = 4;

    std::vector<unsigned char> CheckerPixels()
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(kCheckerW) * kCheckerH * 4);
        for (std::uint32_t y = 0; y < kCheckerH; ++y)
        {
            for (std::uint32_t x = 0; x < kCheckerW; ++x)
            {
                const unsigned char v = ((x + y) % 2 == 0) ? 255 : 0;
                unsigned char* p = px.data() + (static_cast<std::size_t>(y) * kCheckerW + x) * 4;
                p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
            }
        }
        return px;
    }

    // Writes the 8x4 checker fixture to a uniquely-named temp file and
    // returns its path: a real, loadable PNG written by the test, no vendored
    // fixture asset. AssetsTest.cpp carried the same idiom until ABI v15
    // deleted GetTexture and left its writer with no caller.
    std::filesystem::path WriteCheckerPng(const char* name)
    {
        const auto pixels = CheckerPixels();
        const auto path = std::filesystem::temp_directory_path() / name;
        REQUIRE(stbi_write_png(path.string().c_str(), static_cast<int>(kCheckerW),
                               static_cast<int>(kCheckerH), 4, pixels.data(),
                               static_cast<int>(kCheckerW) * 4) != 0);
        return path;
    }

    // Installs a resolver mapping exactly one Guid -> path (mirrors
    // AssetsTest.cpp's "resolver routes ids into the same cached loaders" test).
    Arcane::Assets::AssetResolver OneShotResolver(const Arcane::Guid& id,
                                                  const std::filesystem::path& path)
    {
        return [id, path](const Arcane::AssetId& a) -> std::optional<std::filesystem::path>
        {
            if (a.Value() == id) return path;
            return std::nullopt;
        };
    }
}

TEST_CASE("assets: PixelsFor decodes a fixture PNG to the exact byte pattern", "[assets][pixels]")
{
    const auto png = WriteCheckerPng("arcane-assets-pixels-exact.png");
    const Arcane::Guid id = Arcane::Guid::Generate();

    auto assets = Arcane::Assets::Create();
    REQUIRE(assets != nullptr);
    assets->SetAssetResolver(OneShotResolver(id, png));

    const Arcane::PixelData* pixels = assets->PixelsFor(id);
    REQUIRE(pixels != nullptr);
    CHECK(pixels->Valid());
    CHECK(pixels->width == kCheckerW);
    CHECK(pixels->height == kCheckerH);
    CHECK(pixels->rgba == CheckerPixels());

    std::remove(png.string().c_str());
}

TEST_CASE("assets: PixelsFor is a cache hit -- the second call returns the same pointer",
          "[assets][pixels]")
{
    const auto png = WriteCheckerPng("arcane-assets-pixels-cachehit.png");
    const Arcane::Guid id = Arcane::Guid::Generate();

    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver(OneShotResolver(id, png));

    const Arcane::PixelData* first = assets->PixelsFor(id);
    const Arcane::PixelData* second = assets->PixelsFor(id);
    REQUIRE(first != nullptr);
    CHECK(second == first);   // identical pointer: decode-once, retained

    std::remove(png.string().c_str());
}

TEST_CASE("assets: PixelsFor on a missing or unresolvable Guid returns null, memoized",
          "[assets][pixels]")
{
    auto assets = Arcane::Assets::Create();
    REQUIRE(assets != nullptr);

    // No resolver installed at all: every id fails, warn-once per id.
    const Arcane::Guid noResolver = Arcane::Guid::Generate();
    CHECK(assets->PixelsFor(noResolver) == nullptr);
    CHECK(assets->PixelsFor(noResolver) == nullptr);   // memoized: no retry storm

    // Resolver installed but this id is unknown to it.
    const Arcane::Guid known = Arcane::Guid::Generate();
    const Arcane::Guid unknown = Arcane::Guid::Generate();
    const auto png = WriteCheckerPng("arcane-assets-pixels-unknownid.png");
    assets->SetAssetResolver(OneShotResolver(known, png));

    CHECK(assets->PixelsFor(unknown) == nullptr);
    CHECK(assets->PixelsFor(unknown) == nullptr);      // memoized

    // Nil Guid -- explicitly invalid, never reaches the resolver.
    CHECK(assets->PixelsFor(Arcane::Guid::Nil()) == nullptr);

    // The known id still resolves and decodes fine -- the failures above
    // did not poison the whole facade.
    const Arcane::PixelData* pixels = assets->PixelsFor(known);
    REQUIRE(pixels != nullptr);
    CHECK(pixels->width == kCheckerW);

    std::remove(png.string().c_str());
}

TEST_CASE("assets: PixelsFor on a resolvable Guid whose file fails to decode returns null, memoized",
          "[assets][pixels]")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "arcane_pixels_corrupt";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path junk = dir / "not-a-png.png";
    { std::ofstream f(junk, std::ios::binary); f << "not a png at all"; }

    const Arcane::Guid id = Arcane::Guid::Generate();
    auto assets = Arcane::Assets::Create();
    assets->SetAssetResolver(OneShotResolver(id, junk));

    CHECK(assets->PixelsFor(id) == nullptr);
    CHECK(assets->PixelsFor(id) == nullptr);   // memoized: no repeat decode attempt

    fs::remove_all(dir, ec);
}

TEST_CASE("assets: PixelsFor budget evicts least-recently-used pixel entries",
          "[assets][pixels][budget]")
{
    namespace fs = std::filesystem;

    // Three checker images (8*4*4 = 128 bytes each) against a budget that
    // fits only two -- same touch-then-insert-evicts-LRU shape as
    // AssetsTest.cpp's "assets: byte budget evicts least-recently-used
    // entries and stays within budget".
    const auto pngA = WriteCheckerPng("arcane-assets-pixels-budget-a.png");
    const auto pngB = WriteCheckerPng("arcane-assets-pixels-budget-b.png");
    const auto pngC = WriteCheckerPng("arcane-assets-pixels-budget-c.png");
    const Arcane::Guid a = Arcane::Guid::Generate();
    const Arcane::Guid b = Arcane::Guid::Generate();
    const Arcane::Guid c = Arcane::Guid::Generate();

    Arcane::AssetsDesc desc;
    desc.byteBudget = 280;   // two 128-byte entries fit (256); three do not (384)
    auto assets = Arcane::Assets::Create(desc);
    REQUIRE(assets != nullptr);
    assets->SetAssetResolver(
        [&](const Arcane::AssetId& id) -> std::optional<fs::path>
        {
            if (id.Value() == a) return pngA;
            if (id.Value() == b) return pngB;
            if (id.Value() == c) return pngC;
            return std::nullopt;
        });

    const Arcane::PixelData* pa = assets->PixelsFor(a);
    const Arcane::PixelData* pb = assets->PixelsFor(b);
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);
    CHECK(assets->Stats().totalBytes == 256);   // both fit

    CHECK(assets->PixelsFor(a) == pa);          // touch A: B is now the LRU

    const Arcane::PixelData* pc = assets->PixelsFor(c);   // 384 > 280 -> evicts B (not A)
    REQUIRE(pc != nullptr);
    CHECK(assets->Stats().totalBytes <= 280);

    CHECK(assets->PixelsFor(a) == pa);          // A survived (recently used)
    // ...and B did NOT: exactly two entries are resident, so inserting C
    // dropped one, and the LRU rule says which. This is the deterministic
    // statement of "B was evicted".
    CHECK(assets->Stats().count == 2);

    // B therefore comes back as a FRESH decode. What is asserted is its
    // CONTENT, not its address: the evicted buffer was freed, and Windows'
    // low-fragmentation heap is free to hand the identical address straight
    // back for the replacement -- the same allocator behaviour BatcherTest's
    // ABA case documents and deliberately retries around. A pointer-identity
    // check here is therefore a coin flip, and it has flipped: it once failed
    // in the ~[gpu] gate with `pb2 != pb` expanding to
    // `0x21a7cb0ed30 != 0x21a7cb0ed30`. Content is what the cache promises.
    const Arcane::PixelData* pb2 = assets->PixelsFor(b);
    REQUIRE(pb2 != nullptr);
    CHECK(pb2->Valid());
    CHECK(pb2->width == kCheckerW);
    CHECK(pb2->height == kCheckerH);
    CHECK(pb2->rgba == CheckerPixels());
    CHECK(assets->Stats().totalBytes <= 280);   // never settles over budget

    std::remove(pngA.string().c_str());
    std::remove(pngB.string().c_str());
    std::remove(pngC.string().c_str());
}

// ---- F2b Task 6: TextureInfoFor + PixelsFor's narrowed (artifact-backed) contract ----------

namespace
{
    std::vector<unsigned char> GradientRgba(int w, int h)
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                unsigned char* p = px.data() + (static_cast<std::size_t>(y) * w + x) * 4;
                p[0] = static_cast<unsigned char>((x * 255) / (w > 1 ? w - 1 : 1));
                p[1] = static_cast<unsigned char>((y * 255) / (h > 1 ? h - 1 : 1));
                p[2] = 128;
                p[3] = 255;
            }
        }
        return px;
    }

    std::vector<std::byte> ReadWholeFileAsBytes(const std::filesystem::path& path)
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

    // A Content/ source PNG plus an Intermediate/Artifacts/*.arcart cooked from it via the
    // REAL production import path (Arcane::AssetPipeline::ImportTexture + WriteTextureArtifact
    // -- the same path arccook drives), wired into an Assets instance exactly the way
    // Runtime::OpenProject wires a real project's content root + resolver. The artifact's own
    // on-disk PATH does not need to match a real cook-key's 256-way shard -- FindArtifactForGuid
    // scans Intermediate/Artifacts/**/*.arcart by CONTENT (each file's own header sourceGuid),
    // never by filename, so any path under that tree is a valid fixture location.
    struct ArtifactSandbox
    {
        std::filesystem::path root;
        std::filesystem::path sourcePng;
        std::filesystem::path artifactPath;
        Arcane::Guid guid;
        Arcane::AssetPipeline::ImportedTexture imported;
    };

    ArtifactSandbox MakeArtifactSandbox(const char* leaf, int w, int h,
                                        std::optional<std::uint32_t> importerVersionOverride = std::nullopt)
    {
        namespace fs = std::filesystem;

        ArtifactSandbox sb;
        sb.root = fs::temp_directory_path() / "arcane_assets_artifact_sandbox" / leaf;
        std::error_code ec;
        fs::remove_all(sb.root, ec);
        fs::create_directories(sb.root / "Content" / "textures");
        fs::create_directories(sb.root / "Intermediate" / "Artifacts" / "aa");

        sb.sourcePng = sb.root / "Content" / "textures" / "big.png";
        const std::vector<unsigned char> rgba = GradientRgba(w, h);
        REQUIRE(stbi_write_png(sb.sourcePng.string().c_str(), w, h, 4, rgba.data(), w * 4) != 0);

        sb.guid = Arcane::Guid::Generate();
        const std::vector<std::byte> sourceBytes = ReadWholeFileAsBytes(sb.sourcePng);

        Arcane::AssetPipeline::TextureMetaSettings settings;
        settings.format = Arcane::AssetPipeline::TextureMetaSettings::Format::Rgba8;
        auto imported = Arcane::AssetPipeline::ImportTexture(sourceBytes, sb.guid, settings);
        REQUIRE(imported.has_value());
        sb.imported = *imported;

        Arcane::AssetPipeline::TextureArtifactDesc desc = sb.imported.desc;
        if (importerVersionOverride)
            desc.importerVersion = *importerVersionOverride;

        sb.artifactPath = sb.root / "Intermediate" / "Artifacts" / "aa" / "fixture.arcart";
        REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(sb.artifactPath, desc, sb.imported.payload,
                                                            sb.imported.thumbRgba));
        return sb;
    }

    Arcane::Assets::AssetResolver SandboxResolver(const ArtifactSandbox& sb)
    {
        return [&sb](const Arcane::AssetId& id) -> std::optional<std::filesystem::path>
        {
            if (id.Value() == sb.guid) return sb.sourcePng;
            return std::nullopt;
        };
    }

    // ContentArtifactRefusalObserved is a PROCESS-WIDE latch (Assets.hpp), so a test that
    // fires it must not leak that fact into an unrelated case in the same Catch2 process
    // (random order) -- same RAII idiom NriDiagnosticsTest.cpp's ScopedGpuDeviceLostLatch
    // already uses for GpuInstrumentation's own device-lost latch.
    struct ScopedContentArtifactRefusalLatch
    {
        ScopedContentArtifactRefusalLatch() { Arcane::ResetContentArtifactRefusal(); }
        ~ScopedContentArtifactRefusalLatch() { Arcane::ResetContentArtifactRefusal(); }
    };
}

TEST_CASE("assets: TextureInfoFor serves HEADER (true) dims while PixelsFor serves THUMBNAIL "
          "dims for the SAME artifact-backed guid -- the wrong-dims bug class this split "
          "prevents (F2b Task 6)", "[assets][pixels][artifact]")
{
    const ArtifactSandbox sb = MakeArtifactSandbox("dims_pin", 128, 64);
    // Sanity: the thumbnail must actually be SMALLER than the source for this pin to mean
    // anything -- TextureImporter halves the top level until max(dim)<=64, so 128x64 must
    // produce a strictly smaller thumbnail (64x32).
    REQUIRE(sb.imported.desc.thumbWidth < sb.imported.desc.width);
    REQUIRE(sb.imported.desc.thumbHeight < sb.imported.desc.height);

    auto assets = Arcane::Assets::Create();
    REQUIRE(assets != nullptr);
    assets->SetContentRoot(sb.root / "Content");
    assets->SetAssetResolver(SandboxResolver(sb));

    const Arcane::TextureInfo* info = assets->TextureInfoFor(sb.guid);
    REQUIRE(info != nullptr);
    CHECK(info->width == sb.imported.desc.width);
    CHECK(info->height == sb.imported.desc.height);
    CHECK(info->mipCount == sb.imported.desc.mipCount);
    CHECK(info->srgb == sb.imported.desc.srgb);

    const Arcane::PixelData* pixels = assets->PixelsFor(sb.guid);
    REQUIRE(pixels != nullptr);
    CHECK(pixels->width == sb.imported.desc.thumbWidth);
    CHECK(pixels->height == sb.imported.desc.thumbHeight);

    // THE PIN: for the SAME guid, the two accessors answer DIFFERENT questions.
    CHECK(pixels->width != info->width);
    CHECK(pixels->height != info->height);
}

TEST_CASE("assets: a guid with no cooked artifact yet keeps the legacy full-decode behaviour "
          "unchanged (Task 8 retires this fallback)", "[assets][pixels][artifact]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_artifact_sandbox" / "no_artifact";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Content" / "textures");
    // Deliberately NO Intermediate/Artifacts anywhere under root.

    const Arcane::Guid guid = Arcane::Guid::Generate();
    const fs::path png = root / "Content" / "textures" / "plain.png";
    const auto checker = CheckerPixels();
    REQUIRE(stbi_write_png(png.string().c_str(), static_cast<int>(kCheckerW), static_cast<int>(kCheckerH),
                           4, checker.data(), static_cast<int>(kCheckerW) * 4) != 0);

    auto assets = Arcane::Assets::Create();
    assets->SetContentRoot(root / "Content");
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        if (id.Value() == guid) return png;
        return std::nullopt;
    });

    const Arcane::TextureInfo* info = assets->TextureInfoFor(guid);
    REQUIRE(info != nullptr);
    CHECK(info->width == kCheckerW);
    CHECK(info->height == kCheckerH);
    CHECK(info->mipCount == 1);

    const Arcane::PixelData* pixels = assets->PixelsFor(guid);
    REQUIRE(pixels != nullptr);
    CHECK(pixels->width == kCheckerW);
    CHECK(pixels->height == kCheckerH);
    CHECK(pixels->rgba == checker);   // legacy full decode, byte-exact, unchanged

    fs::remove_all(root, ec);
}

TEST_CASE("assets: a HashMismatch artifact REFUSES loudly (never limps to the source decode) "
          "on BOTH accessors, memoized, and latches the process-wide refusal -- the Assets-"
          "facade-layer half of Step 3's watched scratch scenario", "[assets][pixels][artifact]")
{
    ScopedContentArtifactRefusalLatch latchGuard;
    REQUIRE_FALSE(Arcane::ContentArtifactRefusalObserved());

    const ArtifactSandbox sb = MakeArtifactSandbox("hash_mismatch", 16, 16);

    // Edit the STAGED source AFTER cooking, without recooking -- exactly the scratch scenario
    // Step 3's watched ArcaneRuntime refusal exercises, reproduced here at the facade layer.
    {
        std::vector<unsigned char> edited = GradientRgba(16, 16);
        edited[0] = static_cast<unsigned char>(edited[0] ^ 0xFF);
        REQUIRE(stbi_write_png(sb.sourcePng.string().c_str(), 16, 16, 4, edited.data(), 16 * 4) != 0);
    }

    auto assets = Arcane::Assets::Create();
    assets->SetContentRoot(sb.root / "Content");
    assets->SetAssetResolver(SandboxResolver(sb));

    CHECK(assets->TextureInfoFor(sb.guid) == nullptr);
    CHECK(assets->TextureInfoFor(sb.guid) == nullptr);   // memoized: no retry storm
    CHECK(assets->PixelsFor(sb.guid) == nullptr);

    CHECK(Arcane::ContentArtifactRefusalObserved());
    const std::string detail = Arcane::ContentArtifactRefusalDetail();
    CHECK(detail.find("HashMismatch") != std::string::npos);
    CHECK(detail.find(sb.guid.ToString()) != std::string::npos);
}

TEST_CASE("assets: a VersionNewerThanEngine artifact refuses loudly too", "[assets][pixels][artifact]")
{
    ScopedContentArtifactRefusalLatch latchGuard;
    REQUIRE_FALSE(Arcane::ContentArtifactRefusalObserved());

    const ArtifactSandbox sb = MakeArtifactSandbox(
        "version_newer", 8, 8, Arcane::kClientTextureImporterVersionMirror + 1);

    auto assets = Arcane::Assets::Create();
    assets->SetContentRoot(sb.root / "Content");
    assets->SetAssetResolver(SandboxResolver(sb));

    CHECK(assets->PixelsFor(sb.guid) == nullptr);
    CHECK(assets->TextureInfoFor(sb.guid) == nullptr);

    CHECK(Arcane::ContentArtifactRefusalObserved());
    CHECK(Arcane::ContentArtifactRefusalDetail().find("VersionNewerThanEngine") != std::string::npos);
}
