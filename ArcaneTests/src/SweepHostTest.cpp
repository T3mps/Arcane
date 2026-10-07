#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Config/ConsoleSettings.hpp>
#include <Arcane/Host/HostSettings.hpp>
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Project/AppSplashSettings.hpp>
#include "Settings/EditorPerfSettings.hpp"
#include <chrono>
using namespace Arcane;
TEST_CASE("sweep: host window/boot defaults are the pre-sweep literals (goldens are 1280x720)", "[sweep][host]")
{
    CHECK(RenderWindowSettings{}.width == 1280u);
    CHECK(RenderWindowSettings{}.height == 720u);
    CHECK(RenderWindowSettings{}.resizable);
    // WindowDesc defaults from the plain RenderWindowSettings struct (Platform/RenderWindowSettings.hpp).
    CHECK(WindowDesc{}.width == RenderWindowSettings{}.width);
    CHECK(WindowDesc{}.height == RenderWindowSettings{}.height);
    CHECK(WindowDesc{}.resizable == RenderWindowSettings{}.resizable);
    CHECK(AppWindowSettings{}.minimizedSleepMs == 1u);
    CHECK(BootSettings{}.scanProgressStride == 32u);
    CHECK(BootSettings{}.splashPumpMs == 8u);
    CHECK(AppSplashSettings{}.width == 480u);
    CHECK(AppSplashSettings{}.height == 270u);
    CHECK(ToSrgb8(AppSplashSettings{}.textColor) == 0xA0A0A0u);
    CHECK(ConsoleSettings{}.windowWidth == 640u);
    CHECK(ConsoleSettings{}.windowHeight == 280u);
    CHECK(ConsoleSettings{}.maxLineChars == 512u);
    CHECK(Editor::EditorPerfSettings{}.backgroundFps == 0u);
    Test::RequireDefault("render.window.width", CVarValue::UInt32(1280u));
    Test::RequireDefault("render.window.height", CVarValue::UInt32(720u));
    Test::RequireDefault("render.window.resizable", CVarValue::Bool(true));
    Test::RequireDefault("app.window.minimizedSleepMs", CVarValue::UInt32(1u));
    Test::RequireDefault("boot.scanProgressStride", CVarValue::UInt32(32u));
    Test::RequireDefault("boot.splashPumpMs", CVarValue::UInt32(8u));
    Test::RequireDefault("console.windowWidth", CVarValue::UInt32(640u));
    Test::RequireDefault("console.windowHeight", CVarValue::UInt32(280u));
    Test::RequireDefault("console.maxLineChars", CVarValue::UInt32(512u));
}

TEST_CASE("sweep: host rows carry the inventory metadata", "[sweep][host]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    for (const char* n : { "render.window.width", "render.window.height" })
    {
        const auto e = reg.Explain(n);
        REQUIRE(e);
        CHECK(e->audience == Audience::PlayerSafe);
        CHECK(e->scope == SettingScope::Project);
        CHECK(e->apply == ApplyMode::Restart);
        const auto d = reg.Describe(n);
        REQUIRE(d);
        REQUIRE(d->min);
        REQUIRE(d->max);
        CHECK(*d->min == CVarValue::UInt32(64u));    // HostConfig::kMinWindowSide
        CHECK(*d->max == CVarValue::UInt32(8192u));  // HostConfig::kMaxWindowSide
    }
    const auto resizable = reg.Explain("render.window.resizable");
    REQUIRE(resizable);
    CHECK(resizable->audience == Audience::Game);
    CHECK(resizable->apply == ApplyMode::Restart);

    struct Row { const char* name; ApplyMode apply; };
    for (const Row r : { Row{ "app.window.minimizedSleepMs", ApplyMode::Live },
                         Row{ "boot.scanProgressStride", ApplyMode::Live },
                         Row{ "boot.splashPumpMs", ApplyMode::Restart },
                         Row{ "console.windowWidth", ApplyMode::Live },
                         Row{ "console.windowHeight", ApplyMode::Live },
                         Row{ "console.maxLineChars", ApplyMode::Restart } })
    {
        INFO(r.name);
        const auto e = reg.Explain(r.name);
        REQUIRE(e);
        CHECK(e->audience == Audience::Game);
        CHECK(HasFlag(e->flags, CVarFlags::Dev));
        CHECK(e->scope == SettingScope::PreferencesProject);
        CHECK(e->apply == r.apply);
    }

    const auto fps = reg.Explain("editor.perf.backgroundFps");
    REQUIRE(fps);
    CHECK(fps->audience == Audience::Editor);
    CHECK(fps->scope == SettingScope::PreferencesMachine);
    CHECK(fps->apply == ApplyMode::Live);
    Test::RequireDefault("editor.perf.backgroundFps", CVarValue::UInt32(0u));
}

TEST_CASE("sweep: ShouldReportScanProgress is the one throttle for both hosts", "[sweep][host]")
{
    CHECK(HostBoot::ShouldReportScanProgress(1, 100));
    CHECK(HostBoot::ShouldReportScanProgress(32, 100));
    CHECK_FALSE(HostBoot::ShouldReportScanProgress(33, 100));
    CHECK(HostBoot::ShouldReportScanProgress(100, 100));
}

TEST_CASE("sweep: boot.scanProgressStride reaches the throttle live", "[sweep][host]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("boot.scanProgressStride"), CVarValue::UInt32(10u), SetBy::Code);
    reg.PublishImmediate();
    const bool at10 = HostBoot::ShouldReportScanProgress(10, 100);
    const bool at32 = HostBoot::ShouldReportScanProgress(32, 100);
    reg.RevertLayer(SetBy::Code);
    reg.PublishImmediate();

    CHECK(at10);
    CHECK_FALSE(at32);
    CHECK_FALSE(HostBoot::ShouldReportScanProgress(10, 100));
}

TEST_CASE("sweep: editor.perf.backgroundFps paces an unfocused frame; 0 never waits", "[sweep][host]")
{
    using std::chrono::milliseconds;
    CHECK(Editor::BackgroundFrameWait(0, milliseconds(0)) == milliseconds(0));
    CHECK(Editor::BackgroundFrameWait(10, milliseconds(0)) == milliseconds(100));
    CHECK(Editor::BackgroundFrameWait(10, milliseconds(40)) == milliseconds(60));
    CHECK(Editor::BackgroundFrameWait(10, milliseconds(150)) == milliseconds(0));
    CHECK(Editor::BackgroundFrameWait(240, milliseconds(0)) == milliseconds(4));
}
