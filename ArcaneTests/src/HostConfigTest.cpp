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
#if !defined(ARCANE_DIST)
// --nri-graph is a retired no-op (the NRI frame graph is the only render path,
// unconditionally) and is registered unconditionally, so the every-configuration
// claim belongs to the last case in this file. What THIS one pins, still inside
// the Dist guard, is the part that was never Dist-specific: a command line
// pairing --nri-graph with the rest of the run vocabulary parses cleanly.
TEST_CASE("host config: --nri-graph still composes with the run vocabulary", "[host][nri]") {
    const auto paired = Run({"--nri-graph", "--frames", "120", "--screenshot", "nri-graph.png",
                             "--backend", "vulkan", "--no-vsync"});
    REQUIRE(paired.config.has_value());
    CHECK(paired.config->maxFrames == 120u);
    CHECK(paired.config->screenshotPath == "nri-graph.png");
    CHECK(paired.config->backend == Arcane::GraphicsBackend::Vulkan);
    CHECK_FALSE(paired.config->vsync);
}
// HostConfig::Parse is shared by both hosts, so the obligation pinned here is
// that a SCRIPTED EDITOR RUN is a legal command line: the editor's own launch
// vocabulary (--project / --plugin / --scene) is disjoint from the runtime's
// usual one, and this is what proves the two compose.
//
// As with the case above, this is the only headless coverage available: the
// editor's boot lives in EditorApp.cpp, which is not compiled into this exe,
// and everything past the flag needs a window and a real device.
TEST_CASE("host config: --nri-graph composes with the EDITOR's launch vocabulary", "[host][nri]") {
    const auto editorRun = Run({"--nri-graph", "--project", "ReferenceProject",
                                "--scene", "a5e0c1de-1111-4222-8333-444455556666",
                                "--frames", "120", "--backend", "vulkan"});
    REQUIRE(editorRun.config.has_value());
    CHECK(editorRun.config->projectPath == "ReferenceProject");
    CHECK(editorRun.config->sceneOverride == "a5e0c1de-1111-4222-8333-444455556666");
    CHECK(editorRun.config->maxFrames == 120u);
    CHECK(editorRun.config->backend == Arcane::GraphicsBackend::Vulkan);

    // The engine-dev path (host a plugin with no project) is equally legal --
    // the editor tolerates having nothing to host, and the flag changes only
    // which recorder draws.
    const auto pluginRun = Run({"--nri-graph", "--plugin", "Game.dll"});
    REQUIRE(pluginRun.config.has_value());
    CHECK(pluginRun.config->pluginPath == "Game.dll");
    CHECK(pluginRun.config->projectPath.empty());

    // And --crash-gpu, which the editor now honours on BOTH of its render arms
    // (FireDeliberateGpuFault dispatches through NriDiagnostics::FireFault on
    // the graph flavor), still pairs with it.
    const auto faultRun = Run({"--nri-graph", "--project", "ReferenceProject",
                               "--crash-gpu", "30", "--frames", "60"});
    REQUIRE(faultRun.config.has_value());
    CHECK(faultRun.config->crashGpuFrame == 30u);
}
// The pick/outline probe. Guarded like --nri-graph
// (both are DEV scaffolding registered inside HostConfig.cpp's
// `#if !defined(ARCANE_DIST)` block), and, like it, the parse round-trip is the
// ONLY headless coverage the flag can have: everything past it needs a window,
// a device and a scene.
//
// THE POINT of testing the refusals rather than just the happy path: the flag's
// whole value is being SCRIPTABLE, so a malformed coordinate that parsed to
// something plausible would report a confident id for a pixel nobody asked
// about -- worse than an error.
TEST_CASE("host config: --pick-probe round-trips x,y and defaults off", "[host][nri]") {
    const auto o = Run({"--nri-graph", "--frames", "120", "--pick-probe", "640,360"});
    REQUIRE(o.config.has_value());
    CHECK(o.config->pickProbe);
    CHECK(o.config->pickProbeX == 640);
    CHECK(o.config->pickProbeY == 360);

    // Absent flag = today's frame exactly: no pick node, no outline chain.
    const auto def = Run({"--nri-graph"});
    REQUIRE(def.config.has_value());
    CHECK_FALSE(def.config->pickProbe);
    CHECK(def.config->pickProbeX == 0);
    CHECK(def.config->pickProbeY == 0);

    // 0,0 is a legal probe (the canvas origin) and must not read as "absent".
    const auto origin = Run({"--nri-graph", "--frames", "10", "--pick-probe", "0,0"});
    REQUIRE(origin.config.has_value());
    CHECK(origin.config->pickProbe);
    CHECK(origin.config->pickProbeX == 0);
    CHECK(origin.config->pickProbeY == 0);

    // It composes with the rest of the vehicle's vocabulary, deliberately --
    // a probe run is an ordinary --nri-graph run with two extra nodes.
    const auto paired = Run({"--nri-graph", "--frames", "120", "--pick-probe", "12,34",
                             "--backend", "vulkan", "--no-vsync", "--screenshot", "probe.png"});
    REQUIRE(paired.config.has_value());
    CHECK(paired.config->pickProbe);
    CHECK(paired.config->pickProbeX == 12);
    CHECK(paired.config->pickProbeY == 34);
    CHECK(paired.config->backend == Arcane::GraphicsBackend::Vulkan);
    CHECK(paired.config->screenshotPath == "probe.png");
}
TEST_CASE("host config: --pick-probe refuses bad syntax at parse time", "[host][nri]") {
    const std::vector<std::string> bad[] = {
        {"--pick-probe", "640"},        // no comma at all
        {"--pick-probe", "640,"},       // nothing after it
        {"--pick-probe", ",360"},       // nothing before it
        {"--pick-probe", "640,360,1"},  // a third component is not a pixel
        {"--pick-probe", "a,b"},        // not numbers
        {"--pick-probe", "640, 360"},   // a space is not a digit -- refuse, do not trim
        {"--pick-probe", "-1,360"},     // negative: a typo, not a coordinate
        {"--pick-probe", "640,-1"},
        {"--pick-probe", "6.5,360"},    // fractional pixels are not a thing here
        {"--pick-probe", "99999999999,0"},   // past int32
    };
    for (const auto& args : bad) {
        std::vector<std::string> full{"--nri-graph", "--frames", "60"};
        full.insert(full.end(), args.begin(), args.end());
        const auto o = Run(full);
        REQUIRE_FALSE(o.config.has_value());
        CHECK(o.exitCode == 2);
    }
}
// --pick-probe does NOT require --nri-graph: the pick/outline nodes are always
// present, because the NRI frame graph is the only render path. It DOES still
// require --frames -- the readback lands a couple of frames after the pass that
// wrote it, so an open-ended run would exit on a window close having reported
// nothing at all.
TEST_CASE("host config: --pick-probe still requires --frames, no longer requires --nri-graph", "[host][nri]") {
    const auto noGraph = Run({"--frames", "60", "--pick-probe", "640,360"});
    REQUIRE(noGraph.config.has_value());
    CHECK(noGraph.config->pickProbe);
    CHECK(noGraph.config->pickProbeX == 640);
    CHECK(noGraph.config->pickProbeY == 360);

    const auto noFrames = Run({"--nri-graph", "--pick-probe", "640,360"});
    REQUIRE_FALSE(noFrames.config.has_value());
    CHECK(noFrames.exitCode == 2);

    // ...but neither refusal touches a run that simply did not pass the flag.
    const auto plain = Run({"--nri-graph"});
    REQUIRE(plain.config.has_value());
    CHECK_FALSE(plain.config->pickProbe);
}
#endif
// UNGUARDED, deliberately: every case above this point that touches
// --nri-graph is Dist-excluded (several pair it with --pick-probe/--crash-gpu,
// which stay Dist-only), so none of it can show that --nri-graph itself
// parses in a Dist build. This case is the one that does: the flag is
// retired to a no-op rather than rejected -- scripts, the Hub's saved launch
// args and the desk batteries all still pass it, and a hard "unknown
// argument" refusal would turn a harmless no-op into a boot failure at the
// worst moment (a shipped Dist build, launched from a saved arg list nobody
// is watching). Proving it composes with real vocabulary (not just alone) is
// why --frames rides along rather than a bare flag.
TEST_CASE("host config: --nri-graph is accepted and ignored in every configuration", "[host][nri]") {
    const auto with    = Run({"--nri-graph", "--frames", "1"});
    const auto without = Run({"--frames", "1"});
    REQUIRE(with.config.has_value());
    REQUIRE(without.config.has_value());
    CHECK(with.exitCode == without.exitCode);
}
