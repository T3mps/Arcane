// Settings arc S6-17: render.framesInFlight -- a Restart setting under the
// kMaxFramesInFlight storage ceiling. The default depth (2) is the value the
// fixed constant it replaces had, and the depth is latched once per process.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Render/FramePacing.hpp>
#include <Arcane/Render/RenderDeviceSettings.hpp>

using namespace Arcane;

TEST_CASE("sweep: frames in flight defaults to 2 under a ceiling of 3", "[sweep][frames-in-flight]")
{
    CHECK(RenderSettings{}.framesInFlight == 2u);
    STATIC_REQUIRE(kMaxFramesInFlight == 3u);
    Test::RequireDefault("render.framesInFlight", CVarValue::UInt32(2u));
    LatchFramesInFlight();
    CHECK(FramesInFlight() == 2u);
    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("render.framesInFlight"), CVarValue::UInt32(3u), SetBy::Code);
    reg.PublishImmediate();
    LatchFramesInFlight();
    CHECK(FramesInFlight() == 2u);   // Restart: latched once per process
    reg.RevertLayer(SetBy::Code); reg.PublishImmediate();
}
