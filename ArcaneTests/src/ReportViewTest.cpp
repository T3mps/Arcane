// The reporter's envelope-to-view model (crash window plan 2, task 6; spec §6
// "Window" content).
//
// ReportView.cpp source-compiles into this exe (premake5.lua, ArcaneTests'
// `files` list) exactly as ReporterArgs.cpp and SymbolizedText.cpp do -- and
// that list is NOT gated on the target OS, so this TU and the one it drives
// must stay free of windows.h, directly and transitively. LogTail.cpp (the
// filesystem half) is NOT compiled here.

#include "ReportView.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace Arcane::Reporter;

namespace
{
    Arcane::Diag::Envelope Crash()
    {
        Arcane::Diag::Envelope e;
        e.guid = Arcane::Guid::Generate();
        e.kind = "crash";
        e.reason = "crash (unhandled exception)";
        e.timestampUtc = "2026-09-23T10:11:12Z";
        e.appName = "ArcaneEditor";
        e.phase = "switch_plugin_load";
        e.buildInfo = "Arcane 0.1 Debug";
        e.cpuThreadSummary = "--- thread 4242 (MAIN)\n00 ArcaneCore.dll + 0x1234\n";
        e.foreignModules = { "GTIII-OSD64.dll", "SomethingElse.dll" };
        e.commandLine = "D:/bin/ArcaneEditor.exe --project D:/p";
        e.siblingTxt = "D:/p/Saved/Diagnostics/ArcaneEditor-20260923-101112-pid4242.txt";
        return e;
    }
    Args Attended() { Args a; a.envelopePath = "D:/p/Saved/Diagnostics/ArcaneEditor-20260923-101112-pid4242.arcdiag"; a.product = "Arcane Editor"; return a; }
}

TEST_CASE("report view: plain-words kinds", "[reporter]")
{
    CHECK(PlainWordsKind("crash") == "crashed");
    CHECK(PlainWordsKind("hang") == "stopped responding");
    CHECK(PlainWordsKind("gpu-stall") == "the GPU stopped responding");
    CHECK(PlainWordsKind("gpu-crash") == "the GPU device was lost");
    CHECK(PlainWordsKind("assert") == "an assertion failed");
    CHECK(PlainWordsKind("terminate") == "terminated");
    CHECK(PlainWordsKind("out-of-memory") == "ran out of memory");
    CHECK(PlainWordsKind("ensure") == "hit a recoverable check");
    CHECK(PlainWordsKind("abnormal-exit") == "exited abnormally");
    CHECK(PlainWordsKind("whatever") == "stopped (whatever)");
}

TEST_CASE("report view: a crash envelope becomes header, reason, named injected modules, one portable thread, relaunch", "[reporter]")
{
    const ReportView v = BuildReportView(Crash(), Attended(), nullptr, "line1\nline2\n");
    CHECK(v.title == "Arcane Editor -- crashed");
    CHECK(v.headline == "Arcane Editor crashed");
    CHECK(v.whenLine.find("2026-09-23T10:11:12Z") != std::string::npos);
    CHECK(v.whenLine.find("switch_plugin_load") != std::string::npos);
    CHECK(v.reasonText == "crash (unhandled exception)");
    CHECK(v.injectedText.find("GTIII-OSD64.dll") != std::string::npos);
    CHECK(v.injectedText.find("GPU Tweak III") != std::string::npos);     // the table names the product
    CHECK(v.injectedText.find("tier 1") != std::string::npos);
    CHECK(v.injectedText.find("SomethingElse.dll -- injected, uncatalogued") != std::string::npos);
    REQUIRE(v.threads.size() == 1);
    CHECK(v.threads[0].label == "thread 4242 (MAIN)");
    CHECK(v.threads[0].text.find("ArcaneCore.dll + 0x1234") != std::string::npos);
    CHECK(v.logTail == "line1\nline2\n");
    CHECK(v.reportFolder == "D:/p/Saved/Diagnostics");
    CHECK(v.relaunchLine == "D:/bin/ArcaneEditor.exe --project D:/p");
    CHECK(v.canRelaunch);
    CHECK_FALSE(v.isHang);
    CHECK(v.gpuText.empty());
}

TEST_CASE("report view: symbolized threads replace the portable one, faulting first; a hang disables relaunch; --relaunch overrides", "[reporter]")
{
    Symbolized s;
    s.engineAvailable = true;
    s.threads.push_back({ 4242, true,  { { 0x1, "ArcaneCore", "Arcane::Diagnostics::SubmitReport", 0x10, "D.cpp", 9 } } });
    s.threads.push_back({ 7,    false, { { 0x2, "ntdll", "NtWaitForSingleObject", 0x14, "", 0 } } });
    Args a = Attended();
    a.relaunch = "override.exe";
    const ReportView v = BuildReportView(Crash(), a, &s, "");
    REQUIRE(v.threads.size() == 2);
    CHECK(v.threads[0].label == "thread 4242 (faulting)");
    CHECK(v.threads[0].text.find("ArcaneCore!Arcane::Diagnostics::SubmitReport+0x10 [D.cpp:9]") != std::string::npos);
    CHECK(v.threads[1].label == "thread 7");
    CHECK(v.relaunchLine == "override.exe");

    Arcane::Diag::Envelope hang = Crash();
    hang.kind = "hang";
    hang.reason = "hang (main thread has not ticked for 12.3s)";
    const ReportView h = BuildReportView(hang, Attended(), nullptr, "");
    CHECK(h.isHang);
    CHECK_FALSE(h.canRelaunch);
    CHECK(h.headline == "Arcane Editor stopped responding");
}

TEST_CASE("report view: R110 -- the exit sentinel's 'hang at exit' (kind hang, exitCode 12) is a CRASH view, not a hang window", "[reporter]")
{
    // Diagnostics.cpp's exit sentinel files kind "hang" with ExitCode::kExitSentinel
    // (12): the host is already terminating itself, not stalled and waiting to be
    // watched. The reporter-side mirror of R95 (IsHangProtocolReport, host side)
    // keys isHang on exitCode == 0 too, so this must present as an ordinary crash
    // view -- Relaunch enabled when a relaunch line exists, no HangWatch opened.
    Arcane::Diag::Envelope e = Crash();
    e.kind = "hang";
    e.reason = "hang at exit (Vulkan teardown did not complete)";
    e.exitCode = 12;
    const ReportView v = BuildReportView(e, Attended(), nullptr, "");
    CHECK_FALSE(v.isHang);
    CHECK(v.canRelaunch == !v.relaunchLine.empty());
    CHECK(v.canRelaunch);   // Crash() sets a non-empty commandLine, so this IS a crash view with Relaunch live
    CHECK(v.headline == "Arcane Editor stopped responding");
}

TEST_CASE("report view: the GPU section and the abnormal-exit shape", "[reporter]")
{
    Arcane::Diag::Envelope g = Crash();
    g.kind = "gpu-crash";
    g.queues.push_back({ "graphics", "pass:tonemap", { "pass:gpu-fault" } });
    g.fault = { "page-fault", "0xDEADBEEF0000", "SceneColor" };
    g.activeLayers = { "DRED", "markers" };
    const ReportView v = BuildReportView(g, Attended(), nullptr, "");
    CHECK(v.gpuText.find("graphics: last completed pass:tonemap, in flight pass:gpu-fault") != std::string::npos);
    CHECK(v.gpuText.find("fault: page-fault at 0xDEADBEEF0000 (SceneColor)") != std::string::npos);
    CHECK(v.gpuText.find("layers: DRED, markers") != std::string::npos);

    Arcane::Diag::Envelope ab = Crash();
    ab.kind = "abnormal-exit";
    ab.reason = "abnormal-exit: 0xC0000409 STATUS_STACK_BUFFER_OVERRUN";
    ab.cpuThreadSummary.clear();
    const ReportView x = BuildReportView(ab, Attended(), nullptr, "tail\n");
    CHECK(x.isAbnormalExit);
    CHECK(x.threads.empty());
    CHECK(x.headline == "Arcane Editor exited abnormally");
}

TEST_CASE("report view: LastLines and DetailsText", "[reporter]")
{
    CHECK(LastLines("a\nb\nc\nd\n", 2) == "c\nd\n");
    CHECK(LastLines("a\nb", 5) == "a\nb");
    CHECK(LastLines("", 3).empty());
    const ReportView v = BuildReportView(Crash(), Attended(), nullptr, "L1\n");
    const std::string d = DetailsText(v, 0);
    CHECK(d.find("Arcane Editor crashed") == 0u);
    CHECK(d.find("crash (unhandled exception)") != std::string::npos);
    CHECK(d.find("thread 4242 (MAIN)") != std::string::npos);
    CHECK(d.find("L1") != std::string::npos);
}
