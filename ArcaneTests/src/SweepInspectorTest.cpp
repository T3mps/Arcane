// Settings sweep S6-37: editor.inspector.* (the four one-off inspector cvars
// fold in as InspectorSettings fields) and editor.outliner.slowClickMaxSeconds.
// Defaults are the pre-sweep literals; a published value reaches its reader.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/InspectorSettings.hpp"
#include "Panels/AssetInspectorSource.hpp"
#include "Panels/InspectorHost.hpp"

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Arcane;

TEST_CASE("sweep: inspector defaults are the pre-sweep literals; the one-offs folded in", "[sweep][inspector]")
{
    const Editor::InspectorSettings i{};
    CHECK(i.framePaddingY == 3.0f); CHECK(i.itemSpacingY == 4.0f); CHECK(i.labelColumnFraction == 0.4f);
    CHECK(i.historyDepth == 32); CHECK(i.maxInstances == 8); CHECK(i.dragSpeed == 0.1f);
    CHECK(i.materialPreviewFraction == 0.45f); CHECK(i.nodePageMinTextRun == 16);
    CHECK(i.assetThumbMinPx == 64); CHECK(i.assetThumbHeightFraction == 0.30f); CHECK(i.assetThumbMaxPx == 140.0f);
    CHECK(Editor::OutlinerSettings{}.slowClickMaxSeconds == 1.2);
    Test::RequireDefault("editor.inspector.materialPreviewFraction", CVarValue::Float32(0.45f));
    Test::RequireDefault("editor.outliner.slowClickMaxSeconds", CVarValue::Float64(1.2));
}

TEST_CASE("sweep: the remaining inspector fields register with their pre-sweep defaults", "[sweep][inspector]")
{
    const Editor::InspectorSettings i{};
    CHECK(i.labelSeedMinEm == 8.0f);
    CHECK(i.rotationDragSpeedDeg == 0.5f);
    CHECK(i.rotationDragSpeedDeg * 0.02f == 0.01f);   // the radian drag (InspectorView) keeps its pre-sweep 0.01f
    Test::RequireDefault("editor.inspector.framePaddingY", CVarValue::Float32(3.0f));
    Test::RequireDefault("editor.inspector.itemSpacingY", CVarValue::Float32(4.0f));
    Test::RequireDefault("editor.inspector.labelColumnFraction", CVarValue::Float32(0.4f));
    Test::RequireDefault("editor.inspector.labelSeedMinEm", CVarValue::Float32(8.0f));
    Test::RequireDefault("editor.inspector.assetThumbMaxPx", CVarValue::Float32(140.0f));
    Test::RequireDefault("editor.inspector.historyDepth", CVarValue::Int32(32));
    Test::RequireDefault("editor.inspector.maxInstances", CVarValue::Int32(8));
    Test::RequireDefault("editor.inspector.dragSpeed", CVarValue::Float32(0.1f));
    Test::RequireDefault("editor.inspector.rotationDragSpeedDeg", CVarValue::Float32(0.5f));
    Test::RequireDefault("editor.inspector.nodePageMinTextRun", CVarValue::Int32(16));
    Test::RequireDefault("editor.inspector.assetThumbMinPx", CVarValue::Int32(64));
    Test::RequireDefault("editor.inspector.assetThumbHeightFraction", CVarValue::Float32(0.30f));

    // The one-offs keep their names, ranges and per-machine scope; the
    // thumbnail floor's ceiling is now the (settable) thumbnail maximum's.
    CVarRegistry& reg = CVarRegistry::Get();
    const auto describe = [&](std::string_view n)
    {
        const std::optional<CVarDescInfo> d = reg.Describe(n);
        INFO("cvar " << std::string(n));
        REQUIRE(d.has_value());
        CHECK(d->audience == Audience::Editor);
        CHECK(d->scope == SettingScope::PreferencesMachine);
        return *d;
    };
    (void)describe("editor.inspector.nodePageMinTextRun");
    (void)describe("editor.inspector.assetThumbHeightFraction");
    (void)describe("editor.inspector.historyDepth");
    (void)describe("editor.outliner.slowClickMaxSeconds");
    CHECK(describe("editor.inspector.maxInstances").apply == ApplyMode::Restart);
    CHECK(describe("editor.inspector.dragSpeed").apply == ApplyMode::Live);
    const CVarDescInfo thumbMin = describe("editor.inspector.assetThumbMinPx");
    REQUIRE(thumbMin.min.has_value());
    REQUIRE(thumbMin.max.has_value());
    CHECK(*thumbMin.min == CVarValue::Int32(32));
    CHECK(*thumbMin.max == CVarValue::Int32(512));
    const CVarDescInfo preview = describe("editor.inspector.materialPreviewFraction");
    REQUIRE(preview.min.has_value());
    REQUIRE(preview.max.has_value());
    CHECK(*preview.min == CVarValue::Float32(0.2f));
    CHECK(*preview.max == CVarValue::Float32(0.8f));
}

TEST_CASE("sweep: the asset page thumbnail honours a non-default maximum and keeps the floor under it",
          "[sweep][inspector]")
{
    // Tall, wide page: the height share (0.30 x 1000 = 300) is capped by the maximum.
    CHECK(Editor::AssetPageThumbSize(false, 1000.0f, 8.0f, 1000.0f, 0.30f, 64.0f, 140.0f) == 140.0f);
    CHECK(Editor::AssetPageThumbSize(false, 1000.0f, 8.0f, 1000.0f, 0.30f, 64.0f, 256.0f) == 256.0f);
    // A floor above the maximum is clamped to it by the reader, never past it.
    CHECK(Editor::AssetPageThumbFloor(200, 140.0f) == 140.0f);
    CHECK(Editor::AssetPageThumbFloor(64, 140.0f) == 64.0f);
}

TEST_CASE("sweep: a published editor.inspector.historyDepth caps the history ring at the next push",
          "[sweep][inspector]")
{
    struct SweepPage final : Editor::InspectorPage
    {
        std::vector<Editor::InspectorCrumb> Breadcrumb() const override { return {}; }
        void Draw(Editor::PropertyGrid&) override {}
    };
    struct SweepSource final : Editor::InspectorSource
    {
        std::string key;
        SweepPage page;
        std::string SourceName() const override { return "Scene"; }
        std::string_view Kind() const override { return "scene"; }
        Editor::InspectorPage* Page() override { return &page; }
        Editor::InspectorPage* PageFor(std::string_view) override { return &page; }
        std::string SelectionKey() const override { return key; }
        bool RestoreSelection(std::string_view k) override { key = std::string(k); return true; }
        bool Resolves(std::string_view) const override { return true; }
    };

    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle depth = reg.Find("editor.inspector.historyDepth");
    const CVarHandle maxInst = reg.Find("editor.inspector.maxInstances");
    REQUIRE_FALSE(depth.IsStale());
    REQUIRE_FALSE(maxInst.IsStale());
    const SetResult setDepth = reg.Set(depth, CVarValue::Int32(5), SetBy::Code, {}, CVarContext::Editor);
    const SetResult setMax   = reg.Set(maxInst, CVarValue::Int32(3), SetBy::Code, {}, CVarContext::Editor);
    reg.PublishImmediate();

    SweepSource scene;
    std::optional<Editor::InspectorHost> host;
    host.emplace(scene);   // maxInstances is latched here (Restart)
    for (int k = 0; k < 9; ++k) { scene.key = "k" + std::to_string(k); host->NotifySelected(scene); }
    const std::size_t historyAt5 = host->History().size();
    const std::string frontAt5 = host->History().front().key;
    (void)host->AddInstance();
    (void)host->AddInstance();
    const int thirdAdd = host->AddInstance();
    const std::size_t instancesAt3 = host->Instances().size();

    reg.ClearRung(depth, SetBy::Code);
    reg.ClearRung(maxInst, SetBy::Code);
    reg.PublishImmediate();
    // A Live depth: the very next push trims to the restored default (32 > 10).
    scene.key = "k9"; host->NotifySelected(scene);
    const std::size_t historyAfterReset = host->History().size();
    // A Restart cap: the live host keeps the value it was built with.
    const int maxLatched = host->MaxInstances();

    REQUIRE(setDepth == SetResult::Applied);
    REQUIRE(setMax == SetResult::Applied);
    CHECK(historyAt5 == 5);
    CHECK(frontAt5 == "k4");
    CHECK(thirdAdd == -1);
    CHECK(instancesAt3 == 3);
    CHECK(historyAfterReset == 6);
    CHECK(maxLatched == 3);
    CHECK(Editor::InspectorHost(scene).MaxInstances() == 8);
}
