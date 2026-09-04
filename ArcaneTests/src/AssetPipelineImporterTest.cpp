// F2b Task 3: the texture importer v1 RGBA8 path (Arcane::AssetPipeline::ImportTexture) and the
// grown TextureMetaSettings settings block. Pins: mip dims halve by FLOOR -- max(1, dim >> 1),
// 5 -> 2 -> 1, never 3 -- both square and non-square/NPOT, each axis halving independently;
// mips are built in LINEAR float space (a 2x2 sRGB image of pure black and pure white quads
// averages to the value whose LINEAR average re-encodes to sRGB ~188, never the gamma-space
// wrong answer ~128); `maxSize` drops top mips, the artifact's own width/height become the
// clamped size; the TextureMetaSettings JSON round-trip, including an absent block falling back
// to defaults; the thumbnail is <=64px long edge with aspect kept; `generateMips=false` yields
// mipCount 1; and the RGBA8 artifact this importer produces is cook-twice byte-identical.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/TextureImporter.hpp>
#include <Arcane/AssetPipeline/TextureMetaSettings.hpp>
#include <Arcane/Guid.hpp>

#include <Json.hpp>
#include <stb_image_write.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace Arcane::AssetPipeline;
using Arcane::Guid;

namespace
{
    void CollectPngBytes(void* ctx, void* data, int size)
    {
        auto* out = static_cast<std::vector<unsigned char>*>(ctx);
        out->insert(out->end(), static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + size);
    }

    // Encodes an in-memory RGBA8 buffer to PNG bytes via stb_image_write -- the fixture
    // mechanism AssetPipelineImporterTest.cpp uses instead of committing binary .png files,
    // matching VendorSmokeTest.cpp's own "stb: PNG write -> read round-trip" precedent.
    std::vector<std::byte> EncodePng(int width, int height, const std::vector<unsigned char>& rgba)
    {
        std::vector<unsigned char> pngBytes;
        REQUIRE(stbi_write_png_to_func(&CollectPngBytes, &pngBytes, width, height, 4, rgba.data(), width * 4) != 0);

        std::vector<std::byte> out(pngBytes.size());
        if (!pngBytes.empty())
            std::memcpy(out.data(), pngBytes.data(), pngBytes.size());
        return out;
    }

    std::vector<unsigned char> SolidPixels(int width, int height, unsigned char r, unsigned char g,
                                            unsigned char b, unsigned char a)
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(width) * height * 4);
        for (std::size_t i = 0; i < px.size(); i += 4)
        {
            px[i + 0] = r;
            px[i + 1] = g;
            px[i + 2] = b;
            px[i + 3] = a;
        }
        return px;
    }

    // Deterministic per-pixel variation (not a solid fill) so the cook-twice-identity test
    // actually exercises the box-average math at every mip level, not just a trivial constant.
    std::vector<unsigned char> GradientPixels(int width, int height)
    {
        std::vector<unsigned char> px(static_cast<std::size_t>(width) * height * 4);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
                px[i + 0] = static_cast<unsigned char>((x * 37 + y * 11) & 0xFF);
                px[i + 1] = static_cast<unsigned char>((x * 13 + y * 53) & 0xFF);
                px[i + 2] = static_cast<unsigned char>((x * 97 + y * 5) & 0xFF);
                px[i + 3] = static_cast<unsigned char>(255 - ((x + y) & 0xFF));
            }
        }
        return px;
    }

    const std::byte* MipBytes(const ImportedTexture& tex, std::size_t mipIndex)
    {
        const MipDesc& mip = tex.desc.mips[mipIndex];
        return tex.payload.data() + mip.offset;
    }

    fs::path TempFile(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_pipeline_importer_test";
        std::error_code ec;
        fs::create_directories(d, ec);
        return d / leaf;
    }

    std::vector<std::byte> ReadWholeFile(const fs::path& path)
    {
        std::ifstream ifs(path, std::ios::binary);
        REQUIRE(ifs.good());
        ifs.seekg(0, std::ios::end);
        const auto len = static_cast<std::size_t>(ifs.tellg());
        ifs.seekg(0, std::ios::beg);
        std::vector<std::byte> out(len);
        if (len > 0)
            ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
        REQUIRE(ifs.good());
        return out;
    }
}

// ---- Mip chain: FLOOR halving --------------------------------------------------------------

TEST_CASE("pipeline: ImportTexture mip chain halves by FLOOR -- 5 -> 2 -> 1, never 3", "[pipeline]")
{
    const std::vector<unsigned char> pixels = SolidPixels(5, 5, 200, 100, 50, 255);
    const std::vector<std::byte> png = EncodePng(5, 5, pixels);

    TextureMetaSettings settings{};
    settings.srgb = false;
    settings.generateMips = true;
    settings.maxSize = 0;

    const std::optional<ImportedTexture> imported = ImportTexture(png, Guid::Generate(), settings);
    REQUIRE(imported.has_value());

    REQUIRE(imported->desc.mipCount == 3u);
    REQUIRE(imported->desc.mips.size() == 3u);
    CHECK(imported->desc.mips[0].width == 5u);
    CHECK(imported->desc.mips[0].height == 5u);
    CHECK(imported->desc.mips[1].width == 2u);   // FLOOR(5/2) == 2, never ceil's 3
    CHECK(imported->desc.mips[1].height == 2u);
    CHECK(imported->desc.mips[2].width == 1u);
    CHECK(imported->desc.mips[2].height == 1u);

    CHECK(imported->desc.width == 5u);
    CHECK(imported->desc.height == 5u);
}

TEST_CASE("pipeline: ImportTexture mip chain halves independently per axis (non-square NPOT)", "[pipeline]")
{
    const std::vector<unsigned char> pixels = SolidPixels(6, 3, 10, 20, 30, 255);
    const std::vector<std::byte> png = EncodePng(6, 3, pixels);

    TextureMetaSettings settings{};
    settings.srgb = false;
    settings.generateMips = true;

    const std::optional<ImportedTexture> imported = ImportTexture(png, Guid::Generate(), settings);
    REQUIRE(imported.has_value());

    REQUIRE(imported->desc.mips.size() == 3u);
    CHECK(imported->desc.mips[0].width == 6u);
    CHECK(imported->desc.mips[0].height == 3u);
    CHECK(imported->desc.mips[1].width == 3u);   // FLOOR(6/2)
    CHECK(imported->desc.mips[1].height == 1u);  // FLOOR(3/2) == 1, height reaches 1 first
    CHECK(imported->desc.mips[2].width == 1u);
    CHECK(imported->desc.mips[2].height == 1u);
}

// ---- Linear-space downsample correctness -----------------------------------------------------

TEST_CASE("pipeline: ImportTexture builds mips in LINEAR space, not gamma space", "[pipeline]")
{
    // 2x2 sRGB image: two pure-black quads, two pure-white quads. Averaging the raw 8-bit sRGB
    // bytes directly (WRONG -- gamma-space average) gives ~128. Linearizing, averaging, then
    // re-encoding to sRGB (CORRECT) gives ~188. This is the assertion that catches an
    // accidental gamma-space average.
    std::vector<unsigned char> pixels(2 * 2 * 4);
    // Explicit, unambiguous layout: px0=black, px1=black, px2=white, px3=white.
    auto setPx = [&](int idx, unsigned char v, unsigned char a)
    {
        pixels[idx * 4 + 0] = v;
        pixels[idx * 4 + 1] = v;
        pixels[idx * 4 + 2] = v;
        pixels[idx * 4 + 3] = a;
    };
    setPx(0, 0, 255);
    setPx(1, 0, 255);
    setPx(2, 255, 255);
    setPx(3, 255, 255);

    const std::vector<std::byte> png = EncodePng(2, 2, pixels);

    TextureMetaSettings settings{};
    settings.srgb = true;
    settings.generateMips = true;

    const std::optional<ImportedTexture> imported = ImportTexture(png, Guid::Generate(), settings);
    REQUIRE(imported.has_value());
    REQUIRE(imported->desc.mips.size() == 2u);
    REQUIRE(imported->desc.mips[1].width == 1u);
    REQUIRE(imported->desc.mips[1].height == 1u);

    const std::byte* mip1 = MipBytes(*imported, 1);
    const auto r = static_cast<int>(static_cast<std::uint8_t>(mip1[0]));
    const auto g = static_cast<int>(static_cast<std::uint8_t>(mip1[1]));
    const auto b = static_cast<int>(static_cast<std::uint8_t>(mip1[2]));

    INFO("mip1 rgb = " << r << "," << g << "," << b);
    CHECK(r > 150);   // clearly not the gamma-space wrong answer (~128)
    CHECK(r >= 180);
    CHECK(r <= 195);
    CHECK(g == r);
    CHECK(b == r);

    // Alpha is never gamma-corrected -- both source alphas were already 255, so the linear
    // average is exactly 255 with no rounding ambiguity.
    CHECK(static_cast<int>(static_cast<std::uint8_t>(mip1[3])) == 255);
}

// ---- maxSize drops top mips -------------------------------------------------------------------

TEST_CASE("pipeline: ImportTexture maxSize drops top mips; 0 stays unlimited", "[pipeline]")
{
    const std::vector<unsigned char> pixels = SolidPixels(8, 8, 5, 6, 7, 255);
    const std::vector<std::byte> png = EncodePng(8, 8, pixels);

    TextureMetaSettings clamped{};
    clamped.srgb = false;
    clamped.generateMips = true;
    clamped.maxSize = 4;

    const std::optional<ImportedTexture> importedClamped = ImportTexture(png, Guid::Generate(), clamped);
    REQUIRE(importedClamped.has_value());
    CHECK(importedClamped->desc.width == 4u);
    CHECK(importedClamped->desc.height == 4u);
    REQUIRE(importedClamped->desc.mips.size() == 3u);   // 4 -> 2 -> 1
    CHECK(importedClamped->desc.mips[0].width == 4u);
    CHECK(importedClamped->desc.mips[1].width == 2u);
    CHECK(importedClamped->desc.mips[2].width == 1u);

    TextureMetaSettings unlimited = clamped;
    unlimited.maxSize = 0;
    const std::optional<ImportedTexture> importedUnlimited = ImportTexture(png, Guid::Generate(), unlimited);
    REQUIRE(importedUnlimited.has_value());
    CHECK(importedUnlimited->desc.width == 8u);
    CHECK(importedUnlimited->desc.height == 8u);
    REQUIRE(importedUnlimited->desc.mips.size() == 4u);   // 8 -> 4 -> 2 -> 1
}

// ---- TextureMetaSettings JSON round-trip -------------------------------------------------------

TEST_CASE("pipeline: TextureMetaSettings JSON round-trip, including absent-block defaults", "[pipeline]")
{
    TextureMetaSettings settings{};
    settings.format = TextureMetaSettings::Format::Bc7;
    settings.srgb = false;
    settings.generateMips = false;
    settings.maxSize = 2048;

    const nlohmann::json j = settings.ToMetaJson();
    const TextureMetaSettings back = TextureMetaSettings::FromMetaJson(j);

    CHECK(back.format == settings.format);
    CHECK(back.srgb == settings.srgb);
    CHECK(back.generateMips == settings.generateMips);
    CHECK(back.maxSize == settings.maxSize);

    // Absent block (empty object -- no "texture" key at all in a real .meta file resolves to
    // this) falls back to every field's own struct default.
    const TextureMetaSettings defaulted = TextureMetaSettings::FromMetaJson(nlohmann::json::object());
    CHECK(defaulted.format == TextureMetaSettings::Format::Auto);
    CHECK(defaulted.srgb == true);
    CHECK(defaulted.generateMips == true);
    CHECK(defaulted.maxSize == 0u);

    // Tolerant on a wrong-typed field: a hand-edited file with the wrong JSON type for one
    // field must not throw, and must fall back to that field's default while leaving the
    // well-typed fields around it alone.
    nlohmann::json partiallyWrong;
    partiallyWrong["srgb"] = "not-a-bool";
    partiallyWrong["maxSize"] = 512;
    const TextureMetaSettings tolerant = TextureMetaSettings::FromMetaJson(partiallyWrong);
    CHECK(tolerant.srgb == true);        // default, wrong type ignored
    CHECK(tolerant.maxSize == 512u);     // well-typed field still read
}

// ---- Thumbnail -----------------------------------------------------------------------------

TEST_CASE("pipeline: ImportTexture thumbnail is <=64px long edge with aspect kept", "[pipeline]")
{
    {
        const std::vector<unsigned char> pixels = SolidPixels(256, 128, 1, 2, 3, 255);
        const std::vector<std::byte> png = EncodePng(256, 128, pixels);

        TextureMetaSettings settings{};
        const std::optional<ImportedTexture> imported = ImportTexture(png, Guid::Generate(), settings);
        REQUIRE(imported.has_value());

        CHECK(imported->desc.thumbWidth <= 64u);
        CHECK(imported->desc.thumbHeight <= 64u);
        CHECK(imported->desc.thumbWidth == 64u);
        CHECK(imported->desc.thumbHeight == 32u);   // 256/128 == 2:1, kept exactly by box-halving
        CHECK(imported->thumbRgba.size() == static_cast<std::size_t>(64 * 32 * 4));
    }
    {
        // Already within budget -- no downsampling should occur at all.
        const std::vector<unsigned char> pixels = SolidPixels(32, 16, 4, 5, 6, 255);
        const std::vector<std::byte> png = EncodePng(32, 16, pixels);

        TextureMetaSettings settings{};
        const std::optional<ImportedTexture> imported = ImportTexture(png, Guid::Generate(), settings);
        REQUIRE(imported.has_value());

        CHECK(imported->desc.thumbWidth == 32u);
        CHECK(imported->desc.thumbHeight == 16u);
    }
}

// ---- generateMips=false --------------------------------------------------------------------

TEST_CASE("pipeline: ImportTexture generateMips=false yields mipCount 1", "[pipeline]")
{
    const std::vector<unsigned char> pixels = SolidPixels(16, 16, 9, 8, 7, 255);
    const std::vector<std::byte> png = EncodePng(16, 16, pixels);

    TextureMetaSettings settings{};
    settings.generateMips = false;

    const std::optional<ImportedTexture> imported = ImportTexture(png, Guid::Generate(), settings);
    REQUIRE(imported.has_value());

    CHECK(imported->desc.mipCount == 1u);
    REQUIRE(imported->desc.mips.size() == 1u);
    CHECK(imported->desc.mips[0].width == 16u);
    CHECK(imported->desc.mips[0].height == 16u);
    CHECK(imported->desc.width == 16u);
    CHECK(imported->desc.height == 16u);
}

// ---- RGBA8 cook-twice byte-identity ----------------------------------------------------------

TEST_CASE("pipeline: ImportTexture RGBA8 artifacts are cook-twice byte-identical", "[pipeline]")
{
    const std::vector<unsigned char> pixels = GradientPixels(17, 9);   // NPOT + non-square
    const std::vector<std::byte> png = EncodePng(17, 9, pixels);

    TextureMetaSettings settings{};
    settings.srgb = true;
    settings.generateMips = true;
    settings.maxSize = 0;

    const Guid guid = Guid::Generate();

    const std::optional<ImportedTexture> first = ImportTexture(png, guid, settings);
    const std::optional<ImportedTexture> second = ImportTexture(png, guid, settings);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    const fs::path pathA = TempFile("cook_twice_a.arcart");
    const fs::path pathB = TempFile("cook_twice_b.arcart");

    REQUIRE(WriteTextureArtifact(pathA, first->desc, first->payload, first->thumbRgba));
    REQUIRE(WriteTextureArtifact(pathB, second->desc, second->payload, second->thumbRgba));

    const std::vector<std::byte> bytesA = ReadWholeFile(pathA);
    const std::vector<std::byte> bytesB = ReadWholeFile(pathB);
    CHECK(bytesA == bytesB);
    CHECK(bytesA.size() > 0u);
}

// ---- Decode failure --------------------------------------------------------------------------

TEST_CASE("pipeline: ImportTexture refuses undecodable bytes", "[pipeline]")
{
    const std::vector<std::byte> garbage{ std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03} };
    TextureMetaSettings settings{};
    const std::optional<ImportedTexture> imported = ImportTexture(garbage, Guid::Generate(), settings);
    CHECK_FALSE(imported.has_value());
}
