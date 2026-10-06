#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include "Helpers/TestTypeContext.hpp"
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/CVarFormat.hpp>
#include <Arcane/Project/AppSplashSettings.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Project/ProjectManifest.hpp>
#include <filesystem>
#include <fstream>

using namespace Arcane;

TEST_CASE("sweep: app.splash defaults; the one background colour is 0x0D0D0F", "[sweep][project]")
{
    const AppSplashSettings s{};
    CHECK(s.enabled); CHECK(s.image.empty()); CHECK_FALSE(s.showProgress);
    CHECK(Test::SameBits(s.minDurationSeconds, 0.0f));
    CHECK(ToSrgb8(s.backgroundColor) == 0x0D0D0Fu);   // linear store, exact sRGB8 round trip
    Test::RequireDefault("app.splash.enabled", CVarValue::Bool(true));
}

TEST_CASE("sweep: a legacy .arcproj physics/splash block migrates into Config/ and leaves the manifest", "[sweep][project]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane-sweep-migrate";
    fs::remove_all(root); fs::create_directories(root);
    std::ofstream(root / "M.arcproj") << R"({ "formatVersion": 2, "name": "M", "engine": { "abi": 1 },
        "physics": { "gravity": [0.0, -3.5] }, "splash": { "showProgress": true } })";
    REQUIRE(Project::MigrateLegacySettingsAt(root / "M.arcproj"));
    const auto manifest = nlohmann::json::parse(std::ifstream(root / "M.arcproj"));
    CHECK_FALSE(manifest.contains("physics"));
    CHECK_FALSE(manifest.contains("splash"));
    const auto physics = nlohmann::json::parse(std::ifstream(root / "Config" / "physics.json"));
    CHECK(physics["gravity"][1].get<float>() == -3.5f);
    const auto app = nlohmann::json::parse(std::ifstream(root / "Config" / "app.json"));
    CHECK(app["splash"]["showProgress"].get<bool>());
    CHECK_FALSE(Project::MigrateLegacySettingsAt(root / "M.arcproj"));   // once
    fs::remove_all(root);
}

TEST_CASE("sweep: a legacy splash backgroundColor migrates as the cvar hex; only known splash keys carry over", "[sweep][project]")
{
    // The old manifest stored [r, g, b] in sRGB-normalised floats; (0.05, 0.05,
    // 0.06) is the splash's one colour 0x0D0D0F. A raw float array in
    // Config/app.json would read back as LINEAR, so migration writes the hex.
    const auto m = ProjectManifest::FromJson(nlohmann::json::parse(R"({ "formatVersion": 2, "name": "C",
        "engine": { "abi": 1 }, "splash": { "backgroundColor": [0.05, 0.05, 0.06], "bogus": 1 } })"));
    REQUIRE(m.has_value());
    const auto& splash = m->legacySettings["app"]["splash"];
    CHECK(splash["backgroundColor"].get<std::string>() == "#0D0D0FFF");
    CHECK_FALSE(splash.contains("bogus"));
    const auto colour = CVarColorFromHex(splash["backgroundColor"].get<std::string>());
    REQUIRE(colour.has_value());
    CHECK(ToSrgb8(*colour) == ToSrgb8(AppSplashSettings{}.backgroundColor));
}

TEST_CASE("sweep: a formatVersion 1 gravity is negated on migration (the v1 rule)", "[sweep][project]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane-sweep-migrate-v1";
    fs::remove_all(root); fs::create_directories(root);
    std::ofstream(root / "V.arcproj") << R"({ "formatVersion": 1, "name": "V", "engine": { "abi": 1 }, "physics": { "gravity": [0, 9.81] } })";
    REQUIRE(Project::MigrateLegacySettingsAt(root / "V.arcproj"));
    const auto physics = nlohmann::json::parse(std::ifstream(root / "Config" / "physics.json"));
    CHECK(physics["gravity"][1].get<float>() == -9.81f);
    fs::remove_all(root);
}

TEST_CASE("sweep: an unmigrated read-only legacy block overrides an older Config value in memory", "[sweep][project][runtime]")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "arcane-sweep-readonly-legacy";
    fs::remove_all(root);
    fs::create_directories(root / "Content");
    fs::create_directories(root / "Config");
    std::ofstream(root / "R.arcproj") << R"({ "formatVersion": 2, "name": "R", "engine": { "abi": 1 },
        "physics": { "gravity": [0.0, -3.5] } })";
    std::ofstream(root / "Config" / "physics.json") << R"({ "gravity": [0.0, -1.0] })";

    // A held destination models a shipped/read-only store on Windows: the
    // migration can write its temp file but cannot atomically replace this one.
    std::ifstream held(root / "Config" / "physics.json", std::ios::binary);
    REQUIRE(held.good());

    Runtime runtime(Test::Process());
    REQUIRE(runtime.OpenProject(root));
    CHECK(runtime.ResolvedGravity().y == -3.5f);
    runtime.CloseProject();
    held.close();
    fs::remove_all(root);
}
