// Batcher GPU readbacks: a quad covering the whole canvas must write its
// exact color (alpha-blend with a == 1 is a passthrough); stats must count
// what was submitted.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include "Helpers/GpuTestHelpers.hpp"

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
                                                    "data/shaders");
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

    // HalfToFloat moved to Helpers/GpuTestHelpers.hpp (shared with TextTest).
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

namespace
{
    void CheckCircleCoverage(Arcane::GraphicsBackend backend)
    {
        GpuFixture fx(backend, /*size=*/16);
        fx.commandList->open();
        fx.commandList->clearTextureFloat(fx.canvas->Texture(),
                                          nvrhi::AllSubresources,
                                          nvrhi::Color(0, 0, 0, 1));
        fx.batcher->Begin(fx.commandList, fx.canvas->Framebuffer(), 16, 16);
        fx.batcher->Circle(glm::vec2(8, 8), 6.0f, glm::vec4(0, 1, 0, 1));
        fx.batcher->End();
        fx.commandList->copyTexture(fx.staging, nvrhi::TextureSlice(),
                                    fx.canvas->Texture(), nvrhi::TextureSlice());
        fx.Flush();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(
            fx.device->Nvrhi()->mapStagingTexture(
                fx.staging, nvrhi::TextureSlice(),
                nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);
        CheckPixel(pixels, rowPitch, 8, 8, 0.0f, 1.0f, 0.0f);   // center: solid
        CheckPixel(pixels, rowPitch, 0, 0, 0.0f, 0.0f, 0.0f);   // corner: untouched
        CheckPixel(pixels, rowPitch, 15, 0, 0.0f, 0.0f, 0.0f);
        fx.device->Nvrhi()->unmapStagingTexture(fx.staging);
        fx.device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }

    void CheckLineCoverage(Arcane::GraphicsBackend backend)
    {
        GpuFixture fx(backend, /*size=*/16);
        fx.commandList->open();
        fx.commandList->clearTextureFloat(fx.canvas->Texture(),
                                          nvrhi::AllSubresources,
                                          nvrhi::Color(0, 0, 0, 1));
        fx.batcher->Begin(fx.commandList, fx.canvas->Framebuffer(), 16, 16);
        // Horizontal line through y=8, 4px thick.
        fx.batcher->Line(glm::vec2(0, 8), glm::vec2(16, 8), 4.0f,
                         glm::vec4(1, 0, 1, 1));
        fx.batcher->End();
        fx.commandList->copyTexture(fx.staging, nvrhi::TextureSlice(),
                                    fx.canvas->Texture(), nvrhi::TextureSlice());
        fx.Flush();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(
            fx.device->Nvrhi()->mapStagingTexture(
                fx.staging, nvrhi::TextureSlice(),
                nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);
        CheckPixel(pixels, rowPitch, 8, 8, 1.0f, 0.0f, 1.0f);   // on the line
        CheckPixel(pixels, rowPitch, 8, 1, 0.0f, 0.0f, 0.0f);   // above it
        CheckPixel(pixels, rowPitch, 8, 14, 0.0f, 0.0f, 0.0f);  // below it
        fx.device->Nvrhi()->unmapStagingTexture(fx.staging);
        fx.device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: batcher circle and line coverage", "[gpu][d3d12]")
{
    CheckCircleCoverage(Arcane::GraphicsBackend::D3D12);
    CheckLineCoverage(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: batcher circle and line coverage", "[gpu][vulkan]")
{
    CheckCircleCoverage(Arcane::GraphicsBackend::Vulkan);
    CheckLineCoverage(Arcane::GraphicsBackend::Vulkan);
}

// --- RemoveTexture: binding-set cache eviction (E01-2) ---------------------

namespace
{
    // Solid-color 2x2 RGBA8_UNORM texture (NOT sRGB: sampled value is
    // byte/255 exactly, no transfer curve in the way of readback checks).
    nvrhi::TextureHandle MakeSolidTexture(nvrhi::IDevice* device,
                                          uint8_t r, uint8_t g, uint8_t b,
                                          const char* name)
    {
        auto desc = nvrhi::TextureDesc()
            .setWidth(2).setHeight(2)
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true)
            .setDebugName(name);
        nvrhi::TextureHandle tex = device->createTexture(desc);
        REQUIRE(tex != nullptr);
        uint8_t pixels[2 * 2 * 4];
        for (int i = 0; i < 4; ++i)
        {
            pixels[i * 4 + 0] = r;
            pixels[i * 4 + 1] = g;
            pixels[i * 4 + 2] = b;
            pixels[i * 4 + 3] = 255;
        }
        auto upload = device->createCommandList();
        upload->open();
        upload->writeTexture(tex, 0, 0, pixels, 2 * 4);
        upload->close();
        device->executeCommandList(upload);
        return tex;
    }

    // Current external refcount of a live nvrhi resource (AddRef returns the
    // incremented count; Release undoes the probe).
    unsigned long RefCount(nvrhi::IResource* resource)
    {
        resource->AddRef();
        return resource->Release();
    }

    // Draws `tex` over the whole 8x8 canvas and returns the (4,4) texel's R
    // channel (0..1) from a staging readback.
    float DrawFullscreenAndReadCenterR(GpuFixture& fx, nvrhi::ITexture* tex)
    {
        fx.commandList->open();
        fx.commandList->clearTextureFloat(fx.canvas->Texture(),
                                          nvrhi::AllSubresources,
                                          nvrhi::Color(0, 0, 0, 1));
        fx.batcher->Begin(fx.commandList, fx.canvas->Framebuffer(),
                          fx.canvas->Width(), fx.canvas->Height());
        fx.batcher->Quad(glm::vec2(0, 0), glm::vec2(8, 8), tex,
                         glm::vec2(0, 0), glm::vec2(1, 1), glm::vec4(1.0f));
        fx.batcher->End();
        fx.commandList->copyTexture(fx.staging, nvrhi::TextureSlice(),
                                    fx.canvas->Texture(), nvrhi::TextureSlice());
        fx.Flush();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(
            fx.device->Nvrhi()->mapStagingTexture(
                fx.staging, nvrhi::TextureSlice(),
                nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);
        const auto* texel = reinterpret_cast<const uint16_t*>(
            pixels + 4 * rowPitch + 4 * 8);  // (4,4), RGBA16F = 8 bytes/texel
        const float red = HalfToFloat(texel[0]);
        fx.device->Nvrhi()->unmapStagingTexture(fx.staging);
        fx.device->Nvrhi()->runGarbageCollection();
        return red;
    }
}

TEST_CASE("d3d12: batcher RemoveTexture drops the cached binding set (unpins the texture)",
          "[gpu][d3d12][batcher]")
{
    GpuFixture fx(Arcane::GraphicsBackend::D3D12);
    nvrhi::TextureHandle tex = MakeSolidTexture(
        fx.device->Nvrhi(), 255, 0, 0, "RemoveTexRed");
    fx.device->Nvrhi()->waitForIdle();
    fx.device->Nvrhi()->runGarbageCollection();
    const unsigned long baseline = RefCount(tex);

    // One draw caches a binding set for the texture, which pins it alive.
    CHECK(DrawFullscreenAndReadCenterR(fx, tex) > 0.9f);
    fx.device->Nvrhi()->waitForIdle();
    fx.device->Nvrhi()->runGarbageCollection();
    CHECK(RefCount(tex) > baseline);           // pinned by the cached set

    // Evict: the entry (and its pin) must go away.
    fx.batcher->RemoveTexture(tex);
    CHECK(RefCount(tex) == baseline);

    // Absent key and null are harmless no-ops.
    fx.batcher->RemoveTexture(tex);
    fx.batcher->RemoveTexture(nullptr);

    CHECK(Arcane::RenderErrorCount() == 0);
}

TEST_CASE("d3d12: batcher release->realloc gets a fresh binding set (no stale ABA hit)",
          "[gpu][d3d12][batcher]")
{
    GpuFixture fx(Arcane::GraphicsBackend::D3D12);

    // Pass 1: draw a RED texture and prove it landed.
    nvrhi::TextureHandle texA = MakeSolidTexture(
        fx.device->Nvrhi(), 255, 0, 0, "AbaRed");
    const void* oldAddr = texA.Get();
    CHECK(DrawFullscreenAndReadCenterR(fx, texA) > 0.9f);

    // Release: evict-before-release (the ImGuiNvrhi::DestroyTexture order),
    // then drop the last handle and let the device actually free it.
    fx.batcher->RemoveTexture(texA);
    texA = nullptr;
    fx.device->Nvrhi()->waitForIdle();
    fx.device->Nvrhi()->runGarbageCollection();

    // Realloc an identical texture, now GREEN -- retrying a bounded number
    // of times to coax the heap into handing back the freed address (the
    // ABA case this hook guards; Windows LFH randomizes block reuse, so one
    // attempt often misses). Mismatches are kept alive so every retry pulls
    // a different block. The assertion below is correct either way; INFO
    // records whether this run actually hit the address-reuse path.
    std::vector<nvrhi::TextureHandle> keepAlive;
    nvrhi::TextureHandle texB = MakeSolidTexture(
        fx.device->Nvrhi(), 0, 255, 0, "AbaGreen");
    for (int attempt = 0; attempt < 31 && texB.Get() != oldAddr; ++attempt)
    {
        keepAlive.push_back(texB);
        texB = MakeSolidTexture(fx.device->Nvrhi(), 0, 255, 0, "AbaGreen");
    }
    INFO("address reused: " << (texB.Get() == oldAddr ? "yes" : "no"));

    // A stale cached set for the old texture at this address would sample
    // RED (or a destroyed resource); a fresh set samples GREEN -> R == 0.
    CHECK(DrawFullscreenAndReadCenterR(fx, texB) < 0.1f);

    fx.batcher->RemoveTexture(texB);
    CHECK(Arcane::RenderErrorCount() == 0);
}
