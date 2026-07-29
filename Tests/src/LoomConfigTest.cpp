// HostConfig::Parse round-trip. PRESENTATION-FREE. HostConfig now lives in the
// engine DLL (Arcane/Host/HostConfig.hpp, ARCANE_API) so this test exe links
// it via the "Arcane" link, not a source-compile.
#include <vector>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/HostConfig.hpp>
namespace {
    Arcane::HostConfig::ParseOutcome Run(std::vector<std::string> args) {
        std::vector<char*> argv; argv.push_back(const_cast<char*>("Loom"));
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
    // fallback -- Loom -> Sandbox.dll, the editor -> no game loaded.
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
