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
// Symbolizer.hpp is the pure seam ITSELF free of windows.h/dbgeng (only
// Symbolizer.cpp touches those, and that TU is not compiled into this exe --
// see its own header comment). SymbolizeOptions' defaults are therefore
// testable directly, same as SymbolizedText.hpp above.
#include "Symbolizer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
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

// R112 (post-review UE audit): the FAULTING thread's cap is deep, matching
// UE's MaxFrames = 8192 (WindowsPlatformStackWalkExt.cpp:481); every OTHER
// thread keeps the small cap Symbolizer.cpp has always used. A pure default
// check -- Symbolizer.cpp's dbgeng walk itself is CrashPathTest's job (a real
// minidump), not this TU's.
TEST_CASE("symbolize options: the faulting thread's cap is deep (8192), other threads stay small (64)", "[reporter]")
{
    SymbolizeOptions opt;
    CHECK(opt.maxFramesFaultingThread == 8192u);
    CHECK(opt.maxFramesPerThread == 64u);
    CHECK(opt.maxThreads == 64u);
}

TEST_CASE("symbolized text: ParseSymbolized round-trips FormatSymbolized byte for byte (engine available)", "[reporter]")
{
    Symbolized s;
    s.engineAvailable = true;
    s.symbolPath = "srv*C:\\sym*https://msdl.microsoft.com/download/symbols;D:\\bin";
    s.threads.push_back({ 4242, true, {
        SymFrame{ 0, "ArcaneEditor", "Arcane::Editor::Boom", 0x1a4, "D:\\dev\\Arcane\\Boom.cpp", 52 },
        SymFrame{ 0, "KERNEL32", "BaseThreadInitThunk", 0x14, "", 0 },
        SymFrame{ 0, "death-fixture.exe", "", 0x1234, "", 0 } }, true });
    s.threads.push_back({ 0, false, {} });                       // <unknown> id, <no frames recovered>
    s.threadsTruncated = true;
    const std::string text = FormatSymbolized(s, "Arcane 0.1 Debug@deadbeef", "");

    const std::optional<ParsedSymbolized> p = ParseSymbolized(text);
    REQUIRE(p.has_value());
    CHECK(p->buildInfo == "Arcane 0.1 Debug@deadbeef");
    CHECK(FormatSymbolized(p->sym, p->buildInfo, p->portableBody) == text);
    CHECK(p->sym.engineAvailable);
    CHECK(p->sym.symbolPath == s.symbolPath);
    CHECK(p->sym.threadsTruncated);
    REQUIRE(p->sym.threads.size() == 2);
    const SymThread& t = p->sym.threads[0];
    CHECK(t.systemId == 4242);
    CHECK(t.faulting);
    CHECK(t.framesTruncated);
    REQUIRE(t.frames.size() == 3);
    CHECK(t.frames[0].module == "ArcaneEditor");
    CHECK(t.frames[0].function == "Arcane::Editor::Boom");
    CHECK(t.frames[0].displacement == 0x1a4);
    CHECK(t.frames[0].file == "D:\\dev\\Arcane\\Boom.cpp");    // the drive colon survives
    CHECK(t.frames[0].line == 52);
    CHECK(t.frames[0].address == 0);                           // not in the text
    CHECK(t.frames[2].module == "death-fixture.exe");
    CHECK(t.frames[2].function.empty());
    CHECK(p->sym.threads[1].systemId == 0);
    CHECK(p->sym.threads[1].frames.empty());
}

TEST_CASE("symbolized text: ParseSymbolized keeps an engine error containing ')' and the portable body verbatim", "[reporter]")
{
    Symbolized s;
    s.engineError = "LoadLibrary(dbgeng.dll) failed (126)";
    const std::string portable = "--- thread 7 (MAIN)\n00 ArcaneCore.dll + 0x10\n";
    const std::string text = FormatSymbolized(s, "b", portable);
    const std::optional<ParsedSymbolized> p = ParseSymbolized(text);
    REQUIRE(p.has_value());
    CHECK_FALSE(p->sym.engineAvailable);
    CHECK(p->sym.engineError == "LoadLibrary(dbgeng.dll) failed (126)");
    CHECK(p->portableBody == portable);
    CHECK(p->sym.threads.empty());
    CHECK(FormatSymbolized(p->sym, p->buildInfo, p->portableBody) == text);
}

TEST_CASE("symbolized text: ParseSymbolized refuses a foreign file and skips malformed lines", "[reporter]")
{
    CHECK_FALSE(ParseSymbolized("").has_value());
    CHECK_FALSE(ParseSymbolized("hello\nengine      : dbgeng\n").has_value());
    const std::optional<ParsedSymbolized> p = ParseSymbolized(
        "symbolized by ArcaneCrashReporter x\nengine      : dbgeng\nsymbol path : s\n\n"
        "--- thread 9\n7 too-few-digits\n00 mod!fn+0xzz\nnonsense\n01 mod!fn+0x10\n");
    REQUIRE(p.has_value());
    REQUIRE(p->sym.threads.size() == 1);
    REQUIRE(p->sym.threads[0].frames.size() == 2);
    CHECK(p->sym.threads[0].frames[0].function == "fn+0xzz");   // unparsable offset: kept in the name, not dropped
    CHECK(p->sym.threads[0].frames[1].displacement == 0x10);
}
