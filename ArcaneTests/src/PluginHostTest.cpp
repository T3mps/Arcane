// PluginHost: versioned copy-and-load, ABI check, last-good rollback, hot swap.
// Headless -- no window/device. Uses the HotReloadPlugin V1/V2/Bad DLLs copied
// next to ArcaneTests.exe (relative paths; run the exe FROM its output dir).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>   // RenderErrorCount()
#include <Arcane/Scene/Components.hpp>   // Arcane::Transform (the engine-roster survivor check)

#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"   // the SAME Pulse type the plugin uses
#include <Arcane/Plugin/PluginHost.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>

#include <cstddef>
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
    // Drive k fixed steps through the WHOLE host (primary + every secondary), the way a
    // multi-module host advances the sim -- FixedUpdateAll instead of a single vtable.
    void StepAllK(Arcane::Runtime& rt, Arcane::PluginHost& host, int k)
    {
        for (int i = 0; i < k; ++i)
            rt.Loop().Advance(1.0 / 60.0, [&](double dt){ host.FixedUpdateAll(dt); }, [&](double,double){});
    }
}

TEST_CASE("PluginHost loads a plugin and runs it across the ABI", "[hotreload]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();   // engine sees the type so views resolve

    Arcane::PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
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

    Arcane::PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
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

    Arcane::PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
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

TEST_CASE("Host drives a secondary plugin alongside the primary", "[hotreload]")
{
    // A multi-module host: the primary game module PLUS one secondary plugin module,
    // both sharing the Runtime. AddPlugin registers the secondary BEFORE Load(); Load
    // brings up the primary, then the secondary. Both V1 (primary, +1) and V2 (secondary,
    // +10) target the SAME singleton Pulse entity -- V2's Init finds the one V1 created
    // and caches the same handle -- so ONE FixedUpdateAll step lands +1 AND +10 on it.
    // The value 11 is unique to "both ran": primary-only would be 1, secondary-only 10.
    std::filesystem::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();

    Arcane::PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
    host.AddPlugin(std::filesystem::path("HotReloadPluginV2.dll"));   // secondary, +10 per step
    REQUIRE(host.Load());
    REQUIRE(host.IsLoaded());

    StepAllK(rt, host, 1);
    CHECK(ReadPulse(rt) == 11);                    // primary(+1) AND secondary(+10) both drove

    // A primary hot-reload quiesces the secondary, reloads the primary (state preserved),
    // then re-establishes the secondary on the post-reload registry. If the secondary
    // were dropped, the next step would land only the primary's +1 (12); re-established,
    // both drive again for +11 (22). This exercises ShutdownPluginsLive/InitPluginsLive.
    REQUIRE(host.ForceReload());
    CHECK(ReadPulse(rt) == 11);                    // primary state survived the reload
    StepAllK(rt, host, 1);
    CHECK(ReadPulse(rt) == 22);                    // secondary re-established AND driven

    CHECK(Arcane::RenderErrorCount() == 0);
    host.Unload();
}

TEST_CASE("Plugins-only host (no primary module) loads and drives its secondaries", "[hotreload]")
{
    // The editor opening a project that ships plugin modules but no gameModule: the host is
    // built with an EMPTY primary path and brings up only its AddPlugin() secondaries. No
    // primary means IsLoaded()==false / Vtable()==null, but the secondaries still run through
    // the *All drivers.
    std::filesystem::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();

    Arcane::PluginHost host(rt, std::filesystem::path{});           // no primary game module
    host.AddPlugin(std::filesystem::path("HotReloadPluginV1.dll")); // one secondary (+1/step)
    REQUIRE(host.Load());
    CHECK_FALSE(host.IsLoaded());            // no PRIMARY is loaded...
    CHECK(host.Vtable() == nullptr);

    StepAllK(rt, host, 3);
    CHECK(ReadPulse(rt) == 3);               // ...yet the secondary runs via FixedUpdateAll

    // Reload is a clean no-op success on a plugins-only host (secondaries never hot-reload),
    // leaving the running plugin and its state untouched.
    CHECK(host.ForceReload());
    StepAllK(rt, host, 1);
    CHECK(ReadPulse(rt) == 4);
    CHECK(Arcane::RenderErrorCount() == 0);
    host.Unload();
}

TEST_CASE("Unloading a plugin restores the descriptors it overrode", "[hotreload]")
{
    // REGRESSION lineage: diagnosed 2026-07-31 as a permanent dangling-descriptor
    // AV; first fixed by UnregisterModuleRange (purge to a hole); now the
    // ComponentModule owner stack RESTORES the previous owner instead. This
    // test binary registers Pulse anonymously (below), the plugin's handle
    // overrides it, and unload must pop back to the test binary's entry --
    // non-null AND callable (a dangling restore faults right here).
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();

    const Astra::ComponentID pulseId = Astra::TypeID<Pulse>::Value();
    const Astra::ComponentDescriptor* base = rt.Components()->GetComponentDescriptor(pulseId);
    REQUIRE(base != nullptr);

    {
        Arcane::PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
        REQUIRE(host.Load());
        REQUIRE(rt.Components()->GetComponentDescriptor(pulseId) != nullptr);
        host.Unload();
    }

    // Restored to the test binary's own registration: same slot address
    // (m_components[id] is pointer-stable), callable code in THIS image.
    const Astra::ComponentDescriptor* restored = rt.Components()->GetComponentDescriptor(pulseId);
    REQUIRE(restored == base);
    REQUIRE(restored->defaultConstruct != nullptr);
    alignas(Pulse) std::byte pbuf[sizeof(Pulse)];
    restored->DefaultConstruct(pbuf);
    restored->Destruct(pbuf);

    // The engine roster half is unchanged in spirit: Transform survives, callable.
    const Astra::ComponentID transformId = Astra::TypeID<Arcane::Transform>::Value();
    const Astra::ComponentDescriptor* tf = rt.Components()->GetComponentDescriptor(transformId);
    REQUIRE(tf != nullptr);
    alignas(Arcane::Transform) std::byte buf[sizeof(Arcane::Transform)];
    tf->DefaultConstruct(buf);
    tf->Destruct(buf);

    // Meta acceptance (spec 2026-08-09 section 4): Pulse is REFLECTED, and its meta was
    // rebound to the plugin image while loaded. After unload+restore, consumers
    // that look meta up fresh must get a live entry -- and the restored
    // descriptor's cached meta pointer must AGREE with the registry (the Astra
    // side rebinds in place at a stable address on pop).
    const Astra::TypeMeta* liveMeta = Astra::MetaRegistry::Instance().Get(Astra::TypeID<Pulse>::Hash());
    REQUIRE(liveMeta != nullptr);
    CHECK(restored->meta == liveMeta);

    CHECK(Arcane::RenderErrorCount() == 0);
}

TEST_CASE("Unloading secondaries leaves no descriptor aimed at their images", "[hotreload]")
{
    // The audited live bug: plugins.clear() used to FreeLibrary secondaries with
    // no purge, leaving Pulse's descriptor aimed at the unmapped V2 image
    // permanently. Now V2's own ComponentModule resets in its Shutdown (and the
    // host's DisownPluginImages nets a forgetter). After Unload, the descriptor
    // must be the test binary's restored base -- calling through it faults if
    // any dangling entry survived.
    std::filesystem::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadPluginV1.dll",
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();
    const Astra::ComponentID pulseId = Astra::TypeID<Pulse>::Value();
    const Astra::ComponentDescriptor* base = rt.Components()->GetComponentDescriptor(pulseId);
    REQUIRE(base != nullptr);

    {
        Arcane::PluginHost host(rt, std::filesystem::path("HotReloadPluginV1.dll"));
        host.AddPlugin(std::filesystem::path("HotReloadPluginV2.dll"));   // secondary
        REQUIRE(host.Load());
        StepAllK(rt, host, 1);
        // Primary AND secondary both pushed handles over the base: depth-2 stack.
        host.Unload();
    }

    const Astra::ComponentDescriptor* restored = rt.Components()->GetComponentDescriptor(pulseId);
    REQUIRE(restored == base);
    alignas(Pulse) std::byte buf[sizeof(Pulse)];
    restored->DefaultConstruct(buf);      // faults if a dead image's entry was restored
    restored->Destruct(buf);
    CHECK(Arcane::RenderErrorCount() == 0);
}

TEST_CASE("Reload failure with no last-good yields an honest dead state", "[hotreload]")
{
    // The double-failure fix's reachable branch: a reload whose new image fails AND
    // for which there is no last-good to roll back to (rolledBack == false). The host
    // must surface an honest dead state -- IsLoaded()==false / Vtable()==null truly
    // meaning "no plugin" -- rather than installing a half-assigned "loaded" image
    // that would silently freeze the sim behind the main loop's `if (vt)` guards.
    //
    // NOTE: the OTHER double-failure sub-case (a real last-good whose plugin the
    // teardown already emptied, then its temp copy fails to reload) is NOT cleanly
    // reachable from the public fixture API: that temp copy is OS-locked while the
    // module is loaded and is only unlocked mid-ForceReload (inside TeardownImage),
    // where a test cannot intervene. Both sub-cases run the SAME fix code (the
    // rolledBack gate -> reset current + honest ARC_ERROR), so this covers it.

    // A dedicated bad-image source so we never clobber the shared V1 fixture.
    std::filesystem::copy_file("HotReloadPluginBad.dll", "HotReloadBadSrc.dll",
                               std::filesystem::copy_options::overwrite_existing);

    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Pulse>();

    Arcane::PluginHost host(rt, std::filesystem::path("HotReloadBadSrc.dll"));
    CHECK_FALSE(host.ForceReload());   // new image fails, no last-good -> double failure
    CHECK_FALSE(host.IsLoaded());      // honest: no plugin
    CHECK(host.Vtable() == nullptr);

    // The session recovers cleanly from the dead state: a valid image still loads.
    std::filesystem::copy_file("../HotReloadPluginV1/HotReloadPluginV1.dll", "HotReloadBadSrc.dll",
                               std::filesystem::copy_options::overwrite_existing);
    REQUIRE(host.Load());
    REQUIRE(host.IsLoaded());
    REQUIRE(host.Vtable() != nullptr);
    StepK(rt, *host.Vtable(), 3);
    CHECK(ReadPulse(rt) == 3);
    CHECK(Arcane::RenderErrorCount() == 0);
    host.Unload();

    std::error_code ec;
    std::filesystem::remove("HotReloadBadSrc.dll", ec);
}
