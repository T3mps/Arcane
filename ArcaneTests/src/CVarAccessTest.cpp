// The cvar access model (settings spec s3.2): CVarContext, the audience x
// context table, the game's CVarPolicy and the audit sink. CPU-only ([cvar]).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/ConsoleModel.hpp>

#include "Helpers/CVarTestDesc.hpp"

#include <algorithm>
#include <cstdint>
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

namespace
{
    struct PolicyProbe { int asked = 0; };
    constexpr std::uint64_t kModerator = 1;

    PolicyVerdict LobbyPolicy(const CVarInfo& cvar, const CVarRequest& req, void* user)
    {
        ++static_cast<PolicyProbe*>(user)->asked;
        const bool moderator = req.caller && (req.caller->roles & kModerator) != 0;
        if (cvar.name.starts_with("match.") && moderator) return PolicyVerdict::Allow;              // widen a Game setting
        if (cvar.name == "server.gravity" && req.context == CVarContext::Client && moderator) return PolicyVerdict::Allow;
        if (cvar.name == "render.quality" && req.write) return PolicyVerdict::Deny;                // narrow: a ranked lock
        if (cvar.name.starts_with("net.") || cvar.name.starts_with("editor.") || cvar.name.starts_with("game.hidden"))
            return PolicyVerdict::Allow;                                                            // reaches for what it may not
        return PolicyVerdict::Default;
    }
}

TEST_CASE("CVarPolicy: Allow widens, Deny narrows, Default keeps the table; it never reaches Editor, Hidden or Protected reads; module unload clears it", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle rounds  = reg.Register(Test::Desc("match.rounds", CVarValue::Int32(3), Audience::Game));
    const CVarHandle gravity = reg.Register(Test::Desc("server.gravity", CVarValue::Float32(-9.81f), Audience::Server));
    const CVarHandle quality = reg.Register(Test::Desc("render.quality", CVarValue::Int32(2), Audience::PlayerSafe));
    REQUIRE_FALSE(reg.Register(Test::Desc("net.secret", CVarValue::String("hunter2"), Audience::Server, CVarFlags::Protected)).IsStale());
    const CVarHandle editorKnob = reg.Register(Test::Desc("editor.knob", CVarValue::Int32(0), Audience::Editor));
    REQUIRE_FALSE(reg.Register(Test::Desc("game.hiddenKnob", CVarValue::Int32(0), Audience::PlayerSafe, CVarFlags::Hidden)).IsStale());

    PolicyProbe probe;
    reg.SetPolicy(&LobbyPolicy, &probe, "game");
    const CVarCaller moderator{ "mod-7", kModerator, nullptr };
    const CVarCaller player{ "player-3", 0, nullptr };

    CHECK(reg.Set(rounds, CVarValue::Int32(5), SetBy::Console, {}, CVarContext::Client, &moderator) == SetResult::Applied);
    CHECK(reg.Set(rounds, CVarValue::Int32(6), SetBy::Console, {}, CVarContext::Client, &player) == SetResult::Denied);
    CHECK(reg.Set(gravity, CVarValue::Float32(-1.0f), SetBy::Console, {}, CVarContext::Client, &moderator) == SetResult::Applied);
    CHECK(reg.Set(gravity, CVarValue::Float32(-2.0f), SetBy::Console, {}, CVarContext::Client, &player) == SetResult::Denied);
    CHECK(reg.Set(quality, CVarValue::Int32(1), SetBy::Console, {}, CVarContext::LocalHost, &player) == SetResult::Denied);
    CHECK(reg.Execute("render.quality", CVarContext::LocalHost, SetBy::Console, &player).ok);   // the lock is on writes only
    CHECK_FALSE(reg.Execute("net.secret", CVarContext::Client, SetBy::Console, &moderator).ok);  // Protected read: Allow ignored
    CHECK(reg.Execute("editor.knob 1", CVarContext::Client, SetBy::Console, &moderator).text.find("unknown") != std::string::npos);
    CHECK(reg.Execute("game.hiddenKnob 1", CVarContext::LocalHost, SetBy::Console, &moderator).text.find("unknown") != std::string::npos);

    const int asked = probe.asked;
    CHECK(reg.Set(editorKnob, CVarValue::Int32(1), SetBy::Console, {}, CVarContext::Editor) == SetResult::Applied);
    CHECK(probe.asked == asked);                                   // the editor never consults the policy

    reg.UnregisterModule("game");                                  // the game module unloaded
    CHECK(reg.Set(rounds, CVarValue::Int32(7), SetBy::Console, {}, CVarContext::Client, &moderator) == SetResult::Denied);

    // A Dev setting a Dist build compiled out is not there for the policy to
    // bring back: a `net.` name LobbyPolicy would Allow, never asked about.
    CVarRegistry dist{ false };
    PolicyProbe distProbe;
    dist.SetPolicy(&LobbyPolicy, &distProbe, "game");
    CHECK(dist.Register(Test::Desc("net.devKnob", CVarValue::Int32(1), Audience::Game, CVarFlags::Dev)).IsStale());
    CHECK(dist.Execute("net.devKnob", CVarContext::LocalHost, SetBy::Console, &moderator).text.find("unknown") != std::string::npos);
    CHECK(dist.Execute("net.devKnob 2", CVarContext::LocalHost, SetBy::Console, &moderator).text.find("unknown") != std::string::npos);
    CHECK(distProbe.asked == 0);
}

TEST_CASE("cvar audit sink: every non-Editor change of a Server setting, and every policy denial of one, is recorded with caller, old, new and verdict", "[cvar]")
{
    CVarRegistry reg;
    std::vector<CVarAuditRecord> records;
    reg.SetAuditSink([](const CVarAuditRecord& r, void* u) { static_cast<std::vector<CVarAuditRecord>*>(u)->push_back(r); }, &records);
    const CVarHandle tick = reg.Register(Test::Desc("server.tickHz", CVarValue::Int32(60), Audience::Server));
    const CVarHandle speed = reg.Register(Test::Desc("game.speed", CVarValue::Int32(1), Audience::PlayerSafe));
    const CVarCaller admin{ "admin-1", 0, nullptr };

    REQUIRE(reg.Set(tick, CVarValue::Int32(30), SetBy::Console, {}, CVarContext::Editor) == SetResult::Applied);
    REQUIRE(reg.Set(speed, CVarValue::Int32(2), SetBy::Console, {}, CVarContext::LocalHost, &admin) == SetResult::Applied);
    CHECK(records.empty());                                        // Editor context, and a non-Server setting: not audited

    REQUIRE(reg.Set(tick, CVarValue::Int32(90), SetBy::Console, {}, CVarContext::ServerAdmin, &admin) == SetResult::Applied);
    REQUIRE(records.size() == 1);
    CHECK(records[0].name == "server.tickHz");
    CHECK(records[0].callerId == "admin-1");
    CHECK(records[0].oldValue == "30");
    CHECK(records[0].newValue == "90");
    CHECK(records[0].context == CVarContext::ServerAdmin);
    CHECK(records[0].verdict == PolicyVerdict::Default);
    CHECK(records[0].unixMs > 0);

    REQUIRE(reg.Set(tick, CVarValue::Int32(10), SetBy::Console, {}, CVarContext::Client, &admin) == SetResult::Denied);
    CHECK(records.size() == 1);                                    // a table denial is not a change

    reg.SetPolicy([](const CVarInfo&, const CVarRequest& r, void*) { return r.context == CVarContext::LocalHost ? PolicyVerdict::Deny : PolicyVerdict::Allow; },
                  nullptr, "game");
    REQUIRE(reg.Set(tick, CVarValue::Int32(20), SetBy::Console, {}, CVarContext::LocalHost, &admin) == SetResult::Denied);
    REQUIRE(records.size() == 2);
    CHECK(records[1].verdict == PolicyVerdict::Deny);
    CHECK(records[1].oldValue == "90");
    CHECK(records[1].newValue == "20");
    REQUIRE(reg.Set(tick, CVarValue::Int32(15), SetBy::Console, {}, CVarContext::Client, &admin) == SetResult::Applied);
    REQUIRE(records.size() == 3);
    CHECK(records[2].verdict == PolicyVerdict::Allow);
    reg.SetAuditSink(nullptr, nullptr);
}

namespace
{
    // A ranked lock on READING: a player may not see render.quality, a moderator may.
    PolicyVerdict ReadLockPolicy(const CVarInfo& cvar, const CVarRequest& req, void* user)
    {
        ++static_cast<PolicyProbe*>(user)->asked;
        const bool moderator = req.caller && (req.caller->roles & kModerator) != 0;
        if (!req.write && cvar.name == "render.quality" && !moderator) return PolicyVerdict::Deny;
        return PolicyVerdict::Default;
    }

    // Game code that REGISTERS during the policy's write question: the slot
    // array may move under the registry's own references.
    struct GrowingPolicyState { CVarRegistry* reg = nullptr; int grown = 0; };

    PolicyVerdict GrowingPolicy(const CVarInfo& cvar, const CVarRequest& req, void* user)
    {
        auto* state = static_cast<GrowingPolicyState*>(user);
        const bool rotation = cvar.name == "lobby.mapRotation";   // read BEFORE growing: cvar.name points into the slot
        if (req.write)
        {
            for (int i = 0; i < 128; ++i)
            {
                const std::string name = "lobby.grown." + std::to_string(state->grown);
                if (state->reg->Register(Test::Desc(name, CVarValue::Int32(0), Audience::Game)).IsStale()) break;
                ++state->grown;
            }
        }
        return rotation ? PolicyVerdict::Allow : PolicyVerdict::Default;
    }
}

// cvar_explain carries no caller of its own (a CommandFn), so it asks the same
// read question as a plain `name` through the Execute that dispatched it: the
// table, then the game's policy. A Deny prints no value.
TEST_CASE("cvar_explain honours the game's policy: a read Deny for the caller refuses it and prints no value; a moderator, or the editor, reads it", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle quality = reg.Register(Test::Desc("render.quality", CVarValue::Int32(2), Audience::PlayerSafe));
    REQUIRE_FALSE(quality.IsStale());
    PolicyProbe probe;
    reg.SetPolicy(&ReadLockPolicy, &probe, "game");
    const CVarCaller moderator{ "mod-7", kModerator, nullptr };
    const CVarCaller player{ "player-3", 0, nullptr };

    const ExecResult denied = reg.Execute("cvar_explain render.quality", CVarContext::LocalHost, SetBy::Console, &player);
    CHECK_FALSE(denied.ok);
    CHECK(denied.text.find("= 2") == std::string::npos);
    CHECK(denied.text.find("policy") != std::string::npos);
    CHECK(reg.Execute("cvar_explain render.quality", CVarContext::LocalHost, SetBy::Console, &moderator).text.find("= 2") != std::string::npos);
    CHECK(reg.Execute("cvar_explain render.quality", CVarContext::ServerAdmin, SetBy::Console, &moderator).ok);
    const int asked = probe.asked;
    CHECK(reg.Execute("cvar_explain render.quality", CVarContext::Editor, SetBy::Console, &player).text.find("= 2") != std::string::npos);
    CHECK(probe.asked == asked);                                   // the editor never consults the policy
    // The plain read is the same question.
    CHECK_FALSE(reg.Execute("render.quality", CVarContext::LocalHost, SetBy::Console, &player).ok);
    CHECK(reg.Execute("render.quality", CVarContext::LocalHost, SetBy::Console, &moderator).ok);
    // A Protected read outside ServerAdmin is the table's refusal, not the policy's: still "protected".
    REQUIRE_FALSE(reg.Register(Test::Desc("net.secret", CVarValue::String("hunter2"), Audience::Server, CVarFlags::Protected)).IsStale());
    const ExecResult secret = reg.Execute("cvar_explain net.secret", CVarContext::LocalHost, SetBy::Console, &moderator);
    CHECK_FALSE(secret.ok);
    CHECK(secret.text.find("hunter2") == std::string::npos);
    CHECK(secret.text.find("protected") != std::string::npos);
    // Absent stays unknown, whatever the policy would say.
    REQUIRE_FALSE(reg.Register(Test::Desc("editor.knob", CVarValue::Int32(0), Audience::Editor)).IsStale());
    CHECK(reg.Execute("cvar_explain editor.knob", CVarContext::LocalHost, SetBy::Console, &moderator).text.find("unknown") != std::string::npos);

    // The query the command asks, on its own.
    CHECK(reg.CanRead(quality, CVarContext::LocalHost, &moderator));
    CHECK_FALSE(reg.CanRead(quality, CVarContext::LocalHost, &player));
    CHECK(reg.CanRead(quality, CVarContext::Editor, &player));
    CHECK_FALSE(reg.CanRead(reg.Find("net.secret"), CVarContext::Client, &moderator));
    CHECK(reg.CanRead(reg.Find("net.secret"), CVarContext::ServerAdmin, &player));
    CHECK_FALSE(reg.CanRead(reg.Find("editor.knob"), CVarContext::LocalHost, &moderator));
    CHECK_FALSE(reg.CanRead(CVarHandle{}, CVarContext::Editor));
}

// The policy is game code and may Register; Register may grow the slot
// vector. Nothing in Execute may hold a Slot reference across Set.
TEST_CASE("cvar policy: a policy that registers cvars during the write question moves the slot array; Execute still names the setting it set", "[cvar]")
{
    CVarRegistry reg;
    const CVarHandle rotation = reg.Register(Test::Desc("lobby.mapRotation", CVarValue::Int32(1), Audience::Game));
    REQUIRE_FALSE(rotation.IsStale());
    GrowingPolicyState state{ &reg, 0 };
    reg.SetPolicy(&GrowingPolicy, &state, "game");
    const CVarCaller host{ "host", 0, nullptr };

    const ExecResult set = reg.Execute("lobby.mapRotation 4", CVarContext::LocalHost, SetBy::Console, &host);
    CHECK(state.grown == 128);
    CHECK(set.ok);
    CHECK(set.text.find("lobby.mapRotation") != std::string::npos);
    CHECK(reg.Find("lobby.mapRotation") == rotation);          // the handle is stable: Register never moves a live slot
    reg.Publish();
    CHECK(reg.Get(rotation)->AsInt32() == 4);

    const ExecResult weaker = reg.Execute("lobby.mapRotation 5", CVarContext::LocalHost, SetBy::Code, &host);
    CHECK(state.grown == 256);
    CHECK_FALSE(weaker.ok);                                        // Code sits beneath the Console record
    CHECK(weaker.text.find("lobby.mapRotation") != std::string::npos);
    CHECK(reg.Find("lobby.grown.255") != CVarHandle{});
}
