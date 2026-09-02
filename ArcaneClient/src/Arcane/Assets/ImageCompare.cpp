#include <Arcane/Assets/ImageCompare.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace Arcane
{
    double BlendWithWhite(double c, double a) noexcept
    {
        return 255.0 + (c - 255.0) * a;
    }

    int Rgb2Gray(int r, int g, int b) noexcept
    {
        return (77 * r + 150 * g + 29 * b + 128) >> 8;
    }

    void Srgb2Xyz(const double rgb[3], double xyz[3]) noexcept
    {
        auto toLinear = [](double v) noexcept
        {
            v /= 255.0;
            return (v > 0.04045) ? std::pow((v + 0.055) / 1.055, 2.4) : v / 12.92;
        };
        const double r = toLinear(rgb[0]);
        const double g = toLinear(rgb[1]);
        const double b = toLinear(rgb[2]);

        xyz[0] = r * 0.4124 + g * 0.3576 + b * 0.1805;
        xyz[1] = r * 0.2126 + g * 0.7152 + b * 0.0722;
        xyz[2] = r * 0.0193 + g * 0.1192 + b * 0.9505;
    }

    void Xyz2Lab(const double xyz[3], double lab[3]) noexcept
    {
        // sigma = 6/29; the piecewise split keeps the curve finite-sloped at 0.
        constexpr double kSigmaPow2 = 6.0 * 6.0 / 29.0 / 29.0;
        constexpr double kSigmaPow3 = 6.0 * 6.0 * 6.0 / 29.0 / 29.0 / 29.0;

        const double x = xyz[0] / 0.950489;
        const double y = xyz[1];
        const double z = xyz[2] / 1.088840;

        // Upstream is `x ** (1/3)`; std::pow(v, 1.0/3.0) is the literal port of
        // that call, not std::cbrt. Bit-parity binds every arithmetic choice
        // here, and pow(v, 1/3) and a correctly-rounded cbrt are NOT the same
        // function -- JS's exponent is the inexact double 0.3333333333333333,
        // so the two diverge by several ULPs in the shadows, which is exactly
        // where Lab spends its precision and this function feeds every dE94.
        // It is the `v > kSigmaPow3` guard above -- not the function choice --
        // that excludes the negative branch where cbrt and pow(., 1/3) would
        // otherwise disagree (pow rejects a negative base with a fractional
        // exponent; cbrt does not).
        auto f = [](double v) noexcept
        {
            return v > kSigmaPow3 ? std::pow(v, 1.0 / 3.0) : v / 3.0 / kSigmaPow2 + 4.0 / 29.0;
        };
        const double fx = f(x), fy = f(y), fz = f(z);

        lab[0] = 116.0 * fy - 16.0;
        lab[1] = 500.0 * (fx - fy);
        lab[2] = 200.0 * (fy - fz);
    }

    double ColorDeltaE94(const double rgb1[3], const double rgb2[3]) noexcept
    {
        double xyz1[3] = {}, xyz2[3] = {}, lab1[3] = {}, lab2[3] = {};
        Srgb2Xyz(rgb1, xyz1);
        Srgb2Xyz(rgb2, xyz2);
        Xyz2Lab(xyz1, lab1);
        Xyz2Lab(xyz2, lab2);

        const double deltaL = lab1[0] - lab2[0];
        const double deltaA = lab1[1] - lab2[1];
        const double deltaB = lab1[2] - lab2[2];

        const double c1 = std::sqrt(lab1[1] * lab1[1] + lab1[2] * lab1[2]);
        const double c2 = std::sqrt(lab2[1] * lab2[1] + lab2[2] * lab2[2]);
        const double deltaC = c1 - c2;

        double deltaH = deltaA * deltaA + deltaB * deltaB - deltaC * deltaC;
        deltaH = deltaH < 0.0 ? 0.0 : std::sqrt(deltaH);

        // "Graphic arts" weights. ASYMMETRIC: sC/sH use c1 (the EXPECTED
        // image's chroma) only -- upstream does the same, deliberately.
        constexpr double k1 = 0.045, k2 = 0.015;
        constexpr double kL = 1.0, kC = 1.0, kH = 1.0;
        const double sL = 1.0;
        const double sC = 1.0 + k1 * c1;
        const double sH = 1.0 + k2 * c1;

        const double tL = deltaL / sL / kL;
        const double tC = deltaC / sC / kC;
        const double tH = deltaH / sH / kH;
        return std::sqrt(tL * tL + tC * tC + tH * tH);
    }

    void ImageChannel::BoundXY(std::int64_t x, std::int64_t y,
                               std::uint32_t& outX, std::uint32_t& outY) const noexcept
    {
        const std::int64_t maxX = static_cast<std::int64_t>(width)  - 1;
        const std::int64_t maxY = static_cast<std::int64_t>(height) - 1;
        outX = static_cast<std::uint32_t>(std::clamp<std::int64_t>(x, 0, maxX));
        outY = static_cast<std::uint32_t>(std::clamp<std::int64_t>(y, 0, maxY));
    }

    void IntoRgb(std::uint32_t width, std::uint32_t height,
                 const unsigned char* rgba, const PaddingOptions& options,
                 ImageChannel& r, ImageChannel& g, ImageChannel& b)
    {
        const std::uint32_t pad       = options.paddingSize;
        const std::uint32_t newWidth  = width  + 2 * pad;
        const std::uint32_t newHeight = height + 2 * pad;
        const std::size_t   count     = static_cast<std::size_t>(newWidth) * newHeight;

        auto init = [&](ImageChannel& c)
        {
            c.width  = newWidth;
            c.height = newHeight;
            c.data.assign(count, 0);
        };
        init(r); init(g); init(b);

        for (std::uint32_t y = 0; y < newHeight; ++y)
        {
            for (std::uint32_t x = 0; x < newWidth; ++x)
            {
                const std::size_t index = static_cast<std::size_t>(y) * newWidth + x;
                const bool inside = y >= pad && y < newHeight - pad &&
                                    x >= pad && x < newWidth  - pad;
                if (inside)
                {
                    const std::size_t offset =
                        (static_cast<std::size_t>(y - pad) * width + (x - pad)) * 4;
                    // Upstream keeps alpha == 255 as exactly 1 rather than
                    // 255/255, so the common opaque case stays bit-exact.
                    const double alpha = rgba[offset + 3] == 255
                                       ? 1.0
                                       : rgba[offset + 3] / 255.0;
                    // TRUNCATION, not rounding -- see BlendWithWhite's comment.
                    r.data[index] = static_cast<unsigned char>(BlendWithWhite(rgba[offset + 0], alpha));
                    g.data[index] = static_cast<unsigned char>(BlendWithWhite(rgba[offset + 1], alpha));
                    b.data[index] = static_cast<unsigned char>(BlendWithWhite(rgba[offset + 2], alpha));
                }
                else
                {
                    const unsigned char* color = ((y + x) % 2 == 0)
                                               ? options.colorEven
                                               : options.colorOdd;
                    r.data[index] = color[0];
                    g.data[index] = color[1];
                    b.data[index] = color[2];
                }
            }
        }
    }

    FastStats::FastStats(const ImageChannel& c1, const ImageChannel& c2)
        : m_width(c1.width), m_height(c1.height)
    {
        const std::size_t count = static_cast<std::size_t>(m_width) * m_height;
        m_sumC1.assign(count, 0.0);
        m_sumC2.assign(count, 0.0);
        m_sumSq1.assign(count, 0.0);
        m_sumSq2.assign(count, 0.0);
        m_sumMult.assign(count, 0.0);

        const std::uint32_t w = m_width;
        auto recalc = [w](std::vector<double>& table, std::size_t idx, double initial,
                          std::uint32_t x, std::uint32_t y)
        {
            double v = initial;
            if (y > 0) v += table[idx - w];
            if (x > 0) v += table[idx - 1];
            if (x > 0 && y > 0) v -= table[idx - w - 1];
            table[idx] = v;
        };

        for (std::uint32_t y = 0; y < m_height; ++y)
        {
            for (std::uint32_t x = 0; x < m_width; ++x)
            {
                const std::size_t idx = static_cast<std::size_t>(y) * m_width + x;
                const double v1 = c1.data[idx];
                const double v2 = c2.data[idx];
                recalc(m_sumC1,   idx, v1,      x, y);
                recalc(m_sumC2,   idx, v2,      x, y);
                recalc(m_sumSq1,  idx, v1 * v1, x, y);
                recalc(m_sumSq2,  idx, v2 * v2, x, y);
                recalc(m_sumMult, idx, v1 * v2, x, y);
            }
        }
    }

    double FastStats::Sum(const std::vector<double>& table,
                          std::uint32_t x1, std::uint32_t y1,
                          std::uint32_t x2, std::uint32_t y2) const noexcept
    {
        const std::uint32_t w = m_width;
        double result = table[static_cast<std::size_t>(y2) * w + x2];
        if (y1 > 0) result -= table[static_cast<std::size_t>(y1 - 1) * w + x2];
        if (x1 > 0) result -= table[static_cast<std::size_t>(y2) * w + x1 - 1];
        if (x1 > 0 && y1 > 0) result += table[static_cast<std::size_t>(y1 - 1) * w + x1 - 1];
        return result;
    }

    namespace
    {
        [[nodiscard]] double WindowN(std::uint32_t x1, std::uint32_t y1,
                                     std::uint32_t x2, std::uint32_t y2) noexcept
        {
            return static_cast<double>(y2 - y1 + 1) * static_cast<double>(x2 - x1 + 1);
        }
    }

    double FastStats::MeanC1(std::uint32_t x1, std::uint32_t y1,
                             std::uint32_t x2, std::uint32_t y2) const noexcept
    {
        return Sum(m_sumC1, x1, y1, x2, y2) / WindowN(x1, y1, x2, y2);
    }

    double FastStats::MeanC2(std::uint32_t x1, std::uint32_t y1,
                             std::uint32_t x2, std::uint32_t y2) const noexcept
    {
        return Sum(m_sumC2, x1, y1, x2, y2) / WindowN(x1, y1, x2, y2);
    }

    double FastStats::VarianceC1(std::uint32_t x1, std::uint32_t y1,
                                 std::uint32_t x2, std::uint32_t y2) const noexcept
    {
        const double n = WindowN(x1, y1, x2, y2);
        const double s = Sum(m_sumC1, x1, y1, x2, y2);
        return (Sum(m_sumSq1, x1, y1, x2, y2) - (s * s) / n) / n;
    }

    double FastStats::VarianceC2(std::uint32_t x1, std::uint32_t y1,
                                 std::uint32_t x2, std::uint32_t y2) const noexcept
    {
        const double n = WindowN(x1, y1, x2, y2);
        const double s = Sum(m_sumC2, x1, y1, x2, y2);
        return (Sum(m_sumSq2, x1, y1, x2, y2) - (s * s) / n) / n;
    }

    double FastStats::Covariance(std::uint32_t x1, std::uint32_t y1,
                                 std::uint32_t x2, std::uint32_t y2) const noexcept
    {
        const double n  = WindowN(x1, y1, x2, y2);
        const double s1 = Sum(m_sumC1, x1, y1, x2, y2);
        const double s2 = Sum(m_sumC2, x1, y1, x2, y2);
        return (Sum(m_sumMult, x1, y1, x2, y2) - s1 * s2 / n) / n;
    }

    double Ssim(const FastStats& stats, std::uint32_t x1, std::uint32_t y1,
                std::uint32_t x2, std::uint32_t y2) noexcept
    {
        const double mean1 = stats.MeanC1(x1, y1, x2, y2);
        const double mean2 = stats.MeanC2(x1, y1, x2, y2);
        const double var1  = stats.VarianceC1(x1, y1, x2, y2);
        const double var2  = stats.VarianceC2(x1, y1, x2, y2);
        const double cov   = stats.Covariance(x1, y1, x2, y2);

        constexpr double kDynamicRange = 255.0;   // 2^8 - 1
        constexpr double c1 = (0.01 * kDynamicRange) * (0.01 * kDynamicRange);
        constexpr double c2 = (0.03 * kDynamicRange) * (0.03 * kDynamicRange);

        return (2.0 * mean1 * mean2 + c1) * (2.0 * cov + c2)
             / (mean1 * mean1 + mean2 * mean2 + c1) / (var1 + var2 + c2);
    }

    // ---- the cascade (compare.ts) ----------------------------------------

    namespace
    {
        // compare.ts's own constants. VARIANCE_WINDOW_RADIUS is 1 -- a RADIUS,
        // giving a 3x3 window. Writing 3 here is the classic misport.
        constexpr std::int64_t kSsimWindowRadius     = 15;   // 31x31
        constexpr std::int64_t kVarianceWindowRadius = 1;    // 3x3
        constexpr double       kSsimAntialiasing     = 0.99;

        // Block sizing for the 10x10 spatial-concentration grid: CEIL, not
        // truncation. With ceil, (extent-1)/blockSize is 9 for every extent,
        // so the index stays in [0,99] by construction and needs no clamp --
        // a clamp would be actively wrong, piling the remainder into the last
        // block and inflating its score. max(1u, ...) guards a degenerate
        // zero extent. Shared by Compare() (array-index divisor) and
        // CompareImages() (block-area divisor) so the two cannot desync --
        // see maxLocalDifference's comment.
        std::uint32_t CeilBlockExtent(std::uint32_t extent) noexcept
        {
            return std::max(1u, (extent + 9u) / 10u);
        }

        void DrawPixel(unsigned char* diff, std::uint32_t width,
                       std::uint32_t x, std::uint32_t y,
                       unsigned char r, unsigned char g, unsigned char b) noexcept
        {
            const std::size_t idx = (static_cast<std::size_t>(y) * width + x) * 4;
            diff[idx + 0] = r;
            diff[idx + 1] = g;
            diff[idx + 2] = b;
            diff[idx + 3] = 255;
        }
    }

    std::uint64_t Compare(const unsigned char* expected, const unsigned char* actual,
                          unsigned char* diff,
                          std::uint32_t width, std::uint32_t height,
                          const CompareOptions& options,
                          std::array<std::uint64_t, 100>* localBlocks)
    {
        const std::uint32_t pad =
            static_cast<std::uint32_t>(std::max(kVarianceWindowRadius, kSsimWindowRadius));

        PaddingOptions padding;
        padding.paddingSize = pad;
        // compare.ts's assignment: even magenta, odd green.
        padding.colorEven[0] = 255; padding.colorEven[1] = 0;   padding.colorEven[2] = 255;
        padding.colorOdd[0]  = 0;   padding.colorOdd[1]  = 255; padding.colorOdd[2]  = 0;

        ImageChannel r1, g1, b1, r2, g2, b2;
        IntoRgb(width, height, expected, padding, r1, g1, b1);
        IntoRgb(width, height, actual,   padding, r2, g2, b2);

        // Built lazily: three FastStats over padded planes cost ~118 MB at
        // 720p, and an image that matches exactly never needs them at all.
        std::optional<FastStats> fastR, fastG, fastB;

        std::uint64_t diffCount = 0;

        // See CeilBlockExtent's comment for the sizing rationale.
        const std::uint32_t blockW = CeilBlockExtent(width);
        const std::uint32_t blockH = CeilBlockExtent(height);
        if (localBlocks)
            localBlocks->fill(0);

        // Hashes on the DIFF-IMAGE coordinates (dx, dy), not the padded r1
        // coordinates -- r1 carries a pad border that would offset every block.
        const auto bump = [&](std::uint32_t dx, std::uint32_t dy)
        {
            if (!localBlocks) return;
            const std::size_t idx =
                static_cast<std::size_t>(dy / blockH) * 10u + (dx / blockW);
            ++(*localBlocks)[idx];
        };

        for (std::uint32_t y = pad; y < r1.height - pad; ++y)
        {
            for (std::uint32_t x = pad; x < r1.width - pad; ++x)
            {
                const std::uint32_t dx = x - pad;   // diff-image coordinates
                const std::uint32_t dy = y - pad;

                auto drawGrey = [&]()
                {
                    if (!diff) return;
                    const int grey = Rgb2Gray(r1.Get(x, y), g1.Get(x, y), b1.Get(x, y));
                    const auto v = static_cast<unsigned char>(BlendWithWhite(grey, 0.1));
                    DrawPixel(diff, width, dx, dy, v, v, v);
                };

                // Stage 1: exact equality.
                if (r1.Get(x, y) == r2.Get(x, y) &&
                    g1.Get(x, y) == g2.Get(x, y) &&
                    b1.Get(x, y) == b2.Get(x, y))
                {
                    drawGrey();
                    continue;
                }

                // Stage 2: perceptual colour difference. Named expectedRgb/
                // actualRgb, not c1/c2 -- those identifiers are the SSIM
                // stabilising constants one function away in this file, and
                // reusing them here (different scope, so it compiles, but
                // it is a readability trap when scanning) invites confusing
                // the two.
                const double expectedRgb[3] = { static_cast<double>(r1.Get(x, y)),
                                                static_cast<double>(g1.Get(x, y)),
                                                static_cast<double>(b1.Get(x, y)) };
                const double actualRgb[3] = { static_cast<double>(r2.Get(x, y)),
                                              static_cast<double>(g2.Get(x, y)),
                                              static_cast<double>(b2.Get(x, y)) };
                if (ColorDeltaE94(expectedRgb, actualRgb) <= options.maxColorDeltaE94)
                {
                    drawGrey();
                    continue;
                }

                if (!fastR)
                {
                    fastR.emplace(r1, r2);
                    fastG.emplace(g1, g2);
                    fastB.emplace(b1, b2);
                }

                // Stage 3: flood fill in either image means this cannot be
                // antialiasing, so it must be a real difference.
                std::uint32_t vx1 = 0, vy1 = 0, vx2 = 0, vy2 = 0;
                r1.BoundXY(static_cast<std::int64_t>(x) - kVarianceWindowRadius,
                           static_cast<std::int64_t>(y) - kVarianceWindowRadius, vx1, vy1);
                r1.BoundXY(static_cast<std::int64_t>(x) + kVarianceWindowRadius,
                           static_cast<std::int64_t>(y) + kVarianceWindowRadius, vx2, vy2);

                const double var1 = fastR->VarianceC1(vx1, vy1, vx2, vy2)
                                  + fastG->VarianceC1(vx1, vy1, vx2, vy2)
                                  + fastB->VarianceC1(vx1, vy1, vx2, vy2);
                const double var2 = fastR->VarianceC2(vx1, vy1, vx2, vy2)
                                  + fastG->VarianceC2(vx1, vy1, vx2, vy2)
                                  + fastB->VarianceC2(vx1, vy1, vx2, vy2);
                if (var1 == 0.0 || var2 == 0.0)
                {
                    if (diff) DrawPixel(diff, width, dx, dy, 255, 0, 0);
                    ++diffCount;
                    bump(dx, dy);
                    continue;
                }

                // Stage 4: SSIM.
                std::uint32_t sx1 = 0, sy1 = 0, sx2 = 0, sy2 = 0;
                r1.BoundXY(static_cast<std::int64_t>(x) - kSsimWindowRadius,
                           static_cast<std::int64_t>(y) - kSsimWindowRadius, sx1, sy1);
                r1.BoundXY(static_cast<std::int64_t>(x) + kSsimWindowRadius,
                           static_cast<std::int64_t>(y) + kSsimWindowRadius, sx2, sy2);

                const double ssimRgb = (Ssim(*fastR, sx1, sy1, sx2, sy2)
                                      + Ssim(*fastG, sx1, sy1, sx2, sy2)
                                      + Ssim(*fastB, sx1, sy1, sx2, sy2)) / 3.0;

                if (ssimRgb >= kSsimAntialiasing)
                {
                    if (diff) DrawPixel(diff, width, dx, dy, 255, 255, 0);
                }
                else
                {
                    if (diff) DrawPixel(diff, width, dx, dy, 255, 0, 0);
                    ++diffCount;
                    bump(dx, dy);
                }
            }
        }

        return diffCount;
    }

    // ---- image-level entry point (comparators.ts) ------------------------

    namespace
    {
        // imageUtils.ts's padImageToSize: anchored TOP-LEFT, filled with
        // transparent black. Transparent black then blends to WHITE in
        // IntoRgb, which is what makes a padded region compare equal against
        // another padded region.
        std::vector<unsigned char> PadToSize(const PixelData& src,
                                             std::uint32_t width, std::uint32_t height)
        {
            std::vector<unsigned char> out(static_cast<std::size_t>(width) * height * 4, 0);
            for (std::uint32_t y = 0; y < src.height && y < height; ++y)
            {
                const std::size_t from = static_cast<std::size_t>(y) * src.width * 4;
                const std::size_t to   = static_cast<std::size_t>(y) * width * 4;
                const std::size_t run  = static_cast<std::size_t>(std::min(src.width, width)) * 4;
                std::copy_n(src.rgba.begin() + from, run, out.begin() + to);
            }
            return out;
        }

        // comparators.ts:102's `ratio.toFixed(2)`. The ratio itself (see
        // below) is already rounded UP to the nearest hundredth, so this only
        // needs to print exactly two fractional digits -- never
        // std::to_string's default six, which would read e.g. "0.010000" and
        // break bit-parity with the fixture corpus's recorded error text.
        std::string FormatRatio2(double ratio)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f", ratio);
            return buf;
        }
    }

    ImageCompareResult CompareImages(const PixelData& expected, const PixelData& actual,
                                     const ImageCompareOptions& options)
    {
        ImageCompareResult result;

        if (!expected.Valid() || !actual.Valid())
        {
            result.errorMessage = "could not compare: one of the images is not a valid "
                                  "tight RGBA8 buffer";
            return result;
        }

        const std::uint32_t width  = std::max(expected.width,  actual.width);
        const std::uint32_t height = std::max(expected.height, actual.height);
        result.width  = width;
        result.height = height;

        std::string sizesMismatchError;
        const unsigned char* expectedPixels = expected.rgba.data();
        const unsigned char* actualPixels   = actual.rgba.data();
        std::vector<unsigned char> paddedExpected, paddedActual;

        if (expected.width != actual.width || expected.height != actual.height)
        {
            result.sizesMismatch = true;
            sizesMismatchError = "Expected an image " + std::to_string(expected.width) +
                                 "px by " + std::to_string(expected.height) +
                                 "px, received " + std::to_string(actual.width) +
                                 "px by " + std::to_string(actual.height) + "px. ";
            paddedExpected = PadToSize(expected, width, height);
            paddedActual   = PadToSize(actual,   width, height);
            expectedPixels = paddedExpected.data();
            actualPixels   = paddedActual.data();
        }

        std::vector<unsigned char> diff(static_cast<std::size_t>(width) * height * 4, 0);

        CompareOptions cascade;
        cascade.maxColorDeltaE94 = options.maxColorDeltaE94;
        std::array<std::uint64_t, 100> localBlocks{};
        result.diffCount = Compare(expectedPixels, actualPixels, diff.data(),
                                   width, height, cascade, &localBlocks);

        // The largest block's mismatch fraction. Uses the SAME CeilBlockExtent
        // Compare() used for its array-index divisor, so the two cannot
        // disagree -- blockArea is always >= 1.0 (CeilBlockExtent's own
        // max(1u, ...) guard), so there is no zero-area case to special-case.
        {
            const double blockArea = static_cast<double>(CeilBlockExtent(width)) *
                                     static_cast<double>(CeilBlockExtent(height));
            std::uint64_t worst = 0;
            for (const std::uint64_t n : localBlocks)
                worst = std::max(worst, n);
            result.maxLocalDifference = static_cast<double>(worst) / blockArea;
        }

        // comparators.ts:96-102 -- if both knobs are set take the smaller; if
        // neither is set the budget is zero. The ratio is against EXPECTED's
        // own dimensions, not the padded extent. Upstream keeps maxDiffPixels2
        // as an un-truncated double and compares `count > maxDiffPixels`
        // directly; flooring it into this uint64_t field first is equivalent
        // for that comparison because count is always integral (for integer
        // n and real x, n > x iff n > floor(x)), so this stays bit-parity-safe
        // while giving the struct an honest integer budget to report.
        const double expectedArea =
            static_cast<double>(expected.width) * static_cast<double>(expected.height);
        //
        // PRECONDITION ON maxDiffPixelRatio, enforced at the CLI BOUNDARY and
        // not here (final-review I-1): the value must be finite and in [0, 1].
        // `static_cast<std::uint64_t>` of a negative, NaN, or out-of-range
        // double is UNDEFINED BEHAVIOUR ([conv.fpint]) -- it does not saturate,
        // so `1e30` ("forgive everything") could yield a budget of 0.
        // HostConfig::Parse refuses anything outside that range with its own
        // message before it can reach here, which is the only place a bad value
        // can still be attributed to the argument that carried it. The cast
        // stays a plain cast deliberately: a guard here would have to invent a
        // policy (clamp? zero? pass?) for a value the boundary has already ruled
        // out, and every one of those is the silent-wrong-answer shape that
        // refusal exists to avoid. ImageCompareOptions is ARCANE_API-exported,
        // so a direct in-process caller (ArcaneTests) owns the same
        // precondition.
        std::optional<std::uint64_t> fromRatio;
        if (options.maxDiffPixelRatio.has_value())
        {
            fromRatio = static_cast<std::uint64_t>(expectedArea * *options.maxDiffPixelRatio);
        }

        if (options.maxDiffPixels.has_value() && fromRatio.has_value())
            result.maxDiffPixelsUsed = std::min(*options.maxDiffPixels, *fromRatio);
        else if (options.maxDiffPixels.has_value())
            result.maxDiffPixelsUsed = *options.maxDiffPixels;
        else if (fromRatio.has_value())
            result.maxDiffPixelsUsed = *fromRatio;
        else
            result.maxDiffPixelsUsed = 0;

        // comparators.ts:102 -- `Math.ceil(count / area * 100) / 100`. This is
        // NOT a plain ratio: it rounds UP to the nearest hundredth, so e.g. 1
        // pixel out of 1024 (0.0009765625) is reported as 0.01, not 0.001.
        // Bit-parity with the fixture corpus's error-message text depends on
        // this, not just on the pass/fail verdict.
        result.diffRatio = expectedArea > 0.0
                         ? std::ceil(static_cast<double>(result.diffCount) / expectedArea * 100.0) / 100.0
                         : 0.0;

        std::string pixelsMismatchError;
        if (result.diffCount > result.maxDiffPixelsUsed)
        {
            pixelsMismatchError = std::to_string(result.diffCount) + " pixels (ratio " +
                                  FormatRatio2(result.diffRatio) +
                                  " of all image pixels) are different.";
        }

        // Only evaluated when the caller set it -- see the option's comment.
        std::string localMismatchError;
        if (options.maxLocalDiffRatio.has_value() &&
            result.maxLocalDifference > *options.maxLocalDiffRatio)
        {
            localMismatchError = "one 10x10-grid block differs by " +
                                 FormatRatio2(result.maxLocalDifference) +
                                 ", above the local budget of " +
                                 FormatRatio2(*options.maxLocalDiffRatio) + ".";
        }

        if (!pixelsMismatchError.empty() || !sizesMismatchError.empty() || !localMismatchError.empty())
        {
            result.errorMessage = sizesMismatchError + pixelsMismatchError + localMismatchError;
            result.diffRgba = std::move(diff);
            result.passed = false;
        }
        else
        {
            result.passed = true;
        }
        return result;
    }
}
