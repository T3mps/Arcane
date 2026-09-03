#pragma once

// HostWitness: the spawn helper Arc B's host-witness scenarios are built on.
//
// It answers three questions a scenario should never have to reimplement:
// did the process finish (or did it have to be killed), can we see what it
// printed, and did it leave behind a report we can parse. GradeProcessFacts
// below judges only the FIRST two -- the process-level facts -- and hands
// back nullopt when a report exists so the SCENARIO judges its CONTENTS.
// This deliberately refuses UE's cascade, which treats a missing report as
// "nothing to compare, so pass": here exit 0 with no report is
// Verdict::Indeterminate, never Verdict::Passed. See GradeProcessFacts below
// for the exact precedence.
//
// WitnessScratch is the fresh-copy hygiene from spec section 6: a scenario
// runs against a COPY of a staged project/scene, never the shared original,
// so one scenario's mutation (or a host crash mid-write) cannot poison the
// next. The copy is deleted on clean teardown and KEPT -- with its path
// printed -- when the destructor runs during a Catch2 REQUIRE failure's
// stack unwind, so a failing scenario leaves evidence behind instead of
// deleting the one thing that would explain it.

#include <Arcane/Host/Verdict.hpp>

#include <Json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Arcane::Test
{
    // What to spawn, and where to look afterward. reportPath is the path the
    // HOST was told (via its own --report switch, a scenario's concern, not
    // this helper's) to write to -- RunWitness never adds it to args itself,
    // it only watches for the file to appear there.
    struct WitnessInvocation
    {
        std::filesystem::path   exePath;
        std::vector<std::string> args;
        std::filesystem::path   workingDir;
        std::filesystem::path   reportPath;
        std::uint32_t            hardCapMs = 60000;
    };

    // Everything RunWitness observed. report/reportParsed are only meaningful
    // together -- report is default-constructed (null) whenever reportParsed
    // is false, so a caller that skips the check and reads report anyway gets
    // an obviously-empty json rather than a stale/partial parse.
    struct WitnessRun
    {
        int          exitCode  = -1;
        bool         timedOut  = false;   // hard cap expired; the process was killed
        bool         progressingAtKill = false; // stdout grew OR CPU advanced in the final poll window
        bool         reportFound  = false;
        bool         reportParsed = false;
        nlohmann::json report;            // valid only when reportParsed
        std::uint64_t wallMs = 0;         // spawn -> exit/kill, measured
        std::filesystem::path stdoutPath, stderrPath; // captured beside reportPath
    };

    // Spawns inv.exePath with inv.args, waits up to inv.hardCapMs (killing
    // and reporting timedOut on expiry -- never hangs the caller even if the
    // kill itself doesn't land cleanly), and loads inv.reportPath if given.
    // Windows-only (CreateProcessW); Arc B's scenarios are Windows hosts.
    [[nodiscard]] WitnessRun RunWitness(const WitnessInvocation& inv);

    // The PROCESS-LEVEL half of the verdict only. Engaged => graded without
    // opening the report (Errored / Indeterminate); nullopt => a parsed
    // report exists and the SCENARIO judges its contents.
    //
    // Precedence, full stop:
    //   timedOut                          -> Errored
    //   !reportFound || !reportParsed      -> Indeterminate (any exit code, including 0)
    //   otherwise                          -> nullopt
    [[nodiscard]] std::optional<Arcane::Verdict> GradeProcessFacts(const WitnessRun& run);

    // Fresh-copy hygiene (spec section 6): copies srcStagedDir to
    // %TEMP%/arcane-witness/<scenarioName>-<pid>, recursively. The copy is
    // deleted on clean destruction; when destroyed during a Catch2 failure's
    // stack unwind (std::uncaught_exceptions() > 0) it is KEPT instead, and
    // its path printed as "KEPT WITNESS ARTIFACT: <path>".
    class WitnessScratch
    {
    public:
        WitnessScratch(const std::filesystem::path& srcStagedDir, std::string scenarioName);
        ~WitnessScratch();

        WitnessScratch(const WitnessScratch&)            = delete;
        WitnessScratch& operator=(const WitnessScratch&) = delete;

        [[nodiscard]] const std::filesystem::path& Dir() const;   // the copy's root

    private:
        std::filesystem::path m_dir;
    };
}
