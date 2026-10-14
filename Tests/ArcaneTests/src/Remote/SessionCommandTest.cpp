#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Commands/SessionCommands.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/HostSession.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Remote/Registry.hpp>
#include "RemoteTestHttp.hpp"
#include "RemoteTestSupport.hpp"
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir
#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

#if !defined(ARC_BUILD_DIST)

TEST_CASE("wait reload completes when NoteReload bumps the generation", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    auto root = std::filesystem::temp_directory_path() / "remote-wait-reload";
    std::filesystem::remove_all(root);
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Runtime;
    cfg.cvarContext = Arcane::CVarContext::LocalHost;
    cfg.projectRoot = root;
    cfg.holdSim = true;
    Arcane::Remote::HostSession session(cfg);
    session.Start();
    const auto& constSession = static_cast<const Arcane::Remote::HostSession&>(session);
    const auto gen = constSession.Gate().pluginGeneration;   // const accessor (ruling F9)
    Arcane::Remote::WaitArgs args;
    args.what = "reload";
    args.timeoutMs = 5000;
    const auto started = std::chrono::steady_clock::now();
    auto pending = Arcane::Remote::WaitPoll(session, args, 0, gen, true, started);
    CHECK_FALSE(pending.data.value("done", false));
    session.NoteReload(gen + 1, true, "reloaded", 0);       // the host passes its PluginHost generation (ruling F10)
    auto done = Arcane::Remote::WaitPoll(session, args, 0, gen, true, started);
    CHECK(done.ok);
    CHECK(done.data.value("done", false));
    CHECK(Arcane::Remote::ExitFromHost(done) == 0);
    session.Stop("test");
}

TEST_CASE("wait settled on a server is capture.no-renderer exit 2", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Server);
    Arcane::Remote::WaitArgs args;
    args.what = "settled";
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Server;
    cfg.cvarContext = Arcane::CVarContext::ServerAdmin;
    cfg.projectRoot = std::filesystem::temp_directory_path() / "remote-wait-server";
    Arcane::Remote::HostSession session(cfg);
    session.Start();
    auto env = Arcane::Remote::WaitPoll(session, args, 0, 0, false, std::chrono::steady_clock::now());
    CHECK_FALSE(env.ok);
    CHECK(env.errors[0].code == "capture.no-renderer");
    CHECK(Arcane::Remote::ExitFromHost(env) == 2);
    session.Stop("test");
}

static bool RemoteSettledNever(void*) { return false; }

TEST_CASE("wait state times out with ok true and exit 8", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Runtime;
    cfg.cvarContext = Arcane::CVarContext::LocalHost;
    cfg.projectRoot = std::filesystem::temp_directory_path() / "remote-wait-timeout";
    cfg.settled = &RemoteSettledNever;
    Arcane::Remote::HostSession session(cfg);
    session.Start();
    session.Gate().SetState("ready");
    Arcane::Remote::WaitArgs args;
    args.what = "state";
    args.state = "playing";
    args.timeoutMs = 0;                                      // in-process: already timed out
    auto env = Arcane::Remote::WaitPoll(session, args, 0, 0, true, std::chrono::steady_clock::now());
    CHECK(env.ok);
    CHECK(env.errors.empty());
    CHECK(env.data["timedOut"] == true);
    CHECK(Arcane::Remote::ExitFromHost(env) == 8);
    // A real elapsed timeout through `started` (ruling F9): 50 ms budget, armed 100 ms ago.
    Arcane::Remote::WaitArgs settled;
    settled.what = "settled";
    settled.timeoutMs = 50;
    const auto armed = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
    auto late = Arcane::Remote::WaitPoll(session, settled, 0, 0, true, armed);
    CHECK(late.ok);
    CHECK(late.data["timedOut"] == true);
    auto early = Arcane::Remote::WaitPoll(session, settled, 0, 0, true, std::chrono::steady_clock::now());
    CHECK_FALSE(early.data.value("done", false));             // settled() is false and 50 ms have not passed
    CHECK_FALSE(early.data.value("timedOut", false));
    session.Stop("test");
}

namespace
{
Arcane::Remote::HostSession::Config SessionConfig(Arcane::Remote::HostKind kind, const std::filesystem::path& root)
{
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = kind;
    cfg.cvarContext = kind == Arcane::Remote::HostKind::Server ? Arcane::CVarContext::ServerAdmin
                                                               : Arcane::CVarContext::LocalHost;
    cfg.projectRoot = root;
    return cfg;
}

Arcane::Remote::Envelope Invoke(const std::string& command, nlohmann::json args)
{
    Arcane::Remote::Call call;
    call.command = command;
    call.args = std::move(args);
    return Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
}

bool RemoteDirtyAlways(void*) { return true; }

bool RemoteSettledFlag(void* user) { return *static_cast<bool*>(user); }
} // namespace

TEST_CASE("wait ready, frames, state and settled complete on their conditions", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-conditions");
    bool settledNow = false;
    auto cfg = SessionConfig(Arcane::Remote::HostKind::Runtime, tmp.path);
    cfg.settled = &RemoteSettledFlag;
    cfg.hookUser = &settledNow;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    const auto now = std::chrono::steady_clock::now();

    Arcane::Remote::WaitArgs ready;
    ready.what = "ready";
    CHECK(Arcane::Remote::WaitPoll(session, ready, 0, 0, true, now).data.value("done", false));
    session.Gate().SetState("stopping");
    CHECK_FALSE(Arcane::Remote::WaitPoll(session, ready, 0, 0, true, now).data.value("done", false));
    session.Gate().SetState("paused");
    CHECK(Arcane::Remote::WaitPoll(session, ready, 0, 0, true, now).data.value("done", false));

    Arcane::Remote::WaitArgs frames;
    frames.what = "frames";
    frames.frames = 3;
    session.Gate().frame = 7;
    CHECK_FALSE(Arcane::Remote::WaitPoll(session, frames, 5, 0, true, now).data.value("done", false));
    session.Gate().frame = 8;
    const auto framesDone = Arcane::Remote::WaitPoll(session, frames, 5, 0, true, now);
    CHECK(framesDone.data.value("done", false));
    CHECK(framesDone.data["frame"] == 8);

    Arcane::Remote::WaitArgs state;
    state.what = "state";
    state.state = "paused";
    CHECK(Arcane::Remote::WaitPoll(session, state, 0, 0, true, now).data.value("done", false));

    Arcane::Remote::WaitArgs settled;
    settled.what = "settled";
    CHECK_FALSE(Arcane::Remote::WaitPoll(session, settled, 0, 0, true, now).data.value("done", false));
    settledNow = true;
    CHECK(Arcane::Remote::WaitPoll(session, settled, 0, 0, true, now).data.value("done", false));

    // A reload that never moves the generation (PluginHost does not bump it on
    // a failed or refused reload) keeps the wait pending; it then times out.
    Arcane::Remote::WaitArgs reload;
    reload.what = "reload";
    reload.timeoutMs = 50;
    session.NoteReload(session.Gate().pluginGeneration, false, "reload failed", 9);
    const auto unchanged = Arcane::Remote::WaitPoll(session, reload, 0, session.Gate().pluginGeneration, true,
                                                    now - std::chrono::milliseconds(100));
    CHECK(unchanged.ok);
    CHECK(unchanged.data["timedOut"] == true);
    CHECK(Arcane::Remote::ExitFromHost(unchanged) == 8);
    session.Stop("test");
}

TEST_CASE("wait settled on a renderer host with no settle hook is host.unavailable", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-nohook");
    Arcane::Remote::HostSession session(SessionConfig(Arcane::Remote::HostKind::Runtime, tmp.path));
    REQUIRE(session.Start());
    Arcane::Remote::WaitArgs settled;
    settled.what = "settled";
    const auto env = Arcane::Remote::WaitPoll(session, settled, 0, 0, true, std::chrono::steady_clock::now());
    CHECK_FALSE(env.ok);
    CHECK(env.errors[0].code == "host.unavailable");
    CHECK(Arcane::Remote::ExitFromHost(env) == 5);
    session.Stop("test");
}

TEST_CASE("host.wait refuses a missing, unknown or ill-typed argument", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-args");
    Arcane::Remote::HostSession session(SessionConfig(Arcane::Remote::HostKind::Runtime, tmp.path));
    REQUIRE(session.Start());
    const auto missing = Invoke("host.wait", nlohmann::json::object());
    CHECK(missing.errors[0].code == "arg.missing");
    CHECK(Arcane::Remote::ExitFromHost(missing) == 2);
    CHECK(Invoke("host.wait", {{"what", "frames"}}).errors[0].code == "arg.missing");
    CHECK(Invoke("host.wait", {{"what", "state"}}).errors[0].code == "arg.missing");
    CHECK(Invoke("host.wait", {{"what", "soon"}}).errors[0].code == "arg.invalid");
    CHECK(Invoke("host.wait", {{"what", "frames"}, {"frames", -1}}).errors[0].code == "arg.invalid");
    CHECK(Invoke("host.wait", {{"what", "frames"}, {"frames", "2"}}).errors[0].code == "arg.invalid");
    CHECK(Invoke("host.wait", {{"what", "ready"}, {"n", 2}}).errors[0].code == "arg.unknown");   // `n` is not this command
    const auto armed = Arcane::Remote::CommandRegistry::Get().Invoke([] {
        Arcane::Remote::Call call;
        call.command = "host.wait";
        call.args = {{"what", "ready"}};
        return call;
    }());
    CHECK(armed.envelope.ok);
    CHECK(armed.completesJob != 0);                          // a job the dispatcher binds the waiter to
    session.Stop("test");
}

TEST_CASE("host.quit requests the quit; a dirty host answers host.busy modal unless forced or discarding", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-quit");
    {
        auto cfg = SessionConfig(Arcane::Remote::HostKind::Editor, tmp.path / "dirty");
        cfg.dirty = &RemoteDirtyAlways;
        Arcane::Remote::HostSession session(cfg);
        REQUIRE(session.Start());
        const auto modal = Invoke("host.quit", nlohmann::json::object());
        REQUIRE_FALSE(modal.errors.empty());
        CHECK(modal.errors[0].code == "host.busy");
        CHECK(modal.errors[0].detail["state"] == "modal");
        CHECK(Arcane::Remote::ExitFromHost(modal) == 5);
        CHECK_FALSE(session.TakeQuit());
        CHECK(Invoke("host.quit", {{"force", "yes"}}).errors[0].code == "arg.invalid");
        CHECK(Invoke("host.quit", {{"report", "x.json"}}).errors[0].code == "arg.unknown");
        CHECK_FALSE(session.TakeQuit());
        const auto discarded = Invoke("host.quit", {{"discardChanges", true}});
        CHECK(discarded.ok);
        CHECK(discarded.data["discardedChanges"] == true);
        CHECK(session.TakeQuit());
        CHECK(session.DrainFrame(0));
        CHECK(std::string(session.ExitReason()) == "remote-quit");
        session.Stop(session.ExitReason());
    }
    {
        auto cfg = SessionConfig(Arcane::Remote::HostKind::Editor, tmp.path / "forced");
        cfg.dirty = &RemoteDirtyAlways;
        Arcane::Remote::HostSession session(cfg);
        REQUIRE(session.Start());
        CHECK(Invoke("host.quit", {{"force", true}}).ok);
        CHECK(session.TakeQuit());
        session.Stop("test");
    }
    {
        Arcane::Remote::HostSession session(SessionConfig(Arcane::Remote::HostKind::Server, tmp.path / "clean"));
        REQUIRE(session.Start());
        const auto clean = Invoke("host.quit", nlohmann::json::object());   // no dirty hook: never modal
        CHECK(clean.ok);
        CHECK(clean.data["discardedChanges"] == false);
        CHECK(session.TakeQuit());
        session.Stop("test");
    }
}

TEST_CASE("session commands answer host.unavailable with no session, and after it stops", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Remote::RegisterSessionCommands(nullptr, nullptr);
    Arcane::Remote::RegisterSessionCommands(nullptr, nullptr);   // twice is safe (ruling F11)
    const auto quit = Invoke("host.quit", nlohmann::json::object());
    CHECK(quit.errors[0].code == "host.unavailable");
    CHECK(Arcane::Remote::ExitFromHost(quit) == 5);
    CHECK(Invoke("host.wait", {{"what", "ready"}}).errors[0].code == "host.unavailable");

    Arcane::Test::TempDir tmp("remote-session-release");
    Arcane::Remote::HostSession session(SessionConfig(Arcane::Remote::HostKind::Runtime, tmp.path));
    REQUIRE(session.Start());
    CHECK(Invoke("host.wait", {{"what", "ready"}}).ok);
    session.Stop("test");                                         // releases the stored pointers
    CHECK(Invoke("host.quit", nlohmann::json::object()).errors[0].code == "host.unavailable");
}

TEST_CASE("host.wait over the listener: the job completes after N frames and answers the parked call", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Runtime);
    Arcane::Test::TempDir tmp("remote-wait-e2e");
    Arcane::Remote::HostSession session(SessionConfig(Arcane::Remote::HostKind::Runtime, tmp.path));
    REQUIRE(session.Start());
    const auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
    REQUIRE(desc);
    const std::uint16_t port = session.Port();
    const std::string token = desc->token;
    const std::string body =
        R"({"id":"w1","command":"host.wait","args":{"what":"frames","frames":3},"options":{"timeoutMs":5000}})";
    auto reply = std::async(std::launch::async, [port, token, body] {
        return RemoteTest::HttpExchange(port, token, "", "POST /remote/v1/call HTTP/1.1", body);
    });
    // The host loop: one Drain per iteration, one sim frame per iteration.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::uint64_t iterations = 0;
    while (reply.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
        session.DrainFrame(0);
        session.Gate().frame.fetch_add(1);
        ++iterations;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(reply.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    const std::string raw = reply.get();
    const auto split = raw.find("\r\n\r\n");
    REQUIRE(split != std::string::npos);
    const auto env = Arcane::Remote::ParseEnvelope(nlohmann::json::parse(raw.substr(split + 4)));
    CHECK(env.ok);
    CHECK(env.id == "w1");
    CHECK(env.command == "host.wait");
    CHECK(env.data["done"] == true);
    CHECK(env.data["what"] == "frames");
    CHECK(env.host.frame >= 3);
    CHECK(Arcane::Remote::ExitFromHost(env) == 0);
    CHECK(iterations >= 3);
    session.Stop("test");
}

#endif
