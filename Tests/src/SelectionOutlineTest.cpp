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

    // The seed+JFA pass builds a nearest-EDGE distance field from a supersampled id
    // buffer: only BOUNDARY 1x pixels seed their sub-pixel centroid; the flood then
    // resolves every other pixel (interior + exterior) to the nearest silhouette edge.
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

        // Interior pixel (10,10): boundary-seeded now, so it is FLOODED to the nearest
        // silhouette EDGE (a boundary pixel of the [8,24)^2 rect) -- still tagged select
        // (+1) with cov>0, and the resolved edge position stays inside [8,24)^2.
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

    // Two disjoint filled rects (id1 at [lo1,hi1)^2, id2 at [lo2,hi2)^2), background
    // 0, into a SUPERSAMPLED R32_UINT id buffer.
    nvrhi::TextureHandle MakeIdTexture2Rects(nvrhi::IDevice* nv, uint32_t size,
                                             uint32_t id1, uint32_t lo1, uint32_t hi1,
                                             uint32_t id2, uint32_t lo2, uint32_t hi2)
    {
        auto desc = nvrhi::TextureDesc()
            .setWidth(size).setHeight(size)
            .setFormat(nvrhi::Format::R32_UINT)
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true)
            .setDebugName("SelectionOutline.IdTex2");
        nvrhi::TextureHandle tex = nv->createTexture(desc);
        REQUIRE(tex != nullptr);

        std::vector<uint32_t> ids(static_cast<size_t>(size) * size, 0u);
        for (uint32_t y = lo1; y < hi1; ++y)
            for (uint32_t x = lo1; x < hi1; ++x)
                ids[static_cast<size_t>(y) * size + x] = id1;
        for (uint32_t y = lo2; y < hi2; ++y)
            for (uint32_t x = lo2; x < hi2; ++x)
                ids[static_cast<size_t>(y) * size + x] = id2;

        nvrhi::CommandListHandle upload = nv->createCommandList();
        upload->open();
        upload->writeTexture(tex, 0, 0, ids.data(), static_cast<size_t>(size) * sizeof(uint32_t));
        upload->close();
        nv->executeCommandList(upload);
        return tex;
    }

    // Read a BGRA8_UNORM target into a row-major vector<uint8_t> (4/px, BGRA order),
    // handling the staging row pitch.
    std::vector<uint8_t> ReadBgra(nvrhi::IDevice* nv, nvrhi::ITexture* tex, uint32_t size)
    {
        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(size).setHeight(size)
            .setFormat(nvrhi::Format::BGRA8_UNORM)
            .setDebugName("SelectionOutline.CompositeReadback");
        nvrhi::StagingTextureHandle staging =
            nv->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
        REQUIRE(staging != nullptr);

        nvrhi::CommandListHandle cl = nv->createCommandList();
        cl->open();
        cl->copyTexture(staging, nvrhi::TextureSlice(), tex, nvrhi::TextureSlice());
        cl->close();
        nv->executeCommandList(cl);
        nv->waitForIdle();

        size_t rowPitch = 0;
        const auto* base = static_cast<const uint8_t*>(nv->mapStagingTexture(
            staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(base != nullptr);
        std::vector<uint8_t> out(static_cast<size_t>(size) * size * 4);
        for (uint32_t y = 0; y < size; ++y)
            std::memcpy(&out[static_cast<size_t>(y) * size * 4], base + static_cast<size_t>(y) * rowPitch,
                        static_cast<size_t>(size) * 4);
        nv->unmapStagingTexture(staging);
        return out;
    }

    // Full pipeline: seed + JFA + composite. Asserts the anti-aliased amber/cyan
    // outline STRADDLES the silhouette in a display-referred BGRA8 target: interior
    // edge pixels painted (the into-the-object half), an exterior tint (the outer
    // half), deep interior + background untouched, and a round (not square) metric.
    // RenderErrorCount()==0.
    void CheckSelectionComposite(Arcane::GraphicsBackend backend)
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

        // 128x128 (ss=2) id buffer: id=5 filling 2x [16,48)^2 -> 1x silhouette [8,24)^2;
        // id=7 filling 2x [80,112)^2 -> 1x silhouette [40,56)^2.
        nvrhi::TextureHandle idTex =
            MakeIdTexture2Rects(nv, kSize * 2u, 5u, 16u, 48u, 7u, 80u, 112u);

        // Display-referred BGRA8 target, pre-cleared to a known background (0.2 gray;
        // byte 51). The composite blends the ring OVER this; discarded texels keep it.
        auto targetDesc = nvrhi::TextureDesc()
            .setWidth(kSize).setHeight(kSize)
            .setFormat(nvrhi::Format::BGRA8_UNORM)
            .setIsRenderTarget(true)
            .setInitialState(nvrhi::ResourceStates::RenderTarget)
            .setKeepInitialState(true)
            .setDebugName("SelectionOutline.CompositeTarget");
        nvrhi::TextureHandle target = nv->createTexture(targetDesc);
        REQUIRE(target != nullptr);
        nvrhi::FramebufferHandle targetFb = nv->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(target));
        REQUIRE(targetFb != nullptr);

        Arcane::SelectionOutline::Params p;
        p.selectedId        = 5;
        p.cursorPx          = glm::ivec2(48, 48);   // inside id=7's 1x rect -> hovered = 7
        p.selectThicknessPx = 3.0f;
        p.hoverThicknessPx  = 3.0f;
        p.edgeSoftnessPx    = 2.0f;                 // centered band: halfW=1.5, ramp d in (-0.5,1.5)

        nvrhi::CommandListHandle cmd = nv->createCommandList();
        cmd->open();
        cmd->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.2f, 0.2f, 0.2f, 1.0f));
        outline->Render(cmd, idTex, targetFb, p);
        cmd->close();
        nv->executeCommandList(cmd);
        nv->waitForIdle();

        const std::vector<uint8_t> px = ReadBgra(nv, target, kSize);
        auto B = [&](int x, int y) { return (int)px[(static_cast<size_t>(y) * kSize + x) * 4 + 0]; };
        auto G = [&](int x, int y) { return (int)px[(static_cast<size_t>(y) * kSize + x) * 4 + 1]; };
        auto R = [&](int x, int y) { return (int)px[(static_cast<size_t>(y) * kSize + x) * 4 + 2]; };

        // (1) STRADDLE -- interior edge painted. (23,15) is the id=5 rect's rightmost
        //     covered column; with boundary-seeding the band now paints amber ON the
        //     object (the 1px-into-the-object the outline gained). Amber = R dominant.
        CHECK(R(23, 15) > 150);
        CHECK(R(23, 15) > G(23, 15));
        CHECK(G(23, 15) > B(23, 15));

        // (2) ...and just OUTSIDE the same edge (24,15) is amber-tinted too (the outer
        //     half of the centered band), clearly above the 0.2 gray background.
        CHECK(R(24, 15) > 65);
        CHECK(R(24, 15) > B(24, 15));

        // (3) CYAN straddle on the hovered id=7 rect (silhouette [40,56)^2): its
        //     rightmost covered column (55,48) paints cyan ON the object. B dominant.
        CHECK(B(55, 48) > 150);
        CHECK(B(55, 48) > R(55, 48));

        // (4) BACKGROUND far from both outlines is untouched (still 0.2 gray, byte ~51).
        CHECK(std::abs(R(62, 62) - 51) <= 3);
        CHECK(std::abs(G(62, 62) - 51) <= 3);
        CHECK(std::abs(B(62, 62) - 51) <= 3);

        // (4b) DEEP INTERIOR of id=5 ([8,24)^2) is untouched -- the band straddles the
        //      edge, it does NOT flood the whole silhouette. (15,15), 7px inside, must
        //      stay the unmodified background. (A bug that seeded the fill instead of the
        //      boundary would paint this solid amber; (4) alone wouldn't catch it.)
        CHECK(std::abs(R(15, 15) - 51) <= 3);
        CHECK(std::abs(G(15, 15) - 51) <= 3);
        CHECK(std::abs(B(15, 15) - 51) <= 3);

        // (5) UNIFORM on all four sides (id=5): each rect-edge column/row paints amber
        //     ON the object -- left (8,15), top (15,8), bottom (15,23).
        CHECK(R(8, 15)  > 150);
        CHECK(R(15, 8)  > 150);
        CHECK(R(15, 23) > 150);

        // (5b) ROUND (not square) metric: the exterior diagonal pixel (24,24) is ~1.41px
        //      from the id=5 corner seed -> nearly faded, while the axis pixel (24,15)
        //      at distance ~1 is mid-ramp. A square metric would paint both equally; a
        //      round one fades the diagonal.
        CHECK(R(24, 24) < R(24, 15));
        CHECK(R(24, 24) < 90);   // essentially background -> the corner is rounded

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

TEST_CASE("d3d12: SelectionOutline draws an anti-aliased amber/cyan straddling outline",
          "[gpu][selection][d3d12]")
{
    CheckSelectionComposite(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: SelectionOutline draws an anti-aliased amber/cyan straddling outline",
          "[gpu][selection][vulkan]")
{
    CheckSelectionComposite(Arcane::GraphicsBackend::Vulkan);
}
