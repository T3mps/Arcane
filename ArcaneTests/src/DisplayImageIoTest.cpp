// The two CPU halves split out at NRI Phase 3, Task 11, both for the same
// reason: the graph path has a SECOND uploader and NO nvrhi::IDevice, so the
// rules about what an image IS had to stop living inside functions that take
// one.
//
//   LoadDisplayPixels      -- once the CPU half of LoadDisplayTexture, and the
//                             whole of it since ABI v15 (exe-relative
//                             resolve + decode + maxSize area-average
//                             downscale). Feeds the editor's toolbar logo into
//                             NriTextureCache's PixelSupplyFn on the graph arm.
//   WriteThumbnailPngRgba  -- once the CPU half of SaveTexturePng, and the
//                             whole of it since ABI v15 (opaque alpha +
//                             width cap + downscale + write). The editor's
//                             cover-thumbnail write reads pixels through
//                             NriGraphContext::ReadCapture and lands here --
//                             the only caller since the NVRHI arm
//                             (ReadTexturePixels -> SaveTexturePng) was
//                             deleted at ABI v15.
//
// Same precedent as RepackStagingToRgba, which was exported at the previous
// phase for exactly this: one definition, two ways of getting the pixels.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Assets/ImageIo.hpp>

#include <cstdint>
#include <filesystem>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    fs::path TempDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_display_image_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    // A solid-colour image with a DELIBERATELY non-opaque alpha channel --
    // the case the opaque rule exists for.
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

TEST_CASE("WriteThumbnailPngRgba caps the WIDTH, keeps the aspect, and forces alpha opaque",
          "[render][assets]")
{
    const fs::path dir = TempDir("thumbnail");
    const fs::path out = dir / "cover.png";

    // 400x100, half-transparent: a viewport whose alpha channel still carries
    // coverage math. A cover that kept it would render as holes in the Hub.
    const std::uint32_t w = 400, h = 100;
    REQUIRE(Arcane::WriteThumbnailPngRgba(out, w, h, Solid(w, h, 10, 200, 30, 0x40),
                                          /*maxWidth=*/100));

    std::uint32_t rw = 0, rh = 0;
    std::vector<unsigned char> back;
    REQUIRE(Arcane::LoadPngRgba(out, rw, rh, back));
    CHECK(rw == 100);
    CHECK(rh == 25);   // aspect preserved: 100 * (100/400)
    REQUIRE(back.size() == static_cast<std::size_t>(rw) * rh * 4);
    // The colour survives the box filter (it is constant), and alpha is 255
    // everywhere -- forced BEFORE the downscale, so the filter cannot average a
    // transparent texel back in.
    for (std::size_t i = 0; i < back.size(); i += 4)
    {
        REQUIRE(back[i + 0] == 10);
        REQUIRE(back[i + 1] == 200);
        REQUIRE(back[i + 2] == 30);
        REQUIRE(back[i + 3] == 0xFF);
    }
}

TEST_CASE("WriteThumbnailPngRgba below the cap writes the source size unscaled",
          "[render][assets]")
{
    const fs::path dir = TempDir("thumbnail_nocap");
    const fs::path out = dir / "small.png";

    REQUIRE(Arcane::WriteThumbnailPngRgba(out, 8, 4, Solid(8, 4, 1, 2, 3, 255), 512));

    std::uint32_t rw = 0, rh = 0;
    std::vector<unsigned char> back;
    REQUIRE(Arcane::LoadPngRgba(out, rw, rh, back));
    CHECK(rw == 8);
    CHECK(rh == 4);
}

TEST_CASE("WriteThumbnailPngRgba refuses a buffer that does not describe its own size",
          "[render][assets]")
{
    // The one input the callers cannot get wrong today but a future one could:
    // a width/height pair that does not match the byte count. Refusing beats
    // reading off the end of the vector inside the downscale.
    const fs::path dir = TempDir("thumbnail_bad");
    CHECK_FALSE(Arcane::WriteThumbnailPngRgba(dir / "a.png", 8, 8, Solid(4, 4, 0, 0, 0, 255), 0));
    CHECK_FALSE(Arcane::WriteThumbnailPngRgba(dir / "b.png", 0, 8, {}, 0));
}

TEST_CASE("LoadDisplayPixels decodes device-free and honours the larger-dimension cap",
          "[render][assets]")
{
    const fs::path dir = TempDir("displaypixels");
    const fs::path src = dir / "src.png";
    // 200x50 -- LARGER dimension capped (the loader's rule), unlike the
    // thumbnail writer's width cap.
    REQUIRE(Arcane::WritePngRgba(src, 200, 50, Solid(200, 50, 90, 91, 92, 255).data()));

    Arcane::PixelData pixels;
    REQUIRE(Arcane::LoadDisplayPixels(src, /*maxSize=*/50, pixels));
    CHECK(pixels.Valid());
    CHECK(pixels.width == 50);
    CHECK(pixels.height == 13);   // 50 * (50/200) rounded

    // No cap: the source size, tight-packed, which is what Valid() asserts.
    Arcane::PixelData full;
    REQUIRE(Arcane::LoadDisplayPixels(src, 0, full));
    CHECK(full.width == 200);
    CHECK(full.height == 50);
    CHECK(full.Valid());

    // A cap ABOVE the source leaves it alone rather than upscaling.
    Arcane::PixelData big;
    REQUIRE(Arcane::LoadDisplayPixels(src, 4096, big));
    CHECK(big.width == 200);
    CHECK(big.height == 50);

    // A missing file leaves `out` INVALID rather than partially filled -- the
    // supply that feeds NriTextureCache reads Valid() and nothing else.
    Arcane::PixelData missing;
    missing.width = 7;   // pre-dirtied, to prove the reset
    CHECK_FALSE(Arcane::LoadDisplayPixels(dir / "nope.png", 64, missing));
    CHECK_FALSE(missing.Valid());
    CHECK(missing.width == 0);
}
