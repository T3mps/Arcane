// The Runtime/ClientRuntime split (spec 2026-09-15 s2): a headless Runtime has NO
// presentation -- no client, no hooks, an EMPTY render scheduler; a ClientRuntime
// owns one, attaches itself as the client hooks, and keeps RenderSubmissionSystem
// installed across ClearSystems (which PluginHost -- now Core -- calls on every
// module unload/reload).
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Plugin/ClientHooks.hpp>
#include <Arcane/Plugin/PluginABI.hpp>   // EngineContext (the hooks' fill target)
#include <Arcane/Render/RenderSystems.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include "Helpers/TestTypeContext.hpp"

TEST_CASE("a headless Runtime carries no client and installs only the two headless engine systems", "[runtime][client]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    CHECK(rt.Client() == nullptr);
    CHECK(rt.ClientHooks() == nullptr);
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem>());
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<Arcane::TransformPropagationSystem>());
    CHECK_FALSE(rt.Schedulers().render.HasSystem<Arcane::RenderSubmissionSystem>());
    rt.ClearSystems();
    CHECK_FALSE(rt.Schedulers().render.HasSystem<Arcane::RenderSubmissionSystem>());
}

TEST_CASE("ClientRuntime owns a Runtime, attaches as its client, and keeps render submission across ClearSystems", "[runtime][client]")
{
    Arcane::ClientRuntime crt(Arcane::Test::Process());
    Arcane::Runtime& core = crt.Core();
    CHECK(core.Client() == &crt);
    CHECK(core.ClientHooks() != nullptr);
    CHECK(core.Schedulers().render.HasSystem<Arcane::RenderSubmissionSystem>());
    core.ClearSystems();                                   // the hook path PluginHost takes
    CHECK(core.Schedulers().render.HasSystem<Arcane::RenderSubmissionSystem>());
    CHECK(core.Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem>());
    // the aliases (P5) are the same objects
    CHECK(&crt.Registry() == &core.Registry());
    CHECK(&crt.Loop()     == &core.Loop());
}

TEST_CASE("ClientRuntime's ImGui handoff reaches an EngineContext only through the hooks", "[runtime][client]")
{
    Arcane::ClientRuntime crt(Arcane::Test::Process());
    int a = 0, b = 0, c = 0, d = 0;
    crt.SetImGui(&a, &b, &c, &d);
    Arcane::EngineContext ctx{};
    crt.Core().ClientHooks()->FillEngineContext(ctx);
    CHECK(ctx.imguiContext == &a); CHECK(ctx.imguiAlloc == &b); CHECK(ctx.imguiFree == &c); CHECK(ctx.imguiUserData == &d);
    Arcane::Runtime bare(Arcane::Test::Process());
    CHECK(bare.ClientHooks() == nullptr);                  // a headless host hands the module null ImGui, as before
}
