// Selection outline: PickPassId reverse-lookup ([pick], CPU) + the outline pass ([gpu]).
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/PickEmit.hpp>

#include <Astra/Entity/Entity.hpp>

#include <vector>

TEST_CASE("PickPassId maps ordered entities to k+1, 0 for absent/invalid", "[pick]")
{
    const Astra::Entity a = Astra::Entity(1, 0);
    const Astra::Entity b = Astra::Entity(2, 0);
    const Astra::Entity c = Astra::Entity(3, 0);
    const std::vector<Astra::Entity> ordered{ a, b, c };

    CHECK(Arcane::PickPassId(ordered, a) == 1u);
    CHECK(Arcane::PickPassId(ordered, b) == 2u);
    CHECK(Arcane::PickPassId(ordered, c) == 3u);
    CHECK(Arcane::PickPassId(ordered, Astra::Entity(9, 0)) == 0u);   // absent
    CHECK(Arcane::PickPassId(ordered, Astra::Entity::Invalid()) == 0u);
    CHECK(Arcane::PickPassId({}, a) == 0u);                          // empty
}

// ===========================================================================
// GPU test ([gpu][selection]) -- DESK/CI-DRIVEN. Needs a real device (per-backend,
// like TonemapTest/PickBufferTest) and hits the Parsec GPU-driver hazard headless,
// so the ~[gpu] dev-loop excludes it. Run at the desk/CI:
//   ArcaneTests.exe "[gpu][selection]"
// ===========================================================================

#include <cstdint>
#include <cstring>

#include <glm/glm.hpp>

#include <nvrhi/nvrhi.h>

#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/SelectionOutline.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

namespace
{
    // Two id rects into one buffer: id=5 in [8,24), id=7 in [40,56).
    nvrhi::TextureHandle MakeTwoIdTexture(nvrhi::IDevice* nv, uint32_t size)
    {
        auto desc = nvrhi::TextureDesc()
            .setWidth(size).setHeight(size)
            .setFormat(nvrhi::Format::R32_UINT)
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true)
            .setDebugName("SelectionOutline.IdTex");
        nvrhi::TextureHandle tex = nv->createTexture(desc);
        REQUIRE(tex != nullptr);

        std::vector<uint32_t> ids(static_cast<size_t>(size) * size, 0u);
        auto fill = [&](uint32_t id, uint32_t lo, uint32_t hi) {
            for (uint32_t y = lo; y < hi; ++y)
                for (uint32_t x = lo; x < hi; ++x)
                    ids[static_cast<size_t>(y) * size + x] = id;
        };
        fill(5u, 8u, 24u);    // selected rect
        fill(7u, 40u, 56u);   // hovered rect

        nvrhi::CommandListHandle upload = nv->createCommandList();
        upload->open();
        upload->writeTexture(tex, 0, 0, ids.data(), static_cast<size_t>(size) * sizeof(uint32_t));
        upload->close();
        nv->executeCommandList(upload);
        return tex;
    }

    // Read the full BGRA8 target into a row-major vector<uint8_t> (4 bytes/px),
    // handling the staging row pitch.
    std::vector<uint8_t> ReadBgra8(nvrhi::IDevice* nv, nvrhi::ITexture* target, uint32_t size)
    {
        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(size).setHeight(size)
            .setFormat(nvrhi::Format::BGRA8_UNORM)
            .setDebugName("SelectionOutline.Readback");
        nvrhi::StagingTextureHandle staging =
            nv->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
        REQUIRE(staging != nullptr);

        nvrhi::CommandListHandle cl = nv->createCommandList();
        cl->open();
        cl->copyTexture(staging, nvrhi::TextureSlice(), target, nvrhi::TextureSlice());
        cl->close();
        nv->executeCommandList(cl);
        nv->waitForIdle();

        size_t rowPitch = 0;
        const auto* bytes = static_cast<const uint8_t*>(nv->mapStagingTexture(
            staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(bytes != nullptr);
        std::vector<uint8_t> out(static_cast<size_t>(size) * size * 4);
        for (uint32_t y = 0; y < size; ++y)
            std::memcpy(&out[static_cast<size_t>(y) * size * 4],
                        bytes + static_cast<size_t>(y) * rowPitch,
                        static_cast<size_t>(size) * 4);
        nv->unmapStagingTexture(staging);
        return out;
    }

    // The outline pass draws amber around the selected id and cyan around the
    // hovered id (derived in-shader from the cursor pixel), leaving the rest of
    // the display-referred target untouched.
    void CheckSelectionOutline(Arcane::GraphicsBackend backend)
    {
        constexpr uint32_t kSize = 64;
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        nvrhi::IDevice* nv = device->Nvrhi();

        auto shaders = Arcane::ShaderLibrary::Create(nv, backend, "shaders");
        REQUIRE(shaders != nullptr);

        auto outline = Arcane::SelectionOutline::Create(nv, *shaders);
        REQUIRE(outline != nullptr);

        // id=5 rect [8,24), id=7 rect [40,56); background 0.
        nvrhi::TextureHandle idTex = MakeTwoIdTexture(nv, kSize);

        // Display-referred BGRA8 target cleared to mid-gray (128 each channel).
        auto targetDesc = nvrhi::TextureDesc()
            .setWidth(kSize).setHeight(kSize)
            .setFormat(nvrhi::Format::BGRA8_UNORM)
            .setIsRenderTarget(true)
            .setInitialState(nvrhi::ResourceStates::RenderTarget)
            .setKeepInitialState(true)
            .setDebugName("SelectionOutline.Target");
        nvrhi::TextureHandle target = nv->createTexture(targetDesc);
        REQUIRE(target != nullptr);
        nvrhi::FramebufferHandle targetFb = nv->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(target));
        REQUIRE(targetFb != nullptr);

        Arcane::SelectionOutline::Params p;
        p.selectedId = 5;
        p.cursorPx   = glm::ivec2(48, 48);   // inside the id=7 rect -> hoveredId = 7

        nvrhi::CommandListHandle cmd = nv->createCommandList();
        cmd->open();
        cmd->clearTextureFloat(target, nvrhi::AllSubresources,
                               nvrhi::Color(0.5f, 0.5f, 0.5f, 1.0f));
        outline->Render(cmd, idTex, targetFb, p);
        cmd->close();
        nv->executeCommandList(cmd);
        nv->waitForIdle();

        const std::vector<uint8_t> px = ReadBgra8(nv, target, kSize);
        auto B = [&](uint32_t x, uint32_t y) { return (int)px[(static_cast<size_t>(y) * kSize + x) * 4 + 0]; };
        auto G = [&](uint32_t x, uint32_t y) { return (int)px[(static_cast<size_t>(y) * kSize + x) * 4 + 1]; };
        auto R = [&](uint32_t x, uint32_t y) { return (int)px[(static_cast<size_t>(y) * kSize + x) * 4 + 2]; };

        // Just OUTSIDE the id=5 rect's left edge (x=7, rect starts x=8): amber
        // (R high, B low) -- distinct from cyan and the gray clear.
        CHECK(R(7, 16) > 200);
        CHECK(B(7, 16) < 80);

        // Just OUTSIDE the id=7 rect's left edge (x=39, rect starts x=40): cyan
        // (B high, R low).
        CHECK(B(39, 48) > 200);
        CHECK(R(39, 48) < 120);

        // Deep background between both rects (Chebyshev distance > either
        // thickness): untouched -> still the mid-gray clear (~128 each channel).
        CHECK(std::abs(R(32, 32) - 128) <= 2);
        CHECK(std::abs(G(32, 32) - 128) <= 2);
        CHECK(std::abs(B(32, 32) - 128) <= 2);

        nv->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: SelectionOutline draws amber around selectedId, cyan around hovered",
          "[gpu][selection][d3d12]")
{
    CheckSelectionOutline(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: SelectionOutline draws amber around selectedId, cyan around hovered",
          "[gpu][selection][vulkan]")
{
    CheckSelectionOutline(Arcane::GraphicsBackend::Vulkan);
}
