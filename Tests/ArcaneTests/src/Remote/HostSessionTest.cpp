#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/HostSession.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Core/AbiVersion.hpp>
#include <filesystem>
#include "RemoteTestSupport.hpp"

#if !defined(ARC_BUILD_DIST)

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
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Server;
    cfg.cvarContext = Arcane::CVarContext::ServerAdmin;
    cfg.projectRoot = root;
    cfg.capabilities = {"console", "cvar", "ecs", "logs", "play", "problems", "stats"};
    Arcane::Remote::HostSession session(cfg);
    const auto kindBefore = Arcane::Remote::CommandRegistry::Get().GetHostKind();
    session.Start();
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
}

#endif
