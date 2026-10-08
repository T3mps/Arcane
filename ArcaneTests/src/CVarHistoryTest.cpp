// History holds ONE record per (rung, source) (settings spec s4.5, O3). [cvar]

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarRegistry.hpp>

#include "Helpers/CVarTestDesc.hpp"

using namespace Arcane;

TEST_CASE("cvar history keeps one record per (rung, source); a weaker rung is recorded beneath the winner", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle h = reg.Register(Test::Desc("test.knob", CVarValue::Int32(1)));
    REQUIRE_FALSE(h.IsStale());

    for (int i = 0; i < 5; ++i)
        REQUIRE(reg.Set(h, CVarValue::Int32(10 + i), SetBy::Project, "project") == SetResult::Applied);
    auto explained = reg.Explain("test.knob");
    REQUIRE(explained->history.size() == 2);                      // Default + ONE Project record
    CHECK(explained->history.back().value.AsInt32() == 14);

    REQUIRE(reg.Set(h, CVarValue::Int32(50), SetBy::User, "user") == SetResult::Applied);
    CHECK(reg.Set(h, CVarValue::Int32(20), SetBy::Project, "project") == SetResult::RefusedWeaker);
    reg.Publish();
    CHECK(reg.Get(h)->AsInt32() == 50);                           // the User rung still wins
    explained = reg.Explain("test.knob");
    REQUIRE(explained->history.size() == 3);
    CHECK(explained->history[1].by == SetBy::Project);
    CHECK(explained->history[1].value.AsInt32() == 20);           // replaced in place, beneath User

    reg.RevertLayer(SetBy::User);
    reg.Publish();
    CHECK(reg.Get(h)->AsInt32() == 20);                           // the re-applied project value, not the stale 14

    // Two sources on one rung: each replaces its own record; the newest wins.
    REQUIRE(reg.Set(h, CVarValue::Int32(7), SetBy::Console, "a") == SetResult::Applied);
    REQUIRE(reg.Set(h, CVarValue::Int32(8), SetBy::Console, "b") == SetResult::Applied);
    REQUIRE(reg.Set(h, CVarValue::Int32(9), SetBy::Console, "a") == SetResult::Applied);
    reg.Publish();
    CHECK(reg.Get(h)->AsInt32() == 9);
    explained = reg.Explain("test.knob");
    REQUIRE(explained->history.size() == 4);                      // Default, Project, Console(b), Console(a)
    CHECK(explained->history[2].module == "b");
    CHECK(explained->history[3].module == "a");
}
