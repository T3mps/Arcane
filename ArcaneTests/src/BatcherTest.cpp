// Batcher GPU readbacks: a quad covering the whole canvas must write its
// exact color (alpha-blend with a == 1 is a passthrough); stats must count
// what was submitted.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <Arcane/Guid.hpp>
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
            batcher = Arcane::Batcher2D::Create(device->Nvrhi(), shaders.get());
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

TEST_CASE("d3d12: Drain() reports the SAME stats End() does -- the HUD's parity line",
          "[gpu][d3d12]")
{
    // NRI Phase 2, Task 12. Both hosts print "Quads: %u  Draws: %u" from
    // Stats(), and the graph path DRAINS the batcher where the NVRHI path ENDS
    // it. Until Drain() wrote m_stats, the graph path's HUD read a permanent
    // 0/0 -- a text difference in the `full` stage golden, in the one HUD line
    // that says what the frame drew. Backend-agnostic CPU math; it needs a
    // device only because Batcher2D::Create does.
    GpuFixture fx(Arcane::GraphicsBackend::D3D12);

    // Two quads that COALESCE into one draw (same kind + white texture),
    // submitted out of layer order -- so a stat that merely counted records
    // would disagree with End()'s run count.
    const auto submit = [&fx]
    {
        fx.batcher->SetLayer(1, 0);
        fx.batcher->Rect(glm::vec2(0, 0), glm::vec2(8, 8), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
        fx.batcher->SetLayer(0, 0);
        fx.batcher->Rect(glm::vec2(0, 0), glm::vec2(4, 4), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
    };

    // The NVRHI recorder's numbers.
    fx.commandList->open();
    fx.batcher->Begin(fx.commandList, fx.canvas->Framebuffer(), 8, 8);
    submit();
    fx.batcher->End();
    const Arcane::Batch2DStats ended = fx.batcher->Stats();
    fx.Flush();
    CHECK(ended.quads == 2);
    CHECK(ended.drawCalls == 1);

    // The graph recorder's: Begin with NO command list and NO framebuffer --
    // exactly what RuntimeApp's --nri-graph branch does -- then Drain.
    fx.batcher->Begin(nullptr, nullptr, 8, 8);
    submit();
    const Arcane::Batch2DDrained drained = fx.batcher->Drain();
    const Arcane::Batch2DStats after = fx.batcher->Stats();

    CHECK(after.quads == ended.quads);
    CHECK(after.drawCalls == ended.drawCalls);
    // ...and the draw count IS the span count the graph's Batch2DNode records,
    // so the number the HUD prints is the number of draws that happened.
    CHECK(after.drawCalls == (uint32_t)drained.spans.size());

    fx.device->Nvrhi()->runGarbageCollection();
    CHECK(Arcane::RenderErrorCount() == 0);
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

TEST_CASE("d3d12: a DEVICE-LESS batcher drains identical spans to a device-carrying one",
          "[gpu][d3d12]")
{
    // THE SEVERANCE'S FLOOR (NRI Phase 3, Task 2). The headless half of this
    // -- that a device-less instance exists, drains, splits runs on the asset
    // Guid and refuses End() -- is SeveranceTest.cpp, inside the ~[gpu] gate.
    // What needs a device, and therefore lives here, is the COMPARISON: the
    // batching a device-less batcher produces must be the SAME batching the
    // NVRHI one produces, or the two recorders would disagree about what the
    // frame contains and the phase's golden floor would move under it.
    //
    // Everything is compared except the `texture` POINTERS, which cannot
    // match by construction: one instance has texture objects and the other
    // has no device to have made any on.
    GpuFixture fx(Arcane::GraphicsBackend::D3D12);

    auto deviceless = Arcane::Batcher2D::Create(nullptr, nullptr);
    REQUIRE(deviceless != nullptr);

    const Arcane::Guid texId =
        *Arcane::Guid::FromString("0f0f0f0f-1e1e-2d2d-3c3c-4b4b4b4b4b4b");

    // A mixed batch: two layers, a registered-id-less material path, a
    // textured quad, an untextured rect, a circle (a different built-in) and
    // a glyph -- i.e. every span-splitting axis the sort key has.
    const auto submit = [&](Arcane::Batcher2D& b, nvrhi::ITexture* tex)
    {
        b.SetLayer(1, 0);
        b.QuadTextured(Arcane::Batcher2D::kMaterialSprite, texId,
                       glm::vec2(0.0f), glm::vec2(4.0f), tex,
                       glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f, 0.5f, 0.25f, 1.0f));
        b.SetLayer(0, 0);
        b.Rect(glm::vec2(1.0f), glm::vec2(2.0f), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
        b.Circle(glm::vec2(4.0f), 2.0f, glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
        b.SetLayer(1, 1);
        b.QuadTextured(Arcane::Batcher2D::kMaterialSprite, texId,
                       glm::vec2(2.0f), glm::vec2(4.0f), tex,
                       glm::vec2(0.25f), glm::vec2(0.75f), glm::vec4(1.0f));
    };

    // The device-carrying instance, driven exactly as the graph path drives it
    // (Begin with no command list and no framebuffer, then Drain).
    fx.batcher->Begin(nullptr, nullptr, 8, 8);
    submit(*fx.batcher, fx.canvas->Texture());
    const Arcane::Batch2DDrained withDevice = fx.batcher->Drain();

    deviceless->Begin(nullptr, nullptr, 8, 8);
    submit(*deviceless, nullptr);
    const Arcane::Batch2DDrained without = deviceless->Drain();

    // The VERTEX STREAM is byte-identical: nothing in it ever depended on a
    // device (positions, uvs and colors are pure CPU math).
    REQUIRE(without.vertices.size() == withDevice.vertices.size());
    CHECK(std::memcmp(without.vertices.data(), withDevice.vertices.data(),
                      without.vertices.size() * sizeof(Arcane::Batch2DVertex)) == 0);

    // The INDEX stream and the SPAN list agree in size, order and every field
    // but the texture pointer.
    REQUIRE(without.indices.size() == withDevice.indices.size());
    CHECK(std::memcmp(without.indices.data(), withDevice.indices.data(),
                      without.indices.size() * sizeof(std::uint32_t)) == 0);

    REQUIRE(without.spans.size() == withDevice.spans.size());
    for (std::size_t i = 0; i < without.spans.size(); ++i)
    {
        INFO("span " << i);
        CHECK(without.spans[i].material   == withDevice.spans[i].material);
        CHECK(without.spans[i].firstIndex == withDevice.spans[i].firstIndex);
        CHECK(without.spans[i].indexCount == withDevice.spans[i].indexCount);
        CHECK(without.spans[i].textureId  == withDevice.spans[i].textureId);
        // ...and the one field that MUST differ.
        CHECK(without.spans[i].texture == nullptr);
        CHECK(withDevice.spans[i].texture != nullptr);
    }

    CHECK(without.viewport == withDevice.viewport);
    CHECK(deviceless->Stats().quads     == fx.batcher->Stats().quads);
    CHECK(deviceless->Stats().drawCalls == fx.batcher->Stats().drawCalls);

    CHECK(Arcane::RenderErrorCount() == 0);
}
