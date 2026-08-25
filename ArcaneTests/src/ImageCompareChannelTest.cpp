// Channel splitting with padding. Two details here are load-bearing and neither
// is obvious: the padding is a CHECKERBOARD (so a border window can never read
// as a flood fill, which would defeat the variance stage), and RGB is
// composited against WHITE using alpha before anything is compared.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ImageCompare.hpp>

#include <cstdint>
#include <vector>

namespace
{
    // A solid RGBA image, alpha included so the blend path is exercised.
    std::vector<unsigned char> Solid(std::uint32_t w, std::uint32_t h,
                                     unsigned char r, unsigned char g,
                                     unsigned char b, unsigned char a)
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            px[i + 0] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a;
        }
        return px;
    }
}

TEST_CASE("compare: IntoRgb grows the plane by 2*paddingSize on each axis", "[compare]")
{
    const auto px = Solid(4, 3, 10, 20, 30, 255);
    Arcane::ImageChannel r, g, b;
    Arcane::PaddingOptions opt;
    opt.paddingSize = 15;
    Arcane::IntoRgb(4, 3, px.data(), opt, r, g, b);

    CHECK(r.width  == 4 + 30);
    CHECK(r.height == 3 + 30);
    CHECK(r.data.size() == static_cast<std::size_t>(r.width) * r.height);
    CHECK(g.width == r.width);
    CHECK(b.height == r.height);
}

TEST_CASE("compare: an opaque pixel passes through unblended", "[compare]")
{
    const auto px = Solid(2, 2, 10, 20, 30, 255);
    Arcane::ImageChannel r, g, b;
    Arcane::PaddingOptions opt;
    opt.paddingSize = 1;
    Arcane::IntoRgb(2, 2, px.data(), opt, r, g, b);

    CHECK(r.Get(1, 1) == 10);
    CHECK(g.Get(1, 1) == 20);
    CHECK(b.Get(1, 1) == 30);
}

TEST_CASE("compare: a FULLY TRANSPARENT pixel composites to white", "[compare]")
{
    // alpha 0 -> BlendWithWhite(c, 0) == 255 for every channel. This is the
    // behaviour that makes size-mismatch padding (transparent black) read as
    // white, so it must not be optimised away as "our captures are opaque".
    const auto px = Solid(2, 2, 10, 20, 30, 0);
    Arcane::ImageChannel r, g, b;
    Arcane::PaddingOptions opt;
    opt.paddingSize = 1;
    Arcane::IntoRgb(2, 2, px.data(), opt, r, g, b);

    CHECK(r.Get(1, 1) == 255);
    CHECK(g.Get(1, 1) == 255);
    CHECK(b.Get(1, 1) == 255);
}

TEST_CASE("compare: a half-transparent pixel TRUNCATES, it does not round", "[compare]")
{
    // alpha 128 -> 128/255 = 0.50196...; BlendWithWhite(0, 0.50196) = 127.0004...
    // Upstream truncates into a Uint8Array, so this must be 127, not 128.
    const auto px = Solid(1, 1, 0, 0, 0, 128);
    Arcane::ImageChannel r, g, b;
    Arcane::PaddingOptions opt;
    opt.paddingSize = 0;
    Arcane::IntoRgb(1, 1, px.data(), opt, r, g, b);

    CHECK(r.Get(0, 0) == 127);
}

TEST_CASE("compare: padding is a CHECKERBOARD, so no border window is a flood fill", "[compare]")
{
    const auto px = Solid(2, 2, 10, 20, 30, 255);
    Arcane::ImageChannel r, g, b;
    Arcane::PaddingOptions opt;
    opt.paddingSize = 1;                       // colorEven magenta, colorOdd green
    Arcane::IntoRgb(2, 2, px.data(), opt, r, g, b);

    // (0,0): (x+y)%2 == 0 -> even -> magenta (255, 0, 255)
    CHECK(r.Get(0, 0) == 255);
    CHECK(g.Get(0, 0) == 0);
    CHECK(b.Get(0, 0) == 255);

    // (1,0): (x+y)%2 == 1 -> odd -> green (0, 255, 0)
    CHECK(r.Get(1, 0) == 0);
    CHECK(g.Get(1, 0) == 255);
    CHECK(b.Get(1, 0) == 0);

    // The point of the alternation: two adjacent padding pixels DIFFER.
    CHECK(r.Get(0, 0) != r.Get(1, 0));
}

TEST_CASE("compare: BoundXY clamps NEGATIVE coordinates without unsigned underflow", "[compare]")
{
    // The window helpers call this with (x - 15), which goes negative near the
    // edge. Computing that in an unsigned type wraps to ~4 billion and indexes
    // out of bounds -- the single nastiest porting trap in this component.
    const auto px = Solid(4, 4, 0, 0, 0, 255);
    Arcane::ImageChannel r, g, b;
    Arcane::PaddingOptions opt;
    opt.paddingSize = 0;
    Arcane::IntoRgb(4, 4, px.data(), opt, r, g, b);

    std::uint32_t ox = 99, oy = 99;
    r.BoundXY(-15, -15, ox, oy);
    CHECK(ox == 0);
    CHECK(oy == 0);

    r.BoundXY(1000, 1000, ox, oy);
    CHECK(ox == r.width - 1);
    CHECK(oy == r.height - 1);
}
