// Runtime is the engine facade EngineContext.engine points at. It owns the substrate
// that outlives reloads: TypeContext (installed in Arcane.dll), persistent
// ComponentRegistry, a swappable Registry, schedulers, RunLoop, and the JobSystem.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Audio/AudioDevice.hpp>
#include <Arcane/Base/Runtime.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <Astra/Registry/Registry.hpp>
#include <Astra/Reflection/Reflection.hpp>

#include <atomic>

namespace { struct Counter { int value = 0; }; }
namespace { ASTRA_REFLECT_TYPE(Counter) ASTRA_REFLECT_FIELD(Counter, value) ASTRA_END_REFLECT_TYPE() }

// A void(Registry&) LAMBDA does NOT satisfy Astra's LambdaLike (that concept is for
// per-entity lambdas); systems must be NAMED types registered via AddSystem<T>().
namespace { struct NoOpSystem { void operator()(Astra::Registry&) const {} }; }

TEST_CASE("Runtime boots a usable substrate", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    REQUIRE(rt.TypeContext() != nullptr);
    REQUIRE(rt.WorkScheduler() != nullptr);
    REQUIRE(rt.WorkScheduler()->WorkerCount() >= 1);
    REQUIRE(rt.AudioSystem().IsInitialized());
    CHECK(rt.AssetsFacade().Stats().count == 0);

    rt.Components()->RegisterComponent<Counter>();
    auto& reg = rt.Registry();
    for (int i = 0; i < 8; ++i) reg.CreateEntityWith(Counter{i});

    int seen = 0;
    reg.CreateView<Counter>().ForEach([&](Astra::Entity, Counter&) { ++seen; });
    CHECK(seen == 8);
}

TEST_CASE("Runtime resets audio without disturbing the engine substrate", "[runtime][audio]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    REQUIRE(rt.AudioSystem().IsInitialized());

    rt.ResetAudio();

    CHECK(rt.AudioSystem().IsInitialized());
    CHECK(rt.WorkScheduler() != nullptr);
    CHECK(rt.Registry().IsEmpty());
}

TEST_CASE("Runtime snapshot/restore preserves state AND the scheduler", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Counter>();
    auto& reg = rt.Registry();
    constexpr int kN = 2048;
    for (int i = 0; i < kN; ++i) reg.CreateEntityWith(Counter{7});

    std::vector<std::byte> snap = rt.SnapshotRegistry();
    REQUIRE(!snap.empty());

    // Mutate the live registry, then restore the snapshot.
    reg.CreateView<Counter>().ForEach([](Astra::Entity, Counter& c) { c.value = 0; });
    REQUIRE(rt.RestoreRegistry(snap));

    std::atomic<int> visited{0};
    std::atomic<int> sum{0};
    rt.Registry().CreateView<Counter>().ParallelForEach([&](Astra::Entity, Counter& c) {
        visited.fetch_add(1, std::memory_order_relaxed);
        sum.fetch_add(c.value, std::memory_order_relaxed);
    });
    CHECK(visited.load() == kN);     // state survived
    CHECK(sum.load() == 7 * kN);     // values survived (== 7, not the mutated 0)
}

TEST_CASE("Runtime ClearSystems empties all phase schedulers", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Schedulers().fixedUpdate.AddSystem<NoOpSystem>();   // each scheduler has its own
    rt.Schedulers().update.AddSystem<NoOpSystem>();        // type index, so reusing the
    rt.Schedulers().render.AddSystem<NoOpSystem>();        // same type across them is fine
    rt.ClearSystems();
    CHECK(rt.Schedulers().fixedUpdate.Empty());
    CHECK(rt.Schedulers().update.Empty());
    CHECK(rt.Schedulers().render.Empty());
}

TEST_CASE("Runtime ResetRegistry empties the registry but keeps the ComponentRegistry", "[runtime]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    rt.Components()->RegisterComponent<Counter>();
    rt.Registry().CreateEntityWith(Counter{42});
    REQUIRE(rt.Registry().Size() == 1);

    rt.ResetRegistry();
    CHECK(rt.Registry().IsEmpty());

    // The shared ComponentRegistry still knows Counter -> this must not crash and must land.
    rt.Registry().CreateEntityWith(Counter{1});
    CHECK(rt.Registry().Size() == 1);
}
