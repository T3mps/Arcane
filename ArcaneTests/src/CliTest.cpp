// Arcane::Cli -- typed declarative argument parser. PRESENTATION-FREE + C++23-clean.
#include <cstdint>
#include <vector>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Cli/Cli.hpp>
using Arcane::Cli;
using Arcane::CliType;
namespace {
    // Catch2 needs a mutable char** argv; build one from a literal list (argv[0] = prog).
    Cli::Result ParseArgs(const Cli& cli, std::vector<std::string> args) {
        std::vector<char*> argv; argv.push_back(const_cast<char*>("prog"));
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        return cli.Parse(static_cast<int>(argv.size()), argv.data());
    }
    Cli MakeCli() {
        Cli c{"prog", "desc"};
        c.Flag("verbose", "verbose logging").Short('v');
        c.Option("backend", "dx12", "graphics backend").Short('b').Choices({"dx12", "vulkan"});
        c.Option("frames", "0", "frame count").Type(CliType::Uint);
        c.Option("name", "world", "a name");
        return c;
    }
}
TEST_CASE("Cli: defaults when nothing passed", "[cli]") {
    const Cli c = MakeCli();
    const Cli::Result r = ParseArgs(c, {});
    REQUIRE(r.ok);
    REQUIRE_FALSE(r.Flag("verbose"));
    REQUIRE(r.Get("backend") == "dx12");
    REQUIRE(r.GetAs<std::uint64_t>("frames") == 0u);
    REQUIRE(r.Get("name") == "world");
}
TEST_CASE("Cli: --name value and --name=value both parse", "[cli]") {
    const Cli c = MakeCli();
    const Cli::Result r1 = ParseArgs(c, {"--name", "alice"});
    REQUIRE(r1.ok); REQUIRE(r1.Get("name") == "alice");
    const Cli::Result r2 = ParseArgs(c, {"--name=bob"});
    REQUIRE(r2.ok); REQUIRE(r2.Get("name") == "bob");
}
TEST_CASE("Cli: flag + short alias", "[cli]") {
    const Cli c = MakeCli();
    REQUIRE(ParseArgs(c, {"--verbose"}).Flag("verbose"));
    REQUIRE(ParseArgs(c, {"-v"}).Flag("verbose"));
}
TEST_CASE("Cli: typed conversion", "[cli]") {
    const Cli c = MakeCli();
    const Cli::Result r = ParseArgs(c, {"--frames", "180"});
    REQUIRE(r.ok); REQUIRE(r.GetAs<std::uint64_t>("frames") == 180u);
}
TEST_CASE("Cli: Choices rejects an invalid value", "[cli]") {
    const Cli c = MakeCli();
    const Cli::Result r = ParseArgs(c, {"--backend", "metal"});
    REQUIRE_FALSE(r.ok); REQUIRE(r.exitCode == 2);
}
TEST_CASE("Cli: bad number is a parse error", "[cli]") {
    const Cli c = MakeCli();
    const Cli::Result r = ParseArgs(c, {"--frames", "abc"});
    REQUIRE_FALSE(r.ok); REQUIRE(r.exitCode == 2);
}
TEST_CASE("Cli: unknown arg + missing value are parse errors", "[cli]") {
    const Cli c = MakeCli();
    REQUIRE(ParseArgs(c, {"--nope"}).exitCode == 2);
    REQUIRE(ParseArgs(c, {"--name"}).exitCode == 2);   // option with no value
}
TEST_CASE("Cli: --help requests help with exit 0", "[cli]") {
    const Cli c = MakeCli();
    const Cli::Result r = ParseArgs(c, {"--help"});
    REQUIRE_FALSE(r.ok); REQUIRE(r.helpRequested); REQUIRE(r.exitCode == 0);
}
TEST_CASE("Cli: Required missing is a parse error", "[cli]") {
    Cli c{"prog", "desc"};
    c.Option("out", "", "output path").Required();
    REQUIRE(ParseArgs(c, {}).exitCode == 2);
    REQUIRE(ParseArgs(c, {"--out", "x"}).ok);
}
TEST_CASE("Cli: trailing garbage on a number is rejected (full consume)", "[cli]") {
    const Cli c = MakeCli();
    const Cli::Result r = ParseArgs(c, {"--frames", "180abc"});
    REQUIRE_FALSE(r.ok); REQUIRE(r.exitCode == 2);
}
TEST_CASE("Cli: short-alias =value resolves an option", "[cli]") {
    const Cli c = MakeCli();
    const Cli::Result r = ParseArgs(c, {"-b=vulkan"});
    REQUIRE(r.ok); REQUIRE(r.Get("backend") == "vulkan");
}
TEST_CASE("Cli: multiple options + a flag in one parse", "[cli]") {
    const Cli c = MakeCli();
    const Cli::Result r = ParseArgs(c, {"--backend", "vulkan", "--frames", "180", "--verbose"});
    REQUIRE(r.ok);
    REQUIRE(r.Get("backend") == "vulkan");
    REQUIRE(r.GetAs<std::uint64_t>("frames") == 180u);
    REQUIRE(r.Flag("verbose"));
}
TEST_CASE("Cli: a flag given an inline value is a parse error", "[cli]") {
    const Cli c = MakeCli();
    REQUIRE(ParseArgs(c, {"--verbose=1"}).exitCode == 2);
}
TEST_CASE("cli: a Many() option accumulates every occurrence in order", "[cli]")
{
    Arcane::Cli cli{ "t", "d" };
    cli.Option("probe", "", "repeatable probe").Many();

    const char* argv[] = { "t.exe", "--probe", "luma@1,2", "--probe", "census" };
    const Arcane::Cli::Result r = cli.Parse(5, const_cast<char**>(argv));

    REQUIRE(r.ok);
    const std::vector<std::string> probes = r.GetMany("probe");
    REQUIRE(probes.size() == 2);
    CHECK(probes[0] == "luma@1,2");
    CHECK(probes[1] == "census");
}

TEST_CASE("cli: GetMany on an absent option is empty, not defaulted", "[cli]")
{
    Arcane::Cli cli{ "t", "d" };
    cli.Option("probe", "", "repeatable probe").Many();
    const char* argv[] = { "t.exe" };
    const Arcane::Cli::Result r = cli.Parse(1, const_cast<char**>(argv));
    REQUIRE(r.ok);
    CHECK(r.GetMany("probe").empty());
}

TEST_CASE("cli: a non-Many option repeated is still last-wins", "[cli]")
{
    Arcane::Cli cli{ "t", "d" };
    cli.Option("backend", "dx12", "backend");
    const char* argv[] = { "t.exe", "--backend", "vulkan", "--backend", "dx12" };
    const Arcane::Cli::Result r = cli.Parse(5, const_cast<char**>(argv));
    REQUIRE(r.ok);
    CHECK(r.Get("backend") == "dx12");
}

// Supplied() exists because Get(name) != default cannot tell "explicitly given,
// value happens to equal the default" apart from "never given". All three
// states below must be distinguished; the middle one is the case a
// string-compare-against-the-default gets wrong.
TEST_CASE("cli: Supplied distinguishes explicit-non-default, explicit-equal-to-default, and absent", "[cli]")
{
    Arcane::Cli cli{ "t", "d" };
    cli.Option("fixed-dt", "0.0166666666666666666", "seconds per frame").Type(CliType::Double);

    const char* nonDefault[] = { "t.exe", "--fixed-dt", "0.05" };
    const Arcane::Cli::Result r1 = cli.Parse(3, const_cast<char**>(nonDefault));
    REQUIRE(r1.ok);
    CHECK(r1.Supplied("fixed-dt"));
    CHECK(r1.Get("fixed-dt") == "0.05");

    const char* equalToDefault[] = { "t.exe", "--fixed-dt", "0.0166666666666666666" };
    const Arcane::Cli::Result r2 = cli.Parse(3, const_cast<char**>(equalToDefault));
    REQUIRE(r2.ok);
    CHECK(r2.Supplied("fixed-dt"));
    CHECK(r2.Get("fixed-dt") == "0.0166666666666666666");

    const char* absent[] = { "t.exe" };
    const Arcane::Cli::Result r3 = cli.Parse(1, const_cast<char**>(absent));
    REQUIRE(r3.ok);
    CHECK_FALSE(r3.Supplied("fixed-dt"));
    CHECK(r3.Get("fixed-dt") == "0.0166666666666666666");
}
