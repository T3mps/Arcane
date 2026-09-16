// Spec 2026-09-15 s4 + s9: two Runtimes, ONE loaded module, role-masked
// instantiation; ListenServer takes both; HasAuthority across the four modes; and
// the net-mode/launch-flag INDEPENDENCE the spec names as a bug class (UE's
// IsRunningDedicatedServer vs NetMode).
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>
#include "Helpers/TestTypeContext.hpp"
#include "../plugins/HotReloadShared.hpp"
#include <filesystem>
using namespace Arcane::HotReloadTest;

namespace
{
    void Step(Arcane::Runtime& rt, Arcane::PluginHost& host, int k)
    { for (int i = 0; i < k; ++i) rt.Loop().Advance(1.0 / 60.0, [&](double dt){ host.FixedUpdateAll(dt); }, [&](double,double){}); }
    RoleCounters Read(Arcane::Runtime& rt)
    { RoleCounters out; rt.Registry().CreateView<RoleCounters>().ForEach([&](Astra::Entity, RoleCounters& c){ out = c; }); return out; }
}

TEST_CASE("HasAuthority: every mode but Client", "[runtime][netmode]")
{
    using Arcane::NetMode;
    CHECK(Arcane::Runtime(Arcane::Test::Process(), NetMode::Standalone).HasAuthority());
    CHECK(Arcane::Runtime(Arcane::Test::Process(), NetMode::DedicatedServer).HasAuthority());
    CHECK(Arcane::Runtime(Arcane::Test::Process(), NetMode::ListenServer).HasAuthority());
    CHECK_FALSE(Arcane::Runtime(Arcane::Test::Process(), NetMode::Client).HasAuthority());
}

TEST_CASE("net mode and the launch flag are independent: a DedicatedServer Runtime in a non-server process has authority", "[runtime][netmode]")
{
    REQUIRE_FALSE(Arcane::Test::Process().IsDedicatedServerProcess());
    Arcane::Runtime rt(Arcane::Test::Process(), Arcane::NetMode::DedicatedServer);
    CHECK(rt.HasAuthority());
    CHECK(rt.Mode() == Arcane::NetMode::DedicatedServer);
}

TEST_CASE("two Runtimes, one module: the Server-masked system exists only in the server world and the Client-masked only in the client", "[runtime][netmode][hotreload]")
{
    Arcane::Runtime server(Arcane::Test::Process(), Arcane::NetMode::DedicatedServer);
    Arcane::Runtime client(Arcane::Test::Process(), Arcane::NetMode::Client);
    for (auto* rt : { &server, &client }) { rt->Components()->RegisterComponent<Pulse>(); rt->Components()->RegisterComponent<RoleCounters>(); }

    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(server);
    host.AttachRuntime(client);
    REQUIRE(host.Load());
    REQUIRE(host.Runtimes().size() == 2);

    CHECK(server.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());
    CHECK_FALSE(server.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    CHECK(client.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    CHECK_FALSE(client.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());

    server.Registry().CreateEntityWith(RoleCounters{});
    client.Registry().CreateEntityWith(RoleCounters{});
    Step(server, host, 3); Step(client, host, 3);
    CHECK(Read(server).serverTicks == 3); CHECK(Read(server).clientTicks == 0);
    CHECK(Read(client).clientTicks == 3); CHECK(Read(client).serverTicks == 0);
    host.Unload();
}

TEST_CASE("ListenServer instantiates BOTH masks in its one Runtime", "[runtime][netmode][hotreload]")
{
    Arcane::Runtime listen(Arcane::Test::Process(), Arcane::NetMode::ListenServer);
    listen.Components()->RegisterComponent<Pulse>(); listen.Components()->RegisterComponent<RoleCounters>();
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(listen);
    REQUIRE(host.Load());
    CHECK(listen.Schedulers().fixedUpdate.HasSystem<ServerOnlyTick>());
    CHECK(listen.Schedulers().fixedUpdate.HasSystem<ClientOnlyTick>());
    host.Unload();
}

TEST_CASE("the factory table is the process's, cleared when the module unloads", "[runtime][netmode][hotreload]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    rt.Components()->RegisterComponent<Pulse>(); rt.Components()->RegisterComponent<RoleCounters>();
    const std::size_t before = Arcane::Test::Process().SystemFactories().Size();
    Arcane::PluginHost host(Arcane::Test::Process(), std::filesystem::path("HotReloadPluginV1.dll"));
    host.AttachRuntime(rt);
    REQUIRE(host.Load());
    CHECK(Arcane::Test::Process().SystemFactories().Size() == before + 2);
    host.Unload();
    CHECK(Arcane::Test::Process().SystemFactories().Size() == before);   // std::functions into the image are gone BEFORE the unmap
}
