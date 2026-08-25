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
}
