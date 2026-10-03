// HostPresentationFor (T3-D6 fix round 1): an AUTOMATION run never requests
// the boot splash (a topmost popup that activates on show) nor the activating
// raise of Window::Show -- a scripted windowed capture runs while a person may
// be working at the desk in their own editor. Pure: no window, no device.

#include <catch2/catch_test_macros.hpp>

#include "App/HostPresentation.hpp"

#include <Arcane/Host/HostConfig.hpp>

#include <string>
#include <vector>

using Arcane::Editor::HostPresentationFor;
using Arcane::Editor::IsAutomationRun;

namespace
{
    Arcane::HostConfig Parsed(std::vector<std::string> args)
    {
        args.insert(args.begin(), "ArcaneEditor.exe");
        std::vector<char*> argv;
        for (std::string& a : args)
            argv.push_back(a.data());
        auto outcome = Arcane::HostConfig::Parse(static_cast<int>(argv.size()), argv.data());
        REQUIRE(outcome.config.has_value());
        return *outcome.config;
    }
}

TEST_CASE("HostPresentation: an interactive launch keeps the splash and the activating raise", "[editor][host]")
{
    const auto p = HostPresentationFor(Parsed({ "--project", "P" }));
    CHECK(p.bootSplash);
    CHECK(p.showWindow);
    CHECK(p.activateOnShow);
    CHECK_FALSE(IsAutomationRun(Parsed({ "--project", "P" })));
}

TEST_CASE("HostPresentation: an automation config never requests the splash or the raise", "[editor][host]")
{
    SECTION("--frames N (windowed): shown, but no splash and no activation")
    {
        const auto cfg = Parsed({ "--project", "P", "--frames", "240", "--screenshot", "out.png" });
        CHECK(IsAutomationRun(cfg));
        const auto p = HostPresentationFor(cfg);
        CHECK_FALSE(p.bootSplash);
        CHECK(p.showWindow);   // still mapped: the windowed capture is the real frame
        CHECK_FALSE(p.activateOnShow);
    }
    SECTION("the windowed self-capture cvar alone, both Bool spellings")
    {
        for (const char* on : { "editor.automation.windowedFrameCapture=true",
                                "editor.automation.windowedFrameCapture=1" })
        {
            INFO(on);
            const auto cfg = Parsed({ "--project", "P", "--set", on });
            CHECK(IsAutomationRun(cfg));
            const auto p = HostPresentationFor(cfg);
            CHECK_FALSE(p.bootSplash);
            CHECK_FALSE(p.activateOnShow);
        }
    }
    SECTION("the cvar's LAST --set wins; false / another cvar is not automation")
    {
        CHECK_FALSE(IsAutomationRun(Parsed({ "--project", "P", "--set",
                                             "editor.automation.windowedFrameCapture=false" })));
        CHECK_FALSE(IsAutomationRun(Parsed({ "--project", "P",
                                             "--set", "editor.automation.windowedFrameCapture=true",
                                             "--set", "editor.automation.windowedFrameCapture=0" })));
        CHECK_FALSE(IsAutomationRun(Parsed({ "--project", "P", "--set", "editor.graph.showPinLegend=true" })));
    }
    SECTION("--headless: no splash, no window at all")
    {
        const auto p = HostPresentationFor(Parsed({ "--project", "P", "--headless", "--frames", "60" }));
        CHECK_FALSE(p.bootSplash);
        CHECK_FALSE(p.showWindow);
        CHECK_FALSE(p.activateOnShow);
    }
}
