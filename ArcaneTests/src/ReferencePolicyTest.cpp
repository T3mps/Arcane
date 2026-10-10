// Settings arc S7 (spec s3.2): a game decides who may change its own settings.
// ReferenceProject's real module (ReferenceGameUnderTest.dll) installs a policy:
// a single-player "mods" toggle that widens the LOCAL HOST to every non-cheat
// Game and Server setting -- never a connected client, never a Cheat setting.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Plugin/PluginHost.hpp>

#include <filesystem>

#include "Helpers/ModuleNames.hpp"
#include "Helpers/TestTypeContext.hpp"

using namespace Arcane;

namespace
{
    CVarHandle Knob(std::string_view name, CVarValue def, Audience audience, CVarFlags flags = CVarFlags::None)
    {
        CVarDesc d;
        d.name = name;
        d.type = def.type;
        d.defaultValue = def;
        d.flags = flags;
        d.help = "S7 policy knob.";
        d.module = "test-s7-policy";
        d.audience = audience;
        const CVarHandle h = CVarRegistry::Get().Register(d);
        INFO(CVarRegistry::Get().LastError());
        REQUIRE_FALSE(h.IsStale());
        return h;
    }
}

TEST_CASE("ReferenceGame's mods policy: off by default; on, it widens LocalHost Game writes -- never Client, never Cheat; gone on unload", "[cvar][reference]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle game = Knob("test.s7policy.gameKnob", CVarValue::Float32(1.0f), Audience::Game);
    const CVarHandle cheat = Knob("test.s7policy.cheatKnob", CVarValue::Float32(1.0f), Audience::Game, CVarFlags::Cheat);

    {
        Runtime runtime(Test::Process());
        PluginHost host(Test::Process(), std::filesystem::path(Arcane::Test::ModuleFile("ReferenceGameUnderTest")));
        REQUIRE(host.AttachRuntime(runtime));
        REQUIRE(host.Load());

        const CVarHandle mods = reg.Find("game.mods.enabled");
        REQUIRE_FALSE(mods.IsStale());
        CHECK(reg.Get(mods)->AsBool() == false);
        CHECK(reg.Set(game, CVarValue::Float32(2.0f), SetBy::Console, {}, CVarContext::LocalHost) == SetResult::Denied);

        // The toggle itself is PlayerSafe: the player flips it from their own menu.
        REQUIRE(reg.Set(mods, CVarValue::Bool(true), SetBy::Console, {}, CVarContext::LocalHost) == SetResult::Applied);
        reg.PublishImmediate();

        CHECK(reg.Set(game, CVarValue::Float32(2.0f), SetBy::Console, {}, CVarContext::LocalHost) == SetResult::Applied);
        CHECK(reg.Set(game, CVarValue::Float32(3.0f), SetBy::Console, {}, CVarContext::Client) == SetResult::Denied);
        CHECK(reg.Set(cheat, CVarValue::Float32(2.0f), SetBy::Console, {}, CVarContext::LocalHost) == SetResult::Denied);
        reg.PublishImmediate();
        CHECK(reg.Get(game)->AsFloat32() == 2.0f);

        host.Unload();
        CHECK(reg.Find("game.mods.enabled").IsStale());
        CHECK(reg.Set(game, CVarValue::Float32(4.0f), SetBy::Console, {}, CVarContext::LocalHost) == SetResult::Denied);
    }
    reg.UnregisterModule("test-s7-policy");
}
