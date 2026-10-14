#if !defined(ARC_BUILD_DIST)
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <vector>

using namespace Arcane::Remote;

static Envelope Err(const char* code) {
    return MakeError("cvar.get", code, "m", "r");
}

TEST_CASE("exit table matches s6.6, AM1, and AM6", "[remote][protocol]") {
    CHECK(ExitFromHost(Err("internal")) == 1);
    CHECK(ExitFromHost(Err("capture.no-renderer")) == 2);
    CHECK(ExitFromHost(Err("arg.unknown")) == 2);
    CHECK(ExitFromHost(Err("command.unknown")) == 2);
    CHECK(ExitFromHost(Err("target.ambiguous")) == 2);
    CHECK(ExitFromHost(Err("auth.denied")) == 3);
    CHECK(ExitFromHost(Err("auth.throttled")) == 3);
    CHECK(ExitFromHost(Err("permission.denied")) == 3);
    CHECK(ExitFromHost(Err("permission.read-only")) == 3);
    CHECK(ExitFromHost(Err("cvar.protected")) == 3);
    CHECK(ExitFromHost(Err("protocol.mismatch")) == 4);
    CHECK(ExitFromHost(Err("target.none")) == 4);
    CHECK(ExitFromHost(Err("host.busy")) == 5);
    auto minimized = Err("host.busy");
    minimized.errors[0].detail = nlohmann::json{{"state", "minimized"}};
    CHECK(ExitFromHost(minimized) == 5);
    auto full = Err("host.busy");
    full.errors[0].detail = nlohmann::json{{"state", "queue-full"}};
    CHECK(ExitFromHost(full) == 5);
    auto pending = Err("host.busy");
    pending.errors[0].detail = nlohmann::json{{"state", "capture-pending"}};
    CHECK(ExitFromHost(pending) == 5);
    CHECK(ExitFromHost(Err("capture.minimized")) == 5);
    CHECK(ExitFromHost(Err("host.unavailable")) == 5);
    CHECK(ExitFromHost(Err("entity.not-found")) == 6);
    CHECK(ExitFromHost(Err("compare.missing-reference")) == 6);
    CHECK(ExitFromHost(Err("capture.no-frame")) == 6);
    CHECK(ExitFromHost(Err("job.failed")) == 6);
    CHECK(ExitFromHost(Err("play.edit-only")) == 6);
    CHECK(ExitFromHost(Err("some.future-code")) == 6); // any other non-empty code (ruling F6)
    CHECK(ExitFromHost(Err("")) == 2);                 // empty code on a non-ok envelope
    CHECK(ExitFromHost(Err("host.died")) == 7);
    CHECK(ExitFromHost(Err("host.unreachable")) == 7);
    HostBlock host;
    host.kind = "runtime";
    host.frame = 61;
    host.state = "ready";
    auto compare = MakeOk("capture.compare", nlohmann::json{{"verdict", "Failed"}, {"diffPixels", 4}}, host);
    CHECK(compare.ok);
    CHECK(compare.errors.empty());
    CHECK(ExitFromHost(compare) == 8);
    auto timed = MakeOk("host.wait", nlohmann::json{{"timedOut", true}}, host);
    CHECK(timed.ok);
    CHECK(ExitFromHost(timed) == 8);
}

TEST_CASE("ToJson is the s6.3 object and omits client and exitCode", "[remote][protocol]") {
    HostBlock host;
    host.kind = "editor";
    host.pid = 23180;
    host.world = "client";
    host.frame = 81234;
    host.state = "ready";
    host.catalog = 7;
    host.pluginGeneration = 3;
    Envelope env = MakeOk("entity.get", nlohmann::json{{"entity", "MeshCube"}}, host);
    env.id = "r-17";
    env.timing.queuedMs = 3;
    env.timing.ranMs = 0.4;
    const auto j = ToJson(env);
    CHECK(j["ok"] == true);
    CHECK(j["protocol"] == 1);
    CHECK(j["id"] == "r-17");
    CHECK(j["command"] == "entity.get");
    CHECK(j["host"]["kind"] == "editor");
    CHECK(j["host"]["frame"] == 81234);
    CHECK(j["host"]["catalog"] == 7);
    CHECK(j["data"]["entity"] == "MeshCube");
    CHECK(j["errors"].is_array());
    CHECK(j["warnings"].is_array());
    CHECK(j["notifications"].is_array());
    CHECK(j["artifacts"].is_array());
    CHECK(j["undo"].is_null());
    CHECK(j["timing"]["queuedMs"] == 3);
    CHECK_FALSE(j.contains("result"));
    CHECK_FALSE(j.contains("hostFrame"));
    CHECK_FALSE(j.contains("exitCode"));
    CHECK_FALSE(j.contains("client"));
    const Envelope back = ParseEnvelope(j);
    CHECK(back.command == "entity.get");
    CHECK(back.host.frame == 81234);
    CHECK(back.data["entity"] == "MeshCube");

    UndoBlock refused;
    refused.label = nullptr;
    refused.undoable = false;
    refused.reason = "not-undoable-command";
    refused.transaction = 0;
    env.undo = refused;
    const auto ju = ToJson(env);
    CHECK(ju["undo"]["label"].is_null());
    CHECK(ju["undo"]["undoable"] == false);
    CHECK(ju["undo"]["transaction"] == 0);
    CHECK(ju["undo"]["reason"] == "not-undoable-command");

    UndoBlock recorded;
    recorded.label = "remote: cvar.set";
    recorded.undoable = true;
    recorded.transaction = 412;
    env.undo = recorded;
    const auto jb = ToJson(env);
    CHECK(jb["undo"]["label"] == "remote: cvar.set");
    CHECK(jb["undo"]["undoable"] == true);
    CHECK(jb["undo"]["transaction"] == 412);
    CHECK_FALSE(jb["undo"].contains("reason"));
}

TEST_CASE("WorstExit ranks a dead host above a busy host", "[remote][protocol]") {
    CHECK(WorstExit(std::vector<int>{0, 5, 7}) == 7);
    CHECK(WorstExit(std::vector<int>{8, 6}) == 6);
    CHECK(WorstExit(std::vector<int>{5, 8, 4}) == 8);
    CHECK(WorstExit(std::vector<int>{3, 2}) == 3);
    CHECK(WorstExit(std::vector<int>{}) == 0);
}

TEST_CASE("MakeOk never carries errors", "[remote][protocol]") {
    const Envelope env = MakeOk("status", nlohmann::json::object(), HostBlock{});
    CHECK(env.ok);
    CHECK(env.errors.empty());
    CHECK(env.protocol == 1);
    CHECK(kIdleConnectionSeconds == 30);
}

TEST_CASE("Indeterminate verdict exits 8", "[remote][protocol]") {
    auto env = MakeOk("capture.compare", nlohmann::json{{"verdict", "Indeterminate"}}, HostBlock{});
    CHECK(ExitFromHost(env) == 8);
}

TEST_CASE("TryParseEnvelope rejects malformed replies", "[remote][protocol]") {
    const auto good = ToJson(MakeOk("status", nlohmann::json::object(), HostBlock{}));
    const auto parsed = TryParseEnvelope(good);
    REQUIRE(parsed.has_value());
    CHECK(ToJson(*parsed) == ToJson(ParseEnvelope(good)));

    CHECK_FALSE(TryParseEnvelope(nlohmann::json::array()).has_value());
    auto noOk = good; noOk.erase("ok");
    CHECK_FALSE(TryParseEnvelope(noOk).has_value());
    auto badOk = good; badOk["ok"] = "yes";
    CHECK_FALSE(TryParseEnvelope(badOk).has_value());
    auto noProto = good; noProto.erase("protocol");
    CHECK_FALSE(TryParseEnvelope(noProto).has_value());
    auto badProto = good; badProto["protocol"] = -1;
    CHECK_FALSE(TryParseEnvelope(badProto).has_value());
    auto badErrors = good; badErrors["errors"] = "x";
    CHECK_FALSE(TryParseEnvelope(badErrors).has_value());
    auto noCode = good; noCode["errors"] = nlohmann::json::array({nlohmann::json::object()});
    CHECK_FALSE(TryParseEnvelope(noCode).has_value());
    auto badHost = good; badHost["host"] = 3;
    CHECK_FALSE(TryParseEnvelope(badHost).has_value());
}
#endif
