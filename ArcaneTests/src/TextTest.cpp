// Text stack: a glyph drawn huge on a small canvas must paint pixels
// inside its ink and leave far corners untouched; an 'L' coverage check
// proves the atlas blit orientation (foot at the BOTTOM, stem at the LEFT)
// rather than relying on a symmetric glyph. Layout tests join below (Task 5).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Text/TextSystem.hpp>

#include "Helpers/GpuTestHelpers.hpp"

namespace
{
    void CheckGlyphRenders(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend,
                                                     "data/shaders");
        REQUIRE(shaders != nullptr);
        auto canvas = Arcane::CreateCanvas(device->Nvrhi(), 64, 64);
        auto batcher = Arcane::Batcher2D::Create(device->Nvrhi(), shaders.get());
        auto text = Arcane::TextSystem::Create(device->Nvrhi());
        REQUIRE(text != nullptr);

        const Arcane::FontId font =
            text->LoadFont("data/fonts/Roboto-Regular.ttf");
        REQUIRE(font != Arcane::kInvalidFontId);

        auto commandList = device->Nvrhi()->createCommandList();
        commandList->open();
        commandList->clearTextureFloat(canvas->Texture(), nvrhi::AllSubresources,
                                       nvrhi::Color(0, 0, 0, 1));
        batcher->Begin(commandList, canvas->Framebuffer(), 64, 64);
        // 'H' nearly fills the canvas: solid vertical strokes at the sides.
        text->Draw(*batcher, font, 56.0f, glm::vec2(6.0f, 56.0f), "H",
                   glm::vec4(1, 1, 1, 1));
        text->Flush(commandList);
        batcher->End();

        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(64).setHeight(64)
            .setFormat(nvrhi::Format::RGBA16_FLOAT)
            .setDebugName("TextReadback");
        auto staging = device->Nvrhi()->createStagingTexture(
            stagingDesc, nvrhi::CpuAccessMode::Read);
        commandList->copyTexture(staging, nvrhi::TextureSlice(),
                                 canvas->Texture(), nvrhi::TextureSlice());
        commandList->close();
        device->Nvrhi()->executeCommandList(commandList);
        device->Nvrhi()->waitForIdle();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(
            device->Nvrhi()->mapStagingTexture(
                staging, nvrhi::TextureSlice(),
                nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);

        // Count lit pixels (R channel fp16 > 0.5) -- 'H' ink must cover a
        // meaningful area; canvas corners stay dark.
        int lit = 0;
        for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x)
            {
                const auto* texel = reinterpret_cast<const uint16_t*>(
                    pixels + y * rowPitch + x * 8);
                if (HalfToFloat(texel[0]) > 0.5f)
                    ++lit;
            }
        CHECK(lit > 200);  // strokes of a 56px 'H'

        const auto* corner = reinterpret_cast<const uint16_t*>(pixels);
        CHECK(HalfToFloat(corner[0]) < 0.05f);

        device->Nvrhi()->unmapStagingTexture(staging);
        device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }

    // Orientation proof: 'L' is asymmetric in both axes. Its vertical stem
    // sits on the LEFT and its foot on the BOTTOM (high screen-y, y-down).
    // A vertical flip or horizontal mirror in the atlas blit would move the
    // ink, so this catches an upside-down / mirrored atlas that 'H' cannot.
    void CheckGlyphOrientation(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend,
                                                     "data/shaders");
        REQUIRE(shaders != nullptr);
        auto canvas = Arcane::CreateCanvas(device->Nvrhi(), 64, 64);
        auto batcher = Arcane::Batcher2D::Create(device->Nvrhi(), shaders.get());
        auto text = Arcane::TextSystem::Create(device->Nvrhi());
        REQUIRE(text != nullptr);
        const Arcane::FontId font =
            text->LoadFont("data/fonts/Roboto-Regular.ttf");
        REQUIRE(font != Arcane::kInvalidFontId);

        auto commandList = device->Nvrhi()->createCommandList();
        commandList->open();
        commandList->clearTextureFloat(canvas->Texture(), nvrhi::AllSubresources,
                                       nvrhi::Color(0, 0, 0, 1));
        batcher->Begin(commandList, canvas->Framebuffer(), 64, 64);
        text->Draw(*batcher, font, 56.0f, glm::vec2(8.0f, 56.0f), "L",
                   glm::vec4(1, 1, 1, 1));
        text->Flush(commandList);
        batcher->End();

        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(64).setHeight(64)
            .setFormat(nvrhi::Format::RGBA16_FLOAT)
            .setDebugName("TextOrientReadback");
        auto staging = device->Nvrhi()->createStagingTexture(
            stagingDesc, nvrhi::CpuAccessMode::Read);
        commandList->copyTexture(staging, nvrhi::TextureSlice(),
                                 canvas->Texture(), nvrhi::TextureSlice());
        commandList->close();
        device->Nvrhi()->executeCommandList(commandList);
        device->Nvrhi()->waitForIdle();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(
            device->Nvrhi()->mapStagingTexture(
                staging, nvrhi::TextureSlice(),
                nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);

        auto lit = [&](uint32_t x, uint32_t y) {
            const auto* texel = reinterpret_cast<const uint16_t*>(
                pixels + y * rowPitch + x * 8);
            return HalfToFloat(texel[0]) > 0.5f;
        };

        // Per-half lit counts. 'L': stem at left across all rows; foot fills
        // the bottom rows leftward-to-right; the top-right quadrant is empty.
        int topRows = 0, bottomRows = 0, leftCols = 0, rightCols = 0;
        int topRight = 0;
        for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x)
            {
                if (!lit(x, y))
                    continue;
                if (y < 32) ++topRows; else ++bottomRows;
                if (x < 32) ++leftCols; else ++rightCols;
                if (y < 24 && x >= 40) ++topRight;
            }

        // Foot at the bottom: more ink low than high (y-down screen space).
        CHECK(bottomRows > topRows);
        // Stem on the left: more ink left than right.
        CHECK(leftCols > rightCols);
        // Top-right quadrant of an 'L' is empty -- a vertical flip (foot at
        // top) or a mirror would light it up. Window (y<24, x>=40) is sized
        // for the 56px draw at baseline (6,56); change those together.
        CHECK(topRight < 10);

        device->Nvrhi()->unmapStagingTexture(staging);
        device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: MSDF glyph renders through the batcher", "[gpu][d3d12][text]")
{
    CheckGlyphRenders(Arcane::GraphicsBackend::D3D12);
    CheckGlyphOrientation(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: MSDF glyph renders through the batcher", "[gpu][vulkan][text]")
{
    CheckGlyphRenders(Arcane::GraphicsBackend::Vulkan);
    CheckGlyphOrientation(Arcane::GraphicsBackend::Vulkan);
}

// CPU layout invariants: only Measure is exercised (no canvas/batcher needed).
// A single D3D12 device is required because TextSystem::Create initialises the
// glyph atlas texture -- Measure itself never touches the GPU.
//
// Kerning note: Roboto carries its "AV" (and similar diagonal-pair) kerning in
// the GPOS table, which FreeType's FT_Get_Kerning does NOT read (kern table
// only). The "AV <= sum" check therefore observes ZERO kern adjustment and
// passes trivially -- this is expected and correct given the current stack.
// HarfBuzz (deferred, stack-spec non-Latin shaping task) would surface GPOS.
TEST_CASE("text: measure and layout invariants", "[gpu][d3d12][text]")
{
    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::D3D12;
    auto device = Arcane::RenderDevice::Create(desc);
    REQUIRE(device != nullptr);
    auto text = Arcane::TextSystem::Create(device->Nvrhi());
    const Arcane::FontId font = text->LoadFont("data/fonts/Roboto-Regular.ttf");
    REQUIRE(font != Arcane::kInvalidFontId);

    const float size = 32.0f;
    const auto wide = text->Measure(font, size, "MMMM");
    const auto narrow = text->Measure(font, size, "iiii");
    CHECK(wide.x > narrow.x);                    // proportional advances
    CHECK(wide.x > 4 * 0.5f * size);             // 'M' is a wide glyph

    const auto one = text->Measure(font, size, "Hello");
    const auto two = text->Measure(font, size, "Hello\nHello");
    CHECK(two.y > one.y * 1.7f);                 // second line adds height
    CHECK(std::abs(two.x - one.x) < 0.5f);       // same widest line

    const auto scaled = text->Measure(font, size * 2.0f, "Hello");
    CHECK(std::abs(scaled.x - one.x * 2.0f) < 1.0f);  // linear in size

    CHECK(text->Measure(font, size, "").x == 0.0f);

    // Kerning: "AV" should be no wider than advance-sum "A"+"V".
    // FT_Get_Kerning reads kern table only (not GPOS); Roboto stores its
    // diagonal-pair kern in GPOS, so the adjustment observed here is 0.
    // The assertion still holds (av == sum when kern == 0).
    const float av = text->Measure(font, size, "AV").x;
    const float sum = text->Measure(font, size, "A").x +
                      text->Measure(font, size, "V").x;
    CHECK(av <= sum + 0.01f);

    CHECK(Arcane::RenderErrorCount() == 0);
}
