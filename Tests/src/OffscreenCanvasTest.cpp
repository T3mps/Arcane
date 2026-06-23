// OffscreenCanvas: render a Batcher2D pass into an offscreen texture, get an
// ImTextureID for ImGui::Image. The texture must be valid and the pass must
// run with zero NVRHI validation errors (the foundation rule).

#include <catch2/catch_test_macros.hpp>

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
