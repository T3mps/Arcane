#include <Arcane/Assets/GoldenImage.hpp>

#include <Arcane/Assets/ImageIo.hpp>   // WritePngRgba (diff output)

#include <cstdlib>

namespace Arcane
{
    GoldenCompareResult CompareRgbaImages(
        const unsigned char* a, std::uint32_t aw, std::uint32_t ah,
        const unsigned char* b, std::uint32_t bw, std::uint32_t bh,
        const GoldenCompareParams& params)
    {
        GoldenCompareResult result;
        if (!a || !b || aw != bw || ah != bh || aw == 0 || ah == 0)
            return result;   // ok=false, dimensionsMatch=false
        result.dimensionsMatch = true;

        const std::size_t pixels = static_cast<std::size_t>(aw) * ah;
        std::size_t bad = 0;
        bool haveFirst = false;
        for (std::size_t p = 0; p < pixels; ++p)
        {
            unsigned char worst = 0;
            for (std::size_t c = 0; c < 4; ++c)
            {
                const int delta = std::abs(int(a[p * 4 + c]) - int(b[p * 4 + c]));
                if (delta > worst) worst = static_cast<unsigned char>(delta);
            }
            if (worst > result.maxChannelDelta)
                result.maxChannelDelta = worst;
            if (worst > params.channelTolerance)
            {
                ++bad;
                if (!haveFirst)
                {
                    haveFirst = true;
                    result.firstBadX = static_cast<std::uint32_t>(p % aw);
                    result.firstBadY = static_cast<std::uint32_t>(p / aw);
                }
            }
        }
        result.badPixelFraction = pixels ? float(bad) / float(pixels) : 0.0f;
        result.ok = result.badPixelFraction <= params.maxBadPixelFraction;
        return result;
    }

    bool WriteDiffPng(const std::filesystem::path& path,
                      const unsigned char* a, const unsigned char* b,
                      std::uint32_t width, std::uint32_t height,
                      unsigned char channelTolerance)
    {
        if (!a || !b || width == 0 || height == 0)
            return false;
        const std::size_t pixels = static_cast<std::size_t>(width) * height;
        std::vector<unsigned char> diff(pixels * 4);
        for (std::size_t p = 0; p < pixels; ++p)
        {
            unsigned char worst = 0;
            for (std::size_t c = 0; c < 4; ++c)
            {
                const int d = std::abs(int(a[p * 4 + c]) - int(b[p * 4 + c]));
                if (d > worst) worst = static_cast<unsigned char>(d);
            }
            if (worst > channelTolerance)
            {   // offending pixel: solid red
                diff[p * 4 + 0] = 255; diff[p * 4 + 1] = 0;
                diff[p * 4 + 2] = 0;   diff[p * 4 + 3] = 255;
            }
            else
            {   // clean pixel: dimmed grayscale of the golden
                const unsigned char g = static_cast<unsigned char>(
                    (int(a[p * 4]) + a[p * 4 + 1] + a[p * 4 + 2]) / 3 / 3);
                diff[p * 4 + 0] = g; diff[p * 4 + 1] = g;
                diff[p * 4 + 2] = g; diff[p * 4 + 3] = 255;
            }
        }
        return WritePngRgba(path, width, height, diff.data());
    }
}
