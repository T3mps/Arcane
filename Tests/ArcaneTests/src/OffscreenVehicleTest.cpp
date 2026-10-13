#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/OffscreenVehicle.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>   // RenderDeviceRemovedHookForTest
#include <Arcane/Render/Nri/NriDiagnostics.hpp>

#include "Helpers/GpuCapability.hpp"

TEST_CASE("offscreen vehicle builds a device with no window (d3d12)", "[gpu][offscreen]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::D3D12);

    Arcane::HostConfig cfg;
    cfg.backend   = Arcane::GraphicsBackend::D3D12;
    cfg.headless  = true;

    auto v = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(v != nullptr);
    CHECK(v->Graph().IsOffscreen());
    CHECK(v->Graph().SurfaceWidth()  == 256);
    CHECK(v->Graph().SurfaceHeight() == 128);
}

TEST_CASE("offscreen vehicle builds a device with no window (vulkan)", "[gpu][offscreen]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::Vulkan);

    Arcane::HostConfig cfg;
    cfg.backend   = Arcane::GraphicsBackend::Vulkan;
    cfg.headless  = true;

    auto v = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(v != nullptr);
    CHECK(v->Graph().IsOffscreen());
}

// =============================================================================
// THE VEHICLE ARMS THE CRASH CHAIN ON THE DEVICE IT CREATES (G1, R109)
// =============================================================================
// The vehicle is the device's OWNER under --headless (both hosts), so it is the
// one that arms -- exactly as the windowed NriGraphContext::Create arms the
// device it creates. Before this, no --headless process ever armed: a device
// loss latched through RenderErrorLatch::NoteDeviceLost with no hook installed,
// the host stopped with render-failed, and no gpu-crash report was written.

namespace
{
    void CheckVehicleArmsAndDisarms(Arcane::GraphicsBackend backend)
    {
        REQUIRE_FALSE(Arcane::NriDiagnostics::IsArmed());
        REQUIRE(Arcane::RenderDeviceRemovedHookForTest() == nullptr);

        Arcane::HostConfig cfg;
        cfg.backend  = backend;
        cfg.headless = true;

        {
            auto v = Arcane::OffscreenVehicle::Create(cfg, 64, 64);
            REQUIRE(v != nullptr);

            // The whole chain: the backend slot, the device-removed hook (the
            // piece G1 found missing), all installed by the vehicle's Arm.
            CHECK(Arcane::NriDiagnostics::IsArmed());
            CHECK(Arcane::RenderDeviceRemovedHookForTest() != nullptr);
            CHECK(Arcane::ActiveGpuCrashBackend() == Arcane::NriDiagnostics::ArmedBackend());
        }

        // The armer disarms, before its device goes away.
        CHECK_FALSE(Arcane::NriDiagnostics::IsArmed());
        CHECK(Arcane::RenderDeviceRemovedHookForTest() == nullptr);
        CHECK(Arcane::ActiveGpuCrashBackend() == nullptr);
    }

    void CheckVehicleLeavesAnIncumbentChainAlone(Arcane::GraphicsBackend backend)
    {
        REQUIRE_FALSE(Arcane::NriDiagnostics::IsArmed());

        // The incumbent: a chain someone else armed first (a NONE device
        // stands in for the owner).
        auto incumbentDevice = Arcane::NriDevice::CreateNoneForTests();
        REQUIRE(incumbentDevice != nullptr);
        REQUIRE(Arcane::NriDiagnostics::Arm(*incumbentDevice));
        Arcane::IGpuCrashBackend* const incumbent = Arcane::NriDiagnostics::ArmedBackend();

        Arcane::HostConfig cfg;
        cfg.backend  = backend;
        cfg.headless = true;

        {
            auto v = Arcane::OffscreenVehicle::Create(cfg, 64, 64);
            REQUIRE(v != nullptr);
            CHECK(Arcane::NriDiagnostics::ArmedBackend() == incumbent);
        }

        // A vehicle that did not arm must not disarm the incumbent's chain.
        CHECK(Arcane::NriDiagnostics::IsArmed());
        CHECK(Arcane::NriDiagnostics::ArmedBackend() == incumbent);

        Arcane::NriDiagnostics::Disarm();
        CHECK_FALSE(Arcane::NriDiagnostics::IsArmed());
    }
}

TEST_CASE("offscreen vehicle arms the crash chain on its own device (d3d12)", "[gpu][offscreen]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::D3D12);
    CheckVehicleArmsAndDisarms(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("offscreen vehicle arms the crash chain on its own device (vulkan)", "[gpu][offscreen]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::Vulkan);
    CheckVehicleArmsAndDisarms(Arcane::GraphicsBackend::Vulkan);
}

TEST_CASE("offscreen vehicle leaves an already-armed crash chain alone (vulkan)", "[gpu][offscreen]")
{
    ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::Vulkan);
    CheckVehicleLeavesAnIncumbentChainAlone(Arcane::GraphicsBackend::Vulkan);
}
