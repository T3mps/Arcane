#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Render/RenderShaderSettings.hpp>
using namespace Arcane;
TEST_CASE("sweep: shader and GPU-diagnostics defaults are the pre-sweep literals", "[sweep][shader]")
{
    CHECK(Test::SameBits(RenderShaderSettings{}.compileDebounceSeconds, 0.2));
    CHECK_FALSE(RenderShaderSettings{}.debugInfo);
    CHECK(RenderShaderSettings{}.optimization == ShaderOptimization::Default);
    CHECK(DiagnosticsGpuSettings{}.breadcrumbSlots == 256u);
    CHECK(DiagnosticsGpuFaultSettings{}.removalBudgetSeconds == 45u);
    CHECK(DiagnosticsGpuFaultSettings{}.removalPollMs == 50u);
    Test::RequireDefault("render.shader.compileDebounceSeconds", CVarValue::Float64(0.2));
    CHECK(DxcArgumentsFor(RenderShaderSettings{}).empty());
}

TEST_CASE("sweep: shader DXC overrides add only requested switches", "[sweep][shader]")
{
    RenderShaderSettings settings;
    settings.debugInfo = true;
    settings.optimization = ShaderOptimization::O1;
    CHECK(DxcArgumentsFor(settings) == std::vector<std::wstring>{ L"-Zi", L"-Qembed_debug", L"-O1" });
}
