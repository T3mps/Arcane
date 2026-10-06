#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Plugin/PluginSettings.hpp>
using namespace Arcane;
TEST_CASE("sweep: hot-reload defaults are the pre-sweep literals", "[sweep][plugin]")
{
    CHECK(PluginHotReloadSettings{}.settleMs == 250);
    CHECK(PluginHotReloadSettings{}.copyRetries == 5);
    CHECK(PluginHotReloadSettings{}.copyRetryMs == 50);
    Test::RequireDefault("plugin.hotReload.settleMs", CVarValue::Int32(250));
}
