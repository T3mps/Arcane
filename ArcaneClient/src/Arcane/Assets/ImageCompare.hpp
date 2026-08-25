#pragma once

// The image comparator: a device-free, perceptually-sound answer to "are these
// two captures the same picture?".
//
// PROVENANCE. This is a reimplementation of Playwright's comparator --
// packages/utils/image_tools/{compare,colorUtils,imageChannel,stats}.ts and
// packages/utils/comparators.ts, Apache License 2.0, Copyright (c) Microsoft
// Corporation. Attribution is in NOTICE.md. Every constant below is theirs and
// is DERIVED rather than tuned; dE94's 1.0 is the just-noticeable-difference.
//
// BIT-PARITY IS A REQUIREMENT. Their arithmetic is IEEE 754 binary64 and so is
// C++ double, so an identical input must produce an identical differing-pixel
// count. ImageCompareConformanceTest.cpp asserts exactly that against their own
// fixture corpus. This is why every accumulator here is `double` even where an
// integer would be exact, and why no arithmetic is "improved" -- see
// FastStats' own comment.
//
// Device-free on purpose, beside ImageIo.hpp, whose header comment already
// names "an image comparator" as the consumer it was split out for.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Assets/ImageIo.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Arcane
{
    // ---- colour math (colorUtils.ts) ------------------------------------

    // 255 + (c - 255) * a -- composite `c` against a white background at
    // opacity `a`. Returns a DOUBLE: upstream assigns the result straight into
    // a Uint8Array, which TRUNCATES toward zero. Callers that need a byte must
    // static_cast, never round, or the diff image and the padded channels both
    // drift one level from upstream.
    [[nodiscard]] ARCANE_API double BlendWithWhite(double c, double a) noexcept;

    // (77*r + 150*g + 29*b + 128) >> 8 -- the exact integer formula from
    // SSIM.js, used only to tint unchanged pixels in the diff image.
    [[nodiscard]] ARCANE_API int Rgb2Gray(int r, int g, int b) noexcept;

    // sRGB (0..255 per channel) -> 1-normalised CIE XYZ, D65. Applies the real
    // sRGB transfer function including its linear segment below 0.04045 -- a
    // plain pow(c, 2.2) is wrong in the shadows and would fail conformance.
    ARCANE_API void Srgb2Xyz(const double rgb[3], double xyz[3]) noexcept;

    // 1-normalised CIE XYZ (D65) -> L*a*b*.
    ARCANE_API void Xyz2Lab(const double xyz[3], double lab[3]) noexcept;

    // CIE94 perceived colour difference, "graphic arts" weights
    // (k1=0.045, k2=0.015, kL=kC=kH=1). 1.0 is the just-noticeable-difference.
    //
    // ASYMMETRIC: sC and sH are built from rgb1's chroma alone, so
    // ColorDeltaE94(a, b) != ColorDeltaE94(b, a) in general. rgb1 is always the
    // EXPECTED image. Do not "fix" this into a symmetric formula.
    [[nodiscard]] ARCANE_API double ColorDeltaE94(const double rgb1[3], const double rgb2[3]) noexcept;

    // ---- channels (imageChannel.ts) --------------------------------------

    // One 8-bit plane of an image, optionally surrounded by padding so that a
    // window centred on a real pixel never runs off the end.
    struct ARCANE_API ImageChannel
    {
        std::uint32_t width = 0, height = 0;
        std::vector<unsigned char> data;

        [[nodiscard]] unsigned char Get(std::uint32_t x, std::uint32_t y) const noexcept
        {
            return data[static_cast<std::size_t>(y) * width + x];
        }

        // Clamp a possibly-NEGATIVE window corner into the plane. Takes
        // int64_t deliberately: callers pass (x - SSIM_WINDOW_RADIUS), which
        // goes negative near the edge, and doing that subtraction in an
        // unsigned type wraps instead of clamping.
        void BoundXY(std::int64_t x, std::int64_t y,
                     std::uint32_t& outX, std::uint32_t& outY) const noexcept;
    };

    // The padding fill. Upstream alternates two colours on (x + y) % 2 so a
    // window overlapping the border can never see a uniform field -- a uniform
    // field has zero variance, which the flood-fill stage reads as "this cannot
    // be antialiasing", which would classify every border pixel as a real
    // difference. The checkerboard is load-bearing, not decoration.
    //
    // NOTE the values: compare.ts passes even = magenta, odd = green, which is
    // the OPPOSITE of imageChannel.ts's own defaults. These fields carry
    // compare.ts's assignment, because that is the call site that matters.
    struct PaddingOptions
    {
        std::uint32_t paddingSize = 0;
        unsigned char colorEven[3] = { 255, 0, 255 };
        unsigned char colorOdd[3]  = { 0, 255, 0 };
    };

    // Split tight RGBA8 into three padded planes, compositing each channel
    // against WHITE using the pixel's own alpha first. Our captures are opaque,
    // so the blend is a no-op on them -- but size-mismatch padding is
    // transparent black, which must read as white, and the conformance corpus
    // contains genuinely translucent fixtures.
    ARCANE_API void IntoRgb(std::uint32_t width, std::uint32_t height,
                            const unsigned char* rgba, const PaddingOptions& options,
                            ImageChannel& r, ImageChannel& g, ImageChannel& b);

    // ---- windowed statistics (stats.ts) ----------------------------------

    // Five summed-area tables over a pair of planes, so any rectangular
    // window's mean, variance and covariance are O(1).
    //
    // MEMORY: five tables of width*height doubles, per channel. At 1280x720
    // padded to 1310x750 that is ~39 MB per channel and ~118 MB for three. This
    // is why compare() builds them LAZILY -- only once some pixel has already
    // failed the exact and dE94 stages.
    //
    // WHY DOUBLE, NOT UINT64: upstream accumulates in a JS number[], i.e.
    // double. The partial sums themselves are exact in double (max ~6.4e10),
    // but variance computes sum*sum, which reaches ~6.3e16 -- past 2^53, where
    // double rounds. Exact integer arithmetic would produce a slightly
    // different variance and break the bit-parity ImageCompareConformanceTest
    // asserts. The rounding is inherited deliberately.
    class ARCANE_API FastStats
    {
    public:
        // Both planes must have identical dimensions.
        FastStats(const ImageChannel& c1, const ImageChannel& c2);

        // Inclusive window corners, already clamped by ImageChannel::BoundXY.
        [[nodiscard]] double MeanC1(std::uint32_t x1, std::uint32_t y1,
                                    std::uint32_t x2, std::uint32_t y2) const noexcept;
        [[nodiscard]] double MeanC2(std::uint32_t x1, std::uint32_t y1,
                                    std::uint32_t x2, std::uint32_t y2) const noexcept;
        // POPULATION variance (divides by N, not N-1) -- matches upstream.
        [[nodiscard]] double VarianceC1(std::uint32_t x1, std::uint32_t y1,
                                        std::uint32_t x2, std::uint32_t y2) const noexcept;
        [[nodiscard]] double VarianceC2(std::uint32_t x1, std::uint32_t y1,
                                        std::uint32_t x2, std::uint32_t y2) const noexcept;
        [[nodiscard]] double Covariance(std::uint32_t x1, std::uint32_t y1,
                                        std::uint32_t x2, std::uint32_t y2) const noexcept;

    private:
        [[nodiscard]] double Sum(const std::vector<double>& table,
                                 std::uint32_t x1, std::uint32_t y1,
                                 std::uint32_t x2, std::uint32_t y2) const noexcept;

        std::uint32_t m_width = 0, m_height = 0;
        std::vector<double> m_sumC1, m_sumC2, m_sumSq1, m_sumSq2, m_sumMult;
    };

    // Structural similarity over the given window, averaged nowhere -- this is
    // ONE channel. compare() averages the three itself. Stabilising constants
    // are (0.01 * 255)^2 and (0.03 * 255)^2, the standard SSIM choices for an
    // 8-bit dynamic range.
    [[nodiscard]] ARCANE_API double Ssim(const FastStats& stats,
                                         std::uint32_t x1, std::uint32_t y1,
                                         std::uint32_t x2, std::uint32_t y2) noexcept;

    // ---- the cascade (compare.ts) ----------------------------------------

    struct CompareOptions
    {
        // The just-noticeable-difference. DERIVED, not tuned -- see
        // ColorDeltaE94. Raising it to make a flaky test pass is the wrong
        // lever; the aggregate knob in ImageCompareOptions is the right one.
        double maxColorDeltaE94 = 1.0;
    };

    // Count the pixels that genuinely differ between two tight RGBA8 images of
    // the same size, four stages, cheapest first:
    //
    //   1. exact RGB equality              -> not a difference
    //   2. colorDeltaE94 <= 1.0            -> below the JND, not a difference
    //   3. 3x3 flood fill in EITHER image  -> cannot be antialiasing, IS a difference
    //   4. SSIM over 31x31 >= 0.99         -> antialiasing, not a difference
    //
    // `expected` and `actual` must both be width*height*4 bytes. `diff`, if not
    // null, must be the same size and is painted: red for a real difference,
    // yellow for a pixel classified as antialiasing, and a washed-out grey of
    // EXPECTED everywhere else.
    //
    // ARGUMENT ORDER IS SIGNIFICANT. dE94 is asymmetric and the grey is drawn
    // from `expected`; calling this (actual, expected) produces a different
    // number.
    [[nodiscard]] ARCANE_API std::uint64_t Compare(
        const unsigned char* expected, const unsigned char* actual,
        unsigned char* diff,
        std::uint32_t width, std::uint32_t height,
        const CompareOptions& options = {});

    // ---- image-level entry point (comparators.ts) ------------------------
    //
    // CompareImages is the entry point this header is FOR -- the one call an
    // outside consumer should make. Everything above (ImageChannel, FastStats,
    // Compare, ...) is ARCANE_API-exported only so ArcaneTests can exercise
    // the cascade's internals directly across the DLL boundary; it is test
    // surface, not a menu of public API to build against.

    struct ImageCompareOptions
    {
        // The two knobs are INDEPENDENT: one asks "is this pixel different",
        // the other "how many differing pixels are acceptable". If both are
        // set the SMALLER budget wins; if neither is set the budget is ZERO.
        std::optional<std::uint64_t> maxDiffPixels;
        std::optional<double>        maxDiffPixelRatio;
        double maxColorDeltaE94 = 1.0;
    };

    struct ImageCompareResult
    {
        bool          passed = false;
        std::uint64_t diffCount = 0;
        double        diffRatio = 0.0;
        std::uint64_t maxDiffPixelsUsed = 0;   // the budget actually applied
        bool          sizesMismatch = false;
        std::uint32_t width = 0, height = 0;   // the compared (padded) extent
        // Empty iff passed. A size mismatch and a pixel-count failure are
        // reported TOGETHER, concatenated, never one instead of the other.
        std::string   errorMessage;
        // Tight RGBA8, only populated on failure -- the artifact that makes a
        // failure diagnosable rather than a number.
        std::vector<unsigned char> diffRgba;
    };

    // Compare two decoded images. A dimension mismatch is NOT an error: both
    // are padded to the per-axis maximum with transparent black anchored
    // top-left (which the channel split then composites to white), the
    // comparison still runs, and the mismatch is reported as its own named
    // fact beside the pixel count. Never rescales, never throws.
    [[nodiscard]] ARCANE_API ImageCompareResult CompareImages(
        const PixelData& expected, const PixelData& actual,
        const ImageCompareOptions& options = {});
}
