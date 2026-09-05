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

    // Task 8: content is artifact-only, so fixtures that expect PIXELS back cook
    // their checker through the real import path (Rgba8; 8x4 <= 64 so the
    // thumbnail IS the top mip, and 0/255 round-trip the sRGB linearize exactly
    // -- every byte-exact assertion below survives the cutover unchanged).
    struct CookedChecker
    {
        std::filesystem::path png;
        Arcane::Guid guid;
    };

    CookedChecker AddCookedChecker(const std::filesystem::path& root, const char* name)
    {
        namespace fs = std::filesystem;
        fs::create_directories(root / "Content" / "textures");
        fs::create_directories(root / "Intermediate" / "Artifacts" / "aa");

        CookedChecker cc;
        cc.png = root / "Content" / "textures" / (std::string(name) + ".png");
        const auto pixels = CheckerPixels();
        REQUIRE(stbi_write_png(cc.png.string().c_str(), static_cast<int>(kCheckerW),
                               static_cast<int>(kCheckerH), 4, pixels.data(),
                               static_cast<int>(kCheckerW) * 4) != 0);

        std::vector<std::byte> srcBytes;
        {
            std::ifstream f(cc.png, std::ios::binary);
            REQUIRE(f.good());
            f.seekg(0, std::ios::end);
            srcBytes.resize(static_cast<std::size_t>(f.tellg()));
            f.seekg(0, std::ios::beg);
            f.read(reinterpret_cast<char*>(srcBytes.data()),
                   static_cast<std::streamsize>(srcBytes.size()));
            REQUIRE(f.good());
        }

        cc.guid = Arcane::Guid::Generate();
        Arcane::AssetPipeline::TextureMetaSettings settings;
        settings.format = Arcane::AssetPipeline::TextureMetaSettings::Format::Rgba8;
        auto imported = Arcane::AssetPipeline::ImportTexture(srcBytes, cc.guid, settings);
        REQUIRE(imported.has_value());
        REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(
            root / "Intermediate" / "Artifacts" / "aa" / (std::string(name) + ".arcart"),
            imported->desc, imported->payload, imported->thumbRgba));
        return cc;
    }

    std::filesystem::path MakeCookedRoot(const char* leaf)
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / "arcane_assets_pixels_cooked" / leaf;
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        return root;
    }
}

TEST_CASE("assets: PixelsFor serves the artifact thumbnail's exact byte pattern", "[assets][pixels]")
{
    const auto root = MakeCookedRoot("exact");
    const auto fx = AddCookedChecker(root, "checker");

    auto assets = Arcane::Assets::Create();
    REQUIRE(assets != nullptr);
    assets->SetContentRoot(root / "Content");
    assets->SetAssetResolver(OneShotResolver(fx.guid, fx.png));

    const Arcane::PixelData* pixels = assets->PixelsFor(fx.guid);
    REQUIRE(pixels != nullptr);
    CHECK(pixels->Valid());
    CHECK(pixels->width == kCheckerW);   // 8x4 <= 64: the thumbnail IS the top mip
    CHECK(pixels->height == kCheckerH);
    CHECK(pixels->rgba == CheckerPixels());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("assets: PixelsFor is a cache hit -- the second call returns the same pointer",
          "[assets][pixels]")
{
    const auto root = MakeCookedRoot("cachehit");
    const auto fx = AddCookedChecker(root, "checker");

    auto assets = Arcane::Assets::Create();
    assets->SetContentRoot(root / "Content");
    assets->SetAssetResolver(OneShotResolver(fx.guid, fx.png));

    const Arcane::PixelData* first = assets->PixelsFor(fx.guid);
    const Arcane::PixelData* second = assets->PixelsFor(fx.guid);
    REQUIRE(first != nullptr);
    CHECK(second == first);   // identical pointer: resolve-once, retained

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
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
    const auto root = MakeCookedRoot("unknownid");
    const auto fx = AddCookedChecker(root, "checker");
    const Arcane::Guid unknown = Arcane::Guid::Generate();
    assets->SetContentRoot(root / "Content");
    assets->SetAssetResolver(OneShotResolver(fx.guid, fx.png));

    CHECK(assets->PixelsFor(unknown) == nullptr);
    CHECK(assets->PixelsFor(unknown) == nullptr);      // memoized

    // Nil Guid -- explicitly invalid, never reaches the resolver.
    CHECK(assets->PixelsFor(Arcane::Guid::Nil()) == nullptr);

    // The known id still resolves through its artifact fine -- the failures
    // above did not poison the whole facade.
    const Arcane::PixelData* pixels = assets->PixelsFor(fx.guid);
    REQUIRE(pixels != nullptr);
    CHECK(pixels->width == kCheckerW);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
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

    // Three cooked checkers (8x4 thumbnails, 128 bytes each) against a budget
    // that fits only two -- same touch-then-insert-evicts-LRU shape as
    // AssetsTest.cpp's "assets: byte budget evicts least-recently-used
    // entries and stays within budget".
    const auto root = MakeCookedRoot("budget");
    const auto fxA = AddCookedChecker(root, "a");
    const auto fxB = AddCookedChecker(root, "b");
    const auto fxC = AddCookedChecker(root, "c");
    const fs::path pngA = fxA.png, pngB = fxB.png, pngC = fxC.png;
    const Arcane::Guid a = fxA.guid;
    const Arcane::Guid b = fxB.guid;
    const Arcane::Guid c = fxC.guid;

    Arcane::AssetsDesc desc;
    desc.byteBudget = 280;   // two 128-byte entries fit (256); three do not (384)
    auto assets = Arcane::Assets::Create(desc);
    REQUIRE(assets != nullptr);
    assets->SetContentRoot(root / "Content");
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

    std::error_code ec;
    fs::remove_all(root, ec);
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

    // F2b Task 12: the per-refusal observer (Assets.hpp's SetArtifactRefusalObserver) is
    // ALSO process-wide -- same leak-across-cases hazard as the latch above, same RAII fix.
    struct ScopedArtifactRefusalObserver
    {
        ~ScopedArtifactRefusalObserver() { Arcane::SetArtifactRefusalObserver(nullptr, nullptr); }
    };

    struct RefusalRecord
    {
        Arcane::Guid id;
        std::string  kind;
    };

    // Raw fn-ptr + user-data thunk, matching SetArtifactRefusalObserver's own DLL-safe
    // signature (Diagnostics::SetSink's convention) -- `user` is the vector under test.
    void RecordRefusal(const Arcane::Guid& id, const char* kind, void* user)
    {
        static_cast<std::vector<RefusalRecord>*>(user)->push_back(RefusalRecord{ id, kind });
    }
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

TEST_CASE("assets: a guid with no cooked artifact is the ArtifactMissing refusal on ALL THREE "
          "accessors -- loud, memoized, latched (Task 8: content is artifact-only, the stb "
          "fallback is retired)", "[assets][pixels][artifact]")
{
    ScopedContentArtifactRefusalLatch latchGuard;
    REQUIRE_FALSE(Arcane::ContentArtifactRefusalObserved());

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_artifact_sandbox" / "no_artifact";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Content" / "textures");
    // Deliberately NO Intermediate/Artifacts anywhere under root: a perfectly
    // decodable source PNG with no cooked artifact must REFUSE, never limp to stb.

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

    CHECK(assets->TextureInfoFor(guid) == nullptr);
    CHECK(assets->TextureInfoFor(guid) == nullptr);   // memoized: no retry storm
    CHECK(assets->PixelsFor(guid) == nullptr);
    CHECK(assets->ArtifactFor(guid) == nullptr);

    // The watched firing: the process-wide latch names ArtifactMissing and the guid,
    // exactly what ArcaneRuntime turns into its nonzero exit.
    CHECK(Arcane::ContentArtifactRefusalObserved());
    const std::string detail = Arcane::ContentArtifactRefusalDetail();
    CHECK(detail.find("ArtifactMissing") != std::string::npos);
    CHECK(detail.find(guid.ToString()) != std::string::npos);

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

TEST_CASE("assets: the per-refusal observer fires on EVERY refusal, not just the process's "
          "first (F2b Task 12 -- the Problems pane wants all of them, unlike the latch)",
          "[assets][pixels][artifact]")
{
    ScopedContentArtifactRefusalLatch latchGuard;
    ScopedArtifactRefusalObserver observerGuard;
    std::vector<RefusalRecord> seen;
    Arcane::SetArtifactRefusalObserver(&RecordRefusal, &seen);

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_artifact_sandbox" / "observer_fires_always";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Content" / "textures");

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

    // THE PIN: TextureInfoFor/PixelsFor/ArtifactFor each maintain their OWN memo, so a guid
    // with no cooked artifact refuses independently on each -- the process-wide LATCH
    // (ContentArtifactRefusalObserved/Detail) only ever remembers the FIRST of these three,
    // but the observer must see ALL of them.
    CHECK(assets->TextureInfoFor(guid) == nullptr);
    CHECK(assets->PixelsFor(guid) == nullptr);
    CHECK(assets->ArtifactFor(guid) == nullptr);
    // A repeat call on an ALREADY-memoized accessor must NOT fire again -- the memo's whole
    // point is "attempted once", not "reported once".
    CHECK(assets->TextureInfoFor(guid) == nullptr);

    REQUIRE(seen.size() == 3u);
    for (const RefusalRecord& r : seen)
    {
        CHECK(r.id == guid);
        CHECK(r.kind == "ArtifactMissing");
    }

    // Clearing (nullptr, nullptr) stops delivery -- a later refusal (a second, distinct guid)
    // must not reach the now-cleared observer.
    Arcane::SetArtifactRefusalObserver(nullptr, nullptr);
    const Arcane::Guid guid2 = Arcane::Guid::Generate();
    const fs::path png2 = root / "Content" / "textures" / "plain2.png";
    REQUIRE(stbi_write_png(png2.string().c_str(), static_cast<int>(kCheckerW), static_cast<int>(kCheckerH),
                           4, checker.data(), static_cast<int>(kCheckerW) * 4) != 0);
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        if (id.Value() == guid2) return png2;
        return std::nullopt;
    });
    CHECK(assets->TextureInfoFor(guid2) == nullptr);
    CHECK(seen.size() == 3u);   // unchanged -- the cleared observer received nothing more

    fs::remove_all(root, ec);
}

TEST_CASE("assets: InvalidateArtifact clears a memoized ArtifactMissing refusal so a cook that "
          "lands afterward actually promotes -- the un-latch F2b Task 12's cook-completion "
          "callback depends on", "[assets][pixels][artifact]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_artifact_sandbox" / "invalidate_promotes";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Content" / "textures");
    fs::create_directories(root / "Intermediate" / "Artifacts" / "aa");

    const fs::path png = root / "Content" / "textures" / "big.png";
    constexpr int w = 16, h = 16;
    const std::vector<unsigned char> rgba = GradientRgba(w, h);
    REQUIRE(stbi_write_png(png.string().c_str(), w, h, 4, rgba.data(), w * 4) != 0);
    const Arcane::Guid guid = Arcane::Guid::Generate();

    auto assets = Arcane::Assets::Create();
    assets->SetContentRoot(root / "Content");
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        if (id.Value() == guid) return png;
        return std::nullopt;
    });

    // No artifact yet -- ArtifactMissing on all three, memoized (Task 8).
    REQUIRE(assets->ArtifactFor(guid) == nullptr);
    REQUIRE(assets->TextureInfoFor(guid) == nullptr);
    REQUIRE(assets->PixelsFor(guid) == nullptr);

    // NOW a background cook lands (mirrors what CookQueue's RunOnePass -> CookSession::
    // CookProject actually does): import + write the artifact for real.
    const std::vector<std::byte> sourceBytes = ReadWholeFileAsBytes(png);
    Arcane::AssetPipeline::TextureMetaSettings settings;
    settings.format = Arcane::AssetPipeline::TextureMetaSettings::Format::Rgba8;
    const auto imported = Arcane::AssetPipeline::ImportTexture(sourceBytes, guid, settings);
    REQUIRE(imported.has_value());
    const fs::path artifactPath = root / "Intermediate" / "Artifacts" / "aa" / "fixture.arcart";
    REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(artifactPath, imported->desc, imported->payload,
                                                        imported->thumbRgba));

    // Without invalidation the memo is STICKY -- this is the regression InvalidateArtifact
    // exists to prevent; proving it stays stuck here is what makes the promotion below mean
    // something (not just "it would have worked anyway").
    CHECK(assets->ArtifactFor(guid) == nullptr);

    assets->InvalidateArtifact(guid);

    const Arcane::LoadedClientArtifact* artifact = assets->ArtifactFor(guid);
    REQUIRE(artifact != nullptr);
    CHECK(artifact->info.width == static_cast<std::uint32_t>(w));
    CHECK(artifact->info.height == static_cast<std::uint32_t>(h));

    const Arcane::TextureInfo* info = assets->TextureInfoFor(guid);
    REQUIRE(info != nullptr);
    CHECK(info->width == static_cast<std::uint32_t>(w));

    const Arcane::PixelData* pixels = assets->PixelsFor(guid);
    REQUIRE(pixels != nullptr);

    fs::remove_all(root, ec);
}

TEST_CASE("assets: TextureInfoFor/ArtifactFor resolve the FRESH artifact through the REAL "
          "facade even with a stale same-guid artifact still physically on disk (C1b, "
          "final-review wave)", "[assets][pixels][artifact]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_artifact_sandbox" / "c1b_stale_and_fresh";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Content" / "textures");
    // "aa" sorts before "zz" -- the adversarial shard ordering (stale visited before
    // fresh) the pre-fix single-candidate FindArtifactForGuid always lost to.
    fs::create_directories(root / "Intermediate" / "Artifacts" / "aa");
    fs::create_directories(root / "Intermediate" / "Artifacts" / "zz");

    const fs::path png = root / "Content" / "textures" / "big.png";
    constexpr int w = 16, h = 16;
    const std::vector<unsigned char> currentPixels = GradientRgba(w, h);
    REQUIRE(stbi_write_png(png.string().c_str(), w, h, 4, currentPixels.data(), w * 4) != 0);
    const Arcane::Guid guid = Arcane::Guid::Generate();

    Arcane::AssetPipeline::TextureMetaSettings settings;
    settings.format = Arcane::AssetPipeline::TextureMetaSettings::Format::Rgba8;

    // The FRESH artifact -- cooked from the CURRENT staged source -- must be what resolves.
    const std::vector<std::byte> currentSourceBytes = ReadWholeFileAsBytes(png);
    const auto freshImported = Arcane::AssetPipeline::ImportTexture(currentSourceBytes, guid, settings);
    REQUIRE(freshImported.has_value());
    REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(
        root / "Intermediate" / "Artifacts" / "zz" / "fresh.arcart",
        freshImported->desc, freshImported->payload, freshImported->thumbRgba));

    // The STALE artifact -- SAME guid, cooked from OLDER (different-sized, so definitely
    // different-hashing) source bytes -- exactly the shape a source edit + recook leaves
    // behind if CookSession's own C1(a) self-heal somehow didn't run (a locked file, an
    // external tool). Written to the shard that sorts FIRST.
    const fs::path oldPngTemp = root / "old_temp_source.png";
    const std::vector<unsigned char> oldPixels = GradientRgba(4, 4);
    REQUIRE(stbi_write_png(oldPngTemp.string().c_str(), 4, 4, 4, oldPixels.data(), 4 * 4) != 0);
    const std::vector<std::byte> oldSourceBytes = ReadWholeFileAsBytes(oldPngTemp);
    const auto staleImported = Arcane::AssetPipeline::ImportTexture(oldSourceBytes, guid, settings);
    REQUIRE(staleImported.has_value());
    REQUIRE(freshImported->desc.sourceHash != staleImported->desc.sourceHash);
    REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(
        root / "Intermediate" / "Artifacts" / "aa" / "stale.arcart",
        staleImported->desc, staleImported->payload, staleImported->thumbRgba));

    auto assets = Arcane::Assets::Create();
    assets->SetContentRoot(root / "Content");
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        if (id.Value() == guid) return png;
        return std::nullopt;
    });

    // THE pin: resolves cleanly through the REAL public facade despite the stale duplicate
    // sitting in the shard the scan would visit first.
    const Arcane::TextureInfo* info = assets->TextureInfoFor(guid);
    REQUIRE(info != nullptr);
    CHECK(info->width == static_cast<std::uint32_t>(w));
    CHECK(info->height == static_cast<std::uint32_t>(h));

    const Arcane::LoadedClientArtifact* artifact = assets->ArtifactFor(guid);
    REQUIRE(artifact != nullptr);
    CHECK(artifact->info.width == static_cast<std::uint32_t>(w));
    CHECK(artifact->info.height == static_cast<std::uint32_t>(h));

    fs::remove_all(root, ec);
}

// ---- Desk-fix 2: SetCookPendingProbe -- the boot-race quiet seam --------------

TEST_CASE("assets: cook-pending probe installed and true quiets a Missing resolution on ALL "
          "THREE accessors -- no latch, no memo, heals WITHOUT InvalidateArtifact (desk-fix 2)",
          "[assets][pixels][artifact][cook]")
{
    ScopedContentArtifactRefusalLatch latchGuard;
    REQUIRE_FALSE(Arcane::ContentArtifactRefusalObserved());

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_artifact_sandbox" / "cook_pending_quiet";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Content" / "textures");
    fs::create_directories(root / "Intermediate" / "Artifacts" / "aa");
    // Deliberately no .arcart yet -- ArtifactMissing, exactly the boot-race shape:
    // a project just opened, the scene already resolved this guid, and the first
    // CookProject pass has not landed yet.

    const fs::path png = root / "Content" / "textures" / "big.png";
    constexpr int w = 16, h = 16;
    const std::vector<unsigned char> rgba = GradientRgba(w, h);
    REQUIRE(stbi_write_png(png.string().c_str(), w, h, 4, rgba.data(), w * 4) != 0);
    const Arcane::Guid guid = Arcane::Guid::Generate();

    auto assets = Arcane::Assets::Create();
    assets->SetContentRoot(root / "Content");
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        if (id.Value() == guid) return png;
        return std::nullopt;
    });
    assets->SetCookPendingProbe([](const Arcane::Guid&) { return true; });

    CHECK(assets->TextureInfoFor(guid) == nullptr);
    CHECK(assets->PixelsFor(guid) == nullptr);
    CHECK(assets->ArtifactFor(guid) == nullptr);

    // THE FIRST PIN: quiet -- unlike every other Missing case in this file, the
    // process-wide refusal latch never fires.
    CHECK_FALSE(Arcane::ContentArtifactRefusalObserved());

    // THE SECOND PIN: no memo either. A cook lands afterward -- write the real
    // artifact straight to disk -- with NO call to InvalidateArtifact, and
    // resolution simply works on the very next ask (nothing was ever latched,
    // so there is nothing to un-latch).
    Arcane::AssetPipeline::TextureMetaSettings settings;
    settings.format = Arcane::AssetPipeline::TextureMetaSettings::Format::Rgba8;
    const std::vector<std::byte> sourceBytes = ReadWholeFileAsBytes(png);
    const auto imported = Arcane::AssetPipeline::ImportTexture(sourceBytes, guid, settings);
    REQUIRE(imported.has_value());
    REQUIRE(Arcane::AssetPipeline::WriteTextureArtifact(
        root / "Intermediate" / "Artifacts" / "aa" / "fixture.arcart",
        imported->desc, imported->payload, imported->thumbRgba));

    const Arcane::LoadedClientArtifact* artifact = assets->ArtifactFor(guid);
    REQUIRE(artifact != nullptr);   // healed with NO InvalidateArtifact call
    CHECK(artifact->info.width == static_cast<std::uint32_t>(w));

    const Arcane::TextureInfo* info = assets->TextureInfoFor(guid);
    REQUIRE(info != nullptr);
    CHECK(info->width == static_cast<std::uint32_t>(w));

    const Arcane::PixelData* pixels = assets->PixelsFor(guid);
    REQUIRE(pixels != nullptr);

    CHECK_FALSE(Arcane::ContentArtifactRefusalObserved());   // still never latched

    fs::remove_all(root, ec);
}

TEST_CASE("assets: cook-pending probe returning false refuses loudly and memoizes, identical to "
          "no probe installed at all (desk-fix 2)", "[assets][pixels][artifact][cook]")
{
    ScopedContentArtifactRefusalLatch latchGuard;
    REQUIRE_FALSE(Arcane::ContentArtifactRefusalObserved());

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_artifact_sandbox" / "cook_pending_false";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Content" / "textures");

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
    assets->SetCookPendingProbe([](const Arcane::Guid&) { return false; });

    CHECK(assets->ArtifactFor(guid) == nullptr);
    CHECK(assets->ArtifactFor(guid) == nullptr);   // memoized: no retry storm

    CHECK(Arcane::ContentArtifactRefusalObserved());
    CHECK(Arcane::ContentArtifactRefusalDetail().find("ArtifactMissing") != std::string::npos);

    fs::remove_all(root, ec);
}

TEST_CASE("assets: HashMismatch refuses loudly even with a cook-pending probe answering true -- "
          "NEVER quieted, a present-but-invalid artifact is broken regardless of a pending cook "
          "(desk-fix 2, watched)", "[assets][pixels][artifact][cook]")
{
    ScopedContentArtifactRefusalLatch latchGuard;
    REQUIRE_FALSE(Arcane::ContentArtifactRefusalObserved());

    const ArtifactSandbox sb = MakeArtifactSandbox("cook_pending_hashmismatch", 16, 16);

    // Edit the STAGED source AFTER cooking, without recooking -- same shape as the
    // HashMismatch case above, reproduced here with a probe that would quiet a
    // Missing but must NOT quiet this.
    {
        std::vector<unsigned char> edited = GradientRgba(16, 16);
        edited[0] = static_cast<unsigned char>(edited[0] ^ 0xFF);
        REQUIRE(stbi_write_png(sb.sourcePng.string().c_str(), 16, 16, 4, edited.data(), 16 * 4) != 0);
    }

    auto assets = Arcane::Assets::Create();
    assets->SetContentRoot(sb.root / "Content");
    assets->SetAssetResolver(SandboxResolver(sb));
    assets->SetCookPendingProbe([](const Arcane::Guid&) { return true; });

    CHECK(assets->TextureInfoFor(sb.guid) == nullptr);
    CHECK(assets->PixelsFor(sb.guid) == nullptr);
    CHECK(assets->ArtifactFor(sb.guid) == nullptr);

    CHECK(Arcane::ContentArtifactRefusalObserved());
    CHECK(Arcane::ContentArtifactRefusalDetail().find("HashMismatch") != std::string::npos);
}

// ---- Review fix: the probe's part (b) must be a POSITIVE signal --------------
//
// A critical review finding on the FIRST cut of desk-fix 2: the editor's probe
// fell through to IsCookPending(id) post-settling, whose OWN default is
// "pending" whenever m_cookDiagnostics has NO ROW for the guid yet. That
// default is safe for NriTextureCache's render-layer oracle (a wrong guess
// there only costs a checkerboard-vs-refused VISUAL choice, and RefuseArtifact
// fires independently regardless of what that oracle answers) -- but reusing
// it as the FACADE's OWN cook-pending probe made it load-bearing for whether
// RefuseArtifact fires AT ALL, and RefuseArtifact is the ONLY thing that ever
// creates a row in m_cookDiagnostics in the first place. For a guid CookSession
// will NEVER attempt (its source renamed/deleted after registration, or a
// format EnumerateTextureSources skips), no row ever forms: "presumed pending"
// forever, quiet forever, unmemoized, unlatched, unreported -- a closed loop,
// and a regression against "refusals stay loud" for exactly the broken-
// reference case that matters most.
//
// The fix (editor-side, EditorApp.cpp's SetCookPendingProbe installation):
// post-settling, quiet now requires a POSITIVE signal -- the cook queue has
// ACTIVE work (CookQueue::CookPending()) AND the guid's registered source
// still exists on disk (a real filesystem exists() check). Since
// EditorApp.cpp/.hpp are not part of ArcaneTests' compiled sources (only a
// few hand-picked pure-logic files are -- see the workspace premake5.lua's
// ArcaneTests file list), these three tests pin the SAME decision shape at
// the Assets-facade level the editor's probe actually plugs into: a
// test-installed probe modeling "queue active AND source exists" with a REAL
// std::filesystem::exists() check against a REAL file this test creates and
// deletes, proving the facade's behavior responds correctly to that signal in
// all three of the review's required scenarios.
TEST_CASE("assets: cook-pending probe's positive-signal shape -- queue IDLE means never quiet "
          "regardless of row-absence, closing the review's row-absence-implies-pending "
          "regression (desk-fix 2, review fix)", "[assets][pixels][artifact][cook]")
{
    ScopedContentArtifactRefusalLatch latchGuard;
    REQUIRE_FALSE(Arcane::ContentArtifactRefusalObserved());

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_artifact_sandbox" / "review_fix_queue_idle";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Content" / "textures");

    const Arcane::Guid guid = Arcane::Guid::Generate();
    const fs::path png = root / "Content" / "textures" / "orphan.png";
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

    // Models the editor's post-settling probe with the queue-active half of the
    // AND FALSE -- no cook is running or queued for this project at all. Source
    // existence is irrelevant when the queue has no activity (the top-level
    // operator is AND), so this alone must answer false, exactly like "no probe
    // installed at all."
    const bool queueActive = false;
    assets->SetCookPendingProbe([&](const Arcane::Guid&)
    {
        if (!queueActive) return false;
        std::error_code existsEc;
        return fs::exists(png, existsEc);
    });

    // THE REGRESSION PIN: pre-fix, IsCookPending's row-absence default would
    // have answered "pending" here (nothing has ever refused this guid yet, so
    // no row exists) and quieted this forever. Post-fix, "queue idle" answers
    // false unconditionally -- loud, exactly pre-seam.
    CHECK(assets->TextureInfoFor(guid) == nullptr);
    CHECK(assets->PixelsFor(guid) == nullptr);
    CHECK(assets->ArtifactFor(guid) == nullptr);

    CHECK(Arcane::ContentArtifactRefusalObserved());
    CHECK(Arcane::ContentArtifactRefusalDetail().find("ArtifactMissing") != std::string::npos);

    fs::remove_all(root, ec);
}

TEST_CASE("assets: cook-pending probe's positive-signal shape -- queue active AND the guid's "
          "source still exists on disk quiets the legitimate pending case (desk-fix 2, "
          "review fix)", "[assets][pixels][artifact][cook]")
{
    ScopedContentArtifactRefusalLatch latchGuard;
    REQUIRE_FALSE(Arcane::ContentArtifactRefusalObserved());

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_artifact_sandbox" / "review_fix_active_exists";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Content" / "textures");

    const Arcane::Guid guid = Arcane::Guid::Generate();
    const fs::path png = root / "Content" / "textures" / "pending.png";
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

    const bool queueActive = true;
    assets->SetCookPendingProbe([&](const Arcane::Guid&)
    {
        if (!queueActive) return false;
        std::error_code existsEc;
        return fs::exists(png, existsEc);
    });

    CHECK(assets->ArtifactFor(guid) == nullptr);
    CHECK_FALSE(Arcane::ContentArtifactRefusalObserved());   // quiet: the legitimate pending case

    fs::remove_all(root, ec);
}

TEST_CASE("assets: cook-pending probe's positive-signal shape -- queue active but the guid's "
          "source has been DELETED still refuses loudly, the file-exists conjunct firing "
          "(desk-fix 2, review fix, watched)", "[assets][pixels][artifact][cook]")
{
    ScopedContentArtifactRefusalLatch latchGuard;
    REQUIRE_FALSE(Arcane::ContentArtifactRefusalObserved());

    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane_assets_artifact_sandbox" / "review_fix_active_deleted";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Content" / "textures");

    const Arcane::Guid guid = Arcane::Guid::Generate();
    const fs::path png = root / "Content" / "textures" / "deleted.png";
    const auto checker = CheckerPixels();
    REQUIRE(stbi_write_png(png.string().c_str(), static_cast<int>(kCheckerW), static_cast<int>(kCheckerH),
                           4, checker.data(), static_cast<int>(kCheckerW) * 4) != 0);

    auto assets = Arcane::Assets::Create();
    assets->SetContentRoot(root / "Content");
    // The registry keeps resolving this guid to `png` even after the file is
    // deleted below -- exactly "a registry entry outliving its file" (the
    // review finding's own phrasing).
    assets->SetAssetResolver([&](const Arcane::AssetId& id) -> std::optional<fs::path>
    {
        if (id.Value() == guid) return png;
        return std::nullopt;
    });

    const bool queueActive = true;
    assets->SetCookPendingProbe([&](const Arcane::Guid&)
    {
        if (!queueActive) return false;
        std::error_code existsEc;
        return fs::exists(png, existsEc);
    });

    // Delete the source AFTER registration -- a rename/delete race, the exact
    // scenario that regressed pre-fix.
    fs::remove(png, ec);
    REQUIRE_FALSE(fs::exists(png));

    // THE PIN: queue activity ALONE is not enough -- the file-exists conjunct is
    // what fires here, refusing loudly even mid-pass.
    CHECK(assets->ArtifactFor(guid) == nullptr);
    CHECK(Arcane::ContentArtifactRefusalObserved());
    CHECK(Arcane::ContentArtifactRefusalDetail().find("ArtifactMissing") != std::string::npos);

    fs::remove_all(root, ec);
}
