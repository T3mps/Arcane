// Windowed mean/variance/covariance via integral images, and SSIM on top.
//
// The accumulators are `double` ON PURPOSE even though the sums are integers:
// upstream stores them in a JS number[] (also double), and sum*sum reaches
// ~6.3e16, past 2^53, where double ROUNDS. Exact 64-bit integers would give a
// different -- arguably better -- variance, and would break the bit-parity
// that Task 6's conformance corpus depends on. Do not "fix" this.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Assets/ImageCompare.hpp>

#include <cstdint>
#include <vector>

using Catch::Approx;

namespace
{
    // Build a channel directly, bypassing IntoRgb, so the stats are tested
    // against values chosen by hand rather than by a blend.
    Arcane::ImageChannel Channel(std::uint32_t w, std::uint32_t h, unsigned char value)
    {
        Arcane::ImageChannel c;
        c.width = w; c.height = h;
        c.data.assign(static_cast<std::size_t>(w) * h, value);
        return c;
    }
}

TEST_CASE("compare: mean over a uniform window is the uniform value", "[compare]")
{
    const auto a = Channel(8, 8, 100);
    const auto b = Channel(8, 8, 200);
    const Arcane::FastStats s(a, b);

    CHECK(s.MeanC1(0, 0, 7, 7) == Approx(100.0));
    CHECK(s.MeanC2(0, 0, 7, 7) == Approx(200.0));
    CHECK(s.MeanC1(2, 2, 4, 4) == Approx(100.0));
}

TEST_CASE("compare: variance of a FLOOD FILL is exactly zero -- the stage-3 predicate", "[compare]")
{
    // The flood-fill test is `var1 == 0 || var2 == 0`, an exact comparison
    // against zero. If the variance of a uniform window is 1e-13 instead of 0,
    // stage 3 silently stops firing and everything falls through to SSIM.
    const auto a = Channel(8, 8, 77);
    const auto b = Channel(8, 8, 77);
    const Arcane::FastStats s(a, b);

    CHECK(s.VarianceC1(0, 0, 7, 7) == 0.0);
    CHECK(s.VarianceC2(0, 0, 7, 7) == 0.0);
}

TEST_CASE("compare: variance of a two-value checkerboard is population variance", "[compare]")
{
    Arcane::ImageChannel a;
    a.width = 2; a.height = 2;
    a.data = { 0, 255, 255, 0 };
    const auto b = Channel(2, 2, 0);
    const Arcane::FastStats s(a, b);

    // Population variance of {0,255,255,0}: mean 127.5, var = 127.5^2.
    CHECK(s.VarianceC1(0, 0, 1, 1) == Approx(127.5 * 127.5));
}

TEST_CASE("compare: covariance of identical planes equals their variance", "[compare]")
{
    Arcane::ImageChannel a;
    a.width = 2; a.height = 2;
    a.data = { 0, 255, 255, 0 };
    const Arcane::FastStats s(a, a);

    CHECK(s.Covariance(0, 0, 1, 1) == Approx(s.VarianceC1(0, 0, 1, 1)));
}

TEST_CASE("compare: SSIM of a plane against ITSELF is 1", "[compare]")
{
    Arcane::ImageChannel a;
    a.width = 4; a.height = 4;
    a.data = { 0, 40, 80, 120,  160, 200, 240, 255,  10, 50, 90, 130,  170, 210, 250, 5 };
    const Arcane::FastStats s(a, a);

    CHECK(Arcane::Ssim(s, 0, 0, 3, 3) == Approx(1.0).epsilon(1e-12));
}

TEST_CASE("compare: SSIM of black against white is far below the antialiasing threshold", "[compare]")
{
    const auto black = Channel(4, 4, 0);
    const auto white = Channel(4, 4, 255);
    const Arcane::FastStats s(black, white);

    CHECK(Arcane::Ssim(s, 0, 0, 3, 3) < 0.99);
}

TEST_CASE("compare: a one-pixel window is legal and does not divide by zero", "[compare]")
{
    // The window helpers clamp, so a corner pixel can produce x1==x2, y1==y2.
    const auto a = Channel(4, 4, 60);
    const auto b = Channel(4, 4, 60);
    const Arcane::FastStats s(a, b);

    CHECK(s.MeanC1(2, 2, 2, 2) == Approx(60.0));
    CHECK(s.VarianceC1(2, 2, 2, 2) == 0.0);
}

TEST_CASE("compare: SSIM algebraically reduces to the luminance factor alone -- pins C1", "[compare]")
{
    // Case A, derived BY HAND (not by running the code under test): uniform
    // plane of 100 against uniform plane of 200, 4x4, window (0,0,3,3).
    // var1 = var2 = cov = 0 here, so the contrast/structure factor
    // (2*cov + C2) / (var1 + var2 + C2) is C2/C2 = 1 and cancels, leaving:
    //   C1   = (0.01*255)^2 = 6.5025
    //   SSIM = (2*100*200 + C1) / (100^2 + 200^2 + C1)
    //        = 40006.5025 / 50006.5025
    //        ~= 0.800026007
    // This is a real check unlike "SSIM of a plane against itself is 1"
    // above: a wrong C1, or a wrong luminance-factor shape, moves this
    // number away from the hand-derived quotient.
    const auto a = Channel(4, 4, 100);
    const auto b = Channel(4, 4, 200);
    const Arcane::FastStats s(a, b);

    CHECK(Arcane::Ssim(s, 0, 0, 3, 3) == Approx(40006.5025 / 50006.5025).epsilon(1e-12));
}

TEST_CASE("compare: SSIM algebraically reduces to the contrast factor alone -- pins C2 and covariance's sign", "[compare]")
{
    // Case B, derived BY HAND: a = {0,255,255,0} against b = {255,0,0,255},
    // 2x2, window (0,0,1,1). mean1 == mean2 == 127.5 here, so the luminance
    // factor (2*mean1*mean2 + C1) / (mean1^2 + mean2^2 + C1) is X/X = 1 and
    // cancels, leaving:
    //   var1 = var2 = 16256.25             (population variance of {0,255,255,0})
    //   cov  = (0 - 510*510/4) / 4 = -16256.25
    //   C2   = (0.03*255)^2 = 58.5225
    //   SSIM = (2*cov + C2) / (var1 + var2 + C2)
    //        = -32453.9775 / 32571.0225
    //        ~= -0.996407
    // This pins the SIGN of the covariance term as well as C2: flip either
    // and the result moves off this hand-derived quotient.
    Arcane::ImageChannel a;
    a.width = 2; a.height = 2;
    a.data = { 0, 255, 255, 0 };
    Arcane::ImageChannel b;
    b.width = 2; b.height = 2;
    b.data = { 255, 0, 0, 255 };
    const Arcane::FastStats s(a, b);

    CHECK(Arcane::Ssim(s, 0, 0, 1, 1) == Approx(-32453.9775 / 32571.0225).epsilon(1e-12));
}

TEST_CASE("compare: an INTERIOR window exercises Sum's subtraction branches", "[compare]")
{
    // Every other case in this file is either uniform data (any correctly
    // sized window of a constant field gives the same answer no matter
    // where the corners land) or anchored at (0,0) spanning the full
    // extent (where Sum's `if (x1 > 0)` / `if (y1 > 0)` / `if (x1 > 0 &&
    // y1 > 0)` subtraction branches never fire against distinguishable
    // values). This is the one case that actually exercises those
    // branches, on non-uniform data.
    //
    // a = 0..15 row-major on a 4x4 channel; b = 15 - a. Window (1,1)-(3,2)
    // is deliberately RECTANGULAR (3 wide x 2 tall), not square: a square
    // window centred symmetrically maps onto itself under an x/y
    // transposition, so a transposed-index bug in Sum/recalc would read
    // the same value set and still pass. This window has no such
    // symmetry, so it catches transposition as well as an off-by-one.
    //
    // Window values, hand-derived (not by running the code under test):
    //   a: {5, 6, 7, 9, 10, 11}   sum 48   mean 8   sumSq 412
    //   b: {10, 9, 8, 6, 5, 4}    sum 42   mean 7   sumSq 322
    //   varianceA = (412 - 48*48/6) / 6 = 28/6
    //   varianceB = (322 - 42*42/6) / 6 = 28/6
    //   sumMult    = 5*10 + 6*9 + 7*8 + 9*6 + 10*5 + 11*4 = 308
    //   covariance = (308 - 48*42/6) / 6 = -28/6
    // b = 15 - a forces covariance == -variance, an independent check on
    // the arithmetic above. The two means being DIFFERENT (8 vs 7) is
    // deliberate too: it makes a MeanC1/MeanC2 (or C1/C2 table) mix-up
    // visible, which equal means would hide.
    Arcane::ImageChannel a;
    a.width = 4; a.height = 4;
    a.data = { 0, 1, 2, 3,  4, 5, 6, 7,  8, 9, 10, 11,  12, 13, 14, 15 };
    Arcane::ImageChannel b;
    b.width = 4; b.height = 4;
    b.data = { 15, 14, 13, 12,  11, 10, 9, 8,  7, 6, 5, 4,  3, 2, 1, 0 };
    const Arcane::FastStats s(a, b);

    CHECK(s.MeanC1(1, 1, 3, 2) == Approx(8.0));
    CHECK(s.MeanC2(1, 1, 3, 2) == Approx(7.0));
    CHECK(s.VarianceC1(1, 1, 3, 2) == Approx(28.0 / 6.0));
    CHECK(s.VarianceC2(1, 1, 3, 2) == Approx(28.0 / 6.0));
    CHECK(s.Covariance(1, 1, 3, 2) == Approx(-28.0 / 6.0));
}
