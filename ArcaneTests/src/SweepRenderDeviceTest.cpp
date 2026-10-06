#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Render/RenderDeviceSettings.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>
#include <Arcane/Config/CVarFormat.hpp>
#include <Arcane/Host/EarlyConfig.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <array>
#include <string>
#include <vector>
using namespace Arcane;
TEST_CASE("sweep: render device defaults are the pre-sweep values, per configuration", "[sweep][render-device]")
{
#if defined(ARC_BUILD_DEBUG)
    constexpr bool debugOn = true;
#else
    constexpr bool debugOn = false;
#endif
    const RenderDebugSettings d{};
    CHECK(d.validation == debugOn);
    CHECK(d.d3d12DebugLayer == debugOn);     // the graph vehicle forced it in Debug
    CHECK(d.vkSyncValidation == debugOn);
    CHECK(RenderSettings{}.backend == GraphicsBackend::D3D12);
    CHECK(RenderSettings{}.vsync);
    CHECK(RenderSettings{}.adapter == -1);
    CHECK_FALSE(RenderSettings{}.allowTearing);
    CHECK(RenderD3d12Settings{}.zeroBufferBytes == 0u);
    CHECK(RenderD3d12Settings{}.enhancedBarriers);
    CHECK(RenderD3d12Settings{}.deviceArmorRefs == 65536u);
    CHECK(RenderVulkanSettings{}.foreignModuleFallback);
    Test::RequireDefault("render.vsync", CVarValue::Bool(true));
    Test::RequireDefault("render.debug.validation", CVarValue::Bool(debugOn));
    const RenderDeviceDesc desc = MakeRenderDeviceDesc();
    CHECK(desc.enableValidation == debugOn);
    CHECK(desc.enableD3D12DebugLayer == debugOn);
    CHECK(desc.enableSyncValidation == debugOn);
}

TEST_CASE("sweep: the debug-message policy defaults are today's (no break, Warning and worse reported)", "[sweep][render-device]")
{
    const RenderDebugSettings d{};
    for (BreakSeverity s : { BreakSeverity::Corruption, BreakSeverity::Error, BreakSeverity::Warning })
        CHECK_FALSE(BreaksOn(d.breakOnSeverity, s));          // SetBreakOnSeverity(FALSE) x3
    CHECK_FALSE(Reports(d.minSeverity, MinSeverity::Info));   // the INFO/MESSAGE deny list; no eInfo bit
    CHECK(Reports(d.minSeverity, MinSeverity::Warning));
    CHECK(Reports(d.minSeverity, MinSeverity::Error));

    // The other settings mean what they say: a severity breaks along with every worse one.
    CHECK(BreaksOn(BreakSeverity::Error, BreakSeverity::Corruption));
    CHECK(BreaksOn(BreakSeverity::Error, BreakSeverity::Error));
    CHECK_FALSE(BreaksOn(BreakSeverity::Error, BreakSeverity::Warning));
    CHECK(Reports(MinSeverity::Info, MinSeverity::Info));
    CHECK_FALSE(Reports(MinSeverity::Error, MinSeverity::Warning));
}

TEST_CASE("sweep: the pending-cook checker defaults are the old bytes, magenta and (16,16,16)", "[sweep][render-device]")
{
    const RenderDebugSettings d{};
    CHECK(CVarColorToHex(d.pendingCookChecker) == "#FF00FFFF");
    CHECK(CVarColorToHex(d.pendingCookCheckerAlt) == "#101010FF");
    CHECK(Srgb8Bytes(d.pendingCookChecker) == std::array<std::uint8_t, 4>{ 255, 0, 255, 255 });
    CHECK(Srgb8Bytes(d.pendingCookCheckerAlt) == std::array<std::uint8_t, 4>{ 16, 16, 16, 255 });
    Test::RequireDefault("render.debug.pendingCookChecker", CVarValue::Color(d.pendingCookChecker));
}

TEST_CASE("sweep: --backend and --no-vsync reach render.backend/render.vsync on the CommandLine rung, and the HostConfig adopts the published values",
          "[sweep][render-device]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const auto parse = [](std::vector<std::string> words) {
        std::vector<char*> argv;
        for (std::string& w : words) argv.push_back(w.data());
        HostConfig::ParseOutcome out = HostConfig::Parse(static_cast<int>(argv.size()), argv.data());
        REQUIRE(out.config.has_value());
        return *out.config;
    };

    {
        HostConfig cfg = parse({ "ArcaneRuntime", "--backend", "vulkan", "--no-vsync" });
        HostBoot::ApplyEarlyConfigRungs(cfg, CVarContext::Editor, /*editor*/ false);
        CHECK(Settings<RenderSettings>().backend == GraphicsBackend::Vulkan);
        CHECK_FALSE(Settings<RenderSettings>().vsync);
        CHECK(cfg.backend == GraphicsBackend::Vulkan);
        CHECK_FALSE(cfg.vsync);
        reg.RevertLayer(SetBy::CommandLine); reg.PublishImmediate();
    }
    {
        // No flag: the setting (here from --set) is what the host boots with,
        // and an absent --backend does not pin the rung to D3D12.
        HostConfig cfg = parse({ "ArcaneRuntime", "--set", "render.backend=Vulkan", "--set", "render.vsync=false" });
        CHECK(cfg.backend == GraphicsBackend::D3D12);   // the parsed default, before the rungs
        HostBoot::ApplyEarlyConfigRungs(cfg, CVarContext::Editor, /*editor*/ false);
        CHECK(cfg.backend == GraphicsBackend::Vulkan);
        CHECK_FALSE(cfg.vsync);
        reg.RevertLayer(SetBy::CommandLine); reg.PublishImmediate();
    }
    {
        HostConfig cfg = parse({ "ArcaneRuntime" });
        HostBoot::ApplyEarlyConfigRungs(cfg, CVarContext::Editor, /*editor*/ false);
        CHECK(cfg.backend == Settings<RenderSettings>().backend);
        CHECK(cfg.vsync == Settings<RenderSettings>().vsync);
        reg.RevertLayer(SetBy::CommandLine); reg.PublishImmediate();
    }
}
