// The four-stage cascade: exact -> dE94 <= 1.0 -> 3x3 flood-fill variance ->
// SSIM 31x31 >= 0.99. Cheapest test first; each stage exists to suppress a
// false positive the previous one would raise.
//
// ARGUMENT ORDER IS LOAD-BEARING: Compare(expected, actual, ...). dE94 is
// asymmetric and the diff image's grey is drawn from EXPECTED.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ImageCompare.hpp>

#include <cstdint>
#include <vector>

namespace
{
    std::vector<unsigned char> Solid(std::uint32_t w, std::uint32_t h,
                                     unsigned char r, unsigned char g, unsigned char b)
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            px[i + 0] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255;
        }
        return px;
    }

    // A hard vertical edge: columns [0, edgeX) get (valA,valA,valA), columns
    // [edgeX, w) get (valB,valB,valB), uniform down every row. Used to reach
    // stage 4 (SSIM): a uniform field forces var == 0 and stage 3 fires
    // first, so exercising SSIM at all requires non-zero variance in BOTH
    // images, which only a real edge provides.
    std::vector<unsigned char> VerticalEdge(std::uint32_t w, std::uint32_t h, std::uint32_t edgeX,
                                            unsigned char valA, unsigned char valB)
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
        for (std::uint32_t y = 0; y < h; ++y)
        {
            for (std::uint32_t x = 0; x < w; ++x)
            {
                const unsigned char v = x < edgeX ? valA : valB;
                const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
                px[i + 0] = v; px[i + 1] = v; px[i + 2] = v; px[i + 3] = 255;
            }
        }
        return px;
    }

    void SetPixel(std::vector<unsigned char>& px, std::uint32_t w,
                  std::uint32_t x, std::uint32_t y,
                  unsigned char r, unsigned char g, unsigned char b)
    {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
        px[i + 0] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255;
    }
}

TEST_CASE("compare: identical images produce zero differing pixels", "[compare]")
{
    const auto a = Solid(64, 64, 30, 60, 90);
    CHECK(Arcane::Compare(a.data(), a.data(), nullptr, 64, 64) == 0);
}

TEST_CASE("compare: a single BLACK pixel on white is a real difference", "[compare]")
{
    // Survives all four stages: dE94 is enormous, and the 3x3 window around it
    // in the expected image is a flood fill, so stage 3 fires before SSIM.
    auto expected = Solid(64, 64, 255, 255, 255);
    auto actual   = expected;
    SetPixel(actual, 64, 32, 32, 0, 0, 0);

    CHECK(Arcane::Compare(expected.data(), actual.data(), nullptr, 64, 64) == 1);
}

TEST_CASE("compare: a sub-JND channel step is absorbed by stage 2", "[compare]")
{
    // 128 -> 129 on one channel is below the just-noticeable-difference, which
    // is exactly the format=9 vs format=11 conversion this stage exists for.
    auto expected = Solid(64, 64, 128, 128, 128);
    auto actual   = Solid(64, 64, 129, 128, 128);

    CHECK(Arcane::Compare(expected.data(), actual.data(), nullptr, 64, 64) == 0);
}

TEST_CASE("compare: the flood-fill stage classifies a lone change as REAL, not antialiasing", "[compare]")
{
    // A uniform field has zero variance, so a differing pixel inside one cannot
    // be an antialiasing artifact -- there is no edge to antialias.
    //
    // The altered pixel must be 100 -> 120, NOT 100 -> 160: at 160 the 31x31
    // SSIM window (fully interior on a 64x64 image, no padding contamination)
    // computes var1 = 0 (expected is uniform), m1 = 100, m2 = 100.0624,
    // var2 = 3.743, and the luminance factors cancel to ~1 because m1 ~ m2,
    // leaving SSIM ~= C2 / (var2 + C2) = 58.5225 / 62.266 ~= 0.9399 -- already
    // BELOW the 0.99 antialiasing threshold, so stage 4 alone would also
    // classify this as a real difference and deleting stage 3 would not
    // change the count. At 120 the same algebra gives var2 = 400*960/961^2
    // ~= 0.4158, SSIM ~= 58.5225 / 58.938 ~= 0.9929 -- AT OR ABOVE 0.99, so
    // stage 4 would forgive it as antialiasing (count 0) while dE94(grey 100,
    // grey 120) ~= 6.3 comfortably clears the 1.0 JND (stage 2 does not
    // absorb it) and var1 == 0 (stage 3 fires). With stage 3: count 1;
    // without it: count 0 -- a real discriminator for the flood-fill stage.
    auto expected = Solid(64, 64, 100, 100, 100);
    auto actual   = expected;
    SetPixel(actual, 64, 32, 32, 120, 120, 120);

    CHECK(Arcane::Compare(expected.data(), actual.data(), nullptr, 64, 64) == 1);
}

TEST_CASE("compare: stage 4 forgives an edge-adjacent perturbation as antialiasing", "[compare]")
{
    // A hard black/white vertical edge at column 32 gives BOTH images
    // non-zero 3x3 and 31x31 variance everywhere near the edge, so stage 3
    // never fires (var1 != 0 and var2 != 0) and every altered pixel here is
    // adjudicated by SSIM alone -- the antialiasing scenario stage 4 exists
    // for.
    //
    // Hand derivation (image-space coordinates, 64x64, edge at x=32, radius
    // 15 window around x=31 spans columns [16,46], fully interior):
    //   expected: 16 columns @0, 15 columns @255, N=961
    //     mean1 = 118575/961 = 123.38710, var1 = 16239.334
    //   actual: same, except pixel (31,32) changed from 0 to 128
    //     mean2 = 118703/961 = 123.52029, var2 = 16223.496
    //     cov   = 97.328 away from a perfect match -> cov = 16222.899
    //   SSIM = [(2*m1*m2+C1)(2*cov+C2)] / [(m1^2+m2^2+C1)(var1+var2+C2)]
    //        ~= 0.999999 * 0.999476 ~= 0.9995  -- >= 0.99, so this pixel is
    //   painted YELLOW and diffCount is NOT incremented for it. Since it is
    //   the only pixel that differs between the two images, the whole-image
    //   count must be 0.
    auto expected = VerticalEdge(64, 64, 32, 0, 255);
    auto actual   = expected;
    SetPixel(actual, 64, 31, 32, 128, 128, 128);

    std::vector<unsigned char> diff(static_cast<std::size_t>(64) * 64 * 4, 0);
    const std::uint64_t count =
        Arcane::Compare(expected.data(), actual.data(), diff.data(), 64, 64);
    CHECK(count == 0);

    const std::size_t hit = (static_cast<std::size_t>(32) * 64 + 31) * 4;
    CHECK(diff[hit + 0] == 255);
    CHECK(diff[hit + 1] == 255);
    CHECK(diff[hit + 2] == 0);
}

TEST_CASE("compare: stage 4 condemns an edge-adjacent perturbation as a real difference", "[compare]")
{
    // A shallow edge (0 vs 20 -- dE94(grey 0, grey 20) ~= 6.3, comfortably
    // above the 1.0 JND so stage 2 does not absorb it) has much smaller
    // baseline variance than a full 0/255 edge, so a single pixel driven all
    // the way to the opposite extreme (0 -> 255) is a much larger fraction of
    // that variance and CANNOT be mistaken for antialiasing.
    //
    // Hand derivation (columns [16,31] @0 = 16 cols, [32,46] @20 = 15 cols,
    // N=961, pixel (31,32) changed from 0 to 255):
    //   expected: mean1 = 9300/961 = 9.6774, var1 = 99.91
    //   actual:   mean2 = 9555/961 = 9.9428, var2 = 162.36
    //   cov = 97.328 (Sum(e*a)/N = 193.548, minus mean1*mean2 = 96.220)
    //   luminance factor ~= 0.9996 (m1 ~ m2)
    //   contrast factor  = (2*cov+C2)/(var1+var2+C2)
    //                    = 253.18 / 320.80 ~= 0.7892
    //   SSIM ~= 0.9996 * 0.7892 ~= 0.789 -- well BELOW 0.99, so this pixel is
    //   painted RED and diffCount IS incremented. It is the only pixel that
    //   differs, so the whole-image count must be 1.
    //
    // (The 3x3 window around (31,32) also has non-zero variance in both
    // images -- columns 30/31 @0 and 32 @20 in expected; the same plus the
    // one changed pixel in actual -- so stage 3 does not fire here either.)
    auto expected = VerticalEdge(64, 64, 32, 0, 20);
    auto actual   = expected;
    SetPixel(actual, 64, 31, 32, 255, 255, 255);

    std::vector<unsigned char> diff(static_cast<std::size_t>(64) * 64 * 4, 0);
    const std::uint64_t count =
        Arcane::Compare(expected.data(), actual.data(), diff.data(), 64, 64);
    CHECK(count == 1);

    const std::size_t hit = (static_cast<std::size_t>(32) * 64 + 31) * 4;
    CHECK(diff[hit + 0] == 255);
    CHECK(diff[hit + 1] == 0);
    CHECK(diff[hit + 2] == 0);
}

TEST_CASE("compare: the diff image paints red for a real difference and grey elsewhere", "[compare]")
{
    auto expected = Solid(16, 16, 255, 255, 255);
    auto actual   = expected;
    SetPixel(actual, 16, 8, 8, 0, 0, 0);

    std::vector<unsigned char> diff(static_cast<std::size_t>(16) * 16 * 4, 0);
    const std::uint64_t count =
        Arcane::Compare(expected.data(), actual.data(), diff.data(), 16, 16);
    CHECK(count == 1);

    const std::size_t hit = (static_cast<std::size_t>(8) * 16 + 8) * 4;
    CHECK(diff[hit + 0] == 255);
    CHECK(diff[hit + 1] == 0);
    CHECK(diff[hit + 2] == 0);
    CHECK(diff[hit + 3] == 255);

    // An unchanged white pixel: grey of white is 255, blended toward white at
    // 0.1 is still 255.
    const std::size_t bg = (static_cast<std::size_t>(2) * 16 + 2) * 4;
    CHECK(diff[bg + 0] == 255);
    CHECK(diff[bg + 1] == 255);
    CHECK(diff[bg + 2] == 255);

    // Every diff pixel is opaque, including the ones nothing wrote to.
    CHECK(diff[bg + 3] == 255);
}

TEST_CASE("compare: unchanged DARK pixels are blended toward white, not left dark", "[compare]")
{
    // BlendWithWhite(gray, 0.1) = 255 + (gray-255)*0.1. For black that is
    // 229.5, truncated to 229 -- so the diff image reads as a washed-out ghost
    // of the expected image rather than a copy of it.
    const auto expected = Solid(16, 16, 0, 0, 0);
    std::vector<unsigned char> diff(static_cast<std::size_t>(16) * 16 * 4, 0);
    CHECK(Arcane::Compare(expected.data(), expected.data(), diff.data(), 16, 16) == 0);

    CHECK(diff[0] == 229);
    CHECK(diff[1] == 229);
    CHECK(diff[2] == 229);
}

TEST_CASE("compare: a null diff buffer is legal and changes no count", "[compare]")
{
    auto expected = Solid(32, 32, 10, 10, 10);
    auto actual   = expected;
    SetPixel(actual, 32, 16, 16, 250, 250, 250);

    std::vector<unsigned char> diff(static_cast<std::size_t>(32) * 32 * 4, 0);
    const std::uint64_t withDiff =
        Arcane::Compare(expected.data(), actual.data(), diff.data(), 32, 32);
    const std::uint64_t without =
        Arcane::Compare(expected.data(), actual.data(), nullptr, 32, 32);
    CHECK(withDiff == without);
}
