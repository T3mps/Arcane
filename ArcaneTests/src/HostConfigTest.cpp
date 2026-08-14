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
// NRI Phase 2, Task 2 -- the stage-golden flag. Registered beside the other
// golden flags (NOT Dist-guarded, unlike --nri-graph below), so this case is
// unconditional too.
TEST_CASE("host config: --golden-stage round-trips all three values", "[host][golden]") {
    // Default: Full == the pre-Phase-2 frame, and it needs no golden run to be
    // legal (it asks for nothing that does not already happen).
    const auto def = Run({});
    REQUIRE(def.config.has_value());
    CHECK(def.config->goldenStage == Arcane::GoldenStage::Full);

    const std::vector<std::string> golden{"--golden-capture", "goldens/out", "--frames", "60"};
    auto withStage = [&](const char* stage) {
        std::vector<std::string> args = golden;
        args.push_back("--golden-stage");
        args.push_back(stage);
        return Run(args);
    };

    const auto full = withStage("full");
    REQUIRE(full.config.has_value());
    CHECK(full.config->goldenStage == Arcane::GoldenStage::Full);

    const auto batch = withStage("batch");
    REQUIRE(batch.config.has_value());
    CHECK(batch.config->goldenStage == Arcane::GoldenStage::Batch);

    const auto post = withStage("post");
    REQUIRE(post.config.has_value());
    CHECK(post.config->goldenStage == Arcane::GoldenStage::Post);

    // Unknown stage: Cli::Choices refuses it at parse time, like --backend metal.
    const auto bad = withStage("imgui");
    REQUIRE_FALSE(bad.config.has_value());
    CHECK(bad.exitCode == 2);

    // A non-Full stage outside golden mode would draw the full frame anyway and
    // exit 0 -- a silent no-op, refused for the same reason golden mode without
    // --frames is.
    const auto orphan = Run({"--golden-stage", "batch"});
    REQUIRE_FALSE(orphan.config.has_value());
    CHECK(orphan.exitCode == 2);
    // ...but the default spelling of the same flag stays legal off a golden run.
    const auto orphanFull = Run({"--golden-stage", "full"});
    REQUIRE(orphanFull.config.has_value());
    CHECK(orphanFull.config->goldenStage == Arcane::GoldenStage::Full);
}
#if !defined(ARCANE_DIST)
// NRI Phase 2, Task 7 -- the graph vehicle's flag. DEV scaffolding registered
// inside HostConfig.cpp's `#if !defined(ARCANE_DIST)` block, so in a Dist
// build it is an unknown argument (exit 2) and HostConfig has no nriGraph
// member at all. This is the ONLY headless coverage the vehicle can have:
// everything past the flag needs a window and a real device (desk-only on
// this machine), so the parse round-trip is what the ~[gpu] gate can
// actually prove.
TEST_CASE("host config: --nri-graph round-trips and defaults off", "[host][nri]") {
    const auto o = Run({"--nri-graph"});
    REQUIRE(o.config.has_value());
    CHECK(o.config->nriGraph);
    // Absent flag = the normal NVRHI render half (today's behavior, unchanged).
    const auto def = Run({});
    REQUIRE(def.config.has_value());
    CHECK_FALSE(def.config->nriGraph);
    // Shares the whole run vocabulary with the normal boot -- deliberately.
    const auto paired = Run({"--nri-graph", "--frames", "120", "--screenshot", "nri-graph.png",
                             "--backend", "vulkan", "--no-vsync"});
    REQUIRE(paired.config.has_value());
    CHECK(paired.config->nriGraph);
    CHECK(paired.config->maxFrames == 120u);
    CHECK(paired.config->screenshotPath == "nri-graph.png");
    CHECK(paired.config->backend == Arcane::GraphicsBackend::Vulkan);
    CHECK_FALSE(paired.config->vsync);
}
TEST_CASE("host config: the golden flags ARE legal with --nri-graph", "[host][nri][golden]") {
    // THE POINT OF THE VEHICLE. The golden harness lives in
    // RuntimeApp::MainLoop, which --nri-graph reaches (it boots the real
    // engine and swaps only the RENDER half), so the combination is
    // meaningful and must survive parse.
    const auto capture = Run({"--nri-graph", "--golden-capture", "goldens/out", "--frames", "60"});
    REQUIRE(capture.config.has_value());
    CHECK(capture.config->nriGraph);
    CHECK(capture.config->GoldenMode());
    CHECK(capture.config->goldenCapturePath == "goldens/out");

    const auto compare = Run({"--nri-graph", "--golden-compare", "goldens/out",
                              "--golden-name", "main-dx12", "--golden-stage", "batch",
                              "--frames", "60"});
    REQUIRE(compare.config.has_value());
    CHECK(compare.config->nriGraph);
    CHECK(compare.config->goldenComparePath == "goldens/out");
    CHECK(compare.config->goldenName == "main-dx12");
    CHECK(compare.config->goldenStage == Arcane::GoldenStage::Batch);

    // The generic golden rules still apply to the graph path -- --nri-graph
    // buys no exemption from "a golden run captures on the LAST frame".
    const auto noFrames = Run({"--nri-graph", "--golden-capture", "goldens/out"});
    REQUIRE_FALSE(noFrames.config.has_value());
    CHECK(noFrames.exitCode == 2);
}
// NRI Phase 2, Task 11 -- the pick/outline probe. Guarded like --nri-graph
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
TEST_CASE("host config: --pick-probe refuses the two silent-no-op combinations", "[host][nri]") {
    // The pick/outline nodes live ONLY in NriGraphContext's frame, so a probe
    // on the NVRHI path would render nothing extra and exit 0 -- the same
    // silent-no-op class --golden-stage outside golden mode is refused for.
    const auto noGraph = Run({"--frames", "60", "--pick-probe", "640,360"});
    REQUIRE_FALSE(noGraph.config.has_value());
    CHECK(noGraph.exitCode == 2);

    // And the readback lands a couple of frames after the pass that wrote it,
    // so an open-ended run would exit on a window close having reported
    // nothing at all.
    const auto noFrames = Run({"--nri-graph", "--pick-probe", "640,360"});
    REQUIRE_FALSE(noFrames.config.has_value());
    CHECK(noFrames.exitCode == 2);

    // ...but neither refusal touches a run that simply did not pass the flag.
    const auto plain = Run({"--nri-graph"});
    REQUIRE(plain.config.has_value());
    CHECK_FALSE(plain.config->pickProbe);
}
#endif
