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

TEST_CASE("sweep: shader and GPU-fault metadata matches the frozen inventory", "[sweep][shader]")
{
    const SettingsTypeDesc shader = DescribeSettings<RenderShaderSettings>();
    REQUIRE(shader.error.empty());
    REQUIRE_FALSE(shader.fields.empty());
    const SettingsFieldDesc& debounce = shader.fields.front();
    CHECK(debounce.name == "render.shader.compileDebounceSeconds");
    REQUIRE(debounce.min.has_value());
    REQUIRE(debounce.max.has_value());
    CHECK(*debounce.min == CVarValue::Float64(0.0));
    CHECK(*debounce.max == CVarValue::Float64(2.0));

    const SettingsTypeDesc fault = DescribeSettings<DiagnosticsGpuFaultSettings>();
    REQUIRE(fault.error.empty());
    REQUIRE(fault.fields.size() == 2);
    const SettingsFieldDesc& budget = fault.fields[0];
    const SettingsFieldDesc& poll = fault.fields[1];
    CHECK(budget.name == "diagnostics.gpuFault.removalBudgetSeconds");
    CHECK(budget.scope == SettingScope::PreferencesProject);
    CHECK(budget.apply == ApplyMode::Live);
    REQUIRE(budget.min.has_value());
    REQUIRE(budget.max.has_value());
    CHECK(*budget.min == CVarValue::UInt32(5));
    CHECK(*budget.max == CVarValue::UInt32(300));
    CHECK(poll.name == "diagnostics.gpuFault.removalPollMs");
    CHECK(poll.scope == SettingScope::PreferencesProject);
    CHECK(poll.apply == ApplyMode::Live);
    REQUIRE(poll.min.has_value());
    REQUIRE(poll.max.has_value());
    CHECK(*poll.min == CVarValue::UInt32(1));
    CHECK(*poll.max == CVarValue::UInt32(1000));
}
