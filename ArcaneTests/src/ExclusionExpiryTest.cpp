// THE MECHANISM. An exclusion with a date but no check is a permanent
// exclusion wearing a date, which is exactly what
// feedback_time_boxed_stances_need_checked_expiry warns about. This case runs
// in every suite on every runner -- including GitHub Actions, which is why it
// lives here and not only in the gate script -- and turns a stale entry into a
// test failure.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/ExclusionList.hpp>

#include <fstream>
#include <sstream>
#include <string>

TEST_CASE("automation exclusions: the file parses and none has expired", "[exclusions][meta]")
{
    // Staged beside the exe by the ArcaneTests postbuild step. An ABSENT file
    // legitimately means "no exclusions" and is not a failure; a PRESENT but
    // malformed one is.
    std::ifstream in("automation-exclusions.json", std::ios::binary);
    if (!in)
    {
        SUCCEED("no automation-exclusions.json staged -- no exclusions declared");
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    std::string error;
    const auto list = Arcane::ParseExclusions(ss.str(), error);
    INFO("parse error: " << error);
    REQUIRE(list.has_value());

    const std::string today = Arcane::TodayIso();
    for (const Arcane::ExclusionEntry& e : *list)
    {
        INFO("exclusion target: " << e.target
             << "\n  reason:  " << e.reason
             << "\n  expires: " << e.expires
             << "\n  today:   " << today
             << "\n\nThis exclusion is PAST ITS DATE. Either the underlying problem is"
                "\nfixed -- delete the entry -- or it is not, and the entry needs a new"
                "\ndate and a fresh justification. A stale exclusion is the failure here,"
                "\nnot the thing it excludes.");
        CHECK_FALSE(Arcane::IsExpired(e, today));
    }
}
