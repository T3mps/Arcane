// RunLoop: a fixed-timestep accumulator drives the FixedUpdate scheduler N times
// per real frame and the Update scheduler once, exposing a render alpha in [0,1).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>

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
}
