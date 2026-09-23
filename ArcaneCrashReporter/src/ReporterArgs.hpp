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
        std::uint32_t deadlineSeconds = 60;  // --deadline <s>             (unattended; tests lower it)
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
