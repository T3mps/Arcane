// Settings arc S6-36: editor.assetGraph.* -- the Graph lens's depth/breadth
// caps (per-project), the focus popup's hit cap, the layout pitch, the wire
// and ghost dims and the dashed in-flight wire's LOD floor (per-machine).

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/AssetGraphSettings.hpp"
#include "Panels/AssetGraphViewModel.hpp"

#include <Arcane/Config/CVarRegistry.hpp>

#include <optional>
#include <string>
#include <string_view>

using namespace Arcane;

TEST_CASE("sweep: asset graph defaults are the pre-sweep literals", "[sweep][asset-graph]")
{
    const Editor::AssetGraphSettings a{};
    CHECK(a.defaultDepth == 2); CHECK(a.breadthCap == 20); CHECK(a.focusHitCap == 12);
    CHECK(a.layoutColumnPitch == 300.0f); CHECK(a.layoutRowPitch == 90.0f);
    CHECK(a.wireDim == 0.62f); CHECK(a.overflowDim == 0.78f); CHECK(a.ghostWash == 0.55f); CHECK(a.dashMaxCells == 256);
    CHECK(Editor::MakeAssetGraphQuery().depthLimit == 2);
    Test::RequireDefault("editor.assetGraph.breadthCap", CVarValue::Int32(20));
}

TEST_CASE("sweep: asset graph caps are per-project, the rest per-machine", "[sweep][asset-graph]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const auto scopeOf = [&](std::string_view n)
    {
        const std::optional<CVarDescInfo> d = reg.Describe(n);
        INFO("cvar " << std::string(n));
        REQUIRE(d.has_value());
        return d->scope;
    };
    CHECK(scopeOf("editor.assetGraph.defaultDepth") == SettingScope::PreferencesProject);
    CHECK(scopeOf("editor.assetGraph.breadthCap")   == SettingScope::PreferencesProject);
    CHECK(scopeOf("editor.assetGraph.focusHitCap")  == SettingScope::PreferencesMachine);
    CHECK(scopeOf("editor.assetGraph.wireDim")      == SettingScope::PreferencesMachine);
    Test::RequireDefault("editor.assetGraph.defaultDepth", CVarValue::Int32(2));
    Test::RequireDefault("editor.assetGraph.layoutColumnPitch", CVarValue::Float32(300.0f));
    Test::RequireDefault("editor.assetGraph.ghostWash", CVarValue::Float32(0.55f));
    Test::RequireDefault("editor.assetGraph.dashMaxCells", CVarValue::Int32(256));
}

TEST_CASE("sweep: a published depth/breadth reaches the Graph lens's query", "[sweep][asset-graph]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle depth   = reg.Find("editor.assetGraph.defaultDepth");
    const CVarHandle breadth = reg.Find("editor.assetGraph.breadthCap");
    REQUIRE_FALSE(depth.IsStale());
    REQUIRE_FALSE(breadth.IsStale());
    REQUIRE(reg.Set(depth,   CVarValue::Int32(4),  SetBy::Code, {}, CVarContext::Editor) == SetResult::Applied);
    REQUIRE(reg.Set(breadth, CVarValue::Int32(7),  SetBy::Code, {}, CVarContext::Editor) == SetResult::Applied);
    reg.PublishImmediate();
    const Editor::GraphBuildInput q = Editor::MakeAssetGraphQuery();
    reg.ClearRung(depth, SetBy::Code);
    reg.ClearRung(breadth, SetBy::Code);
    reg.PublishImmediate();
    CHECK(q.depthLimit == 4);
    CHECK(q.breadthCap == 7);
    CHECK(Editor::MakeAssetGraphQuery().depthLimit == 2);
    CHECK(Editor::MakeAssetGraphQuery().breadthCap == 20);
}

// S6-44: the Graph lens's canvas-space node geometry (the S5-2 review restored
// these from DERIVED; they scale with the graph zoom, not with editor.ui.scale).
TEST_CASE("sweep: asset graph node geometry defaults are the pre-sweep literals", "[sweep][asset-graph]")
{
    const Editor::AssetGraphNodeSettings n{};
    CHECK(n.minWidth == 180.0f); CHECK(n.maxWidth == 220.0f);
    CHECK(n.headerHeight == 24.0f); CHECK(n.accentBarWidth == 3.0f);
    CHECK(n.padding.x == 8.0f); CHECK(n.padding.y == 8.0f); CHECK(n.padding.z == 6.0f); CHECK(n.padding.w == 6.0f);
    const Editor::AssetGraphSettings a{};
    CHECK(a.pinRadius == 4.5f); CHECK(a.overflowWireThickness == 1.5f); CHECK(a.labelPad == 3.0f);

    Test::RequireDefault("editor.assetGraph.node.minWidth",       CVarValue::Float32(180.0f));
    Test::RequireDefault("editor.assetGraph.node.maxWidth",       CVarValue::Float32(220.0f));
    Test::RequireDefault("editor.assetGraph.node.headerHeight",   CVarValue::Float32(24.0f));
    Test::RequireDefault("editor.assetGraph.node.accentBarWidth", CVarValue::Float32(3.0f));
    Test::RequireDefault("editor.assetGraph.node.padding",        CVarValue::Vec4(CVarVec4{ 8.0f, 8.0f, 6.0f, 6.0f }));
    Test::RequireDefault("editor.assetGraph.pinRadius",             CVarValue::Float32(4.5f));
    Test::RequireDefault("editor.assetGraph.overflowWireThickness", CVarValue::Float32(1.5f));
    Test::RequireDefault("editor.assetGraph.labelPad",              CVarValue::Float32(3.0f));

    CVarRegistry& reg = CVarRegistry::Get();
    for (std::string_view name : { "editor.assetGraph.node.minWidth", "editor.assetGraph.node.padding",
                                   "editor.assetGraph.pinRadius", "editor.assetGraph.labelPad" })
    {
        if (!Test::InThisBuild(name)) continue;   // Dev: compiled out of Dist
        const std::optional<CVarDescInfo> d = reg.Describe(name);
        INFO("cvar " << std::string(name));
        REQUIRE(d.has_value());
        CHECK(d->audience == Audience::Editor);
        CHECK(d->scope == SettingScope::PreferencesMachine);
        CHECK(d->apply == ApplyMode::Live);
        CHECK(HasFlag(d->flags, CVarFlags::Dev));
        CHECK_FALSE(d->help.empty());
    }
}
