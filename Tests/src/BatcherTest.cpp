// Batcher GPU readbacks: a quad covering the whole canvas must write its
// exact color (alpha-blend with a == 1 is a passthrough); stats must count
// what was submitted.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

namespace
{
    struct GpuFixture
    {
        std::unique_ptr<Arcane::RenderDevice> device;
        std::unique_ptr<Arcane::ShaderLibrary> shaders;
        std::unique_ptr<Arcane::Canvas> canvas;
        std::unique_ptr<Arcane::Batcher2D> batcher;
        nvrhi::StagingTextureHandle staging;
        nvrhi::CommandListHandle commandList;

        explicit GpuFixture(Arcane::GraphicsBackend backend, uint32_t size = 8)
        {
            Arcane::RenderDeviceDesc desc;
            desc.backend = backend;
            device = Arcane::RenderDevice::Create(desc);
            REQUIRE(device != nullptr);
            shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend,
                                                    "shaders");
            REQUIRE(shaders != nullptr);
            canvas = Arcane::CreateCanvas(device->Nvrhi(), size, size);
            REQUIRE(canvas != nullptr);
            batcher = Arcane::Batcher2D::Create(device->Nvrhi(), *shaders);
            REQUIRE(batcher != nullptr);

            auto stagingDesc = nvrhi::TextureDesc()
                .setWidth(size).setHeight(size)
                .setFormat(nvrhi::Format::RGBA16_FLOAT)
                .setDebugName("BatcherReadback");
            staging = device->Nvrhi()->createStagingTexture(
                stagingDesc, nvrhi::CpuAccessMode::Read);
            commandList = device->Nvrhi()->createCommandList();
        }

        // Renders whatever the caller batched, copies the canvas to staging.
        void Flush()
        {
            commandList->close();
            device->Nvrhi()->executeCommandList(commandList);
            device->Nvrhi()->waitForIdle();
        }
    };

    // fp16 -> float for readback checks.
    float HalfToFloat(uint16_t h)
    {
        const uint32_t sign = (uint32_t)(h >> 15) & 1;
        const uint32_t expo = (uint32_t)(h >> 10) & 0x1F;
        const uint32_t mant = (uint32_t)h & 0x3FF;
        if (expo == 0)
            return (sign ? -1.0f : 1.0f) * (float)mant * 5.9604645e-8f;
        if (expo == 31)
            return sign ? -65504.0f : 65504.0f;  // inf/nan clamped; unused here
        const uint32_t bits = (sign << 31) | ((expo - 15 + 127) << 23) | (mant << 13);
        float result;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    void CheckPixel(const uint8_t* base, size_t rowPitch, uint32_t x, uint32_t y,
                    float r, float g, float b, float tolerance = 0.02f)
    {
        const auto* texel = reinterpret_cast<const uint16_t*>(
            base + y * rowPitch + x * 8);  // RGBA16F = 8 bytes/texel
        CHECK(std::abs(HalfToFloat(texel[0]) - r) <= tolerance);
        CHECK(std::abs(HalfToFloat(texel[1]) - g) <= tolerance);
        CHECK(std::abs(HalfToFloat(texel[2]) - b) <= tolerance);
    }

    void CheckQuadFillsCanvas(Arcane::GraphicsBackend backend)
    {
        GpuFixture fx(backend);
        fx.commandList->open();
        fx.commandList->clearTextureFloat(fx.canvas->Texture(),
                                          nvrhi::AllSubresources,
                                          nvrhi::Color(0, 0, 0, 1));
        fx.batcher->Begin(fx.commandList, fx.canvas->Framebuffer(),
                          fx.canvas->Width(), fx.canvas->Height());
        fx.batcher->Rect(glm::vec2(0, 0), glm::vec2(8, 8),
                         glm::vec4(0.9f, 0.4f, 0.1f, 1.0f));
        fx.batcher->End();
        const Arcane::Batch2DStats stats = fx.batcher->Stats();
        CHECK(stats.quads == 1);
        CHECK(stats.drawCalls == 1);
        fx.commandList->copyTexture(fx.staging, nvrhi::TextureSlice(),
                                    fx.canvas->Texture(), nvrhi::TextureSlice());
        fx.Flush();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(
            fx.device->Nvrhi()->mapStagingTexture(
                fx.staging, nvrhi::TextureSlice(),
                nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);
        CheckPixel(pixels, rowPitch, 0, 0, 0.9f, 0.4f, 0.1f);
        CheckPixel(pixels, rowPitch, 7, 7, 0.9f, 0.4f, 0.1f);
        CheckPixel(pixels, rowPitch, 4, 4, 0.9f, 0.4f, 0.1f);
        fx.device->Nvrhi()->unmapStagingTexture(fx.staging);
        fx.device->Nvrhi()->runGarbageCollection();

        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

namespace
{
    // Sort keys do two jobs at once: layer ordering wins over submission
    // order, and same-state draws coalesce ACROSS layer interleaves.
    void CheckSortKeys(Arcane::GraphicsBackend backend)
    {
        GpuFixture fx(backend);
        fx.commandList->open();
        fx.commandList->clearTextureFloat(fx.canvas->Texture(),
                                          nvrhi::AllSubresources,
                                          nvrhi::Color(0, 0, 0, 1));
        fx.batcher->Begin(fx.commandList, fx.canvas->Framebuffer(), 8, 8);
        // Submitted top layer FIRST: without sorting, green would cover red.
        fx.batcher->SetLayer(1, 0);
        fx.batcher->Rect(glm::vec2(0, 0), glm::vec2(8, 8),
                         glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
        fx.batcher->SetLayer(0, 0);
        fx.batcher->Rect(glm::vec2(0, 0), glm::vec2(8, 8),
                         glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
        fx.batcher->End();

        // Same kind + texture (white) on both layers: ONE coalesced draw.
        const Arcane::Batch2DStats stats = fx.batcher->Stats();
        CHECK(stats.quads == 2);
        CHECK(stats.drawCalls == 1);

        fx.commandList->copyTexture(fx.staging, nvrhi::TextureSlice(),
                                    fx.canvas->Texture(), nvrhi::TextureSlice());
        fx.Flush();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(
            fx.device->Nvrhi()->mapStagingTexture(
                fx.staging, nvrhi::TextureSlice(),
                nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);
        CheckPixel(pixels, rowPitch, 4, 4, 1.0f, 0.0f, 0.0f);  // layer 1 on top
        fx.device->Nvrhi()->unmapStagingTexture(fx.staging);
        fx.device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: batcher quad fills the canvas; sort keys order and coalesce", "[gpu][d3d12]")
{
    CheckQuadFillsCanvas(Arcane::GraphicsBackend::D3D12);
    CheckSortKeys(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: batcher quad fills the canvas; sort keys order and coalesce", "[gpu][vulkan]")
{
    CheckQuadFillsCanvas(Arcane::GraphicsBackend::Vulkan);
    CheckSortKeys(Arcane::GraphicsBackend::Vulkan);
}
