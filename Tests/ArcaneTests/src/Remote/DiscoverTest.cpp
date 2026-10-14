#include <catch2/catch_test_macros.hpp>
#include <Servitor/Client.hpp>
#include <Servitor/Discover.hpp>
#include <Servitor/Transport.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/Http.hpp>       // MakeToken (Task 5)
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Platform/Process.hpp>  // ExecutablePath
#include <Arcane/Project/Project.hpp>   // EditorLock::Self (ArcaneEngine)
#include <Json.hpp>
#include "RemoteDemoArgs.hpp"
#include "RemoteTestSupport.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace {

std::int64_t DiscoverNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

TEST_CASE("two live hosts without --pid are ambiguous and a stale file is removed", "[servitor][client]") {
    const auto root = std::filesystem::temp_directory_path() / "remote-scan";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Saved" / "Remote");
    Arcane::Remote::Descriptor a;
    a.protocol = 1;
    a.hostKind = Arcane::Remote::HostKind::Editor;
    a.pid = Arcane::EditorLock::Self().pid;
    a.processStart = Arcane::EditorLock::Self().start;
    a.port = 1;
    a.token = "a";
    a.state = "ready";
    a.heartbeatUnixMs = 1'700'000'000'000;
    Arcane::Remote::Descriptor b = a;
    b.hostKind = Arcane::Remote::HostKind::Runtime;
    b.port = 2;
    b.token = "b";
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(
        Arcane::Remote::DescriptorPath(root, a.hostKind, a.pid), a));
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(
        Arcane::Remote::DescriptorPath(root, b.hostKind, b.pid), b));
    Arcane::Remote::Descriptor stale = a;
    stale.pid = 1;
    stale.processStart = a.processStart + 1;
    stale.hostKind = Arcane::Remote::HostKind::Server;
    const auto stalePath = Arcane::Remote::DescriptorPath(root, stale.hostKind, stale.pid);
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(stalePath, stale));
    const auto scan = Servitor::ScanProject(root);
    CHECK(scan.live.size() == 2);
    CHECK_FALSE(std::filesystem::exists(stalePath));
    Servitor::GlobalOptions opt;
    Arcane::Remote::Envelope err;
    CHECK_FALSE(Servitor::Select(scan, opt, &err));
    CHECK(err.errors[0].code == "target.ambiguous");
    CHECK(Arcane::Remote::ExitFromHost(err) == 2);
    opt.pid = a.pid;
    opt.hostKind = "runtime";
    auto runtime = Servitor::Select(scan, opt, &err);
    REQUIRE(runtime);
    CHECK(runtime->descriptor.port == 2);
    CHECK(runtime->descriptor.hostKind == Arcane::Remote::HostKind::Runtime);
    CHECK(runtime->hung);
    std::filesystem::remove_all(root);
}

TEST_CASE("zero hosts is target.none exit 4", "[servitor][client]") {
    const auto root = std::filesystem::temp_directory_path() / "remote-scan-empty";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto scan = Servitor::ScanProject(root);
    Servitor::GlobalOptions opt;
    Arcane::Remote::Envelope err;
    CHECK_FALSE(Servitor::Select(scan, opt, &err));
    CHECK(err.errors[0].code == "target.none");
    CHECK(Arcane::Remote::ExitFromHost(err) == 4);
    std::filesystem::remove_all(root);
}

TEST_CASE("a fresh heartbeat is not hung and status reports the flag", "[servitor][client]") {
    const auto root = std::filesystem::temp_directory_path() / "remote-scan-fresh";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Saved" / "Remote");
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Arcane::Remote::Descriptor d;
    d.protocol = 1;
    d.hostKind = Arcane::Remote::HostKind::Runtime;
    d.pid = Arcane::EditorLock::Self().pid;
    d.processStart = Arcane::EditorLock::Self().start;
    d.port = 9;
    d.token = "secret-token";
    d.state = "ready";
    d.heartbeatUnixMs = nowMs;
    d.abi = 61;
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(
        Arcane::Remote::DescriptorPath(root, d.hostKind, d.pid), d));
    auto scan = Servitor::ScanProject(root);
    REQUIRE(scan.live.size() == 1);
    CHECK_FALSE(scan.live[0].hung);
    auto data = Servitor::StatusData(scan);
    REQUIRE(data["hosts"].size() == 1);
    CHECK(data["hosts"][0]["hostKind"] == "runtime");
    CHECK(data["hosts"][0]["pid"] == d.pid);
    CHECK(data["hosts"][0]["abi"] == 61);   // from Descriptor::abi (ruling F27)
    CHECK(data["hosts"][0]["hung"] == false);
    CHECK(data["hosts"][0]["heartbeatAgeMs"].get<std::int64_t>() < 5000);
    CHECK(data.dump().find("secret-token") == std::string::npos);
    d.heartbeatUnixMs = nowMs - 60000;
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(
        Arcane::Remote::DescriptorPath(root, d.hostKind, d.pid), d));
    scan = Servitor::ScanProject(root);
    REQUIRE(scan.live.size() == 1);
    CHECK(scan.live[0].hung);
    data = Servitor::StatusData(scan);
    CHECK(data["hosts"][0]["hung"] == true);
    CHECK(data["hosts"][0]["heartbeatAgeMs"].get<std::int64_t>() > 10000);
    Servitor::GlobalOptions opt;
    opt.hostKind = "runtime";
    Arcane::Remote::Envelope err;
    auto selected = Servitor::Select(scan, opt, &err);
    REQUIRE(selected);
    CHECK(selected->hung);
    std::filesystem::remove_all(root);
}

TEST_CASE("a newer protocol major reaches Select and is protocol.mismatch, not target.none", "[servitor][client]") {
    const auto root = std::filesystem::temp_directory_path() / "remote-scan-major";
    std::filesystem::remove_all(root);
    Arcane::Remote::Descriptor d;
    d.protocol = 2;   // a newer host (rev 8.3 V6): ReadDescriptor keeps it
    d.hostKind = Arcane::Remote::HostKind::Server;
    d.pid = Arcane::EditorLock::Self().pid;
    d.processStart = Arcane::EditorLock::Self().start;
    d.port = 7;
    d.token = "t";
    d.heartbeatUnixMs = DiscoverNowMs();
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(
        Arcane::Remote::DescriptorPath(root, d.hostKind, d.pid), d));
    const auto scan = Servitor::ScanProject(root);
    REQUIRE(scan.live.size() == 1);
    Arcane::Remote::Envelope err;
    CHECK_FALSE(Servitor::Select(scan, Servitor::GlobalOptions{}, &err));
    REQUIRE(err.errors.size() == 1);
    CHECK(err.errors[0].code == "protocol.mismatch");
    CHECK(err.errors[0].remediation.find('2') != std::string::npos);
    CHECK(Arcane::Remote::ExitFromHost(err) == 4);
    std::filesystem::remove_all(root);
}

TEST_CASE("--host and --pid that match no live host are target.none naming the candidates", "[servitor][client]") {
    const auto root = std::filesystem::temp_directory_path() / "remote-scan-nomatch";
    std::filesystem::remove_all(root);
    Arcane::Remote::Descriptor d;
    d.hostKind = Arcane::Remote::HostKind::Editor;
    d.pid = Arcane::EditorLock::Self().pid;
    d.processStart = Arcane::EditorLock::Self().start;
    d.port = 5;
    d.token = "t";
    d.heartbeatUnixMs = DiscoverNowMs();
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(
        Arcane::Remote::DescriptorPath(root, d.hostKind, d.pid), d));
    const auto scan = Servitor::ScanProject(root);
    Servitor::GlobalOptions opt;
    opt.hostKind = "server";
    Arcane::Remote::Envelope err;
    CHECK_FALSE(Servitor::Select(scan, opt, &err));
    CHECK(err.errors[0].code == "target.none");
    REQUIRE(err.errors[0].candidates.size() == 1);
    CHECK(err.errors[0].candidates[0] == "editor:" + std::to_string(d.pid));
    std::filesystem::remove_all(root);
}

TEST_CASE("HostsDir: --engine beats SERVITOR_ENGINE beats the sibling; sdk.json picks the config", "[servitor][client]") {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / ("servitor-hostsdir-" + std::to_string(Arcane::EditorLock::Self().pid));
    fs::remove_all(tmp);
    const fs::path pkg = tmp / "pkg";                       // an installed engine
    fs::create_directories(pkg / "sdk");
    { std::ofstream(pkg / "sdk" / "sdk.json") << R"({"engineConfig":"Release"})"; }
    const fs::path src = tmp / "src";                       // a source checkout (no sdk.json)
    fs::create_directories(src);
    const fs::path broken = tmp / "broken";                 // sdk.json without engineConfig
    fs::create_directories(broken / "sdk");
    { std::ofstream(broken / "sdk" / "sdk.json") << R"({})"; }

    const std::string tail = std::string(SERVITOR_OUTPUTDIR).substr(std::string(SERVITOR_OUTPUTDIR).find('-'));
    CHECK(Servitor::HostsDirForRoot(pkg) == pkg / "Binaries" / ("Release" + tail));
    CHECK(Servitor::HostsDirForRoot(src) == src / "Binaries" / SERVITOR_OUTPUTDIR);
    CHECK_FALSE(Servitor::HostsDirForRoot(broken).has_value());

    RemoteTest::ScopedEnv env("SERVITOR_ENGINE", src.string());
    Servitor::GlobalOptions flag;
    flag.engine = pkg.string();
    CHECK(Servitor::HostsDir(flag, nullptr) == Servitor::HostsDirForRoot(pkg));      // the flag wins
    Servitor::GlobalOptions none;
    CHECK(Servitor::HostsDir(none, nullptr) == Servitor::HostsDirForRoot(src));      // then the env var
    Arcane::Remote::Envelope error;
    Servitor::GlobalOptions bad;
    bad.engine = broken.string();
    CHECK_FALSE(Servitor::HostsDir(bad, &error).has_value());
    CHECK(error.errors[0].code == "engine.unresolved");
    CHECK(Arcane::Remote::ExitFromHost(error) == 4);
    fs::remove_all(tmp);
}

TEST_CASE("HostsDir without a flag or env var finds the source sibling", "[servitor][client]") {
    RemoteTest::ScopedEnv env("SERVITOR_ENGINE", std::nullopt);   // unset for this case
    const auto dir = Servitor::HostsDir(Servitor::GlobalOptions{}, nullptr);
    REQUIRE(dir.has_value());   // ArcaneTests runs from Binaries/<cfg>/ArcaneTests; its sibling folders exist
    CHECK(*dir == Arcane::Platform::ExecutablePath().parent_path().parent_path());
}

TEST_CASE("HostDiagnosticsDir resolves through HostsDir and is empty when the engine is unresolved", "[servitor][client]") {
    namespace fs = std::filesystem;
    const fs::path src = fs::temp_directory_path() / ("servitor-diag-" + std::to_string(Arcane::EditorLock::Self().pid));
    fs::remove_all(src);
    fs::create_directories(src);
    Servitor::GlobalOptions opt;
    opt.engine = src.string();
    CHECK(Servitor::HostDiagnosticsDir(Arcane::Remote::HostKind::Runtime, opt) ==
          src / "Binaries" / SERVITOR_OUTPUTDIR / "ArcaneRuntime" / "diagnostics");
    CHECK(Servitor::HostDiagnosticsDir(Arcane::Remote::HostKind::Editor, opt) ==
          src / "Binaries" / SERVITOR_OUTPUTDIR / "ArcaneEditor" / "diagnostics");
    fs::create_directories(src / "sdk");
    { std::ofstream(src / "sdk" / "sdk.json") << R"({"engineConfig":7})"; }   // not a string
    CHECK(Servitor::HostDiagnosticsDir(Arcane::Remote::HostKind::Server, opt).empty());
    fs::remove_all(src);
}

// --- the live transport (rev 8.3 V3) ---------------------------------------

namespace {

Arcane::Remote::CommandResult DiscoverNoop(const Arcane::Remote::Call& call, void*) {
    Arcane::Remote::CommandResult r;
    r.envelope = Arcane::Remote::MakeOk(call.command, nlohmann::json{{"noop", true}}, Arcane::Remote::HostBlock{});
    return r;
}

struct DiscoverUnregister {
    ~DiscoverUnregister() { Arcane::Remote::CommandRegistry::Get().UnregisterModule("discover-test"); }
};

// A descriptor naming this process with a fresh heartbeat, so the watch
// LiveExchange samples sees a live, unhung host.
Servitor::LiveTarget DiscoverTarget(const std::filesystem::path& dir, std::uint16_t port, std::string token) {
    Servitor::LiveTarget t;
    t.descriptor.hostKind = Arcane::Remote::HostKind::Runtime;
    t.descriptor.pid = Arcane::EditorLock::Self().pid;
    t.descriptor.processStart = Arcane::EditorLock::Self().start;
    t.descriptor.port = port;
    t.descriptor.token = std::move(token);
    t.descriptor.state = "ready";
    t.descriptor.heartbeatUnixMs = DiscoverNowMs();
    const auto path = Arcane::Remote::DescriptorPath(dir, t.descriptor.hostKind, t.descriptor.pid);
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(path, t.descriptor));
    t.watch.command = "discover.noop";
    t.watch.pid = t.descriptor.pid;
    t.watch.processStart = t.descriptor.processStart;
    t.watch.descriptorPath = path;
    t.timeoutMs = 5000;
    return t;
}

// Runs LiveExchange on a worker while this thread drains the host queue, as a
// host main thread would. Gives up after 10 s.
Servitor::ExchangeResult DiscoverExchange(Servitor::LiveTarget& target, const std::string& body,
                                          Arcane::Remote::RemoteDispatcher* dispatcher) {
    auto pending = std::async(std::launch::async, [&] { return Servitor::LiveExchange(body, &target); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (pending.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
        if (dispatcher != nullptr)
            dispatcher->Drain(1000.0, 8);
    }
    REQUIRE(pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    return pending.get();
}

}  // namespace

TEST_CASE("LiveExchange returns the command's envelope; a wrong token is auth.denied; a stopped listener refuses before the body",
          "[servitor][client]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("servitor-live-" + std::to_string(Arcane::EditorLock::Self().pid));
    fs::remove_all(dir);
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    DiscoverUnregister unregister;
    Arcane::Remote::Desc desc;
    desc.name = "discover.noop";
    desc.summary = "discover test no-op";
    desc.module = "discover-test";
    desc.argsMeta = Astra::GetMeta<RemoteDemoArgs>();
    desc.fn = &DiscoverNoop;
    std::string regErr;
    REQUIRE(reg.Register(desc, &regErr));

    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    auto status = std::make_shared<Arcane::Remote::HostStatus>();
    status->state = "ready";
    status->hostKind = Arcane::Remote::HostKind::Runtime;
    atoms.status.store(status);
    atoms.catalog.store(std::make_shared<const nlohmann::json>(nlohmann::json::object()));
    Arcane::Remote::HostGate gate;
    gate.kind = "runtime";
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, dir / "audit.ndjson");
    const std::string token = Arcane::Remote::MakeToken();
    Arcane::Remote::RemoteListener listener(queue, atoms, token);
    listener.Start();
    const std::uint16_t port = listener.Port();
    const std::string body =
        R"({"id":"d1","command":"discover.noop","args":{"position":[0,0,0]},"options":{"timeoutMs":5000}})";

    auto good = DiscoverTarget(dir, port, token);
    const auto ok = DiscoverExchange(good, body, &dispatcher);
    CHECK_FALSE(ok.connectionRefusedBeforeBody);
    CHECK(ok.envelope.ok);
    CHECK(ok.envelope.command == "discover.noop");
    CHECK(ok.envelope.data["noop"] == true);
    CHECK(Arcane::Remote::ExitFromHost(ok.envelope) == 0);
    CHECK(good.watch.requestUnixMs > 0);   // stamped when the exchange started

    // GET /remote/v1/status over the same codec.
    Arcane::Remote::Envelope getErr;
    const auto got = Servitor::GetJson(good.descriptor, "/remote/v1/status", 5000, &getErr);
    REQUIRE(got.has_value());
    CHECK((*got)["state"] == "ready");
    CHECK(got->dump().find(token) == std::string::npos);

    auto wrong = DiscoverTarget(dir, port, std::string(64, 'a'));
    const auto denied = DiscoverExchange(wrong, body, nullptr);
    CHECK_FALSE(denied.connectionRefusedBeforeBody);
    REQUIRE_FALSE(denied.envelope.errors.empty());
    CHECK(denied.envelope.errors[0].code == "auth.denied");   // HTTP 401; the envelope decides
    CHECK(Arcane::Remote::ExitFromHost(denied.envelope) == 3);

    listener.Stop();
    auto gone = DiscoverTarget(dir, port, token);
    const auto refused = DiscoverExchange(gone, body, nullptr);
    CHECK(refused.connectionRefusedBeforeBody);
    REQUIRE_FALSE(refused.envelope.errors.empty());       // a real envelope with a code
    CHECK_FALSE(refused.envelope.errors[0].code.empty());
    CHECK(Arcane::Remote::ExitFromHost(refused.envelope) == 5);
    fs::remove_all(dir);
}

TEST_CASE("LiveExchange answers host.died before connecting when the watched process is gone", "[servitor][client]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("servitor-live-died-" + std::to_string(Arcane::EditorLock::Self().pid));
    fs::remove_all(dir);
    auto target = DiscoverTarget(dir, 1, "t");
    target.watch.processStart += 1;   // not this process any more
    const auto r = DiscoverExchange(target, R"({"id":"d","command":"discover.noop","args":{}})", nullptr);
    CHECK_FALSE(r.connectionRefusedBeforeBody);
    REQUIRE_FALSE(r.envelope.errors.empty());
    CHECK(r.envelope.errors[0].code == "host.died");
    CHECK(Arcane::Remote::ExitFromHost(r.envelope) == 7);
    fs::remove_all(dir);
}

// --- Run's flag rules (Task 14 review carries) ------------------------------

TEST_CASE("a value flag refuses a value that is itself a flag", "[servitor][client]") {
    std::vector<std::string> words = {"servitor", "--project", "--json", "status"};
    std::vector<char*> argv;
    for (auto& w : words)
        argv.push_back(w.data());
    Servitor::GlobalOptions opt;
    std::string error;
    CHECK_FALSE(Servitor::ParseGlobals(static_cast<int>(argv.size()), argv.data(), opt, &error));
    CHECK(error.find("--project") != std::string::npos);

    std::vector<std::string> negative = {"servitor", "--engine", "-x", "status"};   // a single '-' is a value
    argv.clear();
    for (auto& w : negative)
        argv.push_back(w.data());
    CHECK(Servitor::ParseGlobals(static_cast<int>(argv.size()), argv.data(), opt, &error));
    CHECK(opt.engine == "-x");
}

TEST_CASE("a leftover --flag after the verb is refused with usage exit 2", "[servitor][client]") {
    const std::vector<std::string> flagged = {"--bogus"};
    const auto problem = Servitor::LeftoverFlag(flagged);
    REQUIRE(problem.has_value());
    CHECK(problem->find("--bogus") != std::string::npos);
    const std::vector<std::string> negative = {"-1", "name=value"};
    CHECK_FALSE(Servitor::LeftoverFlag(negative).has_value());

    std::vector<std::string> words = {"servitor", "status", "--bogus"};
    std::vector<char*> argv;
    for (auto& w : words)
        argv.push_back(w.data());
    CHECK(Servitor::Run(static_cast<int>(argv.size()), argv.data()) == 2);
}

#endif
