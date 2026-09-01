// The verdict vocabulary's string set is a CONTRACT shared with a PowerShell
// consumer that cannot include this header (golden-gate.ps1). This file is one
// of the two independent pins on that list; the other is the literal set in
// golden-gate.ps1, asserted by its -SelfTest. Divergence must be a test
// failure, not a latent inconsistency.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/Verdict.hpp>

#include <string>
#include <vector>

TEST_CASE("verdict: the string set is exactly these seven, in this order", "[host][verdict]")
{
    const std::vector<std::string> expected = {
        "Passed", "PassedOnFallback", "Failed", "Errored",
        "NotRun", "Skipped", "Indeterminate",
    };

    std::vector<std::string> actual;
    for (const Arcane::Verdict v : Arcane::AllVerdicts())
        actual.emplace_back(Arcane::ToString(v));

    CHECK(actual == expected);
}

TEST_CASE("verdict: every string round-trips back to its value", "[host][verdict]")
{
    for (const Arcane::Verdict v : Arcane::AllVerdicts())
    {
        const auto back = Arcane::FromString(Arcane::ToString(v));
        REQUIRE(back.has_value());
        CHECK(*back == v);
    }
}

TEST_CASE("verdict: FromString refuses anything else", "[host][verdict]")
{
    // Case matters: these are wire values, not human input. "PASS" is the OLD
    // vocabulary and must not silently resolve to the new one.
    CHECK_FALSE(Arcane::FromString("PASS").has_value());
    CHECK_FALSE(Arcane::FromString("FAIL").has_value());
    CHECK_FALSE(Arcane::FromString("passed").has_value());
    CHECK_FALSE(Arcane::FromString("").has_value());
}

TEST_CASE("verdict: exactly two values are green", "[host][verdict]")
{
    CHECK(Arcane::IsGreen(Arcane::Verdict::Passed));
    CHECK(Arcane::IsGreen(Arcane::Verdict::PassedOnFallback));
    CHECK_FALSE(Arcane::IsGreen(Arcane::Verdict::Failed));
    CHECK_FALSE(Arcane::IsGreen(Arcane::Verdict::Errored));
    CHECK_FALSE(Arcane::IsGreen(Arcane::Verdict::NotRun));
    // Skipped is NOT green: it does not fail a gate, but it does not satisfy
    // one either (see the spec's gatePassed rule and its vacuity guard).
    CHECK_FALSE(Arcane::IsGreen(Arcane::Verdict::Skipped));
    CHECK_FALSE(Arcane::IsGreen(Arcane::Verdict::Indeterminate));
}
