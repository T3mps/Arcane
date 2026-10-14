#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/TickGate.hpp>
#include <Arcane/Remote/HostSession.hpp>
#include <filesystem>
#include "RemoteTestSupport.hpp"

#if !defined(ARC_BUILD_DIST)

TEST_CASE("Consume is Idle until step or run, and step decrements", "[remote][tick]") {
    Arcane::Remote::TickGate gate;
    gate.SetRunning(false);
    gate.SetPaused(true);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Idle);
    gate.RequestSteps(2);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Step);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Step);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Idle);
    CHECK(gate.StepsRemaining() == 0);
    gate.SetRunning(true);
    gate.SetPaused(false);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Run);
    gate.SetPaused(true);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Idle);
    gate.RequestSteps(1);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Step);
    CHECK(gate.Consume() == Arcane::Remote::TickDecision::Idle);
}

TEST_CASE("holdSim starts Idle and a free run starts Running", "[remote][tick]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Server); // Start sets the process host kind (ruling S3-9)
    Arcane::Remote::HostSession::Config held;
    held.kind = Arcane::Remote::HostKind::Server;
    held.cvarContext = Arcane::CVarContext::ServerAdmin;
    held.projectRoot = std::filesystem::temp_directory_path() / "remote-hold-sim";
    held.holdSim = true;
    Arcane::Remote::HostSession session(held);
    session.Start();
    CHECK(session.Tick().Consume() == Arcane::Remote::TickDecision::Idle);
    session.Stop("test");

    Arcane::Remote::HostSession::Config free = held;
    free.holdSim = false;
    free.projectRoot = std::filesystem::temp_directory_path() / "remote-free-sim";
    Arcane::Remote::HostSession running(free);
    running.Start();
    CHECK(running.Tick().Consume() == Arcane::Remote::TickDecision::Run);
    running.Stop("test");
}

#endif
