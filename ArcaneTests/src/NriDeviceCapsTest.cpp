// NRI substrate: the device capability snapshot (NriDeviceCaps). Proves
// FinishWrap queries nri::DeviceDesc's tiers/features exactly once and the
// snapshot is populated (not left at NriDeviceCaps's construction defaults
// by a missed query). Device-less -- [nri], inside the ~[gpu] dev gate.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>

TEST_CASE("nri device caps: the NONE backend reports a coherent snapshot", "[nri]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    const Arcane::NriDeviceCaps& caps = device->Caps();
    // NONE answers every query with defaults; what is pinned is that the
    // snapshot is POPULATED (not left at construction defaults by a missed
    // query) and internally consistent. All four fields are tied to a LIVE
    // re-query of the same device below, so a field left at NriDeviceCaps's
    // struct default (0 / false) by a missed query in FinishWrap fails here.
    const nri::DeviceDesc& liveDesc = device->Core().GetDeviceDesc(device->Device());
    CHECK(caps.SupportsBindless() == (caps.bindlessTier > 0));
    CHECK(caps.bindlessTier             == liveDesc.tiers.bindless);
    CHECK(caps.rayTracingTier           == liveDesc.tiers.rayTracing);
    CHECK(caps.meshShader               == liveDesc.features.meshShader);
    CHECK(caps.maxDescriptorSetTextures == liveDesc.descriptorSet.textureMaxNum);
}
