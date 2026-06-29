// PluginHost: versioned copy-and-load, ABI check, last-good rollback, hot swap.
// Headless -- no window/device. Uses the HotReloadPlugin V1/V2/Bad DLLs copied
// next to ArcaneTests.exe (relative paths; run the exe FROM its output dir).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Render/Device.hpp>   // RenderErrorCount()

#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"   // the SAME Pulse type the plugin uses
#include <PluginHost.hpp>

#include <Astra/Registry/Registry.hpp>

#include <filesystem>

using Arcane::HotReloadTest::Pulse;

namespace
{
    int ReadPulse(Arcane::Runtime& rt)
    {
        int v = 0;
        rt.Registry().CreateView<Pulse>().ForEach([&](Astra::Entity, Pulse& p) { v = p.ticks; });
        return v;
    }
    void StepK(Arcane::Runtime& rt, const Arcane::PluginVTable& vt, int k)
    {
        for (int i = 0; i < k; ++i)
            rt.Loop().Advance(1.0 / 60.0, [&](double dt){ vt.FixedUpdate(dt); }, [&](double,double){});
    }
}

TEST_CASE("PluginHost loads a plugin and runs it across the ABI", "[hotreload]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();   // engine sees the type so views resolve

    PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
    REQUIRE(host.Load());
    REQUIRE(host.IsLoaded());

    StepK(rt, *host.Vtable(), 5);
    CHECK(ReadPulse(rt) == 5);                     // V1 increments by 1
    CHECK(Arcane::RenderErrorCount() == 0);
    host.Unload();
}

TEST_CASE("Hot swap V1->V2 preserves state AND runs the new code", "[hotreload]")
{
    // Guard against ordering contamination: ensure we start with genuine V1 content.
    std::filesystem::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();

    PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
    REQUIRE(host.Load());
    StepK(rt, *host.Vtable(), 5);
    REQUIRE(ReadPulse(rt) == 5);

    // Swap the binary under the watched path, then force a state-preserving reload.
    std::filesystem::copy_file("HotReloadPluginV2.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);
    REQUIRE(host.ForceReload());                   // SaveState -> unload -> load V2 -> Init -> LoadState

    CHECK(ReadPulse(rt) == 5);                     // STATE SURVIVED the swap
    StepK(rt, *host.Vtable(), 1);
    CHECK(ReadPulse(rt) == 15);                    // NEW CODE LIVE: V2 step is +10
    CHECK(Arcane::RenderErrorCount() == 0);
    host.Unload();

    // restore the fixture for re-runs
    std::filesystem::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);
}

TEST_CASE("ABI mismatch rolls back to last-good; session survives", "[hotreload]")
{
    // Guard against ordering contamination: ensure we start with genuine V1 content.
    std::filesystem::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();

    PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
    REQUIRE(host.Load());
    StepK(rt, *host.Vtable(), 3);
    REQUIRE(ReadPulse(rt) == 3);

    std::filesystem::copy_file("HotReloadPluginBad.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);
    CHECK_FALSE(host.ForceReload());               // ABI mismatch -> reload fails
    CHECK(host.IsLoaded());                        // still on last-good
    StepK(rt, *host.Vtable(), 1);
    CHECK(ReadPulse(rt) == 4);                      // last-good V1 still running, state intact
    host.Unload();

    // restore the fixture for re-runs
    std::filesystem::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);
}
