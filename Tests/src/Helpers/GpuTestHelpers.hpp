#pragma once

// Shared GPU test helper: clears a 4x4 offscreen RGBA8 target to a known
// color, copies it to a staging texture, maps it on the CPU, and asserts
// the bytes. The strongest headless proof that a device does real work.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <Arcane/Render/Device.hpp>

// fp16 -> float for RGBA16_FLOAT readback checks. Shared by every GPU test
// that maps an HDR canvas (BatcherTest, TextTest, ...).
inline float HalfToFloat(uint16_t h)
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

inline void CheckOffscreenClear(Arcane::RenderDevice& device)
{
    nvrhi::IDevice* nv = device.Nvrhi();

    auto targetDesc = nvrhi::TextureDesc()
        .setWidth(4)
        .setHeight(4)
        .setFormat(nvrhi::Format::RGBA8_UNORM)
        .setIsRenderTarget(true)
        .setInitialState(nvrhi::ResourceStates::RenderTarget)
        .setKeepInitialState(true)
        .setDebugName("ClearTarget");
    nvrhi::TextureHandle target = nv->createTexture(targetDesc);
    REQUIRE(target != nullptr);

    auto stagingDesc = nvrhi::TextureDesc()
        .setWidth(4)
        .setHeight(4)
        .setFormat(nvrhi::Format::RGBA8_UNORM)
        .setDebugName("ClearReadback");
    nvrhi::StagingTextureHandle staging =
        nv->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
    REQUIRE(staging != nullptr);

    nvrhi::CommandListHandle commandList = nv->createCommandList();
    commandList->open();
    commandList->clearTextureFloat(target, nvrhi::AllSubresources,
                                   nvrhi::Color(1.0f, 0.5f, 0.25f, 1.0f));
    commandList->copyTexture(staging, nvrhi::TextureSlice(),
                             target, nvrhi::TextureSlice());
    commandList->close();
    nv->executeCommandList(commandList);
    nv->waitForIdle();

    size_t rowPitch = 0;
    const auto* pixels = static_cast<const uint8_t*>(nv->mapStagingTexture(
        staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
    REQUIRE(pixels != nullptr);
    // UNORM bytes for (1.0, 0.5, 0.25, 1.0): 255, ~128, ~64, 255.
    // +-1 tolerance on the rounded channels (UNORM round-to-nearest).
    CHECK((int)pixels[0] == 255);
    CHECK(std::abs((int)pixels[1] - 128) <= 1);
    CHECK(std::abs((int)pixels[2] - 64) <= 1);
    CHECK((int)pixels[3] == 255);
    nv->unmapStagingTexture(staging);
    nv->runGarbageCollection();

    // Foundation rule: NVRHI validation stays silent.
    CHECK(Arcane::RenderErrorCount() == 0);
}
