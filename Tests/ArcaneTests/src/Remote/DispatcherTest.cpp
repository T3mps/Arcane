#include <catch2/catch_test_macros.hpp>
#include <Arcane/Crypto/Crypto.hpp>
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <Arcane/Remote/Http.hpp> // MakeToken
#include "../Helpers/SettingsSweep.hpp" // Arcane::Test::ScopedCodeRung
#include "RemoteDemoArgs.hpp"
#include "RemoteTestHttp.hpp"

#if !defined(ARC_BUILD_DIST)

static int g_ran = 0;
static Arcane::Remote::CommandResult Boom(const Arcane::Remote::Call& call, void*) {
    ++g_ran;
    Arcane::Remote::CommandResult r;
    r.envelope = Arcane::Remote::MakeOk(call.command, nlohmann::json::object(), Arcane::Remote::HostBlock{});
    return r;
}

static Arcane::Remote::Desc MakeDesc(const char* name, Arcane::Remote::Access access,
                                  Arcane::Remote::Mutates mutates, Arcane::Remote::PlayPolicy play) {
    Arcane::Remote::Desc d;
    d.name = name;
    d.summary = "dispatch test";
    d.access = access;
    d.mutates = mutates;
    d.play = play;
    d.module = "dispatch-test";
    d.argsMeta = Astra::GetMeta<RemoteDemoArgs>();
    d.fn = &Boom;
    return d;
}

TEST_CASE("a busy gate does not run the command", "[remote][dispatch]") {
    g_ran = 0;
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    std::string err;
    REQUIRE(reg.Register(MakeDesc("demo.boom", Arcane::Remote::Access::Drive,
                                  Arcane::Remote::Mutates::PlayMode, Arcane::Remote::PlayPolicy::PlayingOnly), &err));
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    gate.SetState("ready");
    Arcane::Remote::JobTable jobs;
    const auto audit = std::filesystem::temp_directory_path() / "remote-dispatch-audit" / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);
    Arcane::Remote::Call call;
    call.command = "demo.boom";
    REQUIRE(queue.TryPush(1, call));
    dispatcher.Drain(1000.0, 8);
    CHECK(g_ran == 0);
    Arcane::Remote::CommandResult done;
    REQUIRE(queue.Wait(1, done, std::chrono::milliseconds(1000)));
    CHECK(Arcane::Remote::ExitFromHost(done.envelope) == 5);
    CHECK(done.envelope.errors[0].code == "host.busy");
    CHECK(done.envelope.errors[0].detail["state"] == "ready");
    CHECK_FALSE(done.envelope.data.contains("hostFrame"));
    gate.SetState("playing");
    REQUIRE(queue.TryPush(2, call));
    dispatcher.Drain(1000.0, 8);
    CHECK(g_ran == 1);
    reg.UnregisterModule("dispatch-test");
}

TEST_CASE("read-only refuses Drive and still runs Observe", "[remote][dispatch]") {
    g_ran = 0;
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    std::string err;
    REQUIRE(reg.Register(MakeDesc("demo.drive", Arcane::Remote::Access::Drive,
                                  Arcane::Remote::Mutates::Preferences, Arcane::Remote::PlayPolicy::Any), &err));
    REQUIRE(reg.Register(MakeDesc("demo.look", Arcane::Remote::Access::Observe,
                                  Arcane::Remote::Mutates::None, Arcane::Remote::PlayPolicy::Any), &err));
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    gate.readOnly = true;
    Arcane::Remote::JobTable jobs;
    const auto audit = std::filesystem::temp_directory_path() / "remote-dispatch-ro" / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);
    Arcane::Remote::Call drive;
    drive.command = "demo.drive";
    Arcane::Remote::Call look;
    look.command = "demo.look";
    REQUIRE(queue.TryPush(1, drive));
    REQUIRE(queue.TryPush(2, look));
    dispatcher.Drain(1000.0, 8);
    Arcane::Remote::CommandResult denied;
    Arcane::Remote::CommandResult allowed;
    REQUIRE(queue.Wait(1, denied, std::chrono::milliseconds(1000)));
    REQUIRE(queue.Wait(2, allowed, std::chrono::milliseconds(1000)));
    CHECK(Arcane::Remote::ExitFromHost(denied.envelope) == 3);
    CHECK(denied.envelope.errors[0].code == "permission.read-only");
    CHECK(allowed.envelope.ok);
    CHECK(g_ran == 1);
    reg.UnregisterModule("dispatch-test");
}

TEST_CASE("budget stops at 8 commands", "[remote][dispatch]") {
    g_ran = 0;
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    std::string err;
    REQUIRE(reg.Register(MakeDesc("demo.look", Arcane::Remote::Access::Observe,
                                  Arcane::Remote::Mutates::None, Arcane::Remote::PlayPolicy::Any), &err));
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    Arcane::Remote::JobTable jobs;
    const auto audit = std::filesystem::temp_directory_path() / "remote-dispatch-budget" / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);
    for (std::uint64_t i = 1; i <= 10; ++i) {
        Arcane::Remote::Call call;
        call.command = "demo.look";
        REQUIRE(queue.TryPush(i, call));
    }
    dispatcher.Drain(1000.0, 8);
    CHECK(dispatcher.LastDrainRan() == 8);
    CHECK(g_ran == 8);
    dispatcher.Drain(1000.0, 8);
    CHECK(g_ran == 10);
    reg.UnregisterModule("dispatch-test");
}

TEST_CASE("Drive audit stores argsDigest and not the args", "[remote][dispatch]") {
    g_ran = 0;
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    std::string err;
    REQUIRE(reg.Register(MakeDesc("demo.drive", Arcane::Remote::Access::Drive,
                                  Arcane::Remote::Mutates::Preferences, Arcane::Remote::PlayPolicy::Any), &err));
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    gate.pid = 42;
    Arcane::Remote::JobTable jobs;
    const auto audit = std::filesystem::temp_directory_path() / "remote-dispatch-audit-body" / "audit.ndjson";
    std::filesystem::remove(audit);
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);
    Arcane::Remote::Call call;
    call.command = "demo.drive";
    call.callerId = "remote-test";
    call.args = nlohmann::json{{"secret", "hunter2"}};
    REQUIRE(queue.TryPush(1, call));
    dispatcher.Drain(1000.0, 8);
    std::ifstream in(audit);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(body.find("argsDigest") != std::string::npos);
    CHECK(body.find("\"command\":\"demo.drive\"") != std::string::npos);
    CHECK(body.find("\"pid\":42") != std::string::npos);
    CHECK(body.find("hunter2") == std::string::npos);
    reg.UnregisterModule("dispatch-test");
}

static int g_steps = 0;
static bool StepOnce(Arcane::Remote::Job& job, Arcane::Remote::HostGate& gate, void*) {
    ++g_steps;
    gate.frame.fetch_add(1);
    job.progress.done += 1;
    job.envelope = Arcane::Remote::MakeOk(job.command, nlohmann::json{{"done", job.progress.done}}, gate.Block());
    return job.progress.done >= job.progress.total;
}

TEST_CASE("one Drain steps a frame job once and still runs a queued command", "[remote][dispatch]") {
    g_ran = 0;
    g_steps = 0;
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    std::string err;
    REQUIRE(reg.Register(MakeDesc("demo.look", Arcane::Remote::Access::Observe,
                                  Arcane::Remote::Mutates::None, Arcane::Remote::PlayPolicy::Any), &err));
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    Arcane::Remote::JobTable jobs;
    const auto audit = std::filesystem::temp_directory_path() / "remote-dispatch-job" / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);
    const auto id = jobs.Arm("play.step", 10, &StepOnce, nullptr);
    Arcane::Remote::Call look;
    look.command = "demo.look";
    REQUIRE(queue.TryPush(1, look));
    dispatcher.Drain(1000.0, 8);
    CHECK(g_steps == 1);
    CHECK(g_ran == 1);
    CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Running);
    CHECK(gate.frame.load() == 1);
    for (int i = 0; i < 9; ++i)
        dispatcher.Drain(1000.0, 8);
    CHECK(g_steps == 10);
    CHECK(gate.frame.load() == 10);
    CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Done);
    dispatcher.Drain(1000.0, 8);
    CHECK(g_steps == 10);
    reg.UnregisterModule("dispatch-test");
}

// Drops the module even when a REQUIRE aborts the case (ruling S3-9).
struct RemoteUnregisterOnExit {
    const char* module;
    ~RemoteUnregisterOnExit() { Arcane::Remote::CommandRegistry::Get().UnregisterModule(module); }
};

static bool ThreeSteps(Arcane::Remote::Job& job, Arcane::Remote::HostGate&, void*) {
    job.progress.done += 1;
    job.envelope = Arcane::Remote::MakeOk(job.command, nlohmann::json{{"done", job.progress.done}},
                                       Arcane::Remote::HostBlock{});
    return job.progress.done >= job.progress.total;
}

static Arcane::Remote::CommandResult ArmThree(const Arcane::Remote::Call& call, void* user) {
    auto* jobs = static_cast<Arcane::Remote::JobTable*>(user);
    const Arcane::Remote::JobId id = jobs->Arm(call.command, 3, &ThreeSteps, nullptr);
    Arcane::Remote::CommandResult r;
    r.envelope = Arcane::Remote::MakeOk(call.command, nlohmann::json{{"job", id.value}}, Arcane::Remote::HostBlock{});
    Arcane::Remote::UndoBlock undo;
    undo.reason = "runtime-host";
    r.envelope.undo = undo;
    r.completesJob = id.value;
    return r;
}

TEST_CASE("a Job-thread command's waiter completes after its job's last drain", "[remote][dispatch]") {
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    RemoteUnregisterOnExit unreg{"dispatch-test"};
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::Desc slow = MakeDesc("demo.slow", Arcane::Remote::Access::Observe,
                                      Arcane::Remote::Mutates::None, Arcane::Remote::PlayPolicy::Any);
    slow.thread = Arcane::Remote::ExecThread::Job;
    slow.fn = &ArmThree;
    slow.user = &jobs;
    std::string err;
    REQUIRE(reg.Register(slow, &err));
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    gate.kind = "runtime";
    const auto audit = std::filesystem::temp_directory_path() / "remote-dispatch-waiter" / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit); // calls jobs.SetQueue(&queue)
    Arcane::Remote::Call call;
    call.command = "demo.slow";
    REQUIRE(queue.TryPush(1, call));
    Arcane::Remote::CommandResult out;
    dispatcher.Drain(1000.0, 8);           // invokes: arms the job, binds waiter 1, no Complete
    CHECK_FALSE(queue.TryTake(1, out));
    dispatcher.Drain(1000.0, 8);           // step 1
    dispatcher.Drain(1000.0, 8);           // step 2
    CHECK_FALSE(queue.TryTake(1, out));
    gate.frame.store(7);
    dispatcher.Drain(1000.0, 8);           // step 3 finishes: StepAll Completes waiter 1
    REQUIRE(queue.TryTake(1, out));
    CHECK(out.envelope.ok);
    CHECK(out.envelope.data["done"] == 3);
    CHECK(out.envelope.host.kind == "runtime");   // stamped from gate.Block() at finish
    CHECK(out.envelope.host.frame == 7);
    REQUIRE(out.envelope.undo.has_value());       // the armed envelope's undo block rides through
    CHECK(out.envelope.undo->reason == "runtime-host");
}

TEST_CASE("a request-level readOnly refuses Drive", "[remote][dispatch]") {
    g_ran = 0;
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    RemoteUnregisterOnExit unreg{"dispatch-test"};
    std::string err;
    REQUIRE(reg.Register(MakeDesc("demo.drive", Arcane::Remote::Access::Drive,
                                  Arcane::Remote::Mutates::Preferences, Arcane::Remote::PlayPolicy::Any), &err));
    REQUIRE(reg.Register(MakeDesc("demo.look", Arcane::Remote::Access::Observe,
                                  Arcane::Remote::Mutates::None, Arcane::Remote::PlayPolicy::Any), &err));
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;          // the host itself is NOT read-only
    Arcane::Remote::JobTable jobs;
    const auto audit = std::filesystem::temp_directory_path() / "remote-dispatch-callro" / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);
    Arcane::Remote::Call drive;
    drive.command = "demo.drive";
    drive.readOnly = true;               // options.readOnly on the wire (ruling F15)
    Arcane::Remote::Call look;
    look.command = "demo.look";
    look.readOnly = true;
    REQUIRE(queue.TryPush(1, drive));
    REQUIRE(queue.TryPush(2, look));
    dispatcher.Drain(1000.0, 8);
    Arcane::Remote::CommandResult denied;
    Arcane::Remote::CommandResult allowed;
    REQUIRE(queue.TryTake(1, denied));
    REQUIRE(queue.TryTake(2, allowed));
    CHECK(denied.envelope.errors[0].code == "permission.read-only");
    CHECK(Arcane::Remote::ExitFromHost(denied.envelope) == 3);
    CHECK(allowed.envelope.ok);
    CHECK(g_ran == 1);
}

static Arcane::Remote::Access DriveWhenBless(const Arcane::Remote::Call& call) {
    return call.args.value("bless", false) ? Arcane::Remote::Access::Drive : Arcane::Remote::Access::Observe;
}

TEST_CASE("accessFor makes a bless Drive: read-only refuses it and the audit records it", "[remote][dispatch]") {
    g_ran = 0;
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    RemoteUnregisterOnExit unreg{"dispatch-test"};
    Arcane::Remote::Desc compare = MakeDesc("demo.compare", Arcane::Remote::Access::Observe,
                                         Arcane::Remote::Mutates::Project, Arcane::Remote::PlayPolicy::Any);
    compare.accessFor = &DriveWhenBless;   // ruling F19
    std::string err;
    REQUIRE(reg.Register(compare, &err));
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    gate.readOnly = true;
    Arcane::Remote::JobTable jobs;
    const auto dir = std::filesystem::temp_directory_path() / "remote-dispatch-bless";
    std::filesystem::remove_all(dir);
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, dir / "audit.ndjson");
    Arcane::Remote::Call bless;
    bless.command = "demo.compare";
    bless.args = nlohmann::json{{"bless", true}};
    Arcane::Remote::Call plain;
    plain.command = "demo.compare";
    plain.args = nlohmann::json{{"bless", false}};
    REQUIRE(queue.TryPush(1, bless));
    REQUIRE(queue.TryPush(2, plain));
    dispatcher.Drain(1000.0, 8);
    Arcane::Remote::CommandResult refused;
    Arcane::Remote::CommandResult ran;
    REQUIRE(queue.TryTake(1, refused));
    REQUIRE(queue.TryTake(2, ran));
    CHECK(refused.envelope.errors[0].code == "permission.read-only");
    CHECK(ran.envelope.ok);
    CHECK(g_ran == 1);
    // Not read-only: the bless runs and is audited as Drive; the plain compare is not audited.
    gate.readOnly = false;
    REQUIRE(queue.TryPush(3, bless));
    REQUIRE(queue.TryPush(4, plain));
    dispatcher.Drain(1000.0, 8);
    CHECK(g_ran == 3);
    std::ifstream in(dir / "audit.ndjson");
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t lines = 0;
    for (char c : body)
        lines += c == '\n' ? 1u : 0u;
    CHECK(lines == 1);
    CHECK(body.find("\"command\":\"demo.compare\"") != std::string::npos);
}

// --- Cases beyond the brief (Task 8 implementer) ---

TEST_CASE("Sha256Hex matches the FIPS 180-2 vectors", "[remote][dispatch]") {
    CHECK(Arcane::Crypto::Sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Arcane::Crypto::Sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("a drained envelope carries the call id, the host block and its queue time", "[remote][dispatch]") {
    g_ran = 0;
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    RemoteUnregisterOnExit unreg{"dispatch-test"};
    std::string err;
    REQUIRE(reg.Register(MakeDesc("demo.look", Arcane::Remote::Access::Observe,
                                  Arcane::Remote::Mutates::None, Arcane::Remote::PlayPolicy::Any), &err));
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    gate.kind = "server";
    gate.frame.store(11);
    Arcane::Remote::JobTable jobs;
    const auto audit = std::filesystem::temp_directory_path() / "remote-dispatch-timing" / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);
    Arcane::Remote::Call look;
    look.id = "req-7";
    look.command = "demo.look";
    REQUIRE(queue.TryPush(1, look));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    dispatcher.Drain(1000.0, 8);
    Arcane::Remote::CommandResult out;
    REQUIRE(queue.TryTake(1, out));
    CHECK(out.envelope.ok);
    CHECK(out.envelope.id == "req-7");
    CHECK(out.envelope.command == "demo.look");
    CHECK(out.envelope.host.kind == "server");
    CHECK(out.envelope.host.frame == 11);
    CHECK(out.envelope.timing.queuedMs >= 20.0);   // pushed 30 ms before the drain popped it
    CHECK(out.envelope.timing.ranMs >= 0.0);
}

TEST_CASE("construction prunes the out directory to the newest 200 files", "[remote][dispatch]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "remote-dispatch-prune";
    fs::remove_all(dir);
    REQUIRE(fs::create_directories(dir / "out"));
    const auto base = fs::file_time_type::clock::now() - std::chrono::hours(1);
    for (int i = 0; i < 205; ++i) {
        const fs::path file = dir / "out" / ("shot-" + std::to_string(i) + ".png");
        std::ofstream(file) << i;
        fs::last_write_time(file, base + std::chrono::seconds(i));   // file 0 is the oldest
    }
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::RemoteDispatcher dispatcher(queue, Arcane::Remote::CommandRegistry::Get(), gate, jobs,
                                                dir / "audit.ndjson");
    std::size_t left = 0;
    for (const auto& entry : fs::directory_iterator(dir / "out")) {
        (void)entry;
        ++left;
    }
    CHECK(left == 200);
    for (int i = 0; i < 5; ++i)
        CHECK_FALSE(fs::exists(dir / "out" / ("shot-" + std::to_string(i) + ".png")));
    CHECK(fs::exists(dir / "out" / "shot-5.png"));
    CHECK(fs::exists(dir / "out" / "shot-204.png"));
    fs::remove_all(dir);
}

// --- Fix round 1 (review of 2b9901f1) ---

TEST_CASE("a job-completed parked answer carries the request id and non-zero timing", "[remote][dispatch]") {
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    RemoteUnregisterOnExit unreg{"dispatch-test"};
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::Desc slow = MakeDesc("demo.slow", Arcane::Remote::Access::Observe,
                                      Arcane::Remote::Mutates::None, Arcane::Remote::PlayPolicy::Any);
    slow.thread = Arcane::Remote::ExecThread::Job;
    slow.fn = &ArmThree;
    slow.user = &jobs;
    std::string err;
    REQUIRE(reg.Register(slow, &err));

    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    auto snap = std::make_shared<Arcane::Remote::HostStatus>();
    snap->state = "ready";
    atoms.status.store(snap);
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    REQUIRE(listener.Port() != 0);
    Arcane::Remote::HostGate gate;
    const auto audit = std::filesystem::temp_directory_path() / "remote-dispatch-parked" / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);

    const std::string payload = R"({"id":"r-job","command":"demo.slow","args":{}})";
    const SocketType sock = RemoteTest::ConnectLoopback(listener.Port());
    RemoteTest::SendAll(sock, "POST /remote/v1/call HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(listener.Port()) +
        "\r\nAuthorization: Bearer " + listener.Token() + "\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(payload.size()) + "\r\n\r\n" + payload);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (queue.Depth() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(queue.Depth() == 1);

    // Drain 1 arms the job and binds the waiter; drains 2-4 step it to Done,
    // and StepAll Completes the parked key with the job's own envelope.
    for (int i = 0; i < 4; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        dispatcher.Drain(1000.0, 8);
    }
    const std::string raw = RemoteTest::RecvUntil(sock, "\x01", std::chrono::seconds(3)); // until the listener closes
    CloseSocket(sock);
    listener.Stop();

    const auto at = raw.find("\r\n\r\n");
    REQUIRE(at != std::string::npos);
    CHECK(raw.find("HTTP/1.1 200") != std::string::npos);
    const Arcane::Remote::Envelope env = Arcane::Remote::ParseEnvelope(nlohmann::json::parse(raw.substr(at + 4)));
    CHECK(env.ok);
    CHECK(env.data["done"] == 3);
    CHECK(env.id == "r-job");
    CHECK(env.timing.queuedMs > 0.0);   // push -> the arming drain's pop
    CHECK(env.timing.ranMs > 0.0);      // pop -> the third step's Complete (three drains, 5 ms apart)
}

TEST_CASE("a non-positive remote.server.frameBudgetMs still drains", "[remote][dispatch]") {
    g_ran = 0;
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    RemoteUnregisterOnExit unreg{"dispatch-test"};
    std::string err;
    REQUIRE(reg.Register(MakeDesc("demo.look", Arcane::Remote::Access::Observe,
                                  Arcane::Remote::Mutates::None, Arcane::Remote::PlayPolicy::Any), &err));
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    Arcane::Remote::JobTable jobs;
    const auto audit = std::filesystem::temp_directory_path() / "remote-dispatch-zero-budget" / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);
    Arcane::Remote::Call look;
    look.command = "demo.look";
    {
        Arcane::Test::ScopedCodeRung zero("remote.server.frameBudgetMs", Arcane::CVarValue::Float64(0.0));
        CHECK(Arcane::Remote::FrameBudgetMsSetting() == 0.0);
        REQUIRE(queue.TryPush(1, look));
        dispatcher.Drain(0.0, 0);   // both budgets from settings: 0 ms reads as kFrameBudgetMs
        CHECK(dispatcher.LastDrainRan() == 1);
        Arcane::Remote::CommandResult out;
        REQUIRE(queue.TryTake(1, out));
        CHECK(out.envelope.ok);
    }
    {
        Arcane::Test::ScopedCodeRung negative("remote.server.frameBudgetMs", Arcane::CVarValue::Float64(-5.0));
        REQUIRE(queue.TryPush(2, look));
        dispatcher.Drain(0.0, 0);
        Arcane::Remote::CommandResult out;
        CHECK(queue.TryTake(2, out));
    }
    CHECK(g_ran == 2);
}

TEST_CASE("a zero remote.server.queueDepth still queues one call", "[remote][dispatch]") {
    g_ran = 0;
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    RemoteUnregisterOnExit unreg{"dispatch-test"};
    std::string err;
    REQUIRE(reg.Register(MakeDesc("demo.look", Arcane::Remote::Access::Observe,
                                  Arcane::Remote::Mutates::None, Arcane::Remote::PlayPolicy::Any), &err));
    Arcane::Test::ScopedCodeRung zero("remote.server.queueDepth", Arcane::CVarValue::UInt32(0));
    CHECK(Arcane::Remote::QueueDepthSetting() == 0u);
    Arcane::Remote::RequestQueue queue(Arcane::Remote::QueueDepthSetting());   // as HostSession sizes it
    Arcane::Remote::Call look;
    look.command = "demo.look";
    REQUIRE(queue.TryPush(1, look));
    CHECK_FALSE(queue.TryPush(2, look));   // clamped to one, not unbounded
    Arcane::Remote::HostGate gate;
    Arcane::Remote::JobTable jobs;
    const auto audit = std::filesystem::temp_directory_path() / "remote-dispatch-zero-depth" / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);
    dispatcher.Drain(1000.0, 8);
    Arcane::Remote::CommandResult out;
    REQUIRE(queue.TryTake(1, out));
    CHECK(out.envelope.ok);
    CHECK(g_ran == 1);
    REQUIRE(queue.TryPush(3, look));   // room again once drained
}

// --- Task 19 fix round 1: the dispatcher stamps the call's command ---

static Arcane::Remote::CommandResult Nameless(const Arcane::Remote::Call&, void*) {
    Arcane::Remote::CommandResult r;
    r.envelope = Arcane::Remote::MakeError({}, "arg.unknown", "a helper's refusal names no command", "fix it");
    return r;
}

TEST_CASE("a drained envelope with no command is stamped with the call's", "[remote][dispatch]") {
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    RemoteUnregisterOnExit unreg{"dispatch-test"};
    Arcane::Remote::Desc nameless = MakeDesc("demo.nameless", Arcane::Remote::Access::Observe,
                                             Arcane::Remote::Mutates::None, Arcane::Remote::PlayPolicy::Any);
    nameless.fn = &Nameless;
    std::string err;
    REQUIRE(reg.Register(nameless, &err));
    REQUIRE(reg.Register(MakeDesc("demo.look", Arcane::Remote::Access::Observe,
                                  Arcane::Remote::Mutates::None, Arcane::Remote::PlayPolicy::Any), &err));
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    Arcane::Remote::JobTable jobs;
    const auto audit = std::filesystem::temp_directory_path() / "remote-dispatch-stamp" / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);
    Arcane::Remote::Call call;
    call.command = "demo.nameless";
    REQUIRE(queue.TryPush(1, call));
    call.command = "demo.look";
    REQUIRE(queue.TryPush(2, call));
    dispatcher.Drain(1000.0, 8);
    Arcane::Remote::CommandResult stamped;
    REQUIRE(queue.TryTake(1, stamped));
    CHECK(stamped.envelope.command == "demo.nameless");
    CHECK(stamped.envelope.errors[0].code == "arg.unknown");
    Arcane::Remote::CommandResult own;
    REQUIRE(queue.TryTake(2, own));
    CHECK(own.envelope.command == "demo.look");   // a command that names itself keeps its name
}

#endif
