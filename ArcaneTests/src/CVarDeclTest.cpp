// ARC_CVAR (settings spec 2026-10-03 s4.3): the declaration macro defines a
// nameable CVarRef; every pre-S1 declaration migrated to it keeps its type,
// default, range and flags byte-identical and gains its metadata.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Config/CVarDecl.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>
#include <Arcane/Render/Nri/nodes/MeshCullNode.hpp>
#include <Arcane/Base/Log.hpp>

#include <optional>

namespace Arcane::Test
{
    // CVarDeclExternTest.cpp names this through ARC_CVAR_EXTERN.
    ARC_CVAR(cvar_declProbe, "tests.decl.probe", std::int32_t, 11,
             .min = 0, .max = 20, .flags = ::Arcane::CVarFlags::Archive,
             .audience = ::Arcane::Audience::Editor, .scope = ::Arcane::SettingScope::PreferencesProject,
             .help = "ARC_CVAR probe (CVarDeclTest).");
}

namespace
{
    struct Migrated
    {
        const char* name;
        Arcane::CVarValue def;
        std::optional<Arcane::CVarValue> min, max;
        Arcane::Audience audience;
        Arcane::SettingScope scope;
        const char* module;
    };
}

TEST_CASE("ARC_CVAR defines a nameable handle with its range, metadata and module", "[cvar]")
{
    using namespace Arcane;
    CHECK(Test::cvar_declProbe.Get() == 11);
    CHECK(Test::cvar_declProbe.Name() == "tests.decl.probe");
    const auto meta = CVarRegistry::Get().Metadata(Test::cvar_declProbe.Handle());
    REQUIRE(meta.has_value());
    CHECK(meta->module == "tests");
    REQUIRE(meta->max.has_value());
    CHECK(*meta->max == CVarValue::Int32(20));
    CHECK(meta->audience == Audience::Editor);
}

TEST_CASE("every migrated Archive declaration keeps type, default, range and flags, and gains its metadata", "[cvar]")
{
    using namespace Arcane;
    const CVarFlags archive = CVarFlags::Archive | CVarFlags::UserSettable;   // v1: Archive implies UserSettable
    const Audience ed = Audience::Editor;
    const SettingScope prefM = SettingScope::PreferencesMachine;
    const SettingScope prefP = SettingScope::PreferencesProject;
    // The editor sources compile into this exe, so their module is "tests" here ("editor" in ArcaneEditor).
    const Migrated rows[] = {
        { "editor.graph.fitMaxZoom", CVarValue::Float32(1.0f), CVarValue::Float32(0.1f), CVarValue::Float32(2.0f), ed, prefM, "tests" },
        { "editor.graph.fitMinZoom", CVarValue::Float32(0.5f), CVarValue::Float32(0.1f), CVarValue::Float32(2.0f), ed, prefM, "tests" },
        { "editor.graph.showPinLegend", CVarValue::Bool(true), std::nullopt, std::nullopt, ed, prefM, "tests" },
        { "editor.inspector.materialPreviewFraction", CVarValue::Float32(0.45f), CVarValue::Float32(0.2f), CVarValue::Float32(0.8f), ed, prefM, "tests" },
        { "editor.inspector.nodePageMinTextRun", CVarValue::Int32(16), CVarValue::Int32(0), CVarValue::Int32(256), ed, prefM, "tests" },
        { "editor.inspector.assetThumbMinPx", CVarValue::Int32(64), CVarValue::Int32(32), CVarValue::Int32(140), ed, prefM, "tests" },
        { "editor.inspector.assetThumbHeightFraction", CVarValue::Float32(0.30f), CVarValue::Float32(0.1f), CVarValue::Float32(0.6f), ed, prefM, "tests" },
        { "editor.undo.maxSteps", CVarValue::Int32(100), CVarValue::Int32(1), CVarValue::Int32(10000), ed, prefP, "tests" },
        { "editor.undo.byteBudgetMB", CVarValue::Int32(512), CVarValue::Int32(16), CVarValue::Int32(65536), ed, prefP, "tests" },
        { "editor.undo.spillThresholdKB", CVarValue::Int32(256), CVarValue::Int32(16), CVarValue::Int32(1048576), ed, prefP, "tests" },
        { "console.historySize", CVarValue::Int32(64), CVarValue::Int32(1), CVarValue::Int32(1024), Audience::Game, prefP, "engine" },
    };
    CVarRegistry& reg = CVarRegistry::Get();
    for (const Migrated& row : rows)
    {
        INFO(row.name);
        const auto meta = reg.Metadata(reg.Find(row.name));
        REQUIRE(meta.has_value());
        CHECK(meta->defaultValue == row.def);
        CHECK(meta->min.has_value() == row.min.has_value());
        if (row.min && meta->min) CHECK(*meta->min == *row.min);
        CHECK(meta->max.has_value() == row.max.has_value());
        if (row.max && meta->max) CHECK(*meta->max == *row.max);
        CHECK(meta->flags == archive);
        CHECK(meta->audience == row.audience);
        CHECK(meta->scope == row.scope);
        CHECK(meta->apply == ApplyMode::Live);
        CHECK(meta->module == row.module);
        CHECK_FALSE(meta->help.empty());
    }
}

TEST_CASE("the Dev declarations keep their defaults and drive their CVarRef consumers", "[cvar]")
{
    using namespace Arcane;
    Log::Init();
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle markers = reg.Find("diagnostics.drawMarkers");
#if defined(ARC_BUILD_DIST)
    CHECK(markers.IsStale());                      // Dev: compiled out
    CHECK_FALSE(GpuDrawMarkersEnabled());          // the declared default
    CHECK(MeshCullFrustumEnabled());
#else
    REQUIRE_FALSE(markers.IsStale());
    const auto meta = reg.Metadata(markers);
    REQUIRE(meta.has_value());
    CHECK(meta->defaultValue == CVarValue::Bool(false));
    CHECK(meta->flags == CVarFlags::Dev);
    CHECK(meta->module == "engine");
    CHECK(meta->scope == SettingScope::Project);
    CHECK_FALSE(GpuDrawMarkersEnabled());
    REQUIRE(reg.Set(markers, CVarValue::Bool(true), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(GpuDrawMarkersEnabled());                // O6's missing GpuDrawMarkersEnabled test
    REQUIRE(reg.Set(markers, CVarValue::Bool(false), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK_FALSE(GpuDrawMarkersEnabled());

    const auto cull = reg.Metadata(reg.Find("render.meshCull"));
    REQUIRE(cull.has_value());
    CHECK(cull->defaultValue == CVarValue::Bool(kMeshCullEnabled));
    CHECK(cull->flags == CVarFlags::Dev);
    CHECK(cull->audience == Audience::Game);
    CHECK(cull->scope == SettingScope::Project);

    const auto level = reg.Metadata(reg.Find("log.level"));
    REQUIRE(level.has_value());
    CHECK(level->audience == Audience::Game);
    CHECK(level->scope == SettingScope::PreferencesProject);
    CHECK(level->flags == (CVarFlags::Archive | CVarFlags::Dev | CVarFlags::UserSettable));
#endif
}
