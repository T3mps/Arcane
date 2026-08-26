// CONFORMANCE against Playwright's own fixture corpus.
//
// Our comparator is a reimplementation with identical constants and identical
// double arithmetic, so it must reach the SAME verdict they do on every pair:
// should-match/ passes at a zero budget, should-fail/ does not.
//
// A failure here is a PORT DIVERGENCE, not a threshold that needs tuning. The
// two SSIM traps exist precisely because SSIM can false-pass; if
// original-ssim-trap or julia-ssim-trap starts passing, stage 3 or stage 4 is
// wrong, and loosening a constant would hide it.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ImageCompare.hpp>
#include <Arcane/Assets/ImageIo.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    fs::path FixtureRoot()
    {
        // Staged beside the exe by the postbuild COPYDIR. Tests run FROM the
        // exe directory, so a relative path is correct here.
        return fs::path("playwright-fixtures");
    }

    // Every case is <dir>/<name>-expected.png + <dir>/<name>-actual.png.
    struct Case { fs::path expected, actual; std::string label; };

    std::vector<Case> CasesUnder(const fs::path& root)
    {
        std::vector<Case> cases;
        if (!fs::exists(root)) return cases;
        for (const auto& entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            const std::string suffix = "-expected.png";
            if (name.size() <= suffix.size()) continue;
            if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;

            const std::string stem = name.substr(0, name.size() - suffix.size());
            fs::path actual = entry.path().parent_path() / (stem + "-actual.png");
            if (!fs::exists(actual)) continue;

            // Label includes the ROOT (should-match/should-fail), not just the
            // immediate parent dir + stem: two different roots can share a
            // subdirectory name (both have a "looks-same-tests"), and while no
            // stem collides across them today, the label exists purely for
            // diagnosability -- disambiguating costs one expression, so do it
            // rather than leave a future upstream addition ambiguous in INFO.
            cases.push_back({ entry.path(), std::move(actual),
                              root.filename().string() + "/" +
                              entry.path().parent_path().filename().string() + "/" + stem });
        }
        return cases;
    }

    bool Load(const fs::path& p, Arcane::PixelData& out)
    {
        return Arcane::LoadPngRgba(p, out.width, out.height, out.rgba);
    }
}

TEST_CASE("compare: the vendored Playwright fixture corpus is present", "[compare][conformance]")
{
    // A silently-missing corpus would turn every assertion below into a
    // vacuous pass -- the same "instrument blind spot" class this repo has
    // already paid for. Assert the corpus exists before asserting on it.
    //
    // EXACT counts, not a floor: upstream's fixture tree has exactly 12
    // should-match pairs and 9 should-fail pairs (verified against
    // microsoft/playwright's own tree, not just the brief that named this
    // task). A ">=" floor would let up to 2 cases silently fail to download
    // on either side and still go green -- and the two irreplaceable SSIM
    // traps are exactly 2 of the 9 should-fail cases, so that floor could
    // pass with BOTH of them missing. The discovery walker above also
    // silently skips any pair whose "-actual.png" didn't land, so a
    // half-downloaded pair vanishes instead of erroring; the exact count is
    // what turns that into a loud failure instead.
    const auto match = CasesUnder(FixtureRoot() / "should-match");
    const auto fail  = CasesUnder(FixtureRoot() / "should-fail");

    INFO("fixtures resolved from " << fs::absolute(FixtureRoot()).string());
    CHECK(match.size() == 12);
    CHECK(fail.size()  == 9);

    // A count alone cannot say WHICH cases went missing. These two are the
    // ones this corpus exists for -- "SSIM false-passing is precisely the
    // failure mode we cannot invent our way to" -- so assert their presence
    // by name, not just by tally.
    std::vector<std::string> failLabels;
    failLabels.reserve(fail.size());
    for (const auto& c : fail) failLabels.push_back(c.label);

    const bool hasJuliaTrap = std::find(failLabels.begin(), failLabels.end(),
                                        "should-fail/julia-ssim-trap/1") != failLabels.end();
    const bool hasOriginalTrap = std::find(failLabels.begin(), failLabels.end(),
                                           "should-fail/original-ssim-trap/sample") != failLabels.end();
    CHECK(hasJuliaTrap);
    CHECK(hasOriginalTrap);
}

TEST_CASE("compare: every should-match fixture passes at a ZERO budget", "[compare][conformance]")
{
    const auto cases = CasesUnder(FixtureRoot() / "should-match");

    // SELF-DEFENSE, deliberately duplicating the corpus-presence test above:
    // Catch2 runs every TEST_CASE independently, so a wrong working directory
    // (or any other reason CasesUnder comes back empty) makes THIS loop
    // iterate zero times and report PASSED with zero assertions -- the sibling
    // presence test failing does not stop this one from going green. A
    // negative-control run (invoking the exe from the wrong directory)
    // demonstrated exactly that before this REQUIRE existed. Do not "tidy
    // this away" as redundant with the presence test; each TEST_CASE must be
    // self-defending in isolation, e.g. under a per-test CI view or a
    // name-filtered re-run that never executes the presence test at all.
    REQUIRE(cases.size() == 12);

    for (const auto& c : cases)
    {
        Arcane::PixelData expected, actual;
        INFO("case " << c.label);
        REQUIRE(Load(c.expected, expected));
        REQUIRE(Load(c.actual, actual));

        const auto res = Arcane::CompareImages(expected, actual);   // default: budget 0
        INFO("diffCount " << res.diffCount << " -- " << res.errorMessage);
        CHECK(res.passed);
    }
}

TEST_CASE("compare: every should-fail fixture is caught, SSIM traps included", "[compare][conformance]")
{
    const auto cases = CasesUnder(FixtureRoot() / "should-fail");

    // SELF-DEFENSE, deliberately duplicating the corpus-presence test above --
    // see the matching comment in the should-match test case for why this is
    // NOT redundant to remove. This is the loop that carries the two SSIM
    // traps; an empty corpus here would silently drop the exact failure mode
    // ("SSIM false-passing") this task exists to catch, while still reporting
    // PASSED.
    REQUIRE(cases.size() == 9);

    for (const auto& c : cases)
    {
        Arcane::PixelData expected, actual;
        INFO("case " << c.label);
        REQUIRE(Load(c.expected, expected));
        REQUIRE(Load(c.actual, actual));

        const auto res = Arcane::CompareImages(expected, actual);
        INFO("diffCount " << res.diffCount);
        CHECK_FALSE(res.passed);
        CHECK(res.diffCount > 0);
        CHECK_FALSE(res.diffRgba.empty());
    }
}

TEST_CASE("compare: the engine trap corpus is classified correctly", "[compare][conformance]")
{
    // Engine-shaped traps, additive on top of Playwright's. These are the
    // regressions this gate actually exists to catch -- a missing mesh, a
    // wrong normal matrix, an unbound post chain -- plus one pair that must
    // NOT be caught (cross-backend text), because a gate that flags
    // legitimate glyph rounding gets loosened until it stops catching
    // anything. Provenance for every pair: ReferenceProject/Verify/Traps/README.md.
    const std::filesystem::path traps("ReferenceProject/Verify/Traps");

    // SELF-DEFENSE, deliberately duplicating the shape of the two Playwright
    // presence/exactness checks above (see their comments for why this is
    // NOT redundant to remove): each TEST_CASE here must be self-defending in
    // isolation. Without this REQUIRE, a missing or unstaged traps/ directory
    // (e.g. the exe run from the wrong working directory, or a build that
    // predates Task 11's premake widening) makes CasesUnder come back empty
    // and BOTH loops below iterate zero times -- reporting PASSED with zero
    // assertions rather than failing loudly. Task 6's own fix round added
    // exactly this guard to exactly this shape of test after a negative
    // control proved the unguarded version passes vacuously; this trap corpus
    // gets the same guard from day one rather than waiting for its own
    // negative control to rediscover the hole.
    const auto shouldFail  = CasesUnder(traps / "should-fail");
    const auto shouldMatch = CasesUnder(traps / "should-match");
    REQUIRE(shouldFail.size()  == 3);
    REQUIRE(shouldMatch.size() == 1);

    for (const auto& c : shouldFail)
    {
        Arcane::PixelData expected, actual;
        INFO("trap " << c.label);
        REQUIRE(Load(c.expected, expected));
        REQUIRE(Load(c.actual, actual));

        const auto res = Arcane::CompareImages(expected, actual);
        INFO("diffCount " << res.diffCount);
        CHECK_FALSE(res.passed);
    }

    for (const auto& c : shouldMatch)
    {
        Arcane::PixelData expected, actual;
        INFO("trap " << c.label);
        REQUIRE(Load(c.expected, expected));
        REQUIRE(Load(c.actual, actual));

        // Cross-backend text needs a budget: 121 pixels measured at the desk,
        // rounded up. This is the ONE place an aggregate budget is
        // legitimate, and it is derived from a measurement, not chosen to
        // make a test pass -- see README.md's should-match section. Do NOT
        // raise this if a should-fail trap ever needs it: that would be a
        // finding about the trap or the comparator, not a threshold to tune.
        Arcane::ImageCompareOptions opt;
        opt.maxDiffPixels = 200;
        const auto res = Arcane::CompareImages(expected, actual, opt);
        INFO("diffCount " << res.diffCount);
        CHECK(res.passed);
    }
}
