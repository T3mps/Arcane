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
#include <Arcane/Base/Assert.hpp>

namespace
{
    bool DividesSafely(int denominator)
    {
        return MOSAIC_ENSURE_ALWAYS(denominator != 0, "denominator must be non-zero");
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
