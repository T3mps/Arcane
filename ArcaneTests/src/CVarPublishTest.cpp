// Publish correctness (settings spec s4.5, O4): the cheat revert fires
// callbacks, and callback dispatch survives a callback that adds callbacks or
// registers cvars. [cvar]

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarRegistry.hpp>

#include "Helpers/CVarTestDesc.hpp"

#include <string>

using namespace Arcane;

TEST_CASE("cheat revert: turning server.cheats off reverts every Cheat setting THROUGH publish, so its callbacks fire", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle noclip = reg.Register(Test::Desc("game.noclip", CVarValue::Bool(false), Audience::Game, CVarFlags::Cheat));
    REQUIRE_FALSE(noclip.IsStale());
    struct Probe { CVarRegistry* reg; int fires = 0; bool last = false; } probe{ &reg };
    reg.AddCallback(noclip, [](CVarHandle h, void* u)
    {
        auto* p = static_cast<Probe*>(u);
        ++p->fires;
        p->last = p->reg->Get(h)->AsBool();
    }, &probe);

    REQUIRE(reg.Set(reg.Find("server.cheats"), CVarValue::Bool(true), SetBy::Console) == SetResult::Applied);
    reg.Publish();
    REQUIRE(reg.Set(noclip, CVarValue::Bool(true), SetBy::Console, {}, CVarContext::LocalHost) == SetResult::Applied);
    reg.Publish();
    REQUIRE(probe.fires == 1);
    REQUIRE(probe.last == true);

    REQUIRE(reg.Set(reg.Find("server.cheats"), CVarValue::Bool(false), SetBy::Console) == SetResult::Applied);
    reg.Publish();
    CHECK(reg.Get(noclip)->AsBool() == false);
    CHECK(probe.fires == 2);                 // the revert fired it (before the fix it did not)
    CHECK(probe.last == false);
    CHECK(reg.Explain("game.noclip")->setBy == SetBy::Default);
}

TEST_CASE("callback dispatch works on a copy: a callback added during dispatch first fires on the next change; one that registers cvars cannot invalidate the dispatch", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle h = reg.Register(Test::Desc("game.knob", CVarValue::Int32(0)));
    struct State { CVarRegistry* reg; int first = 0; int second = 0; int registered = 0; } s{ &reg };
    reg.AddCallback(h, [](CVarHandle handle, void* u)
    {
        auto* st = static_cast<State*>(u);
        if (++st->first != 1) return;
        st->reg->AddCallback(handle, [](CVarHandle, void* u2) { ++static_cast<State*>(u2)->second; }, u);
        for (int i = 0; i < 64; ++i)    // grow the slot array under the dispatch
            if (!st->reg->Register(Test::Desc("grow." + std::to_string(i), CVarValue::Int32(i))).IsStale())
                ++st->registered;
    }, &s);

    REQUIRE(reg.Set(h, CVarValue::Int32(1), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(s.first == 1);
    CHECK(s.second == 0);                    // added during dispatch: not fired by it
    CHECK(s.registered == 64);
    REQUIRE(reg.Set(h, CVarValue::Int32(2), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(s.first == 2);
    CHECK(s.second == 1);
}
