// Real GPU device creation through the engine, headless (no window, no
// swapchain). The offscreen clear+readback helper proves end-to-end GPU
// work: command list, clear, copy to staging, CPU map, byte check.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Device.hpp>

#include "Helpers/GpuTestHelpers.hpp"

#include <string>

TEST_CASE("d3d12: headless device creates and clears an offscreen target", "[gpu][d3d12]")
{
    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::D3D12;
    auto device = Arcane::RenderDevice::Create(desc);
    REQUIRE(device != nullptr);
    REQUIRE(device->Nvrhi() != nullptr);
    REQUIRE_FALSE(device->AdapterName().empty());
    CHECK(std::string(Arcane::ToString(device->Backend())) == "D3D12");

    CheckOffscreenClear(*device);
}

TEST_CASE("vulkan: headless device creates and clears an offscreen target", "[gpu][vulkan]")
{
    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::GraphicsBackend::Vulkan;
    auto device = Arcane::RenderDevice::Create(desc);
    REQUIRE(device != nullptr);
    REQUIRE(device->Nvrhi() != nullptr);
    REQUIRE_FALSE(device->AdapterName().empty());
    CHECK(std::string(Arcane::ToString(device->Backend())) == "Vulkan");

    CheckOffscreenClear(*device);
}
