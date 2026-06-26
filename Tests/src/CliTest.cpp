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
        c.Option("backend", "dx12", "graphics backend").Choices({"dx12", "vulkan"});
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
