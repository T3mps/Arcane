#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Host/RuntimeSettings.hpp>
using namespace Arcane;
TEST_CASE("sweep: the runtime HUD is on in Debug/Release and off in Dist", "[sweep][hud]")
{
#if defined(ARC_BUILD_DIST)
    CHECK_FALSE(RuntimeHudSettings{}.show);
    Test::RequireDefault("runtime.hud.show", CVarValue::Bool(false));
#else
    CHECK(RuntimeHudSettings{}.show);
    Test::RequireDefault("runtime.hud.show", CVarValue::Bool(true));
#endif
    CHECK_FALSE(HasFlag(CVarRegistry::Get().Explain("runtime.hud.show")->flags, CVarFlags::Dev));
}
