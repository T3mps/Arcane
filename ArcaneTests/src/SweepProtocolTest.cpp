// Settings arc S6-12 (inventory R3): protocol.json's `settings` object is a
// Project-rung layer over net.* while the services migrate; GetSettings() is a
// view over the cvars, and HasToken() reads net.tokenLength.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Net/NetSettings.hpp>
#include <Arcane/Net/Protocol.hpp>
#include <filesystem>
#include <fstream>

using namespace Arcane;

TEST_CASE("sweep: protocol.json settings land on the Project rung as net.*", "[sweep][protocol]")
{
    const auto path = std::filesystem::temp_directory_path() / "arcane_sweep_protocol.json";
    std::ofstream(path) << R"({ "version": 1, "name": "T", "settings": { "default_port": 7777,
        "max_message_size": 65536, "message_format": "LENGTH:TYPE|TOKEN|PAYLOAD\n", "token_length": 64,
        "session_lifetime_seconds": 86400, "idle_timeout_seconds": 1800, "heartbeat_interval_seconds": 60,
        "max_connections_per_ip": 12 }, "messages": {} })";
    REQUIRE(ProtocolLoader::Instance().Load(path.string()));
    const NetSettings& n = Settings<NetSettings>();
    CHECK(n.defaultPort == 7777);
    CHECK(n.maxPayloadBytes == 65536u);
    CHECK(n.maxConnectionsPerIp == 12u);
    CHECK(n.maxConnectionsTotal == 2048u);                     // absent key: the net default, as before
    CHECK(ProtocolLoader::Instance().GetSettings().maxMessageSize == 65536);
    const auto e = CVarRegistry::Get().Explain("net.defaultPort");
    CHECK(e->setBy == SetBy::Project);
    Message m; m.token = std::string(63, 'a');
    CHECK_FALSE(m.HasToken());
    m.token.push_back('a');
    CHECK(m.HasToken());
    CVarRegistry::Get().RevertLayer(SetBy::Project); CVarRegistry::Get().PublishImmediate();
    std::filesystem::remove(path);
}
