// The reporter's symbolized-text model (crash window plan 2, task 5; spec §6
// "Symbolization").
//
// SymbolizedText.cpp source-compiles into this exe (premake5.lua, ArcaneTests'
// `files` list) exactly as ReporterArgs.cpp does -- and that list is NOT gated
// on the target OS, so this TU and the one it drives must stay free of
// windows.h, directly and transitively. Every dbgeng and Win32 dependency of
// the symbolizer lives in Symbolizer.cpp, which is NOT compiled here: the pure
// half is the MODEL (frames, threads, the engine's verdict) and its TEXT, and
// that is what these cases pin.
//
// What no unit over a pure formatter can tell you is whether the engine
// actually resolved a name. That is CrashPathTest's job, against a real
// minidump the death fixture wrote.

#include "SymbolizedText.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace Arcane::Reporter;

TEST_CASE("symbolized text: frames format as module!function+0xoff [file:line], degrading to module+0xoff", "[reporter]")
{
    SymFrame full{ 0x7ff6'1234'0000ull, "death_fixture", "main", 0x1a4, "D:\\a\\DeathFixtureMain.cpp", 52 };
    CHECK(FormatFrame(full) == "death_fixture!main+0x1a4 [D:\\a\\DeathFixtureMain.cpp:52]");
    SymFrame noLine{ 0x1, "KERNEL32", "BaseThreadInitThunk", 0x14, "", 0 };
    CHECK(FormatFrame(noLine) == "KERNEL32!BaseThreadInitThunk+0x14");
    SymFrame bare{ 0x1, "death-fixture.exe", "", 0x1234, "", 0 };
    CHECK(FormatFrame(bare) == "death-fixture.exe+0x1234");
}

TEST_CASE("symbolized text: the faulting thread leads, the fallback carries the portable stack", "[reporter]")
{
    Symbolized s;
    s.engineAvailable = true;
    s.symbolPath = "D:\\bin";
    s.threads.push_back({ 11, false, { { 0x1, "ntdll", "NtWaitForSingleObject", 0x14, "", 0 } } });
    s.threads.push_back({ 22, true,  { { 0x2, "death_fixture", "main", 0x1a4, "f.cpp", 52 } } });
    PutFaultingFirst(s, 0);
    REQUIRE(s.threads.front().systemId == 22u);

    const std::string text = FormatSymbolized(s, "Arcane 0.1 Debug", "");
    CHECK(text.find("symbolized by ArcaneCrashReporter Arcane 0.1 Debug") == 0u);
    CHECK(text.find("engine      : dbgeng") != std::string::npos);
    CHECK(text.find("--- thread 22 (faulting)") < text.find("--- thread 11"));
    CHECK(text.find("00 death_fixture!main+0x1a4 [f.cpp:52]") != std::string::npos);

    Symbolized none;
    none.engineError = "DebugCreate failed: 0x80004005";
    const std::string fb = FormatSymbolized(none, "b", "--- thread 5 (MAIN)\n00 x.dll + 0x10\n");
    CHECK(fb.find("engine      : unavailable (DebugCreate failed: 0x80004005)") != std::string::npos);
    CHECK(fb.find("--- thread 5 (MAIN)") != std::string::npos);
}

TEST_CASE("symbolized text: the walked thread id is read from the envelope's stack header and put first", "[reporter]")
{
    CHECK(ParseWalkedThreadId("--- thread 4242 (MAIN)\n00 a + 0x1\n") == 4242u);
    CHECK(ParseWalkedThreadId("--- thread 7\n") == 7u);
    CHECK(ParseWalkedThreadId("") == 0u);
    CHECK(ParseWalkedThreadId("garbage") == 0u);

    Symbolized s;
    s.threads.push_back({ 1, false, {} });
    s.threads.push_back({ 4242, false, {} });
    PutFaultingFirst(s, 4242);
    CHECK(s.threads.front().systemId == 4242u);
    CHECK(s.threads.front().faulting);
}

// R78 + R81 + R77's label. Three things this artifact must never say silently:
//
//  - a walk that hit the reporter's cap reads IDENTICALLY to one that ended at
//    the bottom of the stack, and "is this the whole stack?" is the question a
//    human opening a truncated one is actually asking;
//  - a thread that walked to nothing prints as a blank block, which is
//    indistinguishable from a formatting bug. That line was output the brief
//    did not specify and nothing pinned, so a later edit could drop it unseen;
//  - a thread whose system id the engine REFUSED to give up (R77: the
//    SetCurrentThreadId/GetCurrentThreadSystemId pair can fail) must not print
//    as "thread 0", which reads like a real -- and wrong -- id. A correct stack
//    under a wrong thread id is worse than an obviously missing one.
TEST_CASE("symbolized text: a capped walk says so, an empty thread is not a blank block, "
          "and an unnamed thread is not thread 0", "[reporter]")
{
    Symbolized s;
    s.engineAvailable  = true;
    s.symbolPath       = "D:\\bin";
    s.threadsTruncated = true;
    s.threads.push_back({ 7, true, { { 0x1, "m", "f", 0x2, "", 0 } }, /*framesTruncated*/ true });
    s.threads.push_back({ 8, false, {} });   // walked to nothing
    s.threads.push_back({ 0, false, {} });   // the engine would not name it

    const std::string text = FormatSymbolized(s, "b", "");
    CHECK(text.find("(truncated at the reporter's 1-frame cap)") != std::string::npos);
    CHECK(text.find("(truncated at the reporter's 3-thread cap)") != std::string::npos);
    CHECK(text.find("<no frames recovered>") != std::string::npos);
    CHECK(text.find("--- thread <unknown>") != std::string::npos);
    CHECK(text.find("--- thread 0") == std::string::npos);

    // An UNcapped walk says nothing at all -- a marker that is always there is
    // not a marker.
    Symbolized plain;
    plain.engineAvailable = true;
    plain.threads.push_back({ 9, true, { { 0x1, "m", "f", 0x2, "", 0 } } });
    const std::string quiet = FormatSymbolized(plain, "b", "");
    CHECK(quiet.find("truncated") == std::string::npos);
}
