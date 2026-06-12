// msdfgen arrival gate: load a real glyph from Roboto via FreeType,
// generate a 32x32 MSDF, assert the bitmap has spatial variance (a flat
// bitmap means edge coloring or the FT bridge is broken).
//
// API note (v1.12.0): SDFTransformation takes Projection + DistanceMapping,
// not Projection + Range. loadGlyph requires FontCoordinateScaling as the
// third arg; FONT_SCALING_EM_NORMALIZED is the recommended value for new code.
// See "Recorded msdfgen API forms" in the Task 1 implementation report.

#include <catch2/catch_test_macros.hpp>

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <cmath>

TEST_CASE("msdfgen: glyph MSDF has signal", "[vendor][msdfgen]")
{
    msdfgen::FreetypeHandle* ft = msdfgen::initializeFreetype();
    REQUIRE(ft != nullptr);

    msdfgen::FontHandle* font =
        msdfgen::loadFont(ft, "data/fonts/Roboto-Regular.ttf");
    REQUIRE(font != nullptr);

    msdfgen::Shape shape;
    double advance = 0.0;
    // v1.12: loadGlyph requires FontCoordinateScaling; EM_NORMALIZED is
    // recommended for new code (legacy FONT_SCALING_LEGACY divides by 64).
    REQUIRE(msdfgen::loadGlyph(shape, font, (msdfgen::unicode_t)'A',
                               msdfgen::FONT_SCALING_EM_NORMALIZED, &advance));
    shape.normalize();
    msdfgen::edgeColoringSimple(shape, 3.0);

    msdfgen::Bitmap<float, 3> bitmap(32, 32);

    // SDFTransformation = Projection + DistanceMapping.
    // Projection(scale, translate): scale maps em-normalized glyph coords
    // to pixels; translate shifts the glyph into the bitmap.
    // DistanceMapping(Range): range is the pixel-space half-width of the
    // distance field band (6 pixels at the chosen scale).
    const double scale = 32.0 / 1.0;   // 1 em -> 32 px (EM_NORMALIZED)
    msdfgen::SDFTransformation t(
        msdfgen::Projection(
            msdfgen::Vector2(scale, scale),
            msdfgen::Vector2(4.0, 8.0)),
        msdfgen::DistanceMapping(msdfgen::Range(6.0 / scale)));
    msdfgen::generateMSDF(bitmap, shape, t);

    // Verify the bitmap has spatial variance: a flat bitmap means the
    // FreeType bridge, edge coloring, or transformation is broken.
    float minV = 1e9f, maxV = -1e9f;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            for (int c = 0; c < 3; ++c)
            {
                // bitmap(x, y) returns float* to 3 channels at that pixel.
                float v = bitmap(x, y)[c];
                if (v < minV) minV = v;
                if (v > maxV) maxV = v;
            }
    REQUIRE(maxV - minV > 0.25f);   // real distance signal, not a flat field

    msdfgen::destroyFont(font);
    msdfgen::deinitializeFreetype(ft);
}
