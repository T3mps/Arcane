// The Runtime/ClientRuntime split (spec 2026-09-15 s2): a headless Runtime has NO
// presentation -- no client, no hooks, an EMPTY render scheduler; a ClientRuntime
// owns one, attaches itself as the client hooks, and keeps RenderSubmissionSystem
// installed across ClearSystems (which PluginHost -- now Core -- calls on every
// module unload/reload).
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Input/InputActionAsset.hpp>
#include <Arcane/Input/LocalInputUser.hpp>
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

namespace
{
    Arcane::InputActionAsset RuntimeInputAsset()
    {
        return *Arcane::InputActionAsset::FromJson(nlohmann::json::parse(R"JSON({
            "version":1,"id":"11111111-1111-4111-8111-111111111111",
            "defaultMap":"22222222-2222-4222-8222-222222222222","controlSchemes":[],
            "actionMaps":[{"id":"22222222-2222-4222-8222-222222222222","name":"Player","actions":[
              {"id":"33333333-3333-4333-8333-333333333333","name":"Jump","type":"Button","bindings":[
                {"id":"44444444-4444-4444-8444-444444444444","path":"<Keyboard>/space"}]}
            ]}]
        })JSON"));
    }
}

TEST_CASE("ClientRuntime keeps gameplay actions separate from the raw host snapshot", "[client][input]")
{
    Arcane::ClientRuntime runtime(Arcane::Test::Process());
    const auto project = *Arcane::Guid::FromString("55555555-5555-4555-8555-555555555555");
    REQUIRE(runtime.ConfigureGameInput(RuntimeInputAsset(), project));
    const auto jump = runtime.GameInput().FindAction("Player", "Jump");
    REQUIRE(jump);
    Arcane::InputSnapshot raw;
    raw.AddKeycode(32);
    runtime.SetInputSnapshot(raw);
    CHECK(runtime.Input().KeycodeDown(32));
    CHECK_FALSE(runtime.GameInput().Down(*jump));
    runtime.UpdateGameInput(1.0 / 60.0, raw);
    CHECK(runtime.GameInput().Down(*jump));
    CHECK(runtime.GameInput().Pressed(*jump));
    CHECK(runtime.GameInput().Started(*jump));
    CHECK(runtime.GameInput().Performed(*jump));
    CHECK(runtime.GameInput().ButtonDown(*jump) == true);
    runtime.BeginGameInputFixedStep();
    CHECK(runtime.GameInput().PressedThisFixedStep(*jump));
    runtime.BeginGameInputFixedStep();
    CHECK_FALSE(runtime.GameInput().PressedThisFixedStep(*jump));
    runtime.UpdateGameInput(1.0 / 60.0, {});
    CHECK(runtime.GameInput().Released(*jump));
}

TEST_CASE("ClientRuntime switches gameplay input between project identities", "[client][input]")
{
    Arcane::ClientRuntime runtime(Arcane::Test::Process());
    const auto a = *Arcane::Guid::FromString("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const auto b = *Arcane::Guid::FromString("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    REQUIRE(runtime.ConfigureGameInput(RuntimeInputAsset(), a));
    const auto jump = runtime.GameInput().FindAction("Player", "Jump");
    REQUIRE(jump);
    const auto binding = *Arcane::Guid::FromString("44444444-4444-4444-8444-444444444444");
    REQUIRE(runtime.GameInput().SetOverride(binding, "<Keyboard>/scancode/w"));
    REQUIRE(runtime.ConfigureGameInput(RuntimeInputAsset(), b));
    CHECK(runtime.GameInput().ProjectId() == b);
    CHECK(runtime.GameInput().Bindings(*jump)[0].effectivePath == "<Keyboard>/space");
}
