// Color-pipeline golden test: linear HDR canvas -> ACES -> TRUE sRGB encode must
// byte-match the CPU reference. The curve is the IEC 61966-2-1 piecewise one,
// deliberately NOT pow(1/2.2) -- it matches what SRGBA8_UNORM applies in hardware
// on input, so the pipeline is symmetric. MIRRORS shaders/tonemap.hlsl; keep the
// two in step (and ArcaneEditor/src/EditorWidgets.cpp's branching copy).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Render/TonemapPass.hpp>

namespace
{
    float AcesFilmic(float x)
    {
        const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
        return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
    }

    // Same branchless algebra as the shader, so the only difference between the
    // two is float evaluation order -- which the +/-2 byte tolerance below
    // absorbs. Do NOT "simplify" this to the branching form: an identical
    // expression is what makes this a drift detector rather than a second
    // opinion.
    uint8_t ExpectedByte(float linearChannel)
    {
        const float lin = AcesFilmic(linearChannel);
        const float display = std::min(lin * 12.92f,
                                       std::pow(std::max(lin, 0.0031308f), 1.0f / 2.4f)
                                           * 1.055f - 0.055f);
        return (uint8_t)std::lround(display * 255.0f);
    }

    void CheckTonemapGolden(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        nvrhi::IDevice* nv = device->Nvrhi();

        auto shaders = Arcane::ShaderLibrary::Create(nv, backend, "shaders");
        REQUIRE(shaders != nullptr);

        auto canvas = Arcane::CreateCanvas(nv, 8, 8);
        REQUIRE(canvas != nullptr);
        auto tonemap = Arcane::TonemapPass::Create(nv, *shaders);
        REQUIRE(tonemap != nullptr);

        // Display-referred output target standing in for the backbuffer.
        auto outDesc = nvrhi::TextureDesc()
            .setWidth(8).setHeight(8)
            .setFormat(nvrhi::Format::BGRA8_UNORM)
            .setIsRenderTarget(true)
            .setInitialState(nvrhi::ResourceStates::RenderTarget)
            .setKeepInitialState(true)
            .setDebugName("TonemapOut");
        nvrhi::TextureHandle output = nv->createTexture(outDesc);
        REQUIRE(output != nullptr);
        nvrhi::FramebufferHandle outputFb = nv->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(output));
        REQUIRE(outputFb != nullptr);

        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(8).setHeight(8)
            .setFormat(nvrhi::Format::BGRA8_UNORM)
            .setDebugName("TonemapReadback");
        nvrhi::StagingTextureHandle staging =
            nv->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);

        // 1.5 in red proves the HDR rolloff (would clip without ACES).
        const float lin[3] = { 1.5f, 0.5f, 0.1f };

        nvrhi::CommandListHandle commandList = nv->createCommandList();
        commandList->open();
        commandList->clearTextureFloat(canvas->Texture(), nvrhi::AllSubresources,
                                       nvrhi::Color(lin[0], lin[1], lin[2], 1.0f));
        tonemap->Run(commandList, canvas->Texture(), outputFb);
        commandList->copyTexture(staging, nvrhi::TextureSlice(),
                                 output, nvrhi::TextureSlice());
        commandList->close();
        nv->executeCommandList(commandList);
        nv->waitForIdle();

        size_t rowPitch = 0;
        const auto* pixels = static_cast<const uint8_t*>(nv->mapStagingTexture(
            staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(pixels != nullptr);
        // BGRA byte order; +-2 tolerance (fp16 canvas storage + UNORM rounding).
        CHECK(std::abs((int)pixels[2] - (int)ExpectedByte(lin[0])) <= 2);
        CHECK(std::abs((int)pixels[1] - (int)ExpectedByte(lin[1])) <= 2);
        CHECK(std::abs((int)pixels[0] - (int)ExpectedByte(lin[2])) <= 2);
        CHECK((int)pixels[3] == 255);
        nv->unmapStagingTexture(staging);
        nv->runGarbageCollection();

        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: ACES tonemap matches CPU reference", "[gpu][d3d12]")
{
    CheckTonemapGolden(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: ACES tonemap matches CPU reference", "[gpu][vulkan]")
{
    CheckTonemapGolden(Arcane::GraphicsBackend::Vulkan);
}
