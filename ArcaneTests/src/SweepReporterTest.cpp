// S6-4: the crash reporter's tunables are diagnostics.reporter.* settings
// (DiagnosticsReporterSettings), plus diagnostics.logTailLines and
// ui.copyFlashSeconds shared with the editor. The reporter has no registry:
// the host formats them onto its command line (Diagnostics::ReporterSettingsArgs)
// and ReporterArgs parses them back, each defaulting to the struct's own value.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/DiagnosticsSettings.hpp>
#include <Arcane/Base/ReporterSettings.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/UiSettings.hpp>

#include "ReporterArgs.hpp"

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace Arcane;

namespace
{
    // The host's wide tail as argv tokens. It is ASCII by construction
    // (flags and numbers), so a narrowing copy is exact.
    std::vector<std::string> Tokens(const std::wstring& tail)
    {
        std::vector<std::string> out;
        std::wstringstream ss{ tail };
        for (std::wstring t; ss >> t;)
        {
            std::string& narrow = out.emplace_back();
            for (const wchar_t c : t)
                narrow.push_back(static_cast<char>(c));
        }
        return out;
    }

    std::vector<std::string> ReportLine(std::vector<std::string> extra = {})
    {
        std::vector<std::string> argv = { "x.arcdiag", "--pid", "1", "--kind", "crash" };
        argv.insert(argv.end(), extra.begin(), extra.end());
        return argv;
    }

    void CheckMeta(const char* name, Audience audience, SettingScope scope, ApplyMode apply, bool dev)
    {
        INFO("cvar " << name);
        const auto e = CVarRegistry::Get().Explain(name);
        REQUIRE(e.has_value());
        CHECK(e->audience == audience);
        CHECK(e->scope == scope);
        CHECK(e->apply == apply);
        CHECK(HasFlag(e->flags, CVarFlags::Dev) == dev);
        CHECK_FALSE(e->help.empty());
    }
}

TEST_CASE("sweep: reporter defaults are the pre-sweep literals", "[sweep][reporter]")
{
    const DiagnosticsReporterSettings r{};
    CHECK(r.deadlineSeconds == 60u);  CHECK(r.maxFramesPerThread == 64u);
    CHECK(r.maxFramesFaultingThread == 8192u); CHECK(r.maxThreads == 64u);
    CHECK(r.dbgengWaitMs == 30000u);  CHECK(r.hangLogTailLines == 512u);
    CHECK(r.flushTimeoutMs == 1000u); CHECK(r.uiPollMs == 250u);
    CHECK(r.windowWidth == 1000u);    CHECK(r.windowHeight == 640u);  CHECK(r.windowReadyMs == 5000u);
    CHECK(DiagnosticsSettings{}.logTailLines == 200u);
    CHECK(Test::SameBits(UiSettings{}.copyFlashSeconds, 0.75));
    Test::RequireDefault("diagnostics.reporter.deadlineSeconds", CVarValue::UInt32(60u));
    Test::RequireDefault("diagnostics.reporter.windowWidth", CVarValue::UInt32(1000u));
    Test::RequireDefault("diagnostics.logTailLines", CVarValue::UInt32(200u));
    Test::RequireDefault("ui.copyFlashSeconds", CVarValue::Float64(0.75));
}

TEST_CASE("sweep: reporter settings carry the inventory's audience, scope, apply and Dev flag", "[sweep][reporter]")
{
    CheckMeta("diagnostics.reporter.deadlineSeconds", Audience::Game, SettingScope::Project, ApplyMode::Restart, true);
    CheckMeta("diagnostics.reporter.uiPollMs", Audience::Game, SettingScope::Project, ApplyMode::Restart, true);
    CheckMeta("diagnostics.reporter.windowWidth", Audience::Game, SettingScope::PreferencesProject, ApplyMode::Restart, false);
    CheckMeta("diagnostics.reporter.windowHeight", Audience::Game, SettingScope::PreferencesProject, ApplyMode::Restart, false);
    CheckMeta("diagnostics.logTailLines", Audience::Game, SettingScope::PreferencesProject, ApplyMode::Live, true);
    CheckMeta("ui.copyFlashSeconds", Audience::Game, SettingScope::PreferencesMachine, ApplyMode::Live, true);
}

TEST_CASE("sweep: the host's reporter arguments round-trip through ReporterArgs", "[sweep][reporter]")
{
    DiagnosticsReporterSettings s{};
    s.deadlineSeconds = 9; s.maxFramesPerThread = 33; s.maxFramesFaultingThread = 4096; s.maxThreads = 7;
    s.dbgengWaitMs = 2000; s.hangLogTailLines = 77; s.flushTimeoutMs = 0; s.uiPollMs = 20;
    s.windowWidth = 800; s.windowHeight = 500; s.windowReadyMs = 300;
    const auto parsed = Reporter::ParseArgs(ReportLine(Tokens(Diagnostics::ReporterSettingsArgs(s, 123, 1.5))));
    INFO(parsed.error);
    REQUIRE(parsed.args.has_value());
    CHECK(parsed.args->deadlineSeconds == 9u);
    CHECK(parsed.args->maxFramesPerThread == 33u);
    CHECK(parsed.args->maxFramesFaultingThread == 4096u);
    CHECK(parsed.args->maxThreads == 7u);
    CHECK(parsed.args->dbgengWaitMs == 2000u);
    CHECK(parsed.args->logTailLines == 123u);
    CHECK(parsed.args->hangLogTailLines == 77u);
    CHECK(parsed.args->flushTimeoutMs == 0u);
    CHECK(parsed.args->uiPollMs == 20u);
    CHECK(parsed.args->windowWidth == 800u);
    CHECK(parsed.args->windowHeight == 500u);
    CHECK(parsed.args->windowReadyMs == 300u);
    CHECK(Test::SameBits(parsed.args->copyFlashSeconds, 1.5));

    // The defaults round-trip bit-exactly too (%.17g), so a default host
    // hands the reporter exactly the values it would have used without flags.
    const auto def = Reporter::ParseArgs(ReportLine(Tokens(Diagnostics::ReporterSettingsArgs(
        DiagnosticsReporterSettings{}, DiagnosticsSettings{}.logTailLines, UiSettings{}.copyFlashSeconds))));
    REQUIRE(def.args.has_value());
    CHECK(Test::SameBits(def.args->copyFlashSeconds, 0.75));
    CHECK(def.args->windowWidth == 1000u);
    CHECK(def.args->logTailLines == 200u);
}

TEST_CASE("sweep: a reporter line without the settings flags uses the struct defaults", "[sweep][reporter]")
{
    const auto plain = Reporter::ParseArgs(ReportLine());
    REQUIRE(plain.args.has_value());
    const DiagnosticsReporterSettings d{};
    CHECK(plain.args->deadlineSeconds == d.deadlineSeconds);
    CHECK(plain.args->maxFramesPerThread == d.maxFramesPerThread);
    CHECK(plain.args->maxFramesFaultingThread == d.maxFramesFaultingThread);
    CHECK(plain.args->maxThreads == d.maxThreads);
    CHECK(plain.args->dbgengWaitMs == d.dbgengWaitMs);
    CHECK(plain.args->hangLogTailLines == d.hangLogTailLines);
    CHECK(plain.args->flushTimeoutMs == d.flushTimeoutMs);
    CHECK(plain.args->uiPollMs == d.uiPollMs);
    CHECK(plain.args->windowWidth == d.windowWidth);
    CHECK(plain.args->windowHeight == d.windowHeight);
    CHECK(plain.args->windowReadyMs == d.windowReadyMs);
    CHECK(plain.args->logTailLines == DiagnosticsSettings{}.logTailLines);
    CHECK(Test::SameBits(plain.args->copyFlashSeconds, UiSettings{}.copyFlashSeconds));
}

TEST_CASE("sweep: malformed reporter settings flags are refused, not absorbed", "[sweep][reporter]")
{
    const char* const bad[][2] = {
        { "--max-threads", "0" },          // floor 1: a walk over no thread is no report
        { "--max-frames-thread", "0" },
        { "--max-frames-fault", "0" },
        { "--dbgeng-wait-ms", "0" },
        { "--ui-poll-ms", "0" },           // a zero slice would spin the attended wait
        { "--window-ready-ms", "0" },
        { "--window", "800" },             // no 'x'
        { "--window", "800x" },
        { "--window", "0x500" },
        { "--window", "800x500x2" },
        { "--log-tail", "12a" },
        { "--copy-flash", "nan" },
        { "--copy-flash", "inf" },
        { "--copy-flash", "-1" },
        { "--copy-flash", "0.5s" },
    };
    for (const auto& b : bad)
    {
        INFO(b[0] << " " << b[1]);
        const auto r = Reporter::ParseArgs(ReportLine({ b[0], b[1] }));
        CHECK_FALSE(r.args.has_value());
        CHECK_FALSE(r.error.empty());
    }
    // Zero is legal where the row's floor is 0.
    const auto zero = Reporter::ParseArgs(ReportLine({ "--log-tail", "0", "--hang-log-tail", "0", "--flush-ms", "0", "--copy-flash", "0" }));
    REQUIRE(zero.args.has_value());
    CHECK(zero.args->logTailLines == 0u);
    CHECK(zero.args->hangLogTailLines == 0u);
    CHECK(zero.args->flushTimeoutMs == 0u);
    CHECK(zero.args->copyFlashSeconds == 0.0);
}

#if defined(_WIN32)
// S6-4 carried gap (Live apply): diagnostics.logTailLines and
// ui.copyFlashSeconds are Live, so a reporter spawned after a change in the
// same session must carry the new values -- not Install's snapshot.
TEST_CASE("sweep: a Live reporter setting reaches the next reporter spawn", "[sweep][reporter]")
{
    const Test::ScopedCodeLayer codeLayer;   // reverts the Code rung + publishes even when a REQUIRE fails mid-case
    Diagnostics::Config cfg;
    cfg.appName             = "SweepReporterTest";
    cfg.dumpDir             = (std::filesystem::temp_directory_path() / "arcane-sweep-reporter").string();
    cfg.installCrashHandler = false;   // never hijack the suite's own fault handling
    cfg.startHangWatchdog   = false;
    cfg.spawnReporter       = false;
    struct Armed
    {
        explicit Armed(const Diagnostics::Config& c) { Diagnostics::Install(c); }
        ~Armed() { Diagnostics::Shutdown(); }
    } armed{ cfg };

    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle tail  = reg.Find("diagnostics.logTailLines");
    const CVarHandle flash = reg.Find("ui.copyFlashSeconds");
    REQUIRE_FALSE(tail.IsStale());
    REQUIRE_FALSE(flash.IsStale());
    struct Revert
    {
        CVarRegistry& reg; CVarHandle a, b;
        ~Revert() { reg.ClearRung(a, SetBy::Code); reg.ClearRung(b, SetBy::Code); reg.PublishImmediate(); }
    } revert{ reg, tail, flash };

    const auto before = Reporter::ParseArgs(ReportLine(Tokens(Diagnostics::CurrentReporterSettingsArgs())));
    REQUIRE(before.args.has_value());
    CHECK(before.args->logTailLines == Settings<DiagnosticsSettings>().logTailLines);

    REQUIRE(reg.Set(tail, CVarValue::UInt32(123u), SetBy::Code) == SetResult::Applied);
    REQUIRE(reg.Set(flash, CVarValue::Float64(1.5), SetBy::Code) == SetResult::Applied);
    reg.PublishImmediate();

    const auto after = Reporter::ParseArgs(ReportLine(Tokens(Diagnostics::CurrentReporterSettingsArgs())));
    INFO(after.error);
    REQUIRE(after.args.has_value());
    CHECK(after.args->logTailLines == 123u);
    CHECK(Test::SameBits(after.args->copyFlashSeconds, 1.5));
}
#endif

// S6-5 carried follow-up: the Live watch is armed at Install, which can run
// before a watched cvar is registered. The attach must leave that name for
// the next Install/RetargetDumpDir rather than latch it as watched, and must
// never attach twice. Run on a private registry (the engine's has both cvars
// from static init, so "not registered yet" cannot be staged on it).
TEST_CASE("sweep: a reporter-settings watch armed before registration attaches on a later call", "[sweep][reporter]")
{
    CVarRegistry reg;
    static constexpr std::string_view kNames[] = { "diagnostics.logTailLines", "ui.copyFlashSeconds" };
    bool attached[2]{};
    struct Count { int fired = 0; } count;
    const auto onChange = +[](CVarHandle, void* user) { ++static_cast<Count*>(user)->fired; };

    // "Install" before either cvar exists: nothing attaches, nothing latches.
    CHECK_FALSE(Diagnostics::AttachMissingCVarCallbacks(reg, kNames, attached, onChange, &count));
    CHECK_FALSE(attached[0]);
    CHECK_FALSE(attached[1]);

    const CVarHandle tail = reg.Register(CVarDesc{ .name = "diagnostics.logTailLines", .type = CVarType::UInt32,
                                                   .defaultValue = CVarValue::UInt32(200u), .help = "late tail probe",
                                                   .module = "test" });
    REQUIRE_FALSE(tail.IsStale());
    // One registered, one still missing: the first attaches, the watch is not complete.
    CHECK_FALSE(Diagnostics::AttachMissingCVarCallbacks(reg, kNames, attached, onChange, &count));
    CHECK(attached[0]);
    CHECK_FALSE(attached[1]);

    const CVarHandle flash = reg.Register(CVarDesc{ .name = "ui.copyFlashSeconds", .type = CVarType::Float64,
                                                    .defaultValue = CVarValue::Float64(0.75), .help = "late flash probe",
                                                    .module = "test" });
    REQUIRE_FALSE(flash.IsStale());
    CHECK(Diagnostics::AttachMissingCVarCallbacks(reg, kNames, attached, onChange, &count));
    CHECK(attached[1]);
    // A further call (another RetargetDumpDir) adds nothing.
    CHECK(Diagnostics::AttachMissingCVarCallbacks(reg, kNames, attached, onChange, &count));

    // A change to logTailLines now reaches the callback -- once, not once per call above.
    REQUIRE(reg.Set(tail, CVarValue::UInt32(123u), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(count.fired == 1);
    REQUIRE(reg.Set(flash, CVarValue::Float64(1.5), SetBy::Code) == SetResult::Applied);
    reg.Publish();
    CHECK(count.fired == 2);
}
