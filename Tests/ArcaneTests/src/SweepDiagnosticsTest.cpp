// S6-2: Diagnostics::Config's tunables are DiagnosticsSettings cvars with the
// pre-sweep defaults; the R2 switches are Dev + CommandLineOnly.
#include <catch2/catch_test_macros.hpp>
#include "Helpers/SettingsSweep.hpp"

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/DiagnosticsSettings.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Host/EarlyConfig.hpp>
#include <Arcane/Host/FramePerf.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Project/ProjectPaths.hpp>   // kDistBuild

#include <type_traits>

using namespace Arcane;

TEST_CASE("sweep: diagnostics defaults are the pre-sweep literals", "[sweep][diag]")
{
    const DiagnosticsSettings d{};
    STATIC_REQUIRE(std::is_same_v<decltype(d.hangSeconds), std::uint32_t>);
    CHECK(d.hangSeconds == 12u);
    CHECK(d.gpuStallSeconds == 8u);
    CHECK(d.installCrashHandler);
    CHECK(d.hangWatchdog);
    CHECK(d.spawnReporter);
    CHECK(d.exitSeconds == 30u);
    CHECK(d.crashHandlingTimeoutSeconds == 60u);
    CHECK(d.minidumpKind == MinidumpKind::Default);
    CHECK(d.logFlushTimeoutMs == 2000u);
    CHECK(d.watchdogPollMs == 250u);
    CHECK(d.watchdogJoinTimeoutMs == 5000u);
    CHECK(d.minFatalWaitMs == 5000u);
    CHECK_FALSE(d.perfLog);
    CHECK(d.perfLogIntervalFrames == 60u);
    Test::RequireDefault("diagnostics.hangSeconds", CVarValue::UInt32(12u));
    Test::RequireDefault("diagnostics.installCrashHandler", CVarValue::Bool(true));
    Test::RequireDefault("diagnostics.minidumpKind", CVarValue::Enum(static_cast<std::int32_t>(MinidumpKind::Default)));
}

TEST_CASE("sweep: Diagnostics::Config is built from the settings, with no second default", "[sweep][diag]")
{
    const Diagnostics::Config plain{};
    CHECK(plain.hangSeconds == DiagnosticsSettings{}.hangSeconds);
    CHECK(plain.minFatalWaitMs == DiagnosticsSettings{}.minFatalWaitMs);
    DiagnosticsSettings s{};
    s.hangSeconds = 40; s.logFlushTimeoutMs = 750; s.installCrashHandler = false;
    const Diagnostics::Config c = Diagnostics::ConfigFromSettings(s);
    CHECK(c.hangSeconds == 40u);
    CHECK(c.logFlushTimeoutMs == 750u);
    CHECK_FALSE(c.installCrashHandler);
}

TEST_CASE("sweep: the R2 capture switches are Dev and command-line only", "[sweep][diag]")
{
    if (!Test::InThisBuild("diagnostics.installCrashHandler"))
    {
        // Dist: the switch is compiled out, so nothing can turn crash capture off (inventory R2).
        CHECK(Settings<DiagnosticsSettings>().installCrashHandler);
        return;
    }
    CVarRegistry& reg = CVarRegistry::Get();
    const CVarHandle h = reg.Find("diagnostics.installCrashHandler");
    REQUIRE_FALSE(h.IsStale());
    const auto e = reg.Explain("diagnostics.installCrashHandler");
    CHECK(HasFlag(e->flags, CVarFlags::Dev));
    CHECK(HasFlag(e->flags, CVarFlags::CommandLineOnly));
    CHECK_FALSE(HasFlag(e->flags, CVarFlags::Archive));
    CHECK(reg.Set(h, CVarValue::Bool(false), SetBy::Project) == SetResult::Denied);
    CHECK(reg.Set(h, CVarValue::Bool(false), SetBy::User) == SetResult::Denied);
    CHECK(reg.Set(h, CVarValue::Bool(false), SetBy::Console) == SetResult::Denied);
    CHECK(reg.Set(h, CVarValue::Bool(false), SetBy::CommandLine) == SetResult::Applied);
    reg.PublishImmediate();
    CHECK_FALSE(Settings<DiagnosticsSettings>().installCrashHandler);
    reg.RevertLayer(SetBy::CommandLine);
    reg.PublishImmediate();
    CHECK(Settings<DiagnosticsSettings>().installCrashHandler);
}

TEST_CASE("sweep: ConfigFromSettings copies every tunable and leaves the host identity alone", "[sweep][diag]")
{
    DiagnosticsSettings s{};
    s.dumpDir = "D:/dumps";
    s.hangSeconds = 41;
    s.gpuStallSeconds = 9;
    s.installCrashHandler = false;
    s.hangWatchdog = false;
    s.reporterPath = "D:/r.exe";
    s.spawnReporter = false;
    s.exitSeconds = 0;
    s.crashHandlingTimeoutSeconds = 61;
    s.minidumpKind = MinidumpKind::Full;
    s.logFlushTimeoutMs = 101;
    s.watchdogPollMs = 11;
    s.watchdogJoinTimeoutMs = 102;
    s.minFatalWaitMs = 1001;
    const Diagnostics::Config c = Diagnostics::ConfigFromSettings(s);
    CHECK(c.dumpDir == "D:/dumps");
    CHECK(c.hangSeconds == 41u);
    CHECK(c.gpuStallSeconds == 9u);
    CHECK_FALSE(c.installCrashHandler);
    CHECK_FALSE(c.startHangWatchdog);
    CHECK(c.reporterPath == "D:/r.exe");
    CHECK_FALSE(c.spawnReporter);
    CHECK(c.exitSeconds == 0u);
    CHECK(c.crashHandlingTimeoutSeconds == 61u);
    CHECK(c.minidumpKind == MinidumpKind::Full);
    CHECK(c.logFlushTimeoutMs == 101u);
    CHECK(c.watchdogPollMs == 11u);
    CHECK(c.watchdogJoinTimeoutMs == 102u);
    CHECK(c.minFatalWaitMs == 1001u);
    const Diagnostics::Config plain{};
    CHECK(c.appName == plain.appName);
    CHECK(c.productName == plain.productName);
    CHECK(c.unattended == plain.unattended);
    CHECK(c.launchMonitor == plain.launchMonitor);
    CHECK(c.commandLine == plain.commandLine);
    CHECK(c.logDir == plain.logDir);
}

TEST_CASE("sweep: --perf is diagnostics.perfLog on the CommandLine rung, and an explicit --set wins", "[sweep][diag]")
{
    struct RestoreCommandLine
    {
        ~RestoreCommandLine()
        {
            CVarRegistry::Get().RevertLayer(SetBy::CommandLine);
            CVarRegistry::Get().PublishImmediate();
        }
    } restore;

    HostConfig cfg;
    cfg.perf = true;
    HostBoot::ApplyEarlyConfigRungs(cfg, CommandLineCVarContext(), /*editor*/ false);
    CHECK(Settings<DiagnosticsSettings>().perfLog);

    CVarRegistry::Get().RevertLayer(SetBy::CommandLine);
    cfg.cvarSets = { "diagnostics.perfLog=0" };
    HostBoot::ApplyEarlyConfigRungs(cfg, CommandLineCVarContext(), /*editor*/ false);
    // Dist's --set runs in the LocalHost context (ruling I3), where a Game
    // setting that is not Cheat is read-only (spec s3.2): the player's --set
    // is refused and --perf's value stands. Elsewhere the explicit --set wins.
    CHECK(Settings<DiagnosticsSettings>().perfLog == kDistBuild);
}

TEST_CASE("sweep: FramePerf follows diagnostics.perfLog and perfLogIntervalFrames live", "[sweep][diag]")
{
    CVarRegistry& reg = CVarRegistry::Get();
    struct RestoreConsole
    {
        ~RestoreConsole()
        {
            CVarRegistry::Get().RevertLayer(SetBy::Console);
            CVarRegistry::Get().PublishImmediate();
        }
    } restore;

    FramePerf perf;
    perf.FrameStart();
    CHECK_FALSE(perf.On());

    REQUIRE(reg.Set(reg.Find("diagnostics.perfLog"), CVarValue::Bool(true), SetBy::Console) == SetResult::Applied);
    REQUIRE(reg.Set(reg.Find("diagnostics.perfLogIntervalFrames"), CVarValue::UInt32(2u), SetBy::Console)
            == SetResult::Applied);
    reg.PublishImmediate();
    perf.FrameStart();
    REQUIRE(perf.On());
    perf.accSim = 1.0;
    perf.Tick(0, 0);
    CHECK(perf.accSim == 1.0);      // frame 1 of 2: still accumulating
    perf.FrameStart();
    perf.Tick(0, 0);
    CHECK(perf.accSim == 0.0);      // frame 2 of 2: emitted and reset

    perf.FrameStart();
    perf.accSim = 1.0;
    perf.Tick(0, 0);
    REQUIRE(reg.Set(reg.Find("diagnostics.perfLog"), CVarValue::Bool(false), SetBy::Console) == SetResult::Applied);
    reg.PublishImmediate();
    perf.FrameStart();
    CHECK_FALSE(perf.On());
    CHECK(perf.accSim == 0.0);      // switching off drops the partial window
}

TEST_CASE("sweep: --perf's settings are not Dev, so they resolve in every configuration (Dist included)", "[sweep][diag]")
{
    // User decision 2026-10-06: perf logging stays usable in Dist (the
    // 1080p/144 fps floor is measured on shipping builds). A Dev cvar is
    // compiled out of Dist, where --perf would only warn.
    for (const char* name : { "diagnostics.perfLog", "diagnostics.perfLogIntervalFrames" })
    {
        INFO("cvar " << name);
        REQUIRE_FALSE(CVarRegistry::Get().Find(name).IsStale());
        const auto e = CVarRegistry::Get().Explain(name);
        REQUIRE(e.has_value());
        CHECK_FALSE(HasFlag(e->flags, CVarFlags::Dev));
        CHECK(e->audience == Audience::Game);
        CHECK(e->scope == SettingScope::PreferencesProject);
        CHECK(e->apply == ApplyMode::Live);
    }
    Test::RequireDefault("diagnostics.perfLog", CVarValue::Bool(false));
    Test::RequireDefault("diagnostics.perfLogIntervalFrames", CVarValue::UInt32(60u));
}

TEST_CASE("sweep: --perf turns perf logging on in Dist's command-line context too", "[sweep][diag]")
{
    // Dist runs --set in the LocalHost context, whose table refuses a Game
    // setting. --perf is the host's own flag, not a player's free-form --set,
    // so it must still land there (user decision 2026-10-06).
    struct RestoreCommandLine
    {
        ~RestoreCommandLine()
        {
            CVarRegistry::Get().RevertLayer(SetBy::CommandLine);
            CVarRegistry::Get().PublishImmediate();
        }
    } restore;

    HostConfig cfg;
    cfg.perf = true;
    HostBoot::ApplyEarlyConfigRungs(cfg, CVarContext::LocalHost, /*editor*/ false);
    CHECK(Settings<DiagnosticsSettings>().perfLog);

    // A free-form --set of the same Game setting is still the player's, and
    // LocalHost still refuses it.
    CVarRegistry::Get().RevertLayer(SetBy::CommandLine);
    cfg.perf = false;
    cfg.cvarSets = { "diagnostics.perfLog=1" };
    HostBoot::ApplyEarlyConfigRungs(cfg, CVarContext::LocalHost, /*editor*/ false);
    CHECK_FALSE(Settings<DiagnosticsSettings>().perfLog);
}
