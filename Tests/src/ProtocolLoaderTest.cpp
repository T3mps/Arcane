// ProtocolLoader baseline coverage (E03-4) + the E01-3c MsgId validation
// (range / zero-id / duplicate-id) added in this wave. Fixtures are written to
// temp protocol.json files and loaded through the (singleton) loader. Each
// positive-assertion case performs its own good Load first: Load now REPLACES
// prior state, so a preceding good Load isolates the singleton for this case.
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Net/Protocol.hpp>

using Arcane::ProtocolLoader;
using Arcane::kInvalidMsgId;

namespace
{
    // Write `content` to a unique temp file and return its path.
    std::string WriteTemp(const std::string& content)
    {
        static std::atomic<unsigned> counter{0};
        auto dir = std::filesystem::temp_directory_path();
        auto path = dir / ("arcane_protocol_test_" + std::to_string(counter++) + ".json");
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
        out.close();
        return path.string();
    }

    // A well-formed protocol.json with two messages and one enum.
    const char* kValidProtocol = R"JSON({
        "version": 3,
        "name": "TestProtocol",
        "settings": {
            "default_port": 7777,
            "max_message_size": 65536,
            "message_format": "LENGTH:TYPE|TOKEN|PAYLOAD",
            "token_length": 64,
            "session_lifetime_seconds": 86400,
            "idle_timeout_seconds": 1800,
            "heartbeat_interval_seconds": 30
        },
        "messages": {
            "Login": { "id": 1,  "direction": "client_to_server", "description": "auth", "requires_auth": false },
            "Pull":  { "id": 10, "direction": "client_to_server", "requires_auth": true,  "debug_only": false }
        },
        "enums": {
            "Rarity": { "Common": 1, "Rare": 3, "Epic": 4, "Legendary": 5 }
        }
    })JSON";
}

TEST_CASE("ProtocolLoader: valid protocol loads and exposes its contents", "[protocol]")
{
    auto path = WriteTemp(kValidProtocol);
    auto& proto = ProtocolLoader::Instance();
    REQUIRE(proto.Load(path));
    REQUIRE(proto.IsLoaded());
    REQUIRE(proto.GetVersion() == 3);
    REQUIRE(proto.GetName() == "TestProtocol");

    const auto& s = proto.GetSettings();
    REQUIRE(s.defaultPort == 7777);
    REQUIRE(s.maxMessageSize == 65536);
    REQUIRE(s.messageFormat == "LENGTH:TYPE|TOKEN|PAYLOAD");
    REQUIRE(s.tokenLength == 64);
    REQUIRE(s.sessionLifetimeSeconds == 86400);
    REQUIRE(s.idleTimeoutSeconds == 1800);
    REQUIRE(s.heartbeatIntervalSeconds == 30);
}

TEST_CASE("ProtocolLoader: Id / GetMessageName / GetMessage round-trip", "[protocol]")
{
    auto path = WriteTemp(kValidProtocol);
    auto& proto = ProtocolLoader::Instance();
    REQUIRE(proto.Load(path));

    REQUIRE(proto.Id("Login") == 1);
    REQUIRE(proto.Id("Pull") == 10);
    REQUIRE(proto.Id("DoesNotExist") == kInvalidMsgId);

    REQUIRE(proto.GetMessageName(1) == "Login");
    REQUIRE(proto.GetMessageName(10) == "Pull");
    REQUIRE(proto.GetMessageName(999).empty());

    const auto* pull = proto.GetMessage("Pull");
    REQUIRE(pull != nullptr);
    REQUIRE(pull->requiresAuth);
    REQUIRE(proto.GetMessage("Nope") == nullptr);
}

TEST_CASE("ProtocolLoader: enum parsing and lookups", "[protocol]")
{
    auto path = WriteTemp(kValidProtocol);
    auto& proto = ProtocolLoader::Instance();
    REQUIRE(proto.Load(path));

    REQUIRE(proto.GetEnumValue("Rarity", "Epic") == 4);
    REQUIRE(proto.GetEnumValue("Rarity", "Legendary") == 5);
    REQUIRE(proto.GetEnumValue("Rarity", "Missing") == -1);
    REQUIRE(proto.GetEnumValue("NoSuchEnum", "Epic") == -1);
}

TEST_CASE("ProtocolLoader: IdOrThrow resolves or throws on drift", "[protocol]")
{
    auto path = WriteTemp(kValidProtocol);
    auto& proto = ProtocolLoader::Instance();
    REQUIRE(proto.Load(path));

    REQUIRE(proto.IdOrThrow("Login") == 1);
    REQUIRE_THROWS_AS(proto.IdOrThrow("RenamedOrTypoed"), std::logic_error);
}

TEST_CASE("ProtocolLoader: reload replaces prior message state", "[protocol]")
{
    auto& proto = ProtocolLoader::Instance();
    REQUIRE(proto.Load(WriteTemp(kValidProtocol)));
    REQUIRE(proto.Id("Login") == 1);

    // A second protocol without "Login" must fully replace the first.
    const char* second = R"JSON({
        "version": 4, "name": "SecondProtocol",
        "settings": {
            "default_port": 7777, "max_message_size": 65536,
            "message_format": "x", "token_length": 64,
            "session_lifetime_seconds": 1, "idle_timeout_seconds": 1,
            "heartbeat_interval_seconds": 1
        },
        "messages": { "Register": { "id": 2 } }
    })JSON";
    REQUIRE(proto.Load(WriteTemp(second)));
    REQUIRE(proto.Id("Register") == 2);
    REQUIRE(proto.Id("Login") == kInvalidMsgId); // cleared, not merged
}

TEST_CASE("ProtocolLoader: missing settings section is rejected", "[protocol]")
{
    const char* noSettings = R"JSON({ "version": 1, "name": "x", "messages": {} })JSON";
    REQUIRE_FALSE(ProtocolLoader::Instance().Load(WriteTemp(noSettings)));
}

TEST_CASE("ProtocolLoader: a missing required setting is rejected", "[protocol]")
{
    // token_length is omitted.
    const char* missingKey = R"JSON({
        "version": 1, "name": "x",
        "settings": {
            "default_port": 7777, "max_message_size": 65536,
            "message_format": "x",
            "session_lifetime_seconds": 1, "idle_timeout_seconds": 1,
            "heartbeat_interval_seconds": 1
        }
    })JSON";
    REQUIRE_FALSE(ProtocolLoader::Instance().Load(WriteTemp(missingKey)));
}

TEST_CASE("ProtocolLoader: malformed JSON is rejected", "[protocol]")
{
    REQUIRE_FALSE(ProtocolLoader::Instance().Load(WriteTemp("{ this is not json")));
}

TEST_CASE("ProtocolLoader: a missing file is rejected", "[protocol]")
{
    REQUIRE_FALSE(ProtocolLoader::Instance().Load("this/path/does/not/exist_zzz.json"));
}

// ---- E01-3c MsgId validation --------------------------------------------

TEST_CASE("ProtocolLoader: duplicate message id is rejected (E01-3c)", "[protocol]")
{
    const char* dup = R"JSON({
        "version": 1, "name": "x",
        "settings": {
            "default_port": 7777, "max_message_size": 65536,
            "message_format": "x", "token_length": 64,
            "session_lifetime_seconds": 1, "idle_timeout_seconds": 1,
            "heartbeat_interval_seconds": 1
        },
        "messages": {
            "Alpha": { "id": 5 },
            "Beta":  { "id": 5 }
        }
    })JSON";
    REQUIRE_FALSE(ProtocolLoader::Instance().Load(WriteTemp(dup)));
}

TEST_CASE("ProtocolLoader: zero / missing id is rejected (E01-3c)", "[protocol]")
{
    const char* zeroId = R"JSON({
        "version": 1, "name": "x",
        "settings": {
            "default_port": 7777, "max_message_size": 65536,
            "message_format": "x", "token_length": 64,
            "session_lifetime_seconds": 1, "idle_timeout_seconds": 1,
            "heartbeat_interval_seconds": 1
        },
        "messages": { "Zero": { "id": 0 } }
    })JSON";
    REQUIRE_FALSE(ProtocolLoader::Instance().Load(WriteTemp(zeroId)));

    // A message with no "id" key defaults to 0 and must also be rejected.
    const char* noId = R"JSON({
        "version": 1, "name": "x",
        "settings": {
            "default_port": 7777, "max_message_size": 65536,
            "message_format": "x", "token_length": 64,
            "session_lifetime_seconds": 1, "idle_timeout_seconds": 1,
            "heartbeat_interval_seconds": 1
        },
        "messages": { "NoId": { "direction": "c2s" } }
    })JSON";
    REQUIRE_FALSE(ProtocolLoader::Instance().Load(WriteTemp(noId)));
}

TEST_CASE("ProtocolLoader: id above uint16 range is rejected (E01-3c)", "[protocol]")
{
    // 70000 > 65535 -- would silently truncate to a uint16 MsgId (70000 & 0xFFFF).
    const char* tooBig = R"JSON({
        "version": 1, "name": "x",
        "settings": {
            "default_port": 7777, "max_message_size": 65536,
            "message_format": "x", "token_length": 64,
            "session_lifetime_seconds": 1, "idle_timeout_seconds": 1,
            "heartbeat_interval_seconds": 1
        },
        "messages": { "Huge": { "id": 70000 } }
    })JSON";
    REQUIRE_FALSE(ProtocolLoader::Instance().Load(WriteTemp(tooBig)));
}
