// Golden-image comparator (NRI Phase 0): pure, device-free. The port's
// verdict-giver -- so its own behavior is pinned first. ([golden])

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/GoldenImage.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

using Arcane::CompareRgbaImages;
using Arcane::GoldenCompareParams;
using Arcane::GoldenCompareResult;

namespace
{
    std::vector<unsigned char> Flat(std::uint32_t w, std::uint32_t h, unsigned char v)
    {
        return std::vector<unsigned char>(static_cast<std::size_t>(w) * h * 4, v);
    }
}

TEST_CASE("golden: identical images pass with zero stats", "[golden]")
{
    const auto img = Flat(8, 8, 128);
    const GoldenCompareResult r =
        CompareRgbaImages(img.data(), 8, 8, img.data(), 8, 8);
    CHECK(r.ok);
    CHECK(r.dimensionsMatch);
    CHECK(r.badPixelFraction == 0.0f);
    CHECK(r.maxChannelDelta == 0);
}

TEST_CASE("golden: deltas within channel tolerance are clean", "[golden]")
{
    auto a = Flat(8, 8, 128);
    auto b = Flat(8, 8, 128);
    b[0] = 130;   // +2 on one channel == default tolerance boundary
    const GoldenCompareResult r =
        CompareRgbaImages(a.data(), 8, 8, b.data(), 8, 8);
    CHECK(r.ok);
    CHECK(r.badPixelFraction == 0.0f);
    CHECK(r.maxChannelDelta == 2);   // observed, but tolerated
}

TEST_CASE("golden: a pixel past tolerance is counted and located", "[golden]")
{
    auto a = Flat(8, 8, 128);
    auto b = Flat(8, 8, 128);
    // pixel (x=3, y=2), green channel, +10
    const std::size_t idx = ((2u * 8u) + 3u) * 4u + 1u;
    b[idx] = 138;
    GoldenCompareParams p;                 // tolerance 2, maxBadFraction 0.001
    const GoldenCompareResult r =
        CompareRgbaImages(a.data(), 8, 8, b.data(), 8, 8, p);
    CHECK_FALSE(r.ok);                     // 1/64 = 1.5% > 0.1%
    CHECK(r.maxChannelDelta == 10);
    CHECK(r.firstBadX == 3);
    CHECK(r.firstBadY == 2);
    CHECK(r.badPixelFraction > 0.015f);
    CHECK(r.badPixelFraction < 0.016f);
}

TEST_CASE("golden: bad-pixel budget tolerates edge wobble", "[golden]")
{
    auto a = Flat(100, 100, 50);
    auto b = Flat(100, 100, 50);
    for (int i = 0; i < 5; ++i)            // 5 of 10000 pixels wildly off
        b[static_cast<std::size_t>(i) * 4] = 255;
    GoldenCompareParams p;
    p.maxBadPixelFraction = 0.001f;        // 10 pixels allowed
    CHECK(CompareRgbaImages(a.data(), 100, 100, b.data(), 100, 100, p).ok);
    p.maxBadPixelFraction = 0.0001f;       // 1 pixel allowed
    CHECK_FALSE(CompareRgbaImages(a.data(), 100, 100, b.data(), 100, 100, p).ok);
}

TEST_CASE("golden: dimension mismatch fails loudly, never reads memory", "[golden]")
{
    const auto a = Flat(8, 8, 0);
    const auto b = Flat(4, 4, 0);
    const GoldenCompareResult r =
        CompareRgbaImages(a.data(), 8, 8, b.data(), 4, 4);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.dimensionsMatch);
}

TEST_CASE("golden: diff png lands on disk for a failed compare", "[golden]")
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "arcane_golden_diff";
    std::filesystem::create_directories(dir);
    auto a = Flat(8, 8, 128);
    auto b = Flat(8, 8, 128);
    b[0] = 255;
    const std::filesystem::path diff = dir / "out.diff.png";
    REQUIRE(Arcane::WriteDiffPng(diff, a.data(), b.data(), 8, 8, 2));
    CHECK(std::filesystem::exists(diff));
    std::filesystem::remove_all(dir);
}
