// Selection outline: PickPassId reverse-lookup ([pick], CPU), JfaPassCount
// ([outline], CPU), and the seed+JFA distance-field pass ([gpu]).
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Render/SelectionOutline.hpp>

#include <Astra/Entity/Entity.hpp>

#include <vector>

TEST_CASE("JfaPassCount = ceil(log2(maxThickness)) + 1", "[outline]")
{
    CHECK(Arcane::JfaPassCount(1)  == 1u);
    CHECK(Arcane::JfaPassCount(3)  == 3u);   // ceil(log2 3)=2, +1
    CHECK(Arcane::JfaPassCount(16) == 5u);   // ceil(log2 16)=4, +1
    CHECK(Arcane::JfaPassCount(32) == 6u);
}

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

TEST_CASE("PickSampleTexel maps a 1x click to the center subsample, clamped", "[pick]")
{
    // ss=2, id buffer 128x128 (1x 64x64). Click at 1x pixel (10,20) -> 2x texel (21,41).
    CHECK(Arcane::PickSampleTexel(glm::vec2(10.4f, 20.9f), 2u, 128u, 128u) == glm::ivec2(21, 41));
    // ss=1 is identity (floored), clamped to bounds.
    CHECK(Arcane::PickSampleTexel(glm::vec2(3.7f, 4.2f), 1u, 64u, 64u) == glm::ivec2(3, 4));
    // out-of-range clamps into the buffer.
    CHECK(Arcane::PickSampleTexel(glm::vec2(999.0f, -5.0f), 2u, 128u, 128u) == glm::ivec2(127, 0));
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
#include <Arcane/Render/ShaderLibrary.hpp>

namespace
{
    // A single filled rect [lo,hi)^2 = `id`, background 0, into a SUPERSAMPLED
    // R32_UINT id buffer (2x the composite size).
    nvrhi::TextureHandle MakeIdTexture(nvrhi::IDevice* nv, uint32_t size,
                                       uint32_t id, uint32_t lo, uint32_t hi)
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
        for (uint32_t y = lo; y < hi; ++y)
            for (uint32_t x = lo; x < hi; ++x)
                ids[static_cast<size_t>(y) * size + x] = id;

        nvrhi::CommandListHandle upload = nv->createCommandList();
        upload->open();
        upload->writeTexture(tex, 0, 0, ids.data(), static_cast<size_t>(size) * sizeof(uint32_t));
        upload->close();
        nv->executeCommandList(upload);
        return tex;
    }

    // Read the RGBA16_SNORM distance field into a row-major vector<float> (4/px),
    // decoding int16 SNORM -> float in [-1,1] and handling the staging row pitch.
    std::vector<float> ReadField(nvrhi::IDevice* nv, nvrhi::ITexture* field, uint32_t size)
    {
        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(size).setHeight(size)
            .setFormat(nvrhi::Format::RGBA16_SNORM)
            .setDebugName("SelectionOutline.FieldReadback");
        nvrhi::StagingTextureHandle staging =
            nv->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
        REQUIRE(staging != nullptr);

        nvrhi::CommandListHandle cl = nv->createCommandList();
        cl->open();
        cl->copyTexture(staging, nvrhi::TextureSlice(), field, nvrhi::TextureSlice());
        cl->close();
        nv->executeCommandList(cl);
        nv->waitForIdle();

        size_t rowPitch = 0;
        const auto* base = static_cast<const uint8_t*>(nv->mapStagingTexture(
            staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(base != nullptr);
        std::vector<float> out(static_cast<size_t>(size) * size * 4);
        for (uint32_t y = 0; y < size; ++y)
        {
            const auto* row = reinterpret_cast<const int16_t*>(base + static_cast<size_t>(y) * rowPitch);
            for (uint32_t x = 0; x < size * 4; ++x)
                out[static_cast<size_t>(y) * size * 4 + x] = (float)row[x] / 32767.0f;
        }
        nv->unmapStagingTexture(staging);
        return out;
    }

    // The seed+JFA pass builds a nearest-seed distance field from a supersampled id
    // buffer: covered 1x pixels seed their own centroid; the flood then resolves
    // every background pixel to the nearest silhouette position.
    void CheckSelectionField(Arcane::GraphicsBackend backend)
    {
        constexpr uint32_t kSize = 64;   // 1x (composite) size
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        nvrhi::IDevice* nv = device->Nvrhi();

        auto shaders = Arcane::ShaderLibrary::Create(nv, backend, "shaders");
        REQUIRE(shaders != nullptr);

        auto outline = Arcane::SelectionOutline::Create(nv, *shaders, kSize, kSize);
        REQUIRE(outline != nullptr);

        // 128x128 (ss=2) id buffer: id=5 filling 2x [16,48)^2 -> 1x silhouette [8,24)^2.
        nvrhi::TextureHandle idTex = MakeIdTexture(nv, kSize * 2u, 5u, 16u, 48u);

        Arcane::SelectionOutline::Params p;
        p.selectedId = 5;
        p.cursorPx   = glm::ivec2(-1, -1);   // no hover

        nvrhi::CommandListHandle cmd = nv->createCommandList();
        cmd->open();
        outline->Render(cmd, idTex, /*target (Task 3)*/ nullptr, p);
        cmd->close();
        nv->executeCommandList(cmd);
        nv->waitForIdle();

        const std::vector<float> f = ReadField(nv, outline->DebugDistanceField(), kSize);
        auto at   = [&](int x, int y, int c) { return f[(static_cast<size_t>(y) * kSize + x) * 4 + c]; };
        auto posX = [&](int x, int y) { return (at(x, y, 0) * 0.5f + 0.5f) * (float)kSize; };
        auto posY = [&](int x, int y) { return (at(x, y, 1) * 0.5f + 0.5f) * (float)kSize; };
        auto tag  = [&](int x, int y) { return at(x, y, 2); };
        auto cov  = [&](int x, int y) { return at(x, y, 3); };

        // Interior pixel (10,10): its own seed -- fully covered, tagged select (+1),
        // silhouette position ~(10.5,10.5), inside [8,24)^2.
        CHECK(cov(10, 10) > 0.5f);
        CHECK(tag(10, 10) > 0.5f);
        CHECK(posX(10, 10) >= 8.0f);   CHECK(posX(10, 10) < 24.0f);
        CHECK(posY(10, 10) >= 8.0f);   CHECK(posY(10, 10) < 24.0f);

        // Far background (60,60): flooded to the nearest silhouette corner (~23.5,23.5).
        CHECK(cov(60, 60) > 0.0f);
        CHECK(tag(60, 60) > 0.5f);
        CHECK(posX(60, 60) > 21.0f);   CHECK(posX(60, 60) < 24.5f);
        CHECK(posY(60, 60) > 21.0f);   CHECK(posY(60, 60) < 24.5f);

        nv->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: SelectionOutline JFA builds a nearest-seed distance field",
          "[gpu][selection][d3d12]")
{
    CheckSelectionField(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: SelectionOutline JFA builds a nearest-seed distance field",
          "[gpu][selection][vulkan]")
{
    CheckSelectionField(Arcane::GraphicsBackend::Vulkan);
}
