// Settings metadata (settings spec 2026-10-03 s4.2): every declaration keeps
// its display strings, audience, scope, apply mode, order and widget hint;
// what is left empty is derived from the dotted name.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Config/CVarFormat.hpp>
#include <Arcane/Config/CVarRegistry.hpp>

#include <string>
#include <string_view>

using namespace Arcane;

TEST_CASE("display names and category paths derive from the dotted name", "[cvar]")
{
    CHECK(DeriveCVarDisplayName("editor.graph.fitMinZoom") == "Fit Min Zoom");
    CHECK(DeriveCVarDisplayName("editor.undo.byteBudgetMB") == "Byte Budget MB");
    CHECK(DeriveCVarDisplayName("editor.undo.spillThresholdKB") == "Spill Threshold KB");
    CHECK(DeriveCVarDisplayName("net.HTTPPort") == "HTTP Port");
    CHECK(DeriveCVarDisplayName("render.grid3D") == "Grid3D");
    CHECK(DeriveCVarDisplayName("render.d3d12Debug") == "D3d12 Debug");
    CHECK(DeriveCVarDisplayName("server.max_players") == "Max Players");
    CHECK(DeriveCVarDisplayName("cheats") == "Cheats");

    CHECK(DeriveCVarCategoryPath("physics.solver.substeps") == "Physics/Solver");
    CHECK(DeriveCVarCategoryPath("editor.inspector.assetThumbMinPx") == "Editor/Inspector");
    CHECK(DeriveCVarCategoryPath("net.rateLimit.burst") == "Net/Rate Limit");
    CHECK(DeriveCVarCategoryPath("log.level") == "Log");
    CHECK(DeriveCVarCategoryPath("cheats") == "General");
}

TEST_CASE("Register keeps the declared metadata and derives what was left empty", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle undo = reg.Register(CVarDesc{
        .name = "editor.undo.maxSteps", .type = CVarType::Int32, .defaultValue = CVarValue::Int32(100),
        .min = CVarValue::Int32(1), .max = CVarValue::Int32(10000), .flags = CVarFlags::Archive,
        .help = "Undo history depth in steps.", .module = "editor", .keywords = "history steps",
        .audience = Audience::Editor, .scope = SettingScope::PreferencesProject, .order = 3 });
    REQUIRE_FALSE(undo.IsStale());
    const auto meta = reg.Metadata(undo);
    REQUIRE(meta.has_value());
    CHECK(meta->name == "editor.undo.maxSteps");
    CHECK(meta->help == "Undo history depth in steps.");
    CHECK(meta->module == "editor");
    CHECK(meta->type == CVarType::Int32);
    CHECK(meta->flags == CVarFlags::Archive);   // Editor audience: UserSettable is derived (PlayerSafe/Server), not implied by Archive
    CHECK(meta->defaultValue == CVarValue::Int32(100));
    REQUIRE(meta->min.has_value());
    CHECK(*meta->min == CVarValue::Int32(1));
    REQUIRE(meta->max.has_value());
    CHECK(*meta->max == CVarValue::Int32(10000));
    CHECK(meta->displayName == "Max Steps");
    CHECK(meta->categoryPath == "Editor/Undo");
    CHECK(meta->keywords == "history steps");
    CHECK(meta->widget.empty());
    CHECK(meta->audience == Audience::Editor);
    CHECK(meta->scope == SettingScope::PreferencesProject);
    CHECK(meta->apply == ApplyMode::Live);
    CHECK(meta->order == 3);
    CHECK(meta->enumNames.empty());

    // Declared display strings win verbatim; the defaults are Game / Project / Live.
    const CVarHandle cull = reg.Register(CVarDesc{
        .name = "render.meshCull", .type = CVarType::Bool, .defaultValue = CVarValue::Bool(true),
        .help = "Frustum-cull mesh instances on the GPU.", .module = "engine",
        .displayName = "GPU frustum culling", .apply = ApplyMode::NextWorld, .categoryPath = "Rendering/Culling" });
    const auto cullMeta = reg.Metadata(cull);
    REQUIRE(cullMeta.has_value());
    CHECK(cullMeta->displayName == "GPU frustum culling");
    CHECK(cullMeta->categoryPath == "Rendering/Culling");
    CHECK(cullMeta->apply == ApplyMode::NextWorld);
    CHECK(cullMeta->audience == Audience::Game);
    CHECK(cullMeta->scope == SettingScope::Project);

    // The default Metadata reports is the published (clamped) one.
    const CVarHandle clamped = reg.Register(CVarDesc{
        .name = "t.clamped", .type = CVarType::Int32, .defaultValue = CVarValue::Int32(50),
        .min = CVarValue::Int32(0), .max = CVarValue::Int32(10), .help = "clamp probe", .module = "test" });
    CHECK(reg.Metadata(clamped)->defaultValue == CVarValue::Int32(10));

    CHECK_FALSE(reg.Metadata(CVarHandle{}).has_value());
    reg.UnregisterModule("editor");
    CHECK_FALSE(reg.Metadata(undo).has_value());              // a stale handle has none
}

TEST_CASE("a widget hint must fit the cvar's type", "[cvar]")
{
    CVarRegistry reg;
    const auto add = [&](const char* name, CVarType type, CVarValue def, std::string_view widget) {
        return reg.Register(CVarDesc{ .name = name, .type = type, .defaultValue = std::move(def),
                                      .help = "widget probe", .module = "test", .widget = widget });
    };
    CHECK_FALSE(add("w.keys", CVarType::String, CVarValue::String("Ctrl+Z"), "keychord").IsStale());
    CHECK_FALSE(add("w.font", CVarType::String, CVarValue::String("Inter"), "font").IsStale());
    CHECK_FALSE(add("w.file", CVarType::String, CVarValue::String(""), "path:file").IsStale());
    CHECK_FALSE(add("w.dir", CVarType::String, CVarValue::String(""), "path:dir").IsStale());
    CHECK_FALSE(add("w.tex", CVarType::String, CVarValue::String(""), "asset:texture").IsStale());
    CHECK_FALSE(add("w.slide", CVarType::Float32, CVarValue::Float32(0.5f), "slider").IsStale());
    CHECK(reg.Metadata(reg.Find("w.tex"))->widget == "asset:texture");

    CHECK(add("w.badKeys", CVarType::Int32, CVarValue::Int32(1), "keychord").IsStale());
    CHECK(reg.LastError().find("widget") != std::string::npos);
    CHECK(add("w.badAsset", CVarType::String, CVarValue::String(""), "asset:").IsStale());
    CHECK(add("w.badSlider", CVarType::String, CVarValue::String(""), "slider").IsStale());
    CHECK(add("w.unknown", CVarType::String, CVarValue::String(""), "colourwheel").IsStale());
}
