// Input-seam spec 2026-10-02 s5.2: the game-module system factory registers a
// PARAMETER-style system (operator() taking View&/Res/ResMut/Commands) through
// Astra's param path, keyed by its own type and ordered by its traits.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Plugin/GameSystems.hpp>
#include <Arcane/Plugin/SystemFactory.hpp>
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>
#include <Arcane/Sim/Time.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <cstdint>
#include <string>

namespace
{
    std::string g_order;

    struct TypedLater { void operator()(Astra::Registry&) { g_order += 'T'; } };

    struct ParamProbe : Astra::SystemTraits<Astra::Before<TypedLater>>
    {
        inline static std::uint64_t lastStep = 0;
        void operator()(Astra::Res<Arcane::Time> time) { lastStep = time->fixedStep; g_order += 'P'; }
    };
}

TEST_CASE("AddSystemFactory instantiates a parameter-style system, keyed by its own type, ordered by its traits",
          "[runtime][systems][param]")
{
    Arcane::SystemFactoryTable table;
    const int owner = 1;
    table.BeginOwner(&owner);
    Arcane::Game::Detail::AddSystemFactory<TypedLater>(table, Arcane::RoleMask::Both, Arcane::SystemPhase::FixedUpdate);
    Arcane::Game::Detail::AddSystemFactory<ParamProbe>(table, Arcane::RoleMask::Both, Arcane::SystemPhase::FixedUpdate);
    table.EndOwner();

    Arcane::SystemSchedulers sch(nullptr);
    CHECK(table.InstantiateInto(sch, Arcane::NetMode::Standalone) == 2);
    CHECK(sch.fixedUpdate.HasSystem<ParamProbe>());

    Astra::Registry reg;
    Arcane::RunLoop loop(reg, sch);
    g_order.clear();
    ParamProbe::lastStep = 0;
    loop.RequestSingleStep();
    loop.SetPaused(true);
    loop.Advance(1.0 / 60.0);
    CHECK(ParamProbe::lastStep == 1);   // it read the published Time
    CHECK(g_order == "PT");            // Before<TypedLater> honoured though registered second

    table.ClearOwner(&owner);
}
