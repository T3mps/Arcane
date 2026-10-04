// The cvar access model (settings spec s3.2): CVarContext, the audience x
// context table, the game's CVarPolicy and the audit sink. CPU-only ([cvar]).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/ConsoleModel.hpp>

#include "Helpers/CVarTestDesc.hpp"

#include <string>
#include <vector>

using namespace Arcane;

TEST_CASE("CVarContext replaces Permission: the editor writes a Game setting, a host or client console does not", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle speed = reg.Register(Test::Desc("game.speed", CVarValue::Int32(1)));
    REQUIRE_FALSE(speed.IsStale());
    CHECK(reg.Set(speed, CVarValue::Int32(3), SetBy::Console, {}, CVarContext::LocalHost) == SetResult::Denied);
    CHECK(reg.Set(speed, CVarValue::Int32(3), SetBy::Console, {}, CVarContext::Client) == SetResult::Denied);
    CHECK(reg.Set(speed, CVarValue::Int32(2), SetBy::Console, {}, CVarContext::Editor) == SetResult::Applied);
    CHECK_FALSE(reg.Execute("game.speed 4", CVarContext::Client).ok);
    CHECK(reg.Execute("game.speed 5", CVarContext::Editor).ok);

    ConsoleModel model;
    model.SetInput("game.speed 6");
    model.Submit(reg, CVarContext::LocalHost);
    CHECK_FALSE(model.Lines().back().ok);

    ApplyCVarCommandLine(reg, { "game.speed=7" }, CVarContext::LocalHost);   // warns, sets nothing
    reg.Publish();
    CHECK(reg.Get(speed)->AsInt32() == 5);
    CHECK(reg.Explain("game.speed")->setBy == SetBy::Console);
}
