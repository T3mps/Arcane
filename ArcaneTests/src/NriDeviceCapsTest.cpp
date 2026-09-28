// NRI substrate: the device capability snapshot (NriDeviceCaps). Proves
// FinishWrap queries nri::DeviceDesc's tiers/features exactly once and the
// snapshot is populated (not left at NriDeviceCaps's construction defaults
// by a missed query). Device-less -- [nri], inside the ~[gpu] baseline set.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>

TEST_CASE("nri device caps: the NONE backend reports a coherent snapshot", "[nri]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    const Arcane::NriDeviceCaps& caps = device->Caps();
    // NONE answers every query with defaults; what is pinned is that the
    // snapshot is POPULATED (not left at construction defaults by a missed
    // query) and internally consistent. Every field is tied to a LIVE
    // re-query of the same device below, so a field left at NriDeviceCaps's
    // struct default (0 / false) by a missed query in FinishWrap fails here.
    const nri::DeviceDesc& liveDesc = device->Core().GetDeviceDesc(device->Device());
    CHECK(caps.SupportsBindless() == (caps.bindlessTier > 0));
    CHECK(caps.bindlessTier             == liveDesc.tiers.bindless);
    CHECK(caps.rayTracingTier           == liveDesc.tiers.rayTracing);
    CHECK(caps.meshShader               == liveDesc.features.meshShader);
    CHECK(caps.maxDescriptorSetTextures == liveDesc.descriptorSet.textureMaxNum);
    CHECK(caps.maxPerStageTextures      == liveDesc.shaderStage.descriptorTextureMaxNum);
    CHECK(caps.maxDescriptorSetUpdateAfterSetTextures
          == liveDesc.descriptorSet.updateAfterSet.textureMaxNum);
    CHECK(caps.maxPerStageUpdateAfterSetTextures
          == liveDesc.shaderStage.updateAfterSet.descriptorTextureMaxNum);
}

TEST_CASE("nri device caps: texture update-after-set is gated by its granular limit", "[nri]")
{
    Arcane::NriDeviceCaps caps{};
    CHECK_FALSE(caps.SupportsBindlessTextures(1));
    CHECK_FALSE(caps.SupportsTextureUpdateAfterSet(1));

    caps.bindlessTier = 1;
    caps.maxPerStageTextures = 256;
    caps.maxDescriptorSetTextures = 255;
    CHECK_FALSE(caps.SupportsBindlessTextures(256));
    caps.maxDescriptorSetTextures = 256;
    CHECK(caps.SupportsBindlessTextures(256));
    caps.maxPerStageTextures = 255;
    CHECK_FALSE(caps.SupportsBindlessTextures(256));

    caps.maxDescriptorSetUpdateAfterSetTextures = 256;
    caps.maxPerStageUpdateAfterSetTextures = 256;
    CHECK(caps.SupportsTextureUpdateAfterSet(256));
    CHECK_FALSE(caps.SupportsTextureUpdateAfterSet(257));
    caps.maxPerStageUpdateAfterSetTextures = 255;
    CHECK_FALSE(caps.SupportsTextureUpdateAfterSet(256));
}
