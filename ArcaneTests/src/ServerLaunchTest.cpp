// ServerLaunch: the editor's "Client + separate server process" play mode, PURE
// halves only (Core-DLL split, plan 1 Task 7). ExeCandidates/BuildArgs do no OS
// work and are driven directly here, the same split RuntimeLaunchTest.cpp already
// establishes for the standalone-runtime spawn; ServerProcess::Spawn is the one
// CreateProcessW call in the file and NO test here creates a process -- that is
// desk-verify territory. What IS pinned about ServerProcess is its dormant
// contract: a never-spawned handle reports "not running" and its Stop() is a
// no-op, which is what makes the editor's unconditional `m_serverProcess.Stop()`
// after every Play->Stop safe on the four topologies that never spawned one.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "Project/ServerLaunch.hpp"

TEST_CASE("ServerLaunch::ExeCandidates probes beside the editor first, then the dev sibling dir", "[editor]")
{
    const auto c = Arcane::Editor::ServerLaunch::ExeCandidates("C:/x/ArcaneEditor");
    REQUIRE(c.size() == 2);
    CHECK(c[0].generic_string() == "C:/x/ArcaneEditor/ArcaneServer.exe");
    CHECK(c[1].generic_string() == "C:/x/ArcaneEditor/../ArcaneServer/ArcaneServer.exe");
}

TEST_CASE("ServerLaunch::BuildArgs asks for the project and an unbounded run", "[editor]")
{
    // --frames 0 is ArcaneServer's "run until terminated" (Task 6): the editor
    // owns the child's lifetime through ServerProcess::Stop, so a frame budget
    // would end the server behind the editor's back mid-session.
    const auto a = Arcane::Editor::ServerLaunch::BuildArgs("C:/p");
    REQUIRE(a.size() == 4);
    CHECK(a[0] == L"--project");
    CHECK(a[1] == L"C:/p");
    CHECK(a[2] == L"--frames");
    CHECK(a[3] == L"0");
}

TEST_CASE("ServerLaunch::ServerProcess is dormant until Spawn: not running, and Stop is a no-op", "[editor]")
{
    Arcane::Editor::ServerLaunch::ServerProcess p;
    CHECK_FALSE(p.IsRunning());
    p.Stop();                       // no handle, nothing to kill, nothing logged
    CHECK_FALSE(p.IsRunning());
    p.Stop();                       // and it stays idempotent
    CHECK_FALSE(p.IsRunning());
}
