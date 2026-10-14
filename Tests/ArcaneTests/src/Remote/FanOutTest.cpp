#include <catch2/catch_test_macros.hpp>
#include <Servitor/Client.hpp>
#include <Servitor/Discover.hpp>
#include <Servitor/FanOut.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Json.hpp>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace {

Servitor::LiveHost MakeHost(std::uint32_t pid, Arcane::Remote::HostKind kind, const std::string& token = {})
{
    Servitor::LiveHost h;
    h.descriptor.pid = pid;
    h.descriptor.hostKind = kind;
    h.descriptor.protocol = 1;
    h.descriptor.token = token;
    return h;
}

} // namespace

TEST_CASE("fan-out worst exit is 7 when one host is dead and one is busy", "[servitor][client]") {
    Servitor::Scan scan;
    scan.live = {MakeHost(1, Arcane::Remote::HostKind::Runtime), MakeHost(2, Arcane::Remote::HostKind::Server)};
    int sends = 0;
    auto post = [](const Arcane::Remote::Descriptor& d, const nlohmann::json&, Arcane::Remote::Envelope* out, void* user) {
        *static_cast<int*>(user) += 1;
        if (d.pid == 1)
            *out = Arcane::Remote::MakeError("cvar.get", "host.busy", "full", "retry");
        else
            *out = Arcane::Remote::MakeError("cvar.get", "host.died", "gone", "relaunch");
        return Arcane::Remote::ExitFromHost(*out);
    };
    const auto result = Servitor::FanOut(scan, {}, true,
        nlohmann::json{{"command", "cvar.get"}, {"args", {{"name", "render.vsync"}}}},
        false, false, Servitor::Format::Json, post, &sends);
    CHECK(sends == 2);
    CHECK(result.exitCode == 7);
    REQUIRE(result.wrapper["hosts"].size() == 2);
    CHECK(result.wrapper["hosts"][0]["pid"] == 1);
    CHECK(result.wrapper["hosts"][0]["hostKind"] == "runtime");
    CHECK(result.wrapper["hosts"][1]["envelope"]["errors"][0]["code"] == "host.died");
    CHECK(result.wrapper["hosts"][1]["envelope"]["client"]["exitCode"] == 7);
}

TEST_CASE("drive fan-out without --yes sends nothing", "[servitor][client]") {
    Servitor::Scan scan;
    scan.live = {MakeHost(1, Arcane::Remote::HostKind::Editor)};
    int sends = 0;
    auto post = [](const Arcane::Remote::Descriptor&, const nlohmann::json&, Arcane::Remote::Envelope*, void* user) {
        *static_cast<int*>(user) += 1;
        return 0;
    };
    const auto result = Servitor::FanOut(scan, {}, true,
        nlohmann::json{{"command", "cvar.set"}},
        true, false, Servitor::Format::Json, post, &sends);
    CHECK(sends == 0);
    CHECK(result.exitCode == 2);
    CHECK(result.text.find("arg.inapplicable") != std::string::npos);
    CHECK(result.text.find("--yes") != std::string::npos);
}

TEST_CASE("fan-out kind filter sends only to matching hosts and none is target.none", "[servitor][client]") {
    Servitor::Scan scan;
    scan.live = {MakeHost(1, Arcane::Remote::HostKind::Runtime), MakeHost(2, Arcane::Remote::HostKind::Server)};
    int sends = 0;
    auto post = [](const Arcane::Remote::Descriptor& d, const nlohmann::json&, Arcane::Remote::Envelope* out, void* user) {
        *static_cast<int*>(user) += 1;
        *out = Arcane::Remote::MakeOk("stats.frame", {{"pid", d.pid}}, {});
        return 0;
    };
    const nlohmann::json call = {{"command", "stats.frame"}, {"args", nlohmann::json::object()}};
    auto result = Servitor::FanOut(scan, "server", false, call, false, false, Servitor::Format::Json, post, &sends);
    CHECK(sends == 1);
    CHECK(result.exitCode == 0);
    REQUIRE(result.wrapper["hosts"].size() == 1);
    CHECK(result.wrapper["hosts"][0]["pid"] == 2);

    sends = 0;
    result = Servitor::FanOut(scan, "editor", false, call, false, false, Servitor::Format::Json, post, &sends);
    CHECK(sends == 0);
    CHECK(result.exitCode == 4);
    CHECK(result.wrapper["hosts"].empty());
    CHECK(result.text.find("target.none") != std::string::npos);
}

TEST_CASE("fan-out redacts each host's envelope with that host's own token", "[servitor][client]") {
    const std::string tokenA(64, 'a');
    const std::string tokenB(64, 'b');
    Servitor::Scan scan;
    scan.live = {MakeHost(1, Arcane::Remote::HostKind::Runtime, tokenA),
                 MakeHost(2, Arcane::Remote::HostKind::Server, tokenB)};
    auto post = [](const Arcane::Remote::Descriptor& d, const nlohmann::json&, Arcane::Remote::Envelope* out, void*) {
        *out = Arcane::Remote::MakeError("x", "internal", "leaked " + d.token, "none");
        return 1;
    };
    for (const auto format : {Servitor::Format::Json, Servitor::Format::Ndjson, Servitor::Format::Human}) {
        const auto result = Servitor::FanOut(scan, {}, true, nlohmann::json{{"command", "x"}}, false, false, format,
                                             post, nullptr);
        CHECK(result.wrapper.dump().find(tokenA) == std::string::npos);
        CHECK(result.wrapper.dump().find(tokenB) == std::string::npos);
        const std::string printed = result.text + result.diagnostics;
        CHECK(printed.find(tokenA) == std::string::npos);
        CHECK(printed.find(tokenB) == std::string::npos);
        CHECK(printed.find("<redacted>") != std::string::npos);
    }
}

TEST_CASE("fan-out ndjson is one frame per host then the result frame", "[servitor][client]") {
    Servitor::Scan scan;
    scan.live = {MakeHost(1, Arcane::Remote::HostKind::Runtime), MakeHost(2, Arcane::Remote::HostKind::Server)};
    auto post = [](const Arcane::Remote::Descriptor&, const nlohmann::json&, Arcane::Remote::Envelope* out, void*) {
        *out = Arcane::Remote::MakeOk("stats.frame", nlohmann::json::object(), {});
        return 0;
    };
    const auto result = Servitor::FanOut(scan, {}, true, nlohmann::json{{"command", "stats.frame"}}, false, false,
                                         Servitor::Format::Ndjson, post, nullptr);
    std::vector<nlohmann::json> frames;
    std::size_t start = 0;
    while (start < result.text.size()) {
        const auto end = result.text.find('\n', start);
        REQUIRE(end != std::string::npos);
        frames.push_back(nlohmann::json::parse(result.text.substr(start, end - start)));
        start = end + 1;
    }
    REQUIRE(frames.size() == 3);
    CHECK(frames[0]["type"] == "host");
    CHECK(frames[0]["pid"] == 1);
    CHECK(frames[1]["pid"] == 2);
    CHECK(frames[2]["type"] == "result");
    CHECK(frames[2]["count"] == 2);
    CHECK(frames[2]["exitCode"] == 0);
}

TEST_CASE("drive commands are recognised by name and by bless", "[servitor][client]") {
    using Servitor::IsDriveCommand;
    CHECK(IsDriveCommand(nlohmann::json{{"command", "cvar.set"}}));
    CHECK(IsDriveCommand(nlohmann::json{{"command", "console.exec"}}));
    CHECK(IsDriveCommand(nlohmann::json{{"command", "host.quit"}}));
    CHECK(IsDriveCommand(nlohmann::json{{"command", "play.start"}}));
    CHECK(IsDriveCommand(nlohmann::json{{"command", "capture.compare"}, {"args", {{"bless", true}}}}));
    CHECK_FALSE(IsDriveCommand(nlohmann::json{{"command", "capture.compare"}, {"args", {{"bless", false}}}}));
    CHECK_FALSE(IsDriveCommand(nlohmann::json{{"command", "capture.compare"}}));
    CHECK_FALSE(IsDriveCommand(nlohmann::json{{"command", "cvar.get"}}));
    CHECK_FALSE(IsDriveCommand(nlohmann::json{{"command", "playground.x"}}));
}

TEST_CASE("--all parses with an optional host kind", "[servitor][client]") {
    const auto parse = [](std::initializer_list<const char*> args, Servitor::GlobalOptions& out, std::string* err) {
        std::vector<char*> argv;
        for (const char* a : args)
            argv.push_back(const_cast<char*>(a));
        return Servitor::ParseGlobals(static_cast<int>(argv.size()), argv.data(), out, err);
    };
    std::string err;
    Servitor::GlobalOptions opt;
    REQUIRE(parse({"servitor", "--all", "status"}, opt, &err));
    CHECK(opt.all);
    CHECK(opt.allKind.empty());
    REQUIRE(parse({"servitor", "--all", "editor", "stats", "frame"}, opt, &err));
    CHECK(opt.all);
    CHECK(opt.allKind == "editor");
    REQUIRE(parse({"servitor", "stats", "frame"}, opt, &err));
    CHECK_FALSE(opt.all);
}

#endif
