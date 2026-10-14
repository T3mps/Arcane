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

TEST_CASE("compare: Xyz2Lab pins the white point and the linear branch directly", "[compare]")
{
    // The trap: Xyz2Lab is declared "exposed for tests" but every case above
    // exercises it only transitively through ColorDeltaE94, where a sign error
    // in fx/fy/fz would cancel under the paired deltaL/deltaA/deltaB
    // subtraction and still pass. These two cases pin fx, fy, fz directly.

    // D65 white point maps to Lab's origin: L*=100 (top of the 0..100 scale),
    // a*=b*=0 (achromatic). Exact, not approximate -- pins both white-point
    // divisors (0.950489, 1.088840) and the 116*fy-16 term.
    {
        const double xyz[3] = { 0.950489, 1.0, 1.088840 };
        double lab[3] = {};
        Arcane::Xyz2Lab(xyz, lab);
        CHECK(lab[0] == Approx(100.0).margin(1e-9));
        CHECK(lab[1] == Approx(0.0).margin(1e-9));
        CHECK(lab[2] == Approx(0.0).margin(1e-9));
    }

    // Below (6/29)^3 ~ 0.008856, f(v) switches to the linear branch
    // v/3/(6/29)^2 + 4/29. Three DISTINCT values under the threshold for
    // x, y, z (rather than one shared value) so fx != fy != fz -- a sign
    // error confined to a single channel cannot cancel the way it could in
    // the ColorDeltaE94 cases above. Expected values are the piecewise
    // formula written out here, not a magic number computed once against
    // the implementation.
    {
        constexpr double kSigmaPow2 = 6.0 * 6.0 / 29.0 / 29.0;
        constexpr double kSigmaPow3 = 6.0 * 6.0 * 6.0 / 29.0 / 29.0 / 29.0;
        const double vx = 0.002, vy = 0.005, vz = 0.007;
        REQUIRE(vx < kSigmaPow3);
        REQUIRE(vy < kSigmaPow3);
        REQUIRE(vz < kSigmaPow3);

        // Chosen so that xyz[i]/divisor recovers vx/vy/vz inside Xyz2Lab.
        const double xyz[3] = { vx * 0.950489, vy, vz * 1.088840 };
        double lab[3] = {};
        Arcane::Xyz2Lab(xyz, lab);

        // Mirrors Xyz2Lab's own white-point division so fx/fy/fz below come
        // from the SAME operands in the SAME order as the implementation.
        const double x = xyz[0] / 0.950489;
        const double y = xyz[1];
        const double z = xyz[2] / 1.088840;
        auto fLinear = [&](double v) { return v / 3.0 / kSigmaPow2 + 4.0 / 29.0; };
        const double fx = fLinear(x), fy = fLinear(y), fz = fLinear(z);

        CHECK(lab[0] == Approx(116.0 * fy - 16.0).epsilon(1e-9));
        CHECK(lab[1] == Approx(500.0 * (fx - fy)).epsilon(1e-9));
        CHECK(lab[2] == Approx(200.0 * (fy - fz)).epsilon(1e-9));
    }
}
