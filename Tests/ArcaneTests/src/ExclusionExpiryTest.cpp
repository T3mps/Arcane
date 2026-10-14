// THE MECHANISM. An exclusion with a date but no check is a permanent
// exclusion wearing a date, which is exactly what
// feedback_time_boxed_stances_need_checked_expiry warns about. This case runs
// in every suite on every runner -- including GitHub Actions, which is why it
// lives here and not only in the gate script -- and turns a stale entry into a
// test failure.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/ExclusionList.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    constexpr const char* kExclusionsPath = "automation-exclusions.json";
}

TEST_CASE("automation exclusions: the file parses and none has expired", "[exclusions][meta]")
{
    // Staged beside the exe by the ArcaneTests postbuild step (COPYFILE from
    // scripts/automation-exclusions.json). Tests run FROM the exe directory
    // (the plan's Global Constraints: "fixtures, shaders and data/ are staged
    // relative to it"), and this postbuild copy is unconditional, so the file
    // is ALWAYS supposed to be here. Its absence therefore does NOT mean "no
    // exclusions declared" -- it means STAGING IS BROKEN (wrong working
    // directory, the postbuild step removed, a future premake change to
    // cfg.buildtarget.directory) and must be loud: silently treating "not
    // found" as "nothing excluded" would let this guard quietly stop running
    // forever, which is precisely the failure mode Task 6 exists to close.
    std::ifstream in(kExclusionsPath, std::ios::binary);
    if (!in)
    {
        FAIL("automation-exclusions.json was not found at \""
             << std::filesystem::absolute(kExclusionsPath).string()
             << "\". The ArcaneTests postbuild step stages this file next to "
                "the exe unconditionally, so its absence means the staging "
                "is BROKEN -- not that no exclusions are declared. Check the "
                "working directory this binary was launched from (tests run "
                "FROM the exe directory) and the ArcaneTests "
                "postbuildcommands COPYFILE in premake5.lua.");
    }
    // Binary mode + whole-buffer slurp: fine today (this repo's file is
    // always "[]" or LF-only JSON), and nlohmann's parser (behind
    // ParseExclusions) treats CRLFs as insignificant whitespace, so a real
    // entry authored on Windows would still parse correctly -- no need to
    // text-mode this read.
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
