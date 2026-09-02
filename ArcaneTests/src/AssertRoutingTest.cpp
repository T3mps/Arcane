// Both directions of the assert seam:
//   forward -- a guard that fires inside a test FAILS THAT TEST rather than
//              going to a log or aborting the process;
//   inverse -- a test can REQUIRE that a guard fires, which is how a guard's
//              own correctness gets tested at all.
//
// LATCH NOTE: ARC_ENSURE wraps MOSAIC_ENSURE, which dedups per CALL SITE for
// the life of the process (Assert.hpp:233-243) -- a single call site only
// ever reports its first failure. DividesSafely below is one call site but
// this file calls it with a failing denominator from multiple TEST_CASEs and
// nested scopes, each expecting its own report. Confirmed at source (not
// re-derived): under random test order the case that ran first consumed the
// one-shot latch, and every later DividesSafely(0) call reported Count()==0.
// So DividesSafely uses MOSAIC_ENSURE_ALWAYS (Assert.hpp:245, no per-site
// dedup) directly as a test-local guard, rather than the shared ARC_ENSURE.
#include "Helpers/TestAssertScope.hpp"

#include <catch2/catch_test_macros.hpp>

namespace
{
    bool DividesSafely(int denominator)
    {
        return MOSAIC_ENSURE_ALWAYS(denominator != 0, "denominator must be non-zero");
    }

    // MOSAIC_ASSERT is a STATEMENT (no return value), unlike MOSAIC_ENSURE/
    // MOSAIC_ENSURE_ALWAYS above -- wrapped in a void function so it still
    // satisfies ARC_INTERNAL_ASSERT_FIRES' `(void)(expr)` contract. No latch
    // to work around here: unlike MOSAIC_ENSURE, MOSAIC_ASSERT has no
    // per-call-site dedup (Assert.hpp:203-209 calls FailFatal unconditionally
    // every time), so a plain MOSAIC_ASSERT (not an _ALWAYS variant, which
    // does not exist for this guard) is correct as-is.
    void AssertsPositive(int value)
    {
        MOSAIC_ASSERT(value > 0, "value must be positive");
    }
}

TEST_CASE("assert routing: a failing ARC_ENSURE is observable to the test", "[base][assert]")
{
    ArcaneAssertScope scope;
    CHECK_FALSE(DividesSafely(0));
    CHECK(scope.Count() == 1);
    CHECK(scope.LastMessage() == std::string("denominator must be non-zero"));
}

TEST_CASE("assert routing: a passing guard fires nothing", "[base][assert]")
{
    ArcaneAssertScope scope;
    CHECK(DividesSafely(2));
    CHECK(scope.Count() == 0);
}

TEST_CASE("assert routing: REQUIRE_ARC_ENSURE demands a guard fires", "[base][assert]")
{
    REQUIRE_ARC_ENSURE(DividesSafely(0));
}

// REQUIRE_ARC_ASSERT/CHECK_ARC_ASSERT share ARC_INTERNAL_ASSERT_FIRES with the
// ENSURE pair above, but that textual identity does not by itself prove they
// work for a FATAL guard: MOSAIC_ASSERT's failure path (FailFatal) can call
// std::abort(), which MOSAIC_ENSURE's (FailEnsure) never can -- ENSURE
// coverage says nothing about whether the scope's Continue actually
// neutralises a real abort. This case is that proof, exercised through
// REQUIRE_ARC_ASSERT specifically; CHECK_ARC_ASSERT is not given a separate
// case because it differs from REQUIRE_ARC_ASSERT only in Catch2's own
// REQUIRE-vs-CHECK routing (same asymmetry already accepted for
// CHECK_ARC_ENSURE, which likewise has no dedicated case above), not in the
// neutralisation mechanism this case is here to prove.
TEST_CASE("assert routing: REQUIRE_ARC_ASSERT proves the scope neutralises a fatal guard's abort", "[base][assert]")
{
    REQUIRE_ARC_ASSERT(AssertsPositive(-1));
}

TEST_CASE("assert routing: the scope restores the previous handler", "[base][assert]")
{
    // Nested scopes must not leak: the inner one restores the outer, not the
    // default. Otherwise one test's scope silently disarms the next.
    ArcaneAssertScope outer;
    {
        ArcaneAssertScope inner;
        CHECK_FALSE(DividesSafely(0));
        CHECK(inner.Count() == 1);
    }
    CHECK(outer.Count() == 0);
    CHECK_FALSE(DividesSafely(0));
    CHECK(outer.Count() == 1);
}
