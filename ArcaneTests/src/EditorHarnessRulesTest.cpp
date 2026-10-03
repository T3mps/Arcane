// UnderVerifyHarness (node-page phase s8.2): machine-dependent UI (badges, chip)
// is suppressed under the verify/screenshot harness, a superset of CaptureWanted.
#include <catch2/catch_test_macros.hpp>
#include <App/HarnessRules.hpp>

TEST_CASE("UnderVerifyHarness holds for --headless, --screenshot or --report, and not for a plain windowed run", "[editor]")
{
    Arcane::HostConfig c;
    CHECK_FALSE(Arcane::Editor::UnderVerifyHarness(c));
    c.headless = true;
    CHECK(Arcane::Editor::UnderVerifyHarness(c));
    c = {};
    c.screenshotPath = "s.png";
    CHECK(Arcane::Editor::UnderVerifyHarness(c));
    c = {};
    c.reportPath = "r.json";
    CHECK(Arcane::Editor::UnderVerifyHarness(c));
}
