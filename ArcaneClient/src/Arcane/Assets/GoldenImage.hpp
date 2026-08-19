#pragma once

// NRI Phase 0 golden-image harness: pure, headless, device-free comparator.
// Per-channel tolerance + bad-pixel-fraction threshold; emits diff visualization
// for failures. The verdict-giver for the port's GPU validation.

#include <Arcane/Base/Api.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Arcane
{
    struct GoldenCompareParams
    {
        // Per-channel absolute delta a pixel may have and still count clean.
        // Default 2 absorbs dx12-vs-vulkan rounding; desk calibration
        // (Task 6) owns the final number.
        unsigned char channelTolerance   = 2;
        // Fraction of pixels allowed to exceed the tolerance (rasterization
        // edge wobble). 0.001 = 0.1%.
        float         maxBadPixelFraction = 0.001f;
    };

    struct GoldenCompareResult
    {
        bool          ok               = false;
        bool          dimensionsMatch  = false;
        float         badPixelFraction = 0.0f;   // pixels beyond tolerance / total
        unsigned char maxChannelDelta  = 0;      // worst single-channel delta seen
        std::uint32_t firstBadX = 0, firstBadY = 0;   // first offending pixel (row-major)
    };

    // Compares two tight RGBA8 images. A dimension mismatch returns
    // ok=false, dimensionsMatch=false, everything else zero. Alpha
    // participates like any channel, but the capture path pins it to 255
    // on BOTH the capture side and the compare side -- a broken-alpha
    // pipeline is masked, not caught. This is NOT an alpha-correctness gate.
    [[nodiscard]] ARCANE_API GoldenCompareResult CompareRgbaImages(
        const unsigned char* a, std::uint32_t aw, std::uint32_t ah,
        const unsigned char* b, std::uint32_t bw, std::uint32_t bh,
        const GoldenCompareParams& params = {});

    // Writes a diff visualization beside a failed compare: clean pixels
    // dimmed grayscale of `a`, offending pixels solid red. False on IO
    // failure. No-op-false on dimension mismatch.
    ARCANE_API bool WriteDiffPng(const std::filesystem::path& path,
                                 const unsigned char* a,
                                 const unsigned char* b,
                                 std::uint32_t width, std::uint32_t height,
                                 unsigned char channelTolerance);
}
