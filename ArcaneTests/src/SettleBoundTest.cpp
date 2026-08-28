// The settle bail predicate, extracted so it is testable without a GPU, a
// frame loop or a clock. The loop it governs is desk-verified; THIS is the
// part that can be pinned in CI.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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

TEST_CASE("settle bail: an absurd attempt count cannot overflow the comparison", "[settle]")
{
    // attempts * intervalMs WRAPS uint64_t here: 2^63 * 2 is exactly 2^64, so
    // the product form computed 0 and answered "0 >= 5000 is false", naming the
    // TIMEOUT as governing on a run whose attempt budget dominates it by
    // fifteen orders of magnitude -- sending the caller to the wrong knob. The
    // division form cannot wrap, so it gets this right.
    constexpr std::uint64_t kHuge = std::uint64_t{1} << 63;
    CHECK(SettleBailDecision(kHuge, kHuge, 5000, 5000, 2) == SettleBail::AttemptsBound);
}

TEST_CASE("settle bail: a zero interval is legal and never divides by zero", "[settle]")
{
    // intervalMs 0 is a legal argument. Attempts then consume no time at all,
    // so they can only outlast a timeout that does not exist -- which is
    // exactly what the product form answered (0 >= timeoutMs) before the
    // rewrite, and what the guard must keep answering after it.
    CHECK(SettleBailDecision(30, 30, 0, 0, 0)       == SettleBail::AttemptsBound);
    CHECK(SettleBailDecision(30, 30, 5000, 5000, 0) == SettleBail::TimeoutBound);
}

TEST_CASE("settle bail: attempts-vs-timeout rounds UP, never down", "[settle]")
{
    // timeoutMs is not a multiple of intervalMs. 5001 / 50 FLOORS to 100, but
    // 100 attempts only reach 5000ms -- one interval short of the timeout. A
    // floor would call that attempts-governed and tell the caller to raise
    // --settle, which would not change the outcome. The ceiling (101) is the
    // honest answer, and this pins that the division rewrite kept it.
    CHECK(SettleBailDecision(100, 100, 5001, 5001, 50) == SettleBail::TimeoutBound);
    CHECK(SettleBailDecision(101, 101, 5001, 5001, 50) == SettleBail::AttemptsBound);
}
