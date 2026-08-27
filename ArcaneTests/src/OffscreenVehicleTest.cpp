#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/OffscreenVehicle.hpp>

TEST_CASE("offscreen vehicle builds a device with no window (d3d12)", "[gpu][offscreen]")
{
    Arcane::HostConfig cfg;
    cfg.backend   = Arcane::GraphicsBackend::D3D12;
    cfg.headless = true;

    auto v = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(v != nullptr);
    CHECK(v->Graph().IsOffscreen());
    CHECK(v->Graph().SurfaceWidth()  == 256);
    CHECK(v->Graph().SurfaceHeight() == 128);
}

TEST_CASE("offscreen vehicle builds a device with no window (vulkan)", "[gpu][offscreen]")
{
    Arcane::HostConfig cfg;
    cfg.backend   = Arcane::GraphicsBackend::Vulkan;
    cfg.headless = true;

    auto v = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(v != nullptr);
    CHECK(v->Graph().IsOffscreen());
}
