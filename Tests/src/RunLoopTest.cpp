// RunLoop: a fixed-timestep accumulator drives the FixedUpdate scheduler N times
// per real frame and the Update scheduler once, exposing a render alpha in [0,1).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>
#include <vector>
#include <functional>

namespace
{
    struct Ticks { int fixed = 0; };

    // IncrementTicks is a minimal System (callable with Registry&) that increments
    // the Ticks resource. AddSystem<T> requires the System concept: callable with
    // Registry& -> void. Lambdas taking Registry& are callable but fail the
    // LambdaLike concept guard (LambdaLike excludes System-shaped callables to
    // prevent ambiguity), so we wrap in a named type instead.
    struct IncrementTicks
    {
        void operator()(Astra::Registry& r) const
        {
            r.GetResource<Ticks>()->fixed += 1;
        }
    };
}

namespace
{
    // Named engine system — records 'E' each engine fixed step.
    // Aggregate-initialized by AddSystem<EngineStep>(engineFixed, order).
    // Explicit ctor required because MSVC doesn't support parenthesized
    // aggregate init of structs with reference members under all circumstances.
    struct EngineStep
    {
        int&               n;
        std::vector<char>& order;
        EngineStep(int& n_, std::vector<char>& o_) : n(n_), order(o_) {}
        void operator()(Astra::Registry&) const { ++n; order.push_back('E'); }
    };
}

TEST_CASE("RunLoop interleaves plugin callbacks with engine fixedUpdate", "[sim][runloop]")
{
    Astra::Registry reg;
    int engineFixed = 0;
    std::vector<char> order;
    Arcane::SystemSchedulers sch(nullptr);
    sch.fixedUpdate.AddSystem<EngineStep>(engineFixed, order);

    Arcane::RunLoop loop(reg, sch);
    int pluginFixed = 0, pluginUpdate = 0;
    for (int i = 0; i < 60; ++i)
        loop.Advance(1.0 / 60.0,
            [&](double){ ++pluginFixed; order.push_back('P'); },
            [&](double, double){ ++pluginUpdate; });

    CHECK(pluginFixed >= 58);
    CHECK(pluginFixed == engineFixed);     // one plugin tick per engine fixed step
    CHECK(pluginUpdate == 60);             // once per frame
    // every fixed step is plugin('P') then engine('E'), so the sequence alternates P,E:
    REQUIRE(order.size() >= 2);
    CHECK(order[order.size() - 2] == 'P');
    CHECK(order[order.size() - 1] == 'E');
}

TEST_CASE("RunLoop runs a fixed-rate scheduler and clamps spikes", "[sim][runloop]")
{
    Astra::Registry reg;  // sequential fallback scheduler -- no jobs needed here
    reg.SetResource<Ticks>(Ticks{});

    Arcane::SystemSchedulers schedulers(nullptr);  // null -> sequential executor
    schedulers.fixedUpdate.AddSystem<IncrementTicks>();

    Arcane::RunLoop::Config cfg;   // 60 Hz, maxStepsPerFrame default 5
    Arcane::RunLoop loop(reg, schedulers, cfg);

    for (int i = 0; i < 60; ++i)
    {
        double alpha = loop.Advance(1.0 / 60.0);
        CHECK(alpha >= 0.0);
        CHECK(alpha < 1.0);
    }
    const int afterOneSecond = reg.GetResource<Ticks>()->fixed;
    CHECK(afterOneSecond >= 58);
    CHECK(afterOneSecond <= 62);

    const int before = reg.GetResource<Ticks>()->fixed;
    loop.Advance(10.0);  // would be 600 steps unclamped
    const int stepsTaken = reg.GetResource<Ticks>()->fixed - before;
    CHECK(stepsTaken == cfg.maxStepsPerFrame);
    CHECK(loop.Alpha() >= 0.0);
    CHECK(loop.Alpha() < 1.0);
}
