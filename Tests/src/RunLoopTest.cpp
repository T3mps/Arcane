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

// ---- sim-time control (Epic 04): pause / single-step / time-scale -----------

TEST_CASE("RunLoop paused: fixed phase frozen, Update still runs", "[sim][runloop]")
{
    Astra::Registry reg;
    reg.SetResource<Ticks>(Ticks{});
    Arcane::SystemSchedulers sch(nullptr);
    sch.fixedUpdate.AddSystem<IncrementTicks>();
    Arcane::RunLoop loop(reg, sch);

    loop.SetPaused(true);
    CHECK(loop.IsPaused());

    int updates = 0;
    for (int i = 0; i < 60; ++i)
        loop.Advance(1.0 / 60.0, {}, [&](double, double){ ++updates; });

    CHECK(reg.GetResource<Ticks>()->fixed == 0);  // no fixed steps while paused
    CHECK(updates == 60);                          // ...but the Update phase ran every frame
}

TEST_CASE("RunLoop single-step: exactly one canonical fixed step while paused", "[sim][runloop]")
{
    Astra::Registry reg;
    reg.SetResource<Ticks>(Ticks{});
    Arcane::SystemSchedulers sch(nullptr);
    sch.fixedUpdate.AddSystem<IncrementTicks>();
    Arcane::RunLoop loop(reg, sch);

    loop.SetPaused(true);
    for (int i = 0; i < 10; ++i) loop.Advance(1.0 / 60.0);
    CHECK(reg.GetResource<Ticks>()->fixed == 0);   // frozen

    loop.RequestSingleStep();
    loop.Advance(1.0 / 60.0);
    CHECK(reg.GetResource<Ticks>()->fixed == 1);   // exactly one step

    loop.Advance(1.0 / 60.0);                       // the request was one-shot
    CHECK(reg.GetResource<Ticks>()->fixed == 1);   // still one; no lingering step
    CHECK(loop.IsPaused());                          // and still paused
}

TEST_CASE("RunLoop time-scale scales the sim clock, not the step dt", "[sim][runloop]")
{
    auto stepsOverOneRealSecond = [](double scale)
    {
        Astra::Registry reg;
        reg.SetResource<Ticks>(Ticks{});
        Arcane::SystemSchedulers sch(nullptr);
        sch.fixedUpdate.AddSystem<IncrementTicks>();
        Arcane::RunLoop loop(reg, sch);
        loop.SetTimeScale(scale);
        for (int i = 0; i < 60; ++i) loop.Advance(1.0 / 60.0);  // 1s of real time
        return reg.GetResource<Ticks>()->fixed;
    };

    CHECK(stepsOverOneRealSecond(1.0) >= 58);
    CHECK(stepsOverOneRealSecond(1.0) <= 62);
    // 0.5x: the sim clock runs at half real time -> ~30 canonical steps in one real second.
    CHECK(stepsOverOneRealSecond(0.5) >= 28);
    CHECK(stepsOverOneRealSecond(0.5) <= 32);
    // 2x: ~2 fixed ticks accumulate per real frame (< the 5-step clamp), so ~120 steps.
    CHECK(stepsOverOneRealSecond(2.0) >= 116);
    CHECK(stepsOverOneRealSecond(2.0) <= 124);
    // 0x: the sim clock is stalled entirely.
    CHECK(stepsOverOneRealSecond(0.0) == 0);
}

TEST_CASE("RunLoop unpause does not burst catch-up steps", "[sim][runloop]")
{
    Astra::Registry reg;
    reg.SetResource<Ticks>(Ticks{});
    Arcane::SystemSchedulers sch(nullptr);
    sch.fixedUpdate.AddSystem<IncrementTicks>();
    Arcane::RunLoop loop(reg, sch);

    loop.SetPaused(true);
    for (int i = 0; i < 600; ++i) loop.Advance(1.0 / 60.0);  // 10 real seconds, paused
    CHECK(reg.GetResource<Ticks>()->fixed == 0);

    loop.SetPaused(false);
    loop.Advance(1.0 / 60.0);                                 // one real frame after unpause
    // Paused frames accumulated NOTHING, so there is no 600-step debt to burn down.
    CHECK(reg.GetResource<Ticks>()->fixed <= 1);
}
