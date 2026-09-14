// The settle bail predicate, extracted so it is testable without a GPU, a
// frame loop or a clock. The loop it governs is desk-verified; THIS is the
// part that can be pinned in CI.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <Arcane/Host/SettleBound.hpp>

using Arcane::SettleBail;
using Arcane::SettleBailDecision;
using Arcane::SettleConverged;
using Arcane::SettleProducersIdle;

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

// Task 12a (task12-rca-report.md): byte-equal frames + an idle shader
// compiler proves the RENDER is quiescent, not that the async cook queue or
// thumbnail harvester have finished writing to the Asset Browser's backing
// state -- the 721px/"15 vs 16 assets" race. SettleProducersIdle/
// SettleConverged pin the widened conjunction without a GPU, a frame loop,
// or a live CookQueue/MaterialPreviewHarvester.
TEST_CASE("settle producers: shader idle alone is NOT enough once the cook "
          "queue or harvester are widened in", "[settle]")
{
    // Shader idle, cook queue settling (the first-open window before
    // OnCookCompleted has fired once) -- NOT idle.
    CHECK_FALSE(SettleProducersIdle(/*shaderIdle=*/true, /*cookQueueSettling=*/true,
                                     /*cookPending=*/false, /*harvesterPending=*/false));
    // Shader idle, cook queue no longer settling but a pass is in flight
    // (CookQueue::CookPending()) -- NOT idle. This is the RCA's own
    // reproduction: golden_prop's fresh mtime trips PollAssetWatch's frame-1
    // sweep, NoteChanged() submits an async CookProject pass, and the settle
    // loop must not converge while it is still running.
    CHECK_FALSE(SettleProducersIdle(true, false, /*cookPending=*/true, false));
    // Cook queue fully idle, but the thumbnail harvester still has queued or
    // in-flight work (MaterialPreviewHarvester::PendingCount() != 0) -- NOT
    // idle either.
    CHECK_FALSE(SettleProducersIdle(true, false, false, /*harvesterPending=*/true));
    // The shader compiler itself busy still governs too, unchanged from the
    // pre-Task-12a behaviour this widens.
    CHECK_FALSE(SettleProducersIdle(/*shaderIdle=*/false, false, false, false));
    // All four clear -> idle.
    CHECK(SettleProducersIdle(true, false, false, false));
}

TEST_CASE("settle converged: byte-equal frames + shader idle + cook pending "
          "-> NOT converged; + cook idle + harvester idle -> converged", "[settle]")
{
    // Byte-equal frames, shader idle, but the cook queue is still pending:
    // producersIdle is false, so the overall convergence predicate must stay
    // false regardless of the reference match -- this is exactly the state
    // the RCA shows the pre-fix loop wrongly called "converged" (it only
    // checked shader idle).
    const bool byteEqual = true;
    const bool matches   = true;
    const bool producersIdleWithCookPending =
        SettleProducersIdle(/*shaderIdle=*/true, /*cookQueueSettling=*/false,
                             /*cookPending=*/true, /*harvesterPending=*/false);
    CHECK_FALSE(SettleConverged(byteEqual, producersIdleWithCookPending, matches));

    // Once the cook queue AND the harvester both go idle too, the same
    // byte-equal/matches pair now converges.
    const bool producersFullyIdle =
        SettleProducersIdle(/*shaderIdle=*/true, /*cookQueueSettling=*/false,
                             /*cookPending=*/false, /*harvesterPending=*/false);
    CHECK(SettleConverged(byteEqual, producersFullyIdle, matches));

    // A non-byte-equal frame or a failed --compare still must not converge
    // even with every producer idle -- SettleConverged is a plain AND, not a
    // substitute for either of the other two conjuncts.
    CHECK_FALSE(SettleConverged(/*byteEqual=*/false, producersFullyIdle, matches));
    CHECK_FALSE(SettleConverged(byteEqual, producersFullyIdle, /*matches=*/false));
}
