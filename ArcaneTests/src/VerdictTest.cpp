// The verdict vocabulary's string set is a CONTRACT shared with a PowerShell
// consumer that cannot include this header (golden-gate.ps1). This file is one
// of the two independent pins on that list; the other is the literal set in
// golden-gate.ps1, asserted by its -SelfTest. Divergence must be a test
// failure, not a latent inconsistency.
//
// "TWO PINS" WAS NOT ENOUGH ON ITS OWN, and the gap is why the emitter case at
// the bottom of this file exists. Both halves were pinned to their own copy;
// nothing compared the two copies to each other. -SelfTest ran
// `ArcaneTests.exe "[verdict]"` and checked its EXIT CODE, which proves only
// that the C++ side agrees with itself -- rename Indeterminate to Unknown here
// and in the cases above, exactly as this header's own instruction demands,
// and the probe still exits 0 while golden-gate.ps1 keeps the stale list. The
// emitter publishes what Arcane::ToString ACTUALLY produces so the gate can
// diff it against its own literals instead of taking an exit code's word for
// it.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/Verdict.hpp>
#include <Arcane/Host/VerifyReport.hpp>

#include <filesystem>
#include <fstream>
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

// ---- THE CROSS-LANGUAGE PIN'S PUBLISHING HALF ------------------------------
//
// Emits what the engine ACTUALLY produces into a file beside the exe, which
// golden-gate.ps1's -SelfTest then diffs against its own literals. A file
// rather than stdout, deliberately: whether a Catch2 reporter redirects stdout
// is the reporter's business (junit and xml do, console does not), and a pin
// whose reliability depends on which -r flag the caller happened to pass is not
// a pin. The gate DELETES this file before running the probe and refuses if it
// is not recreated, so a stale copy from an older build cannot stand in for a
// fresh one.
//
// Written to the CURRENT DIRECTORY because this suite runs FROM the exe
// directory -- the same convention automation-exclusions.json is staged and
// read under (ExclusionExpiryTest.cpp) -- and the gate looks for it beside the
// exe it just launched.
//
// The report schema RANGE rides along for the same reason the verdicts do: the
// gate now range-checks every report it parses (it reads compare.
// maxLocalDifference, which only exists at v4), and a second hand-maintained
// copy of two integers is a second thing to drift. Publishing them here means
// the gate never carries its own numbers at all.
constexpr const char* kVocabularyFileName = "automation-vocabulary.txt";

TEST_CASE("verdict: the vocabulary is published for the PowerShell half to diff against", "[host][verdict]")
{
    const std::filesystem::path out = kVocabularyFileName;

    {
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        REQUIRE(f.is_open());
        // One fact per line, `key=value`, LF only -- greppable from PowerShell
        // with a plain -match and immune to the CRLF/LF question entirely.
        for (const Arcane::Verdict v : Arcane::AllVerdicts())
            f << "verdict=" << Arcane::ToString(v) << "\n";
        f << "reportSchemaMin=" << Arcane::VerifyReport::kOldestSupportedSchemaVersion << "\n";
        f << "reportSchemaMax=" << Arcane::VerifyReport::kSchemaVersion << "\n";
        REQUIRE(f.good());
    }

    // Read it back. A write that silently failed would publish an EMPTY or
    // truncated vocabulary, and the gate would then report drift against a file
    // this test claimed to have written -- pointing the next reader at the
    // wrong half of the contract.
    std::vector<std::string> lines;
    {
        std::ifstream in(out, std::ios::binary);
        REQUIRE(in.is_open());
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(line);
        }
    }

    std::vector<std::string> expected;
    for (const Arcane::Verdict v : Arcane::AllVerdicts())
        expected.push_back(std::string("verdict=") + Arcane::ToString(v));
    expected.push_back("reportSchemaMin=" +
                       std::to_string(Arcane::VerifyReport::kOldestSupportedSchemaVersion));
    expected.push_back("reportSchemaMax=" +
                       std::to_string(Arcane::VerifyReport::kSchemaVersion));

    INFO("published at " << std::filesystem::absolute(out).string());
    CHECK(lines == expected);
}
