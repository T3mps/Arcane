// The settle bail predicate, extracted so it is testable without a GPU, a
// frame loop or a clock. The loop it governs is desk-verified; THIS is the
// part that can be pinned in CI.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/SettleBound.hpp>

using Arcane::SettleBail;
using Arcane::SettleBailDecision;

TEST_CASE("settle bail: keeps going until BOTH bounds are spent", "[settle]")
{
    // Neither spent.
    CHECK(SettleBailDecision(0, 30, 0, 5000, 50)    == SettleBail::Keep);
    // Attempts spent, time is not -- this is the DEFECT case: the old code
    // bailed here, ~100ms in, before compilation could drain.
    CHECK(SettleBailDecision(30, 30, 100, 5000, 50) == SettleBail::Keep);
    // Time spent, attempts are not.
    CHECK(SettleBailDecision(5, 30, 5000, 5000, 50) == SettleBail::Keep);
}

TEST_CASE("settle bail: names the BINDING bound, not the tripped one", "[settle]")
{
    // 30 attempts x 50ms = 1500ms < 5000ms timeout -> time governs.
    CHECK(SettleBailDecision(94, 30, 5000, 5000, 50) == SettleBail::TimeoutBound);
    // 200 attempts x 50ms = 10000ms >= 5000ms timeout -> attempts govern.
    CHECK(SettleBailDecision(200, 200, 10000, 5000, 50) == SettleBail::AttemptsBound);
    // Exactly equal: attempts reach the timeout at the same instant. Attempts
    // is reported, because raising the timeout alone would not change it.
    CHECK(SettleBailDecision(100, 100, 5000, 5000, 50) == SettleBail::AttemptsBound);
}

TEST_CASE("settle bail: a zero timeout degrades to the attempt bound alone", "[settle]")
{
    // --settle-timeout 0 means "no time bound": the conjunction reduces to the
    // old attempts-only behaviour rather than becoming unsatisfiable.
    CHECK(SettleBailDecision(29, 30, 0, 0, 50) == SettleBail::Keep);
    CHECK(SettleBailDecision(30, 30, 0, 0, 50) == SettleBail::AttemptsBound);
}
