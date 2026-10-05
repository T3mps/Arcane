// Settings arc S7 (spec s9): ArcaneServer's local admin console. The grammar,
// the replies and the dedicated-host defaults, driven directly: this file is
// source-compiled with ServerAdminConsole.cpp (premake5.lua), the stdin reader
// is covered by the S4 witness.
#include <catch2/catch_test_macros.hpp>

#include "ServerAdminConsole.hpp"

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/RemoteCVarService.hpp>

using namespace Arcane;
using namespace Arcane::Server;

namespace
{
    void Knob(CVarRegistry& reg, std::string_view name, CVarValue def, Audience audience)
    {
        if (!reg.Find(name).IsStale()) return;   // S1 may register the engine knobs on every registry
        CVarDesc d;
        d.name = name;
        d.type = def.type;
        d.defaultValue = def;
        d.help = "S7 console knob.";
        d.module = "test-s7-console";
        d.audience = audience;
        REQUIRE_FALSE(reg.Register(d).IsStale());
    }
}

TEST_CASE("ParseAdminLine: verbs, the bare-name shorthands, comments and blanks", "[remote-cvar][server]")
{
    auto r = ParseAdminLine("  get server.tickHz ", "stdin");
    REQUIRE(r);
    CHECK(r->op == "get"); CHECK(r->name == "server.tickHz"); CHECK(r->callerId == "stdin");

    r = ParseAdminLine("set match.name Grand Final", "stdin");
    REQUIRE(r);
    CHECK(r->op == "set"); CHECK(r->name == "match.name"); CHECK(r->value == "Grand Final");

    r = ParseAdminLine("list server.", "stdin");
    REQUIRE(r);
    CHECK(r->op == "list"); CHECK(r->name == "server.");

    r = ParseAdminLine("list", "stdin");
    REQUIRE(r);
    CHECK(r->op == "list"); CHECK(r->name.empty());

    r = ParseAdminLine("explain server.cheats", "stdin");
    REQUIRE(r);
    CHECK(r->op == "explain");

    r = ParseAdminLine("server.cheats", "stdin");
    REQUIRE(r);
    CHECK(r->op == "get");

    r = ParseAdminLine("server.cheats 1", "stdin");
    REQUIRE(r);
    CHECK(r->op == "set"); CHECK(r->value == "1");

    CHECK_FALSE(ParseAdminLine("   ", "stdin"));
    CHECK_FALSE(ParseAdminLine("# a comment", "stdin"));
}

TEST_CASE("AdminConsole: OK/ERR replies over RemoteCVarService, help, and silence for blank lines", "[remote-cvar][server]")
{
    CVarRegistry reg;
    Knob(reg, "test.console.tick", CVarValue::Int32(60), Audience::Server);
    RemoteCVarService service(reg);
    AdminConsole console(service, "stdin");

    CHECK(console.Submit("test.console.tick") == "OK test.console.tick = 60");
    CHECK(console.Submit("set test.console.tick 30") == "OK test.console.tick = 30");
    CHECK(console.Submit("bogus.cvar.name") == "ERR unknown cvar 'bogus.cvar.name'");
    CHECK(console.Submit("").empty());
    CHECK(console.Submit("help").starts_with("OK get <name>"));
    const std::string list = console.Submit("list test.console");
    CHECK(list.starts_with("OK"));
    CHECK(list.find("test.console.tick = 30") != std::string::npos);
}

TEST_CASE("ApplyDedicatedServerDefaults: a dedicated host turns server.cheatsAllowed off unless the project or operator decided", "[remote-cvar][server]")
{
    CVarRegistry reg;
    Knob(reg, "server.cheatsAllowed", CVarValue::Bool(true), Audience::Server);
    REQUIRE(ApplyDedicatedServerDefaults(reg));
    reg.PublishImmediate();
    CHECK(reg.Get(reg.Find("server.cheatsAllowed"))->AsBool() == false);
    CHECK(reg.Explain("server.cheatsAllowed")->setBy == SetBy::Project);

    CVarRegistry decided;
    Knob(decided, "server.cheatsAllowed", CVarValue::Bool(true), Audience::Server);
    REQUIRE(decided.Set(decided.Find("server.cheatsAllowed"), CVarValue::Bool(true), SetBy::Project, "project",
                        CVarContext::Editor) == SetResult::Applied);
    CHECK_FALSE(ApplyDedicatedServerDefaults(decided));
    decided.PublishImmediate();
    CHECK(decided.Get(decided.Find("server.cheatsAllowed"))->AsBool() == true);
}
