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
