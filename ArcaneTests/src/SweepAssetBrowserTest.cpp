// Settings arc S6-38: editor.assets.* -- the Asset Browser's rail width, the
// peek tooltip's named targets, the activity log capacity (Restart), the new
// material's default surface (per-project), the asset-watch and content-
// discovery polls and the diag:// mount -- and editor.assetStatus.*.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Settings/AssetBrowserSettings.hpp"
#include "Panels/AssetActivityLog.hpp"

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Project/ProjectOpenOptions.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

using namespace Arcane;

TEST_CASE("sweep: asset browser defaults are the pre-sweep literals", "[sweep][asset-browser]")
{
    const Editor::AssetBrowserSettings a{};
    CHECK(a.railWidth == 180.0f); CHECK(a.namedTargets == 3); CHECK(a.activityLogCapacity == 100);
    CHECK(a.newMaterialDefaultSurface == 2); CHECK(a.watchPollSeconds == 1.0); CHECK(a.discoveryPollSeconds == 2.0);
    CHECK(a.mountDiagnostics);
    CHECK(Editor::AssetStatusSettings{}.rightColumnMaxFraction == 0.45f);
    Test::RequireDefault("editor.assets.watchPollSeconds", CVarValue::Float64(1.0));
}

TEST_CASE("sweep: asset browser settings carry the inventory's scope, apply and flags", "[sweep][asset-browser]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const auto describe = [&](std::string_view n)
    {
        const std::optional<CVarDescInfo> d = reg.Describe(n);
        INFO("cvar " << std::string(n));
        REQUIRE(d.has_value());
        CHECK(d->audience == Audience::Editor);
        return *d;
    };
    for (const std::string_view n : { "editor.assets.railWidth", "editor.assets.namedTargets",
                                      "editor.assets.watchPollSeconds", "editor.assets.discoveryPollSeconds",
                                      "editor.assetStatus.rightColumnMaxFraction" })
    {
        const CVarDescInfo d = describe(n);
        CHECK(d.scope == SettingScope::PreferencesMachine);
        CHECK(d.apply == ApplyMode::Live);
    }
    CHECK(describe("editor.assets.newMaterialDefaultSurface").scope == SettingScope::PreferencesProject);
    CHECK(describe("editor.assets.activityLogCapacity").apply == ApplyMode::Restart);
    const CVarDescInfo mount = describe("editor.assets.mountDiagnostics");
    CHECK(mount.scope == SettingScope::PreferencesProject);   // inventory: Part 1 "Preferences" reads as Pref-P
    CHECK(mount.apply == ApplyMode::NextWorld);
    CHECK(HasFlag(mount.flags, CVarFlags::Dev));
    Test::RequireDefault("editor.assets.mountDiagnostics", CVarValue::Bool(true));
    Test::RequireDefault("editor.assets.activityLogCapacity", CVarValue::Int32(100));
    Test::RequireDefault("editor.assets.newMaterialDefaultSurface", CVarValue::Int32(2));
}

TEST_CASE("sweep: the activity log wraps at the capacity it was built with", "[sweep][asset-browser]")
{
    Editor::AssetActivityLog log(5);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 8; ++i)
    {
        Editor::AssetActivityEntry e;
        e.when = t0 + std::chrono::seconds(i);
        e.name = "n" + std::to_string(i);
        log.Push(e);
    }
    CHECK(log.Capacity() == 5);
    CHECK(log.Size() == 5);
    std::string newest;
    log.ForEachNewestFirst([&](const Editor::AssetActivityEntry& e) { if (newest.empty()) newest = e.name; });
    CHECK(newest == "n7");
    // A default-constructed log latches editor.assets.activityLogCapacity (Restart).
    CHECK(Editor::AssetActivityLog{}.Capacity() == 100);
}

TEST_CASE("sweep: editor.assets.mountDiagnostics only ever declines the host's diag:// mount",
          "[sweep][asset-browser]")
{
    ProjectOpenOptions hostOn;  hostOn.mountDiagnostics = true;
    ProjectOpenOptions hostOff; hostOff.mountDiagnostics = false;
    CHECK(Editor::EditorOpenOptions(hostOn).mountDiagnostics);    // default: unchanged
    CHECK_FALSE(Editor::EditorOpenOptions(hostOff).mountDiagnostics);

    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("editor.assets.mountDiagnostics");
    REQUIRE_FALSE(h.IsStale());
    const SetResult set = reg.Set(h, CVarValue::Bool(false), SetBy::Code, {}, CVarContext::Editor);
    reg.PublishImmediate();
    const bool declined = !Editor::EditorOpenOptions(hostOn).mountDiagnostics;
    reg.ClearRung(h, SetBy::Code);
    reg.PublishImmediate();

    REQUIRE(set == SetResult::Applied);
    CHECK(declined);
    CHECK(Editor::EditorOpenOptions(hostOn).mountDiagnostics);
}
