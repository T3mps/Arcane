#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Commands/CaptureCommands.hpp>
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include "RemoteDemoArgs.hpp"
#include "RemoteTestSupport.hpp"
#include <cstdint>
#include <filesystem>
#include <vector>

#if !defined(ARC_BUILD_DIST)

// Installs one case's hooks and JobTable* and clears both on scope exit, so a
// later case never sees this case's stack objects (rulings F11, S3-9).
struct RemoteCaptureScope {
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::CaptureHooks> hooks;
    RemoteTest::ScopedRemotePointer<Arcane::Remote::JobTable> jobs;
    RemoteCaptureScope(const Arcane::Remote::CaptureHooks& h, Arcane::Remote::JobTable& j)
        : hooks([](const Arcane::Remote::CaptureHooks* p) {
                    Arcane::Remote::SetCaptureHooks(p ? *p : Arcane::Remote::CaptureHooks{});
                }, &h),
          jobs(&Arcane::Remote::RegisterCaptureCommands, &j) {}
};

static const std::uint8_t kProbe[12] = {10, 20, 30, 255, 1, 2, 3, 40, 9, 9, 9, 9};
static const std::uint8_t kProbeB[12] = {40, 50, 60, 255, 7, 8, 9, 40, 9, 9, 9, 9};

struct CapUser {
    int copies = 0;
    int reads = 0;
    int idles = 0;
    std::uint64_t armedTicket = 0;
    std::uint64_t wanted = 0;
    bool retired = false;
    bool failMap = false;
};

static bool ArmSlot(std::uint64_t ticket, void* user) {
    auto* u = static_cast<CapUser*>(user);
    ++u->copies;
    u->armedTicket = ticket;
    u->wanted = ticket;
    return true;
}
static bool DrainSlot(std::uint64_t ticket, Arcane::Remote::CaptureMapped* raw, void* user) {
    auto* u = static_cast<CapUser*>(user);
    if (!u->retired || ticket != u->armedTicket)
        return false;
    raw->width = 2;
    raw->height = 1;
    raw->rowPitch = 12;
    raw->bgra = true;
    if (u->failMap) {
        raw->mapped = nullptr;
        raw->mappedBytes = 0;
        return true;
    }
    ++u->reads;
    raw->mapped = kProbe;
    raw->mappedBytes = sizeof(kProbe);
    return true;
}
static Arcane::Remote::CommandResult Ping(const Arcane::Remote::Call& call, void*) {
    Arcane::Remote::CommandResult r;
    r.envelope = Arcane::Remote::MakeOk(call.command, nlohmann::json::object(), {});
    return r;
}
static bool ArmNo(std::uint64_t, void* user) {
    ++static_cast<CapUser*>(user)->copies;
    return false;
}

TEST_CASE("NormalizeCaptureRows drops padding and swaps BGRA", "[remote][capture]") {
    std::vector<unsigned char> rgba;
    Arcane::NormalizeCaptureRows(kProbe, 2, 1, 12, true, rgba);
    REQUIRE(rgba.size() == 8);
    CHECK(rgba[0] == 30);
    CHECK(rgba[1] == 20);
    CHECK(rgba[2] == 10);
    CHECK(rgba[3] == 255);
    CHECK(rgba[4] == 3);
    CHECK(rgba[5] == 2);
    CHECK(rgba[6] == 1);
    CHECK(rgba[7] == 40);
}

TEST_CASE("the copy step reads nothing and the job completes when the ticket is published", "[remote][capture]") {
    CapUser user;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::CaptureHooks hooks;
    hooks.framesInFlight = 2;
    hooks.armWindow = &ArmSlot;
    hooks.drainWindow = &DrainSlot;
    hooks.user = &user;
    hooks.outDir = std::filesystem::temp_directory_path() / "remote-cap";
    std::filesystem::create_directories(hooks.outDir);
    RemoteCaptureScope scope(hooks, jobs);
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    std::string err;
    Arcane::Remote::Desc ping;
    ping.name = "cap.ping";
    ping.summary = "observe while a capture is in flight";
    ping.access = Arcane::Remote::Access::Observe;
    ping.module = "cap-test";
    ping.argsMeta = Astra::GetMeta<RemoteDemoArgs>();
    ping.fn = &Ping;
    ping.needsFrame = false;
    REQUIRE(reg.Register(ping, &err));
    Arcane::Remote::Call call;
    call.command = "capture.window";
    call.args = nlohmann::json::object();
    const auto armed = reg.Invoke(call);
    REQUIRE(armed.completesJob != 0);
    CHECK(user.copies == 0);
    CHECK(user.reads == 0);
    CHECK(user.idles == 0);
    Arcane::Remote::HostGate gate;
    gate.frame = 10;
    gate.minimized = false;
    jobs.StepAll(gate);
    const auto id = Arcane::Remote::JobId{armed.completesJob};
    REQUIRE(jobs.Find(id) != nullptr);
    CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Running);
    CHECK(jobs.Find(id)->progress.label == "copied");
    CHECK(jobs.Find(id)->startedFrame == 0);
    CHECK(gate.frame == 10);
    CHECK(user.copies == 1);
    CHECK(user.armedTicket != 0);
    CHECK(user.reads == 0);
    CHECK(user.idles == 0);
    Arcane::Remote::RequestQueue queue;
    const auto audit = hooks.outDir / "audit.ndjson";
    Arcane::Remote::RemoteDispatcher dispatcher(queue, reg, gate, jobs, audit);
    Arcane::Remote::Call pingCall;
    pingCall.command = "cap.ping";
    REQUIRE(queue.TryPush(2, pingCall));
    dispatcher.Drain(1000.0, 8);
    Arcane::Remote::CommandResult pinged;
    REQUIRE(queue.Wait(2, pinged, std::chrono::milliseconds(1000)));
    CHECK(pinged.envelope.ok);
    CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Running);
    CHECK(user.copies == 1);
    CHECK(user.reads == 0);
    CHECK(user.idles == 0);
    CHECK(gate.frame == 10);
    jobs.StepAll(gate);
    CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Running);
    CHECK(user.reads == 0);
    CHECK(user.copies == 1);
    CHECK(user.idles == 0);
    CHECK(gate.frame == 10);
    user.retired = true;
    jobs.StepAll(gate);
    REQUIRE(jobs.Find(id) != nullptr);
    CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Done);
    CHECK(jobs.Find(id)->envelope.ok);
    CHECK(user.reads == 1);
    CHECK(user.idles == 0);
    CHECK(gate.frame == 10);
    const auto& art = jobs.Find(id)->envelope.artifacts[0];
    CHECK(art.format == "RGBA8_UNORM");
    CHECK(art.width == 2);
    CHECK(art.height == 1);
    CHECK(art.sha256.size() == 64);
    std::uint32_t w = 0, h = 0;
    std::vector<unsigned char> png;
    REQUIRE(Arcane::LoadPngRgba(art.path, w, h, png));
    CHECK(w == 2);
    CHECK(h == 1);
    REQUIRE(png.size() == 8);
    CHECK(png[0] == 30);
    CHECK(png[1] == 20);
    CHECK(png[2] == 10);
    CHECK(png[3] == 255);
    CHECK(png[4] == 3);
    CHECK(png[5] == 2);
    CHECK(png[6] == 1);
    CHECK(png[7] == 40);
    reg.UnregisterModule("cap-test");
}

struct OverlapUser {
    int copies = 0;
    int reads = 0;
    int frames = 0;
    int framesInFlight = 2;
    std::uint64_t wanted = 0;
    std::uint64_t ticket[2]{};
    int retireFrame[2]{-1, -1};
};

static bool ArmOverlap(std::uint64_t ticket, void* user) {
    auto* u = static_cast<OverlapUser*>(user);
    REQUIRE(u->copies < 2);
    u->ticket[u->copies] = ticket;
    u->wanted = ticket;
    u->retireFrame[u->copies] = u->frames + u->framesInFlight;
    ++u->copies;
    return true;
}

static bool DrainOverlap(std::uint64_t ticket, Arcane::Remote::CaptureMapped* raw, void* user) {
    auto* u = static_cast<OverlapUser*>(user);
    int slot = -1;
    if (u->copies > 0 && ticket == u->ticket[0])
        slot = 0;
    else if (u->copies > 1 && ticket == u->ticket[1])
        slot = 1;
    if (slot < 0 || u->frames < u->retireFrame[slot])
        return false;
    raw->width = 2;
    raw->height = 1;
    raw->rowPitch = 12;
    raw->bgra = true;
    raw->mapped = slot == 0 ? kProbe : kProbeB;
    raw->mappedBytes = 12;
    ++u->reads;
    return true;
}

static void ExpectPng(const Arcane::Remote::Artifact& art, unsigned char r, unsigned char g,
                       unsigned char b) {
    CHECK(art.format == "RGBA8_UNORM");
    CHECK(art.width == 2);
    CHECK(art.height == 1);
    CHECK(art.sha256.size() == 64);
    std::uint32_t w = 0, h = 0;
    std::vector<unsigned char> png;
    REQUIRE(Arcane::LoadPngRgba(art.path, w, h, png));
    CHECK(w == 2);
    CHECK(h == 1);
    REQUIRE(png.size() == 8);
    CHECK(png[0] == r);
    CHECK(png[1] == g);
    CHECK(png[2] == b);
    CHECK(png[3] == 255);
}

TEST_CASE("overlapping captures: B is capture-pending until A's ticket retires, then the retry succeeds", "[remote][capture]") {
    OverlapUser user;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::CaptureHooks hooks;
    hooks.framesInFlight = 2;
    hooks.armViewport = &ArmOverlap;
    hooks.drainViewport = &DrainOverlap;
    hooks.armWindow = &ArmOverlap;
    hooks.drainWindow = &DrainOverlap;
    hooks.user = &user;
    hooks.outDir = std::filesystem::temp_directory_path() / "remote-cap-busy";
    std::filesystem::create_directories(hooks.outDir);
    RemoteCaptureScope scope(hooks, jobs);
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    Arcane::Remote::HostGate gate;
    gate.frame = 10;

    Arcane::Remote::Call captureA;
    captureA.command = "capture.window";
    captureA.args = nlohmann::json::object();
    const auto armed = reg.Invoke(captureA);
    REQUIRE(armed.completesJob != 0);
    jobs.StepAll(gate);
    const auto idA = Arcane::Remote::JobId{armed.completesJob};
    REQUIRE(jobs.Find(idA) != nullptr);
    CHECK(jobs.Find(idA)->state == Arcane::Remote::JobState::Running);
    CHECK(user.copies == 1);
    CHECK(user.reads == 0);
    CHECK(user.frames == 0);
    CHECK(user.wanted != 0);
    const auto ticketA = user.wanted;
    CHECK(user.retireFrame[0] == 2);
    CHECK(gate.frame == 10);

    Arcane::Remote::Call captureB;
    captureB.command = "capture.viewport";
    captureB.args = nlohmann::json::object();
    const auto busy = reg.Invoke(captureB);
    CHECK(busy.completesJob == 0);
    CHECK_FALSE(busy.envelope.ok);
    CHECK(busy.envelope.errors[0].code == "host.busy");
    CHECK(busy.envelope.errors[0].detail["state"] == "capture-pending");
    CHECK(Arcane::Remote::ExitFromHost(busy.envelope) == 5);
    CHECK(user.copies == 1);
    CHECK(user.reads == 0);
    CHECK(user.wanted == ticketA);
    CHECK(jobs.Find(idA)->state == Arcane::Remote::JobState::Running);
    CHECK(gate.frame == 10);

    for (int i = 0; i < 2; ++i) {
        ++user.frames;
        jobs.StepAll(gate);
    }
    REQUIRE(jobs.Find(idA) != nullptr);
    CHECK(jobs.Find(idA)->state == Arcane::Remote::JobState::Done);
    CHECK(jobs.Find(idA)->envelope.ok);
    CHECK(user.reads == 1);
    CHECK(user.frames == 2);
    CHECK(user.wanted == ticketA);
    CHECK(user.copies == 1);
    CHECK(gate.frame == 10);
    ExpectPng(jobs.Find(idA)->envelope.artifacts[0], 30, 20, 10);

    const auto retried = reg.Invoke(captureB);
    REQUIRE(retried.completesJob != 0);
    jobs.StepAll(gate);
    const auto idB = Arcane::Remote::JobId{retried.completesJob};
    CHECK(jobs.Find(idB)->state == Arcane::Remote::JobState::Running);
    CHECK(user.copies == 2);
    CHECK(user.wanted != ticketA);
    CHECK(user.retireFrame[1] == user.frames + 2);
    CHECK(user.reads == 1);
    CHECK(gate.frame == 10);
    for (int i = 0; i < 2; ++i) {
        ++user.frames;
        jobs.StepAll(gate);
    }
    CHECK(jobs.Find(idB)->state == Arcane::Remote::JobState::Done);
    CHECK(jobs.Find(idB)->envelope.ok);
    CHECK(user.reads == 2);
    CHECK(user.frames == 4);
    CHECK(gate.frame == 10);
    ExpectPng(jobs.Find(idB)->envelope.artifacts[0], 60, 50, 40);
    ExpectPng(jobs.Find(idA)->envelope.artifacts[0], 30, 20, 10);
}

TEST_CASE("a server with null hooks returns capture.no-renderer and arms nothing", "[remote][capture]") {
    CapUser user;
    Arcane::Remote::JobTable jobs;
    const Arcane::Remote::CaptureHooks none{};   // a server: all six pointers null
    RemoteCaptureScope scope(none, jobs);
    Arcane::Remote::Call call;
    call.command = "capture.window";
    const auto result = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    CHECK(result.completesJob == 0);
    CHECK_FALSE(result.envelope.ok);
    CHECK(result.envelope.errors[0].code == "capture.no-renderer");
    CHECK(Arcane::Remote::ExitFromHost(result.envelope) == 2);
    CHECK(user.copies == 0);
    CHECK(user.reads == 0);
    CHECK(jobs.FinishedCount() == 0);
}

TEST_CASE("a false arm finishes capture.no-frame without a read", "[remote][capture]") {
    CapUser user;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::CaptureHooks hooks;
    hooks.framesInFlight = 2;
    hooks.armWindow = &ArmNo;
    hooks.drainWindow = &DrainSlot;
    hooks.user = &user;
    hooks.outDir = std::filesystem::temp_directory_path() / "remote-cap-arm";
    std::filesystem::create_directories(hooks.outDir);
    RemoteCaptureScope scope(hooks, jobs);
    Arcane::Remote::Call call;
    call.command = "capture.window";
    call.args = nlohmann::json::object();
    const auto armed = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    REQUIRE(armed.completesJob != 0);
    Arcane::Remote::HostGate gate;
    gate.frame = 4;
    jobs.StepAll(gate);
    const auto* job = jobs.Find(Arcane::Remote::JobId{armed.completesJob});
    REQUIRE(job != nullptr);
    CHECK(job->state == Arcane::Remote::JobState::Failed);
    CHECK_FALSE(job->envelope.ok);
    CHECK(job->envelope.errors[0].code == "capture.no-frame");
    CHECK(Arcane::Remote::ExitFromHost(job->envelope) == 6);
    CHECK(user.copies == 1);
    CHECK(user.reads == 0);
    CHECK(gate.frame == 4);
}

TEST_CASE("a published ticket with a null map finishes capture.no-frame", "[remote][capture]") {
    CapUser user;
    user.failMap = true;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::CaptureHooks hooks;
    hooks.framesInFlight = 2;
    hooks.armWindow = &ArmSlot;
    hooks.drainWindow = &DrainSlot;
    hooks.user = &user;
    hooks.outDir = std::filesystem::temp_directory_path() / "remote-cap-drain";
    std::filesystem::create_directories(hooks.outDir);
    RemoteCaptureScope scope(hooks, jobs);
    Arcane::Remote::Call call;
    call.command = "capture.window";
    call.args = nlohmann::json::object();
    const auto armed = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    REQUIRE(armed.completesJob != 0);
    Arcane::Remote::HostGate gate;
    gate.frame = 4;
    jobs.StepAll(gate);
    const auto id = Arcane::Remote::JobId{armed.completesJob};
    CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Running);
    CHECK(user.reads == 0);
    CHECK(gate.frame == 4);
    user.retired = true;
    jobs.StepAll(gate);
    const auto* job = jobs.Find(id);
    REQUIRE(job != nullptr);
    CHECK(job->state == Arcane::Remote::JobState::Failed);
    CHECK_FALSE(job->envelope.ok);
    CHECK(job->envelope.errors[0].code == "capture.no-frame");
    CHECK(Arcane::Remote::ExitFromHost(job->envelope) == 6);
    CHECK(user.reads == 0);
    CHECK(user.idles == 0);
    CHECK(gate.frame == 4);
}

#endif
