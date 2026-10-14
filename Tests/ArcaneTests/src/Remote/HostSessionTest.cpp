#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/HostSession.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Core/AbiVersion.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include "RemoteTestSupport.hpp"
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir, EnvOverride

#if !defined(ARC_BUILD_DIST)

namespace
{
Arcane::Remote::HostSession::Config ServerConfig(const std::filesystem::path& root)
{
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Server;
    cfg.cvarContext = Arcane::CVarContext::ServerAdmin;
    cfg.projectRoot = root;
    cfg.capabilities = {"console", "cvar", "ecs", "logs", "play", "problems", "stats"};
    return cfg;
}
} // namespace

TEST_CASE("a client-mode process does not arm", "[remote][host]") {
    CHECK_FALSE(Arcane::Remote::Arming::Allow(Arcane::NetMode::Client, false, true));
    CHECK(Arcane::Remote::Arming::Allow(Arcane::NetMode::DedicatedServer, false, true));
    CHECK(Arcane::Remote::Arming::Allow(Arcane::NetMode::Standalone, false, true));
    CHECK_FALSE(Arcane::Remote::Arming::Allow(Arcane::NetMode::Standalone, true, true));
    CHECK_FALSE(Arcane::Remote::Arming::Allow(Arcane::NetMode::Standalone, false, false));
}

TEST_CASE("HostSession writes a live descriptor and removes it on Stop", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor); // restores the suite's kind even if a REQUIRE aborts (ruling S3-9)
    auto root = std::filesystem::temp_directory_path() / "remote-session-test";
    std::filesystem::remove_all(root);
    {
        Arcane::Remote::HostSession::Config cfg;
        cfg.kind = Arcane::Remote::HostKind::Server;
        cfg.cvarContext = Arcane::CVarContext::ServerAdmin;
        cfg.projectRoot = root;
        cfg.capabilities = {"console", "cvar", "ecs", "logs", "play", "problems", "stats"};
        Arcane::Remote::HostSession session(cfg);
        const auto kindBefore = Arcane::Remote::CommandRegistry::Get().GetHostKind();
        REQUIRE(session.Start());
        CHECK(Arcane::Remote::CommandRegistry::Get().GetHostKind() == Arcane::Remote::HostKind::Server);
        CHECK(std::filesystem::exists(session.DescriptorFile()));
        auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
        REQUIRE(desc);
        CHECK(desc->port == session.Port());
        CHECK(desc->abi == Arcane::kGamePluginABIVersion);   // ruling F27
        CHECK_FALSE(Arcane::Remote::IsStale(*desc));
        session.NoteReload(desc->abi + 7u, true, "reloaded", 12);   // any value: the host supplies it (ruling F10)
        CHECK(static_cast<const Arcane::Remote::HostSession&>(session).Gate().pluginGeneration == desc->abi + 7u);
        session.Stop("test");
        CHECK_FALSE(std::filesystem::exists(session.DescriptorFile()));
        CHECK(Arcane::Remote::CommandRegistry::Get().GetHostKind() == kindBefore);   // ruling S3-9

        // A session object starts at most once: a re-arm is a new object.
        CHECK_FALSE(session.Start());
        CHECK(session.Port() == 0);
        CHECK_FALSE(std::filesystem::exists(session.DescriptorFile()));
        CHECK(Arcane::Remote::CommandRegistry::Get().GetHostKind() == kindBefore);
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    CHECK_FALSE(std::filesystem::exists(root));
}

TEST_CASE("HostSession: an unwritable descriptor is a failed start that leaves nothing behind", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-session-fail");
    // A regular file where the descriptor's parent directory must be: the
    // directory cannot be created, so WriteDescriptorAtomic fails on every platform.
    const auto blocker = tmp.path / "blocker";
    {
        std::ofstream out(blocker);
        out << "not a directory";
    }
    auto cfg = ServerConfig(tmp.path / "project");
    cfg.descriptorOverride = blocker / "server.json";
    Arcane::Remote::HostSession session(cfg);
    const auto kindBefore = Arcane::Remote::CommandRegistry::Get().GetHostKind();
    CHECK_FALSE(session.Start());
    CHECK(session.Port() == 0);
    CHECK(Arcane::Remote::CommandRegistry::Get().GetHostKind() == kindBefore);
    CHECK_FALSE(std::filesystem::exists(session.DescriptorFile()));
    CHECK(std::filesystem::is_regular_file(blocker));      // the blocker was not replaced
    CHECK_FALSE(session.DrainFrame(1));                     // a failed session never tells the host to stop
    CHECK(session.ExitReason() == nullptr);
    CHECK_FALSE(session.Start());                           // nor does it start on a retry
    CHECK(Arcane::Remote::CommandRegistry::Get().GetHostKind() == kindBefore);
}

TEST_CASE("HostSession: ARC_REMOTE_READ_ONLY=1 makes the host read-only", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-session-ro");
    {
        Arcane::Test::EnvOverride env("ARC_REMOTE_READ_ONLY", "1");   // restored on scope exit
        Arcane::Remote::HostSession session(ServerConfig(tmp.path / "ro"));
        REQUIRE(session.Start());
        CHECK(session.Gate().readOnly.load());
        session.Stop("test");
    }
    {
        Arcane::Test::EnvOverride env("ARC_REMOTE_READ_ONLY", "0");   // only "1" counts
        Arcane::Remote::HostSession session(ServerConfig(tmp.path / "rw"));
        REQUIRE(session.Start());
        CHECK_FALSE(session.Gate().readOnly.load());
        session.Stop("test");
    }
}

TEST_CASE("HostSession: DrainFrame adopts the boot generation, notes a reload, and latches a quit", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-session-quit");
    Arcane::Remote::HostSession session(ServerConfig(tmp.path));
    REQUIRE(session.Start());
    CHECK_FALSE(session.DrainFrame(3));                 // the module the host booted with, not a reload
    CHECK(session.Gate().pluginGeneration == 3u);
    CHECK_FALSE(session.DrainFrame(4));                 // a hot reload
    CHECK(session.Gate().pluginGeneration == 4u);
    CHECK(session.ExitReason() == nullptr);
    session.RequestQuit();
    CHECK(session.DrainFrame(4));
    REQUIRE(session.ExitReason() != nullptr);
    CHECK(std::string(session.ExitReason()) == "remote-quit");

    session.MarkStopping();                             // the host leaves its loop
    CHECK(session.Gate().State() == "stopping");
    const auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
    REQUIRE(desc);
    CHECK(desc->state == "stopping");
    session.Stop(session.ExitReason());
    CHECK_FALSE(std::filesystem::exists(session.DescriptorFile()));
}

TEST_CASE("HostSession: a short idle exit latches remote-idle", "[remote][host]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("remote-session-idle");
    auto cfg = ServerConfig(tmp.path);
    cfg.idleExitSeconds = 1;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    CHECK_FALSE(session.IdleExpired());
    CHECK_FALSE(session.DrainFrame(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CHECK(session.IdleExpired());
    CHECK(session.DrainFrame(1));
    REQUIRE(session.ExitReason() != nullptr);
    CHECK(std::string(session.ExitReason()) == "remote-idle");
    session.RequestQuit();                              // the first reason stays: it is what ended the loop
    CHECK(session.DrainFrame(1));
    CHECK(std::string(session.ExitReason()) == "remote-idle");
    session.Stop(session.ExitReason());
}

#endif
