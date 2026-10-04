// The cvar access model (settings spec s3.2): CVarContext, the audience x
// context table, the game's CVarPolicy and the audit sink. CPU-only ([cvar]).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/ConsoleModel.hpp>

#include "Helpers/CVarTestDesc.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Arcane;

TEST_CASE("CVarContext replaces Permission: the editor writes a Game setting, a host or client console does not", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle speed = reg.Register(Test::Desc("game.speed", CVarValue::Int32(1)));
    REQUIRE_FALSE(speed.IsStale());
    CHECK(reg.Set(speed, CVarValue::Int32(3), SetBy::Console, {}, CVarContext::LocalHost) == SetResult::Denied);
    CHECK(reg.Set(speed, CVarValue::Int32(3), SetBy::Console, {}, CVarContext::Client) == SetResult::Denied);
    CHECK(reg.Set(speed, CVarValue::Int32(2), SetBy::Console, {}, CVarContext::Editor) == SetResult::Applied);
    CHECK_FALSE(reg.Execute("game.speed 4", CVarContext::Client).ok);
    CHECK(reg.Execute("game.speed 5", CVarContext::Editor).ok);

    ConsoleModel model;
    model.SetInput("game.speed 6");
    model.Submit(reg, CVarContext::LocalHost);
    CHECK_FALSE(model.Lines().back().ok);

    ApplyCVarCommandLine(reg, { "game.speed=7" }, CVarContext::LocalHost);   // warns, sets nothing
    reg.Publish();
    CHECK(reg.Get(speed)->AsInt32() == 5);
    CHECK(reg.Explain("game.speed")->setBy == SetBy::Console);
}

namespace
{
    // The spec s3.2 table, transcribed independently of the implementation:
    // may `ctx` WRITE a non-Hidden, non-Protected setting of `audience`?
    bool TableWrite(Audience audience, bool cheat, CVarContext ctx, bool cheatsOn, bool clientMaySetServer)
    {
        if (ctx == CVarContext::Editor) return true;
        switch (audience)
        {
        case Audience::Editor:     return false;
        case Audience::Game:       return cheat && cheatsOn;
        case Audience::PlayerSafe: return !cheat || cheatsOn;
        case Audience::Server:     return (ctx != CVarContext::Client || clientMaySetServer) && (!cheat || cheatsOn);
        }
        return false;
    }
    const char* AudienceName(Audience a)
    {
        switch (a)
        {
        case Audience::Editor: return "editor";
        case Audience::Game: return "game";
        case Audience::PlayerSafe: return "playerSafe";
        case Audience::Server: return "server";
        }
        return "?";
    }
    const char* ContextName(CVarContext c)
    {
        switch (c)
        {
        case CVarContext::Editor: return "Editor";
        case CVarContext::LocalHost: return "LocalHost";
        case CVarContext::ServerAdmin: return "ServerAdmin";
        case CVarContext::Client: return "Client";
        }
        return "?";
    }
}

TEST_CASE("cvar access: every audience x context x cheats x Cheat flag x client knob matches the spec s3.2 table", "[cvar]")
{
    constexpr Audience kAudiences[] = { Audience::Editor, Audience::Game, Audience::PlayerSafe, Audience::Server };
    constexpr CVarContext kContexts[] = { CVarContext::Editor, CVarContext::LocalHost, CVarContext::ServerAdmin, CVarContext::Client };
    for (const bool cheatsOn : { false, true })
    for (const bool clientKnob : { false, true })
    {
        CVarRegistry reg;
        REQUIRE(reg.Set(reg.Find("server.cheats"), CVarValue::Bool(cheatsOn), SetBy::Code, "matrix") == SetResult::Applied);
        REQUIRE(reg.Set(reg.Find("server.allowClientSetServer"), CVarValue::Bool(clientKnob), SetBy::Code, "matrix") == SetResult::Applied);
        reg.Publish();
        for (const Audience audience : kAudiences)
        for (const bool cheat : { false, true })
        {
            const std::string name = std::string("matrix.") + AudienceName(audience) + (cheat ? ".cheat" : ".plain");
            const CVarHandle h = reg.Register(Test::Desc(name, CVarValue::Int32(0), audience, cheat ? CVarFlags::Cheat : CVarFlags::None));
            REQUIRE_FALSE(h.IsStale());
            for (const CVarContext ctx : kContexts)
            {
                INFO(name << " ctx=" << ContextName(ctx) << " cheatsOn=" << cheatsOn << " clientKnob=" << clientKnob);
                const bool expectWrite = TableWrite(audience, cheat, ctx, cheatsOn, clientKnob);
                CHECK(reg.Set(h, CVarValue::Int32(1), SetBy::Console, "matrix", ctx)
                      == (expectWrite ? SetResult::Applied : SetResult::Denied));
                const bool expectRead = ctx == CVarContext::Editor || audience != Audience::Editor;
                const ExecResult read = reg.Execute(name, ctx);
                CHECK(read.ok == expectRead);
                if (!expectRead) CHECK(read.text.find("unknown") != std::string::npos);   // absent, not denied
            }
        }
    }
}

TEST_CASE("server.cheats: 'cheats' is its alias, cheatsAllowed gates turning it on outside the editor, a client only reads it", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle gate = reg.Find("server.cheats");
    REQUIRE_FALSE(gate.IsStale());
    CHECK_FALSE(reg.CheatsEnabled());
    CHECK(reg.Execute("cheats 1", CVarContext::Editor).ok);     // the old name, through the alias
    reg.Publish();
    CHECK(reg.CheatsEnabled());
    CHECK(reg.Set(gate, CVarValue::Bool(false), SetBy::Console, {}, CVarContext::Client) == SetResult::Denied);
    CHECK(reg.Set(gate, CVarValue::Bool(false), SetBy::Console, {}, CVarContext::LocalHost) == SetResult::Applied);
    reg.Publish();
    CHECK_FALSE(reg.CheatsEnabled());

    REQUIRE(reg.Set(reg.Find("server.cheatsAllowed"), CVarValue::Bool(false), SetBy::Project, "project") == SetResult::Applied);
    reg.Publish();
    CHECK(reg.Set(gate, CVarValue::Bool(true), SetBy::Console, {}, CVarContext::LocalHost) == SetResult::Denied);
    CHECK(reg.Set(gate, CVarValue::Bool(true), SetBy::Console, {}, CVarContext::ServerAdmin) == SetResult::Denied);
    CHECK(reg.Set(gate, CVarValue::Bool(true), SetBy::Console, {}, CVarContext::Editor) == SetResult::Applied);   // the editor is never gated
}

TEST_CASE("cvar access: Protected is unreadable outside Editor/ServerAdmin, Hidden is absent outside the editor, Dev is absent from a Dist registry", "[cvar]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(reg.Register(Test::Desc("net.secret", CVarValue::String("hunter2"), Audience::Server, CVarFlags::Protected)).IsStale());
    CHECK(reg.Execute("net.secret", CVarContext::Editor).ok);
    CHECK(reg.Execute("net.secret", CVarContext::ServerAdmin).ok);
    for (const CVarContext ctx : { CVarContext::LocalHost, CVarContext::Client })
    {
        const ExecResult r = reg.Execute("net.secret", ctx);
        CHECK_FALSE(r.ok);
        CHECK(r.text.find("hunter2") == std::string::npos);
    }
    CHECK(reg.Execute("net.secret topsecret", CVarContext::LocalHost).ok);   // writable (Server), still unreadable there

    REQUIRE_FALSE(reg.Register(Test::Desc("game.hiddenKnob", CVarValue::Int32(1), Audience::PlayerSafe, CVarFlags::Hidden)).IsStale());
    CHECK(reg.Execute("game.hiddenKnob 2", CVarContext::Editor).ok);
    CHECK(reg.Execute("game.hiddenKnob 2", CVarContext::LocalHost).text.find("unknown") != std::string::npos);

    CVarRegistry dist{ false };
    CHECK(dist.Register(Test::Desc("game.devKnob", CVarValue::Int32(1), Audience::PlayerSafe, CVarFlags::Dev)).IsStale());
    for (const CVarContext ctx : { CVarContext::Editor, CVarContext::LocalHost, CVarContext::ServerAdmin, CVarContext::Client })
        CHECK(dist.Execute("game.devKnob 2", ctx).text.find("unknown") != std::string::npos);
}

// Review focus 4 (settings plan): what a console LISTS follows the same table
// as what it may read, so a client never learns an Editor, Hidden or
// Protected name, and cvar_explain never prints a Protected value where the
// plain read is refused.
TEST_CASE("cvar access: cvarlist, cvar_explain, List and Complete honour the context -- a client never sees Editor, Hidden or Protected names", "[cvar]")
{
    CVarRegistry reg;
    REQUIRE_FALSE(reg.Register(Test::Desc("acl.editorOnly", CVarValue::Int32(1), Audience::Editor)).IsStale());
    REQUIRE_FALSE(reg.Register(Test::Desc("acl.secret", CVarValue::String("hunter2"), Audience::Server, CVarFlags::Protected)).IsStale());
    REQUIRE_FALSE(reg.Register(Test::Desc("acl.hidden", CVarValue::Int32(1), Audience::PlayerSafe, CVarFlags::Hidden)).IsStale());
    REQUIRE_FALSE(reg.Register(Test::Desc("acl.volume", CVarValue::Int32(1), Audience::PlayerSafe)).IsStale());

    const auto lists = [&](CVarContext ctx, const char* name) {
        const ExecResult r = reg.Execute("cvarlist", ctx);
        return r.ok && r.text.find(std::string(name) + " (") != std::string::npos;
    };
    CHECK(lists(CVarContext::Editor, "acl.editorOnly"));
    CHECK(lists(CVarContext::Editor, "acl.secret"));
    CHECK(lists(CVarContext::Editor, "acl.volume"));
    CHECK_FALSE(lists(CVarContext::Editor, "acl.hidden"));        // Hidden is never listed, even here
    for (const CVarContext ctx : { CVarContext::LocalHost, CVarContext::Client })
    {
        INFO("ctx=" << ContextName(ctx));
        CHECK(lists(ctx, "acl.volume"));
        CHECK_FALSE(lists(ctx, "acl.editorOnly"));
        CHECK_FALSE(lists(ctx, "acl.hidden"));
        CHECK_FALSE(lists(ctx, "acl.secret"));
    }
    CHECK(lists(CVarContext::ServerAdmin, "acl.secret"));           // readable there, so listed there
    CHECK_FALSE(lists(CVarContext::ServerAdmin, "acl.editorOnly"));

    const ExecResult clientExplain = reg.Execute("cvar_explain acl.secret", CVarContext::Client);
    CHECK_FALSE(clientExplain.ok);
    CHECK(clientExplain.text.find("hunter2") == std::string::npos);
    CHECK(reg.Execute("cvar_explain acl.secret", CVarContext::ServerAdmin).text.find("hunter2") != std::string::npos);
    CHECK(reg.Execute("cvar_explain acl.secret", CVarContext::Editor).text.find("hunter2") != std::string::npos);
    CHECK(reg.Execute("cvar_explain acl.editorOnly", CVarContext::LocalHost).text.find("unknown") != std::string::npos);
    CHECK(reg.Execute("cvar_explain acl.hidden", CVarContext::LocalHost).text.find("unknown") != std::string::npos);
    CHECK(reg.Execute("cvar_explain acl.hidden", CVarContext::Editor).ok);   // Hidden: settable and explainable by exact name in the editor

    // List(ctx) and the console's completion are the same view.
    const auto aclNames = [](const std::vector<CVarListEntry>& list) {
        std::vector<std::string> out;
        for (const CVarListEntry& e : list)
            if (e.name.starts_with("acl.")) out.push_back(e.name);
        std::sort(out.begin(), out.end());
        return out;
    };
    using Names = std::vector<std::string>;
    CHECK(aclNames(reg.List()) == Names{ "acl.editorOnly", "acl.secret", "acl.volume" });   // the Editor view, as before
    CHECK(aclNames(reg.List(CVarContext::LocalHost)) == Names{ "acl.volume" });
    CHECK(aclNames(reg.List(CVarContext::Client)) == Names{ "acl.volume" });
    CHECK(aclNames(reg.List(CVarContext::ServerAdmin)) == Names{ "acl.secret", "acl.volume" });
    ConsoleModel model;
    model.SetInput("acl.");
    CHECK(model.Complete(reg, CVarContext::Editor) == Names{ "acl.editorOnly", "acl.secret", "acl.volume" });
    CHECK(model.Complete(reg, CVarContext::Client) == Names{ "acl.volume" });
    CHECK(model.Complete(reg, CVarContext::ServerAdmin) == Names{ "acl.secret", "acl.volume" });
}

// Review focus 4 / integration ruling I3: the command line keeps working for a
// developer. `ArcaneRuntime --set render.meshCull=false` targets a Game
// setting, which only the Editor context may write without cheats, so the
// Debug/Release --set runs there; a Dist build's --set is the local host's.
TEST_CASE("--set runs in the Editor context in a Debug/Release build and as the local host in Dist, so a Game setting on the runtime's command line still applies", "[cvar]")
{
#if defined(ARC_BUILD_DIST)
    STATIC_REQUIRE(CommandLineCVarContext() == CVarContext::LocalHost);
#else
    STATIC_REQUIRE(CommandLineCVarContext() == CVarContext::Editor);
    CVarRegistry reg;
    const CVarHandle cull = reg.Register(Test::Desc("render.meshCull", CVarValue::Bool(true), Audience::Game, CVarFlags::Dev));
    REQUIRE_FALSE(cull.IsStale());
    ApplyCVarCommandLine(reg, { "render.meshCull=false" }, CVarContext::LocalHost);   // what the runtime passed before I3: refused
    reg.Publish();
    CHECK(reg.Get(cull)->AsBool() == true);
    ApplyCVarCommandLine(reg, { "render.meshCull=false" }, CommandLineCVarContext());
    reg.Publish();
    CHECK(reg.Get(cull)->AsBool() == false);
    CHECK(reg.Explain("render.meshCull")->setBy == SetBy::CommandLine);
#endif
}
