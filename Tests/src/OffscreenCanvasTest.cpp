// OffscreenCanvas: render a Batcher2D pass into an offscreen texture, get an
// ImTextureID for ImGui::Image. The texture must be valid and the pass must
// run with zero NVRHI validation errors (the foundation rule).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/OffscreenCanvas.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

namespace
{
    // Mirrors the per-backend GPU harness used by BatcherTest/TonemapTest:
    // each backend builds its own RenderDevice + ShaderLibrary, then the
    // OffscreenCanvas owns the canvas/batcher/tonemap/output internally.
    void CheckOffscreenCanvas(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);

        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend,
                                                     "shaders");
        REQUIRE(shaders != nullptr);

        auto oc = Arcane::OffscreenCanvas::Create(device->Nvrhi(), *shaders,
                                                  256, 256);
        REQUIRE(oc != nullptr);

        oc->Draw(
            [](Arcane::Batcher2D& b) {
                b.Line(glm::vec2(10, 10), glm::vec2(200, 200), 2.0f,
                       glm::vec4(1, 1, 1, 1));
                b.Circle(glm::vec2(128, 128), 40.0f, glm::vec4(1, 0, 0, 1));
            },
            glm::vec4(0, 0, 0, 1));

        REQUIRE(oc->TextureId() != 0);

        // A resize must cleanly rebuild the targets and keep the id valid.
        oc->Resize(128, 96);
        oc->Draw([](Arcane::Batcher2D& b) {
            b.Rect(glm::vec2(0, 0), glm::vec2(128, 96), glm::vec4(0, 1, 0, 1));
        }, glm::vec4(0, 0, 0, 1));
        REQUIRE(oc->TextureId() != 0);

        device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }

    // Pixel-readback proof: Task 4 confirmed the OffscreenCanvas BINDS with a
    // non-zero TextureId and zero validation errors, but never read back a
    // single pixel -- so it never actually proved that Batcher2D geometry lands
    // VISIBLY in the output texture (only the clear color might survive). This
    // test closes that gap: draw a KNOWN bright primitive in PIXEL space on a
    // dark clear, copy the BGRA8_UNORM output to a staging texture, map it, and
    // assert the primitive's pixels are bright while a corner stays near clear.
    //
    // This is the load-bearing evidence for the Minkowski-inset bug: if this
    // FAILS, the inset is blank because OffscreenCanvas doesn't render geometry
    // (a renderer bug). If it PASSES, OffscreenCanvas is sound and the blank
    // inset is purely the inset DRAW logic (SAT/analytic kinds undrawn).
    void CheckOffscreenCanvasReadback(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);

        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend,
                                                     "shaders");
        REQUIRE(shaders != nullptr);

        constexpr uint32_t kSize = 128;
        auto oc = Arcane::OffscreenCanvas::Create(device->Nvrhi(), *shaders,
                                                  kSize, kSize);
        REQUIRE(oc != nullptr);

        // A filled white disc at the center + a thick white line across it, on a
        // dark clear. White is symmetric across BGRA channels, so byte order is
        // irrelevant for the bright assertion. Coordinates are PIXEL space (NOT
        // routed through any fit transform) so we know exactly where to sample.
        const glm::vec2 center(kSize * 0.5f, kSize * 0.5f);
        oc->Draw(
            [center](Arcane::Batcher2D& b) {
                b.Circle(center, 24.0f, glm::vec4(1, 1, 1, 1));
                b.Line(glm::vec2(8, 8), glm::vec2(120, 120), 5.0f,
                       glm::vec4(1, 1, 1, 1));
            },
            glm::vec4(0.03f, 0.03f, 0.05f, 1.0f));
        REQUIRE(oc->TextureId() != 0);

        // The output texture pointer round-trips through TextureId() (it returns
        // (uint64_t)(uintptr_t)m_output.Get()) -- see OffscreenCanvas.cpp.
        auto* output =
            reinterpret_cast<nvrhi::ITexture*>(static_cast<uintptr_t>(oc->TextureId()));
        REQUIRE(output != nullptr);

        nvrhi::IDevice* nv = device->Nvrhi();
        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(kSize).setHeight(kSize)
            .setFormat(nvrhi::Format::BGRA8_UNORM)  // OffscreenCanvas output fmt
            .setDebugName("OffscreenReadback");
        nvrhi::StagingTextureHandle staging =
            nv->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
        REQUIRE(staging != nullptr);

        nvrhi::CommandListHandle cl = nv->createCommandList();
        cl->open();
        cl->copyTexture(staging, nvrhi::TextureSlice(),
                        output, nvrhi::TextureSlice());
        cl->close();
        nv->executeCommandList(cl);
        nv->waitForIdle();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(nv->mapStagingTexture(
            staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read,
            &rowPitch));
        REQUIRE(pixels != nullptr);

        // BGRA8_UNORM: 4 bytes/texel, [B, G, R, A]. Sample the center (inside
        // the white disc) and a corner (clear color only).
        auto Texel = [&](uint32_t x, uint32_t y) {
            return pixels + y * rowPitch + x * 4;
        };
        const uint8_t* mid = Texel(kSize / 2, kSize / 2);
        const uint8_t* corner = Texel(2, 2);  // off both the disc and the line

        // Center is the white disc: every color channel near full. The clear
        // (0.03..0.05 linear -> tonemapped) lands well under 128; the white
        // disc lands well over. Assert a comfortable margin in BOTH directions.
        CHECK((int)mid[0] > 200);   // B
        CHECK((int)mid[1] > 200);   // G
        CHECK((int)mid[2] > 200);   // R
        CHECK((int)mid[3] == 255);  // A (opaque output)

        // Corner stays near the dark clear: each color channel well below mid.
        CHECK((int)corner[0] < 96);
        CHECK((int)corner[1] < 96);
        CHECK((int)corner[2] < 96);

        // Center must be substantially brighter than the corner (the load-
        // bearing "geometry is visible, not just the clear color" assertion).
        CHECK(((int)mid[2] - (int)corner[2]) > 128);  // R channel delta

        nv->unmapStagingTexture(staging);
        nv->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: OffscreenCanvas renders a pass to a texture",
          "[gpu][debugviz][d3d12]")
{
    CheckOffscreenCanvas(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: OffscreenCanvas renders a pass to a texture",
          "[gpu][debugviz][vulkan]")
{
    CheckOffscreenCanvas(Arcane::GraphicsBackend::Vulkan);
}

TEST_CASE("d3d12: OffscreenCanvas geometry is visible in the output texture",
          "[gpu][debugviz][d3d12]")
{
    CheckOffscreenCanvasReadback(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: OffscreenCanvas geometry is visible in the output texture",
          "[gpu][debugviz][vulkan]")
{
    CheckOffscreenCanvasReadback(Arcane::GraphicsBackend::Vulkan);
}
