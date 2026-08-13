// HostConfig::Parse round-trip. PRESENTATION-FREE. HostConfig now lives in the
// engine DLL (Arcane/Host/HostConfig.hpp, ARCANE_API) so this test exe links
// it via the "Arcane" link, not a source-compile.
#include <vector>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/HostConfig.hpp>
namespace {
    Arcane::HostConfig::ParseOutcome Run(std::vector<std::string> args) {
        std::vector<char*> argv; argv.push_back(const_cast<char*>("ArcaneRuntime"));
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        return Arcane::HostConfig::Parse(static_cast<int>(argv.size()), argv.data());
    }
}
TEST_CASE("HostConfig: defaults", "[host]") {
    const auto o = Run({});
    REQUIRE(o.config.has_value());
    REQUIRE(o.config->backend == Arcane::GraphicsBackend::D3D12);
    REQUIRE(o.config->maxFrames == 0u);
    REQUIRE(o.config->vsync);
    REQUIRE_FALSE(o.config->perf);
    // --plugin defaults to EMPTY (was "Sandbox.dll"): each host supplies its own
    // fallback -- ArcaneRuntime -> Sandbox.dll, the editor -> no game loaded.
    REQUIRE(o.config->pluginPath.empty());
}
TEST_CASE("HostConfig: every flag maps", "[host]") {
    const auto o = Run({"--backend", "vulkan", "--frames", "180", "--no-vsync", "--perf", "--plugin", "Game.dll"});
    REQUIRE(o.config.has_value());
    REQUIRE(o.config->backend == Arcane::GraphicsBackend::Vulkan);
    REQUIRE(o.config->maxFrames == 180u);
    REQUIRE_FALSE(o.config->vsync);
    REQUIRE(o.config->perf);
    REQUIRE(o.config->pluginPath == "Game.dll");
}
TEST_CASE("HostConfig: --help exits 0, no config", "[host]") {
    const auto o = Run({"--help"});
    REQUIRE_FALSE(o.config.has_value()); REQUIRE(o.exitCode == 0);
}
TEST_CASE("HostConfig: bad arg exits 2, no config", "[host]") {
    const auto o = Run({"--backend", "metal"});
    REQUIRE_FALSE(o.config.has_value()); REQUIRE(o.exitCode == 2);
}
TEST_CASE("HostConfig parses --scene as a guid override", "[host]") {
    const auto o = Run({"--scene", "a5e0c1de-1111-4222-8333-444455556666"});
    REQUIRE(o.config.has_value());
    CHECK(o.config->sceneOverride == "a5e0c1de-1111-4222-8333-444455556666");
    // Absent flag = empty = manifest bootScene wins (today's behavior).
    const auto def = Run({});
    REQUIRE(def.config.has_value());
    CHECK(def.config->sceneOverride.empty());
}
TEST_CASE("host config: golden flags round-trip and imply golden mode", "[host][golden]") {
    const auto o = Run({"--golden-capture", "goldens/out", "--golden-name", "main-dx12", "--frames", "60"});
    REQUIRE(o.config.has_value());
    CHECK(o.config->goldenCapturePath == "goldens/out");
    CHECK(o.config->goldenComparePath.empty());
    CHECK(o.config->goldenName == "main-dx12");
    CHECK(o.config->GoldenMode());
}
TEST_CASE("host config: golden-compare round-trips symmetrically with golden-capture", "[host][golden]") {
    const auto o = Run({"--golden-compare", "goldens/out", "--golden-name", "main-dx12", "--frames", "60"});
    REQUIRE(o.config.has_value());
    CHECK(o.config->goldenComparePath == "goldens/out");
    CHECK(o.config->goldenCapturePath.empty());
    CHECK(o.config->goldenName == "main-dx12");
    CHECK(o.config->GoldenMode());
}
TEST_CASE("host config: default is not golden mode; default name is empty", "[host][golden]") {
    const auto o = Run({});
    REQUIRE(o.config.has_value());
    CHECK_FALSE(o.config->GoldenMode());
    CHECK(o.config->goldenName.empty());   // resolved at use site: "main-<backend>"
}
TEST_CASE("host config: golden mode without --frames is refused at parse time", "[host][golden]") {
    const auto o = Run({"--golden-capture", "goldens/out"});
    REQUIRE_FALSE(o.config.has_value());
    CHECK(o.exitCode == 2);
}
