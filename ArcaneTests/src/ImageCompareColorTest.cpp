// Colour math for the image comparator. Every constant here is Playwright's
// (packages/utils/image_tools/colorUtils.ts, Apache-2.0, (c) Microsoft) and is
// DERIVED, not tuned: dE94's 1.0 is the just-noticeable-difference. Do not fit
// these to our hardware -- Task 6's conformance corpus asserts bit-parity with
// upstream, and a "better" constant breaks it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Assets/ImageCompare.hpp>

using Catch::Approx;

TEST_CASE("compare: BlendWithWhite is the upstream formula and TRUNCATES into a byte", "[compare]")
{
    // 255 + (c - 255) * a
    CHECK(Arcane::BlendWithWhite(0.0, 1.0)   == Approx(0.0));
    CHECK(Arcane::BlendWithWhite(0.0, 0.0)   == Approx(255.0));
    CHECK(Arcane::BlendWithWhite(0.0, 0.1)   == Approx(229.5));
    CHECK(Arcane::BlendWithWhite(255.0, 0.5) == Approx(255.0));

    // The trap: JS assigns this double into a Uint8Array, which TRUNCATES.
    // 229.5 must become 229, not 230.
    CHECK(static_cast<unsigned char>(Arcane::BlendWithWhite(0.0, 0.1)) == 229);
}

TEST_CASE("compare: Rgb2Gray is SSIM.js's exact integer formula", "[compare]")
{
    // (77*r + 150*g + 29*b + 128) >> 8
    CHECK(Arcane::Rgb2Gray(0, 0, 0)       == 0);
    CHECK(Arcane::Rgb2Gray(255, 255, 255) == 255);
    CHECK(Arcane::Rgb2Gray(255, 0, 0)     == ((77 * 255 + 128) >> 8));
    CHECK(Arcane::Rgb2Gray(0, 255, 0)     == ((150 * 255 + 128) >> 8));
    CHECK(Arcane::Rgb2Gray(0, 0, 255)     == ((29 * 255 + 128) >> 8));
}

TEST_CASE("compare: ColorDeltaE94 is zero for identical colours", "[compare]")
{
    const double a[3] = { 123.0, 45.0, 200.0 };
    CHECK(Arcane::ColorDeltaE94(a, a) == Approx(0.0).margin(1e-12));
}

TEST_CASE("compare: ColorDeltaE94 puts pure black and pure white ~100 apart", "[compare]")
{
    // L* runs 0..100, so black-vs-white is the full-scale case.
    const double black[3] = { 0.0, 0.0, 0.0 };
    const double white[3] = { 255.0, 255.0, 255.0 };
    CHECK(Arcane::ColorDeltaE94(black, white) == Approx(100.0).margin(0.5));
}

TEST_CASE("compare: ColorDeltaE94 is ASYMMETRIC -- argument order is load-bearing", "[compare]")
{
    // sC and sH are built from rgb1's chroma ALONE (colorUtils.ts: sC = 1 + k1*c1,
    // sH = 1 + k2*c1). Swapping the arguments is therefore NOT a no-op, which is
    // why compare() must always be called (expected, actual) and never the reverse.
    const double grey[3]      = { 128.0, 128.0, 128.0 };
    const double saturated[3] = { 200.0,  20.0,  20.0 };
    const double forward  = Arcane::ColorDeltaE94(grey, saturated);
    const double backward = Arcane::ColorDeltaE94(saturated, grey);
    CHECK(forward != Approx(backward));
}

TEST_CASE("compare: a 1-byte channel step on mid-grey is BELOW the JND", "[compare]")
{
    // The whole reason stage 2 exists: the format=9 vs format=11 conversion
    // produces small per-channel deltas that a perceptual test must absorb.
    const double a[3] = { 128.0, 128.0, 128.0 };
    const double b[3] = { 129.0, 128.0, 128.0 };
    CHECK(Arcane::ColorDeltaE94(a, b) < 1.0);
}

TEST_CASE("compare: Srgb2Xyz applies the sRGB transfer function, not a plain power", "[compare]")
{
    // The linear segment below 0.04045 is the part a naive gamma-2.2 port loses.
    double xyz[3] = {};
    const double nearBlack[3] = { 2.0, 2.0, 2.0 };   // 2/255 = 0.00784 -> linear segment
    Arcane::Srgb2Xyz(nearBlack, xyz);
    const double expectedLinear = (2.0 / 255.0) / 12.92;
    CHECK(xyz[1] == Approx(expectedLinear).epsilon(1e-9));
}
