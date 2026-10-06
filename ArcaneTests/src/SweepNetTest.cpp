// Settings arc S6-11: net.*, net.keepalive.*, net.rateLimit.* -- the TCP and
// rate-limit tunables of the header-only Net layer. Every default is the
// pre-sweep literal; the existing [net] / [ratelimiter] / [wire] / [protocol] tests are the
// behavioural proof that nothing moved.

#include <catch2/catch_test_macros.hpp>
#include "Helpers/ProtocolLayer.hpp"
#include "Helpers/SettingsSweep.hpp"
#include <Arcane/Net/NetSettings.hpp>
#include <Arcane/Net/Protocol.hpp>
#include <Arcane/Net/RateLimiter.hpp>
#include <Arcane/Net/TcpSocket.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace Arcane;

namespace
{
    // Drops this test's Code-rung value on one cvar however the case exits.
    struct ClearCodeRung
    {
        CVarHandle handle;
        ~ClearCodeRung()
        {
            CVarRegistry::Get().ClearRung(handle, SetBy::Code);
            CVarRegistry::Get().PublishImmediate();
        }
    };
}

TEST_CASE("sweep: net defaults are the pre-sweep literals", "[sweep][net]")
{
    const NetSettings n{};
    CHECK(n.maxPayloadBytes == 8192u); CHECK(n.maxReceiveBufferBytes == 65536u); CHECK(n.recvTimeoutMs == 100);
    CHECK(n.maxConnectionsTotal == 2048u); CHECK(n.maxConnectionsPerIp == 16u); CHECK(n.listenBacklog == 10);
    CHECK(n.protocolPath == "data/protocol.json");
    CHECK(NetKeepaliveSettings{}.idleSeconds == 120); CHECK(NetKeepaliveSettings{}.intervalSeconds == 30);
    CHECK(NetKeepaliveSettings{}.probeCount == 8);
    const NetRateLimitSettings r{};
    CHECK(r.maxRecords == 10000u); CHECK(r.maxAttempts == 5); CHECK(r.windowSeconds == 60);
    CHECK(r.cooldownSeconds == 30); CHECK(r.cleanupEvery == 100); CHECK(r.idleExpiryMinutes == 10);
    STATIC_REQUIRE(ServerConfig::RECV_CHUNK_SIZE == 4096);   // CONSTANT: an array bound in the services
    Test::RequireDefault("net.maxConnectionsPerIp", CVarValue::UInt64(16u));
    Test::RequireDefault("net.rateLimit.maxAttempts", CVarValue::Int32(5));
}

TEST_CASE("sweep: RateLimiter's config comes from net.rateLimit.*", "[sweep][net]")
{
    const RateLimiter::Config c = RateLimiter::ConfigFromSettings();
    CHECK(c.maxAttempts == 5); CHECK(c.windowSeconds == 60); CHECK(c.cooldownSeconds == 30);
}

TEST_CASE("sweep: net.rateLimit.maxAttempts is Live -- ConfigFromSettings follows a set", "[sweep][net]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("net.rateLimit.maxAttempts");
    const ClearCodeRung restore{ h };
    REQUIRE(reg.Set(h, CVarValue::Int32(2), SetBy::Code) == SetResult::Applied);
    reg.PublishImmediate();

    CHECK(RateLimiter::ConfigFromSettings().maxAttempts == 2);

    // The limiter honours it: two attempts pass, the third trips the cooldown.
    RateLimiter limiter;
    const RateLimiter::Config cfg = RateLimiter::ConfigFromSettings();
    CHECK(limiter.Allow("sweep-net", cfg));
    CHECK(limiter.Allow("sweep-net", cfg));
    CHECK_FALSE(limiter.Allow("sweep-net", cfg));
}

TEST_CASE("sweep: ProtocolLoader::Load() reads net.protocolPath; absent caps fall back to net.*", "[sweep][net]")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "arcane_sweep_net_protocol.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"JSON({ "version": 7, "name": "SweepNetProtocol", "settings": {
            "default_port": 7777, "max_message_size": 65536, "message_format": "LENGTH:TYPE|TOKEN|PAYLOAD",
            "token_length": 64, "session_lifetime_seconds": 86400, "idle_timeout_seconds": 1800,
            "heartbeat_interval_seconds": 30 } })JSON";
    }

    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("net.protocolPath");
    const ClearCodeRung restore{ h };
    REQUIRE(reg.Set(h, CVarValue::String(path.string()), SetBy::Code) == SetResult::Applied);
    reg.PublishImmediate();

    const Test::ProtocolLayerReset layerReset;   // S6-12: a good Load layers net.* at the Project rung
    ProtocolLoader& proto = ProtocolLoader::Instance();
    REQUIRE(proto.Load());
    CHECK(proto.GetName() == "SweepNetProtocol");
    CHECK(proto.GetSettings().maxConnectionsPerIp == 16);
    CHECK(proto.GetSettings().maxConnectionsTotal == 2048);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
