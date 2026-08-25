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

            cases.push_back({ entry.path(), std::move(actual),
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
                                        "julia-ssim-trap/1") != failLabels.end();
    const bool hasOriginalTrap = std::find(failLabels.begin(), failLabels.end(),
                                           "original-ssim-trap/sample") != failLabels.end();
    CHECK(hasJuliaTrap);
    CHECK(hasOriginalTrap);
}

TEST_CASE("compare: every should-match fixture passes at a ZERO budget", "[compare][conformance]")
{
    for (const auto& c : CasesUnder(FixtureRoot() / "should-match"))
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
    for (const auto& c : CasesUnder(FixtureRoot() / "should-fail"))
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
