// Settings arc S7 (spec s9, s3.2): the transport-agnostic admin surface.
// ServerAdmin context; Protected is never readable through it; every set is
// handed to the injected audit sink with who, old, new and the outcome.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/RemoteCVarService.hpp>

#include <string>
#include <vector>

using namespace Arcane;

namespace
{
    struct AuditLog
    {
        std::vector<CVarAuditRecord> records;
        static void Sink(const CVarAuditRecord& r, void* user) { static_cast<AuditLog*>(user)->records.push_back(r); }
    };

    void Knob(CVarRegistry& reg, std::string_view name, CVarValue def, Audience audience, CVarFlags flags = CVarFlags::None)
    {
        CVarDesc d;
        d.name = name;
        d.type = def.type;
        d.defaultValue = def;
        d.flags = flags;
        d.help = "S7 remote knob.";
        d.module = "test-s7-remote";
        d.audience = audience;
        const CVarHandle h = reg.Register(d);
        INFO(reg.LastError());
        REQUIRE_FALSE(h.IsStale());
    }

    RemoteCVarRequest Req(std::string op, std::string name, std::string value = {})
    {
        return RemoteCVarRequest{ std::move(op), std::move(name), std::move(value), "admin-1" };
    }

    struct Fixture
    {
        CVarRegistry reg;
        AuditLog audit;
        RemoteCVarService service{ reg, &AuditLog::Sink, &audit };
        Fixture()
        {
            Knob(reg, "test.server.tick", CVarValue::Int32(60), Audience::Server);
            Knob(reg, "test.server.password", CVarValue::String("hunter2"), Audience::Server, CVarFlags::Protected);
            Knob(reg, "test.server.secretKnob", CVarValue::Bool(false), Audience::Server, CVarFlags::Hidden);
            Knob(reg, "test.serverx.other", CVarValue::Bool(true), Audience::Server);
            Knob(reg, "test.game.speed", CVarValue::Float32(1.0f), Audience::Game);
        }
    };
}

TEST_CASE("RemoteCVarService: get and set a Server setting as ServerAdmin; the set publishes and is audited", "[remote-cvar]")
{
    Fixture f;
    const RemoteCVarResponse got = f.service.Handle(Req("get", "test.server.tick"));
    CHECK(got.ok);
    CHECK(got.text == "test.server.tick = 60");

    const RemoteCVarResponse set = f.service.Handle(Req("set", "test.server.tick", "30"));
    CHECK(set.ok);
    CHECK(set.text == "test.server.tick = 30");
    CHECK(f.reg.Get(f.reg.Find("test.server.tick"))->AsInt32() == 30);   // published, not pending

    REQUIRE(f.audit.records.size() == 1);
    const CVarAuditRecord& r = f.audit.records.front();
    CHECK(r.name == "test.server.tick");
    CHECK(r.callerId == "admin-1");
    CHECK(r.oldValue == "60");
    CHECK(r.newValue == "30");
    CHECK(r.context == CVarContext::ServerAdmin);
    CHECK(r.verdict == PolicyVerdict::Allow);
    CHECK(r.unixMs > 0);
}

TEST_CASE("RemoteCVarService: a refused set changes nothing and is still audited as denied", "[remote-cvar]")
{
    Fixture f;
    const RemoteCVarResponse set = f.service.Handle(Req("set", "test.game.speed", "4"));   // Game, not Cheat
    CHECK_FALSE(set.ok);
    CHECK(f.reg.Get(f.reg.Find("test.game.speed"))->AsFloat32() == 1.0f);
    REQUIRE(f.audit.records.size() == 1);
    CHECK(f.audit.records.front().verdict == PolicyVerdict::Deny);
    CHECK(f.audit.records.front().newValue == "4");   // the attempted value
}

TEST_CASE("RemoteCVarService: Protected is never readable remotely -- get, explain and list hide it; a set is allowed and audited masked", "[remote-cvar]")
{
    Fixture f;
    const RemoteCVarResponse got = f.service.Handle(Req("get", "test.server.password"));
    CHECK_FALSE(got.ok);
    CHECK(got.text.find("protected") != std::string::npos);
    CHECK(got.text.find("hunter2") == std::string::npos);
    CHECK_FALSE(f.service.Handle(Req("explain", "test.server.password")).ok);

    const RemoteCVarResponse set = f.service.Handle(Req("set", "test.server.password", "correcthorse"));
    CHECK(set.ok);
    CHECK(set.text.find("correcthorse") == std::string::npos);
    CHECK(f.reg.Get(f.reg.Find("test.server.password"))->AsString() == "correcthorse");
    REQUIRE(f.audit.records.size() == 1);
    CHECK(f.audit.records.front().oldValue == "<protected>");
    CHECK(f.audit.records.front().newValue == "<protected>");

    const RemoteCVarResponse list = f.service.Handle(Req("list", "test.server"));
    CHECK(list.text.find("test.server.password = <protected>") != std::string::npos);
    CHECK(list.text.find("correcthorse") == std::string::npos);
}

TEST_CASE("RemoteCVarService: Hidden and unknown names answer unknown and are not audited", "[remote-cvar]")
{
    Fixture f;
    CHECK(f.service.Handle(Req("get", "test.server.secretKnob")).text == "unknown cvar 'test.server.secretKnob'");
    CHECK_FALSE(f.service.Handle(Req("set", "test.server.secretKnob", "true")).ok);
    CHECK_FALSE(f.service.Handle(Req("set", "no.such.cvar", "1")).ok);
    CHECK(f.audit.records.empty());
}

TEST_CASE("RemoteCVarService: list honours the dotted prefix boundary and is sorted; explain shows the history", "[remote-cvar]")
{
    Fixture f;
    const RemoteCVarResponse list = f.service.Handle(Req("list", "test.server"));
    CHECK(list.ok);
    CHECK(list.text.find("test.server.tick = 60") != std::string::npos);
    CHECK(list.text.find("test.serverx.other") == std::string::npos);
    CHECK(list.text.find("test.server.secretKnob") == std::string::npos);
    CHECK(list.text.find("test.server.password") < list.text.find("test.server.tick"));

    REQUIRE(f.service.Handle(Req("set", "test.server.tick", "45")).ok);
    const RemoteCVarResponse ex = f.service.Handle(Req("explain", "test.server.tick"));
    CHECK(ex.ok);
    CHECK(ex.text.find("Console") != std::string::npos);
    CHECK(f.service.Handle(Req("list", "nothing.here")).text == "(no cvars match 'nothing.here')");
}

TEST_CASE("RemoteCVarService: a command runs remotely only with ServerCanExecute; an unknown op is refused", "[remote-cvar]")
{
    Fixture f;
    REQUIRE(f.reg.RegisterCommand("test.flush", CVarFlags::ServerCanExecute, "Flush.", "test-s7-remote",
        [](std::string_view, void*) -> CommandResult { return { true, "flushed" }; }, nullptr));
    REQUIRE(f.reg.RegisterCommand("test.local", CVarFlags::None, "Local only.", "test-s7-remote",
        [](std::string_view, void*) -> CommandResult { return { true, "ran" }; }, nullptr));

    const RemoteCVarResponse ok = f.service.Handle(Req("set", "test.flush"));
    CHECK(ok.ok);
    CHECK(ok.text == "flushed");
    const RemoteCVarResponse refused = f.service.Handle(Req("get", "test.local"));
    CHECK_FALSE(refused.ok);
    CHECK(refused.text.find("ServerCanExecute") != std::string::npos);
    CHECK(f.audit.records.size() == 1);   // the command run; the refusal named no value

    CHECK_FALSE(f.service.Handle(Req("drop", "test.server.tick")).ok);
    CHECK_FALSE(f.service.Handle(Req("get", "")).ok);
}
