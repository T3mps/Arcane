// LoomConfig::Parse round-trip. PRESENTATION-FREE. Compiled WITH Loom/src/LoomConfig.cpp
// (see premake ArcaneTests files{}) so it links without the Loom exe.
#include <vector>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include <LoomConfig.hpp>
namespace {
    LoomConfig::ParseOutcome Run(std::vector<std::string> args) {
        std::vector<char*> argv; argv.push_back(const_cast<char*>("Loom"));
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        return LoomConfig::Parse(static_cast<int>(argv.size()), argv.data());
    }
}
TEST_CASE("LoomConfig: defaults", "[loom]") {
    const auto o = Run({});
    REQUIRE(o.config.has_value());
    REQUIRE(o.config->backend == Arcane::GraphicsBackend::D3D12);
    REQUIRE(o.config->maxFrames == 0u);
    REQUIRE(o.config->vsync);
    REQUIRE_FALSE(o.config->perf);
    REQUIRE(o.config->pluginPath == "Sandbox.dll");
}
TEST_CASE("LoomConfig: every flag maps", "[loom]") {
    const auto o = Run({"--backend", "vulkan", "--frames", "180", "--no-vsync", "--perf", "--plugin", "Game.dll"});
    REQUIRE(o.config.has_value());
    REQUIRE(o.config->backend == Arcane::GraphicsBackend::Vulkan);
    REQUIRE(o.config->maxFrames == 180u);
    REQUIRE_FALSE(o.config->vsync);
    REQUIRE(o.config->perf);
    REQUIRE(o.config->pluginPath == "Game.dll");
}
TEST_CASE("LoomConfig: --help exits 0, no config", "[loom]") {
    const auto o = Run({"--help"});
    REQUIRE_FALSE(o.config.has_value()); REQUIRE(o.exitCode == 0);
}
TEST_CASE("LoomConfig: bad arg exits 2, no config", "[loom]") {
    const auto o = Run({"--backend", "metal"});
    REQUIRE_FALSE(o.config.has_value()); REQUIRE(o.exitCode == 2);
}
