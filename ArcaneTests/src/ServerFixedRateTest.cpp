// Core-DLL split, plan 1 Task 6 review round 1: RunLoop::SetFixedHz makes a
// caller's requested fixed-step size REAL, not just the host-loop pacing.
// Before this, StepFixed's canonical `fixedDt` (RunLoop.hpp) was pinned at
// RunLoop::Config::fixedHz's construction-time default (60) regardless of what
// realDt a caller passed into Advance() -- so ArcaneServer's --fixed-dt only
// ever changed how many DEFAULT-SIZED fixed steps accumulated per host frame,
// never their size. Pinned against a real Arcane::Runtime (not a bare
// RunLoop+Registry+SystemSchedulers -- RunLoopTest.cpp's own precedent for
// that shape) because Runtime::Loop() is the exact object
// ArcaneServer/src/ServerApp.cpp drives.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>

#include "Helpers/TestTypeContext.hpp"

TEST_CASE("RunLoop::SetFixedHz makes --fixed-dt control the ACTUAL tick size, not just host-loop pacing", "[server]")
{
    // At the default 60 Hz, a single 0.05s Advance() accumulates exactly 3
    // canonical (1/60s) fixed steps -- the behavior BEFORE this fix, and what a
    // caller who never touches SetFixedHz still gets today.
    {
        Arcane::Runtime rt(Arcane::Test::Process());
        int calls = 0;
        rt.Loop().Advance(0.05, [&](double) { ++calls; }, [&](double, double) {});
        CHECK(calls == 3);
    }

    // The equivalent of `ArcaneServer --fixed-dt 0.05`: SetFixedHz(1/0.05) makes
    // the canonical step ITSELF 0.05s, so the SAME 0.05s Advance() now
    // accumulates exactly ONE fixed step -- the "one fixed step per host frame"
    // shape ArcaneServer's tick loop relies on for --fixed-dt to mean what its
    // help text says.
    {
        Arcane::Runtime rt(Arcane::Test::Process());
        rt.Loop().SetFixedHz(1.0 / 0.05);
        CHECK(rt.Loop().FixedHz() == 1.0 / 0.05);
        int calls = 0;
        rt.Loop().Advance(0.05, [&](double) { ++calls; }, [&](double, double) {});
        CHECK(calls == 1);
    }
}

TEST_CASE("RunLoop::SetFixedHz refuses a non-positive rate", "[server]")
{
    Arcane::Runtime rt(Arcane::Test::Process());
    const double before = rt.Loop().FixedHz();
    rt.Loop().SetFixedHz(0.0);
    CHECK(rt.Loop().FixedHz() == before);
    rt.Loop().SetFixedHz(-1.0);
    CHECK(rt.Loop().FixedHz() == before);
}
