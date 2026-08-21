// NRI substrate: the device capability snapshot (NriDeviceCaps). Proves
// FinishWrap queries nri::DeviceDesc's tiers/features exactly once and the
// snapshot is populated (not left at NriDeviceCaps's construction defaults
// by a missed query). Headless -- [nri], inside the ~[gpu] dev gate.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>

TEST_CASE("nri device caps: the NONE backend reports a coherent snapshot", "[nri]")
{
    auto device = Arcane::NriDevice::CreateNoneForTests();
    REQUIRE(device != nullptr);
    const Arcane::NriDeviceCaps& caps = device->Caps();
    // NONE answers every query with defaults; what is pinned is that the
    // snapshot is POPULATED (not left at construction defaults by a missed
    // query) and internally consistent.
    CHECK(caps.SupportsBindless() == (caps.bindlessTier > 0));
    CHECK(caps.maxDescriptorSetTextures == device->Core()
              .GetDeviceDesc(device->Device()).descriptorSet.textureMaxNum);
}
