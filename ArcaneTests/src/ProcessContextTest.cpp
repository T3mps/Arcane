// ProcessContext (spec docs/specs/2026-09-15-core-dll-split-design.md s3): the
// process-wide state Arcane pays for exactly once. N Runtimes share it; a second
// construction is a REFUSAL. The test exe's own instance (Helpers/TestTypeContext.hpp,
// created in test_main before Catch2 runs) is the live one every case here sees --
// which is the honest shape: the refusal is only observable against a live instance,
// and this process has one for its whole life, like every host.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Runtime.hpp>

#include "Helpers/TestTypeContext.hpp"

TEST_CASE("ProcessContext: exactly one per process -- a second Create is refused", "[process]")
{
    Arcane::ProcessContext& live = Arcane::Test::Process();
    REQUIRE(Arcane::ProcessContext::Current() == &live);
    CHECK(Arcane::ProcessContext::Create({}) == nullptr);                       // owned-context flavour
    Arcane::ProcessContextDesc adopt; adopt.externalTypeContext = &Arcane::Test::SharedTypeContext();
    CHECK(Arcane::ProcessContext::Create(adopt) == nullptr);                    // adopting flavour, same refusal
    CHECK(Arcane::ProcessContext::Current() == &live);                          // the refusal did not disturb the slot
}

TEST_CASE("ProcessContext: the test process is not a dedicated-server process and owns the shared TypeContext", "[process]")
{
    Arcane::ProcessContext& live = Arcane::Test::Process();
    CHECK_FALSE(live.IsDedicatedServerProcess());
    CHECK(&live.TypeContext() == &Arcane::Test::SharedTypeContext());
}

TEST_CASE("Runtime: every Runtime is built on the ProcessContext and reports its TypeContext", "[process][runtime]")
{
    Arcane::Runtime a(Arcane::Test::Process());
    Arcane::Runtime b(Arcane::Test::Process());
    CHECK(a.TypeContext() == &Arcane::Test::Process().TypeContext());
    CHECK(b.TypeContext() == a.TypeContext());
}
