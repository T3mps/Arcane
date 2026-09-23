// ArcaneCrashReporter's argument contract (crash window plan 2, spec §6).
//
// PURE and std-only on purpose: this TU source-compiles into ArcaneTests
// (premake5.lua, ArcaneTests' `files` list) so the [reporter] cases drive the
// whole refusal table directly, the same split arcbuild's Request.cpp and the
// editor's ConsoleBuffer.cpp already use. Nothing Win32, nothing engine-side.
//
// Every argument here arrives from a host that is ALREADY DYING, written by
// Diagnostics::SpawnReporter on the crash thread. A malformed line is refused
// and named (ExitCode::kBadArgs) rather than absorbed into a default -- the
// numbers on it decide which process gets terminated (§9) and how long the
// unattended deadline runs.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace Arcane::Reporter
{
    namespace ExitCode
    {
        inline constexpr int kOk           = 0;
        inline constexpr int kBadArgs      = 2;
        inline constexpr int kNoEnvelope   = 3;
        inline constexpr int kHostMismatch = 4;   // --host-created did not match: terminate refused
        inline constexpr int kDeadline     = 5;   // unattended deadline expired; partial sibling written
        // R64, APPROVED as a plan amendment (controller, fix round 1) -- so
        // this is a CONTRACT ADDITION, not merely a header constant. The plan's
        // global constraints and spec §4's exit-code block both enumerate the
        // reporter's codes as 0/2/3/4/5. TASK 11's spec close-out adds 6 to
        // both of those and to spec §6's failure-mode list; the line to write
        // there is:
        //
        //     6 -- everything parsed and loaded, but the one artifact of this
        //          hand-off could not be written.
        //
        // Why a new code rather than one of the four above: the envelope was
        // READ successfully, so kNoEnvelope would name an untrue cause; no
        // deadline expired, so kDeadline would name one too; and exiting 0 is
        // precisely the lie R64 exists to remove. This is the exact failure a
        // human stares at when the symbolized file is missing and nothing else
        // looks wrong, which is what makes it worth naming. 6 collides with
        // nothing: the reporter owns 0-5, the hosts own 10-13 (spec §4).
        inline constexpr int kWriteFailed  = 6;   // the .symbolized.txt sibling could not be written
    }

    struct Args
    {
        enum class Mode { Report, Monitor };
        Mode          mode = Mode::Report;
        std::string   envelopePath;          // Report: the positional <path.arcdiag>
        std::uint32_t pid = 0;               // Report: --pid; Monitor: --monitor <pid>
        std::string   kind;                  // --kind (informational; the envelope decides)
        std::string   product;               // --product "<name>"  (window title)
        std::string   app;                   // --app "<appName>"   (monitor: report stem)
        bool          unattended = false;    // --unattended
        std::string   recoveredEvent;        // --recovered-event <name>   (hang protocol)
        std::string   relaunch;              // --relaunch "<line>"        (D1: overrides the envelope's commandLine)
        std::uint64_t hostCreated = 0;       // --host-created <u64>       (D7)
        // R33: the host's OWN process handle, duplicated inheritable into this
        // process by the host; 0 = absent, fall back to OpenProcess by pid.
        // The monitor (task 9) respawns itself (D16), and by the time the
        // second instance wants the host, the pid may be dead or reused -- a
        // handle the host opened cannot name a stranger. PARSED here only; no
        // handle is inherited anywhere yet.
        std::uint64_t hostHandle = 0;        // --host-handle <u64>        (R33)
        std::string   sessionPath;           // --session <file>           (monitor, REQUIRED: the host's session record, D15)
        std::string   logPath;               // --log <path>               (monitor, optional override of the record)
        std::string   reportDir;             // --report-dir <dir>         (monitor, optional override of the record)
        std::string   symbolPath;            // --symbol-path "<a;b>"      (D5 test seam)
        // --deadline <s> (unattended; tests lower it). R60: 60 is spec §6's
        // DEFAULT, not the only legal value; the FLOOR is 1 and 0 is refused
        // at parse time -- see ReporterArgs.cpp for the reasoning.
        std::uint32_t deadlineSeconds = 60;
        bool          respawned = false;     // --respawned                (monitor: the second instance, D16)
    };

    struct ParseResult
    {
        std::optional<Args> args;
        std::string         error;   // one line, empty on success
    };

    // argv WITHOUT the program name. Report mode needs the positional envelope
    // path; Monitor mode needs --monitor <pid> and --session. Unknown flags,
    // a missing value, or a non-numeric number are errors.
    [[nodiscard]] ParseResult ParseArgs(std::span<const std::string> argv);
    [[nodiscard]] std::string Usage();
}
