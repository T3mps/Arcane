// ArcaneServer's CLI (spec 2026-09-15 s6: "mirrors the other hosts"). Core-side --
// HostConfig is Client (it names a GraphicsBackend), so the server owns its own
// vocabulary over the same Arcane::Cli. Source-compiled into the tests like
// arcbuild's Request.cpp.
#include <catch2/catch_test_macros.hpp>
#include "ServerConfig.hpp"
#include <vector>
namespace
{
    Arcane::Server::ServerConfig::ParseOutcome Parse(std::vector<const char*> args)
    { args.insert(args.begin(), "ArcaneServer"); return Arcane::Server::ServerConfig::Parse(static_cast<int>(args.size()), const_cast<char**>(args.data())); }
}
TEST_CASE("ServerConfig: --project is required; the defaults are the documented ones", "[server]")
{
    CHECK_FALSE(Parse({}).config.has_value());
    CHECK(Parse({}).exitCode == 2);
    auto ok = Parse({"--project", "ReferenceProject"});
    REQUIRE(ok.config);
    CHECK(ok.config->projectPath == "ReferenceProject");
    CHECK(ok.config->frames == 0);
    CHECK(ok.config->fixedDtSeconds == 1.0 / 60.0);
    CHECK(ok.config->reportPath.empty());
}
TEST_CASE("ServerConfig: --frames, --fixed-dt, --report, --plugin parse; a non-positive --fixed-dt is refused", "[server]")
{
    auto ok = Parse({"--project", "P", "--frames", "30", "--fixed-dt", "0.02", "--report", "r.json", "--plugin", "X.dll"});
    REQUIRE(ok.config);
    CHECK(ok.config->frames == 30); CHECK(ok.config->fixedDtSeconds == 0.02);
    CHECK(ok.config->reportPath == "r.json"); CHECK(ok.config->pluginPath == "X.dll");
    CHECK_FALSE(Parse({"--project", "P", "--fixed-dt", "0"}).config.has_value());
    CHECK_FALSE(Parse({"--project", "P", "--fixed-dt", "-1"}).config.has_value());
}
TEST_CASE("ServerConfig: --print-engine-info needs no --project; --help exits 0", "[server]")
{
    CHECK(Parse({"--print-engine-info"}).config->printEngineInfo);
    auto h = Parse({"--help"}); CHECK_FALSE(h.config); CHECK(h.exitCode == 0);
}
