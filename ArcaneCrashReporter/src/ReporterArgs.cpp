#include "ReporterArgs.hpp"

#include <charconv>

namespace Arcane::Reporter
{
    namespace
    {
        // std::from_chars, NOT strtoul/atoi: it never consumes a leading sign
        // for an unsigned target, never accepts a hex/octal prefix, and the
        // `r.ptr == end` check is what refuses "4242x" and "0x1f4" outright.
        // atoi would have silently produced 0 for both.
        template <typename T>
        bool ParseNumber(const std::string& s, T& out)
        {
            const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
            return r.ec == std::errc{} && r.ptr == s.data() + s.size();
        }

        [[nodiscard]] bool IsFlag(const std::string& s) { return s.rfind("--", 0) == 0; }

        // The options that take a following value. This exists so an UNKNOWN
        // flag is named as unknown BEFORE its value is demanded (R68): with
        // the value fetched first, "--bogus" in final position came back as
        // "--bogus needs a value" -- the wrong defect named, and the
        // unknown-argument branch left unreachable from the end of a line,
        // which is a branch the suite believed it was covering.
        //
        // Kept in sync with the dispatch chain below by the defensive
        // "unknown argument" arm at its end: an option listed here but not
        // dispatched there fails loudly instead of being silently ignored.
        [[nodiscard]] bool TakesValue(const std::string& s)
        {
            return s == "--pid" || s == "--monitor" || s == "--kind" || s == "--product"
                || s == "--app" || s == "--recovered-event" || s == "--relaunch"
                || s == "--host-created" || s == "--host-handle" || s == "--session"
                || s == "--log" || s == "--report-dir" || s == "--symbol-path"
                || s == "--deadline";
        }
    }

    std::string Usage()
    {
        return "ArcaneCrashReporter <report.arcdiag> --pid <n> [--kind <k>] [--product \"<name>\"] [--app <name>]\n"
               "                    [--unattended] [--recovered-event <name>] [--relaunch \"<line>\"]\n"
               "                    [--host-created <u64>] [--host-handle <u64>] [--symbol-path \"<dir;dir>\"]\n"
               "                    [--deadline <s>]\n"
               "ArcaneCrashReporter --monitor <pid> --session <file> [--unattended] [--respawned]\n"
               "                    [--product \"<name>\"] [--app <name>] [--log <path>] [--report-dir <dir>]\n"
               "                    [--host-handle <u64>]\n";
    }

    ParseResult ParseArgs(std::span<const std::string> argv)
    {
        Args a;
        bool haveMonitor = false;
        for (std::size_t i = 0; i < argv.size(); ++i)
        {
            const std::string& s = argv[i];
            auto value = [&](std::string& out) -> bool
            {
                if (i + 1 >= argv.size()) return false;
                // R66: a following token that is itself a flag is a DROPPED
                // value, not a value. No option here can legitimately take a
                // "--"-prefixed argument (--relaunch's own line is
                // "<exe> --project X", which starts with the exe). Without
                // this, "--relaunch --unattended" both makes `relaunch`
                // garbage that task 7's restart button would hand to
                // CreateProcess AND silently drops the host's unattended
                // intent -- putting a window on a machine that explicitly
                // asked for none.
                if (IsFlag(argv[i + 1])) return false;
                out = argv[++i];
                return true;
            };
            std::string v;
            if (!IsFlag(s))
            {
                if (!a.envelopePath.empty()) return { std::nullopt, "two positional arguments: '" + a.envelopePath + "' and '" + s + "'" };
                a.envelopePath = s;
                continue;
            }
            if (s == "--unattended") { a.unattended = true; continue; }
            if (s == "--respawned")  { a.respawned = true; continue; }
            // R68: unknown BEFORE missing-value, so the error names the real defect.
            if (!TakesValue(s)) return { std::nullopt, "unknown argument: " + s };
            if (!value(v)) return { std::nullopt, s + " needs a value" };
            if      (s == "--pid")             { if (!ParseNumber(v, a.pid)) return { std::nullopt, "--pid is not a number: " + v }; }
            else if (s == "--monitor")         { if (!ParseNumber(v, a.pid)) return { std::nullopt, "--monitor is not a pid: " + v }; haveMonitor = true; }
            else if (s == "--kind")            a.kind = v;
            else if (s == "--product")         a.product = v;
            else if (s == "--app")             a.app = v;
            else if (s == "--recovered-event") a.recoveredEvent = v;
            else if (s == "--relaunch")        a.relaunch = v;
            else if (s == "--host-created")    { if (!ParseNumber(v, a.hostCreated)) return { std::nullopt, "--host-created is not a number: " + v }; }
            else if (s == "--host-handle")     { if (!ParseNumber(v, a.hostHandle)) return { std::nullopt, "--host-handle is not a number: " + v }; }
            else if (s == "--session")         a.sessionPath = v;
            else if (s == "--log")             a.logPath = v;
            else if (s == "--report-dir")      a.reportDir = v;
            else if (s == "--symbol-path")     a.symbolPath = v;
            // R60 (controller ruling, task 5): the deadline has a FLOOR of one
            // second, and zero is REFUSED rather than given a meaning.
            //
            // Zero could have meant "no deadline" or "expire immediately".
            // "No deadline" is the worst reading available: the one flag whose
            // entire purpose is to BOUND an unattended child would silently
            // unbound it, which is precisely the headless hazard spec §6 wrote
            // it for. "Expire immediately" is honest but useless -- it
            // guarantees the symbolizing worker is killed before it resolves a
            // single frame, so every run carrying it produces a partial report
            // and exit 5 while looking like it tried; and it is not even a
            // reliable lever for the deadline branch, since wait_for(0s, pred)
            // evaluates the predicate once and a finished worker would return
            // success. So it is refused, on the same fail-closed rule R65
            // applied to the pid: a value that cannot express a legitimate
            // intent is named at the boundary, never absorbed.
            //
            // One second is the floor: the smallest value the flag can express
            // that still gives the worker a real window, and the lever the
            // deadline branch is driven with. Spec §6's 60 s remains the
            // DEFAULT, not the only legal value.
            else if (s == "--deadline")
            {
                if (!ParseNumber(v, a.deadlineSeconds)) return { std::nullopt, "--deadline is not a number: " + v };
                if (a.deadlineSeconds == 0) return { std::nullopt, "--deadline 0 is not a deadline (the floor is 1 second)" };
            }
            // Unreachable by construction -- TakesValue above is the gate.
            // Kept so an option added to that list without a dispatch arm
            // here is refused loudly rather than accepted and ignored.
            else return { std::nullopt, "unknown argument: " + s };
        }
        // R65: the per-mode validation. `pid != 0` is required in BOTH modes,
        // and that is deliberately uniform -- 0 names the System Idle Process,
        // so it can never be the host a report came from or the host a monitor
        // watches. Usage() declared --pid mandatory and nothing enforced it,
        // so a Report line missing it parsed clean with a silent pid == 0: the
        // field tasks 8 and 9 use to FIND AND TERMINATE the host. A loud parse
        // failure here beats a mysterious OpenProcess failure two tasks later,
        // and a trust boundary should fail closed.
        //
        // A Monitor pid cannot be ABSENT (the flag that supplies it selects the
        // mode), but an explicit "--monitor 0" is a different thing, and
        // refusing it at the boundary is cheaper than making task 9 reason
        // about a value the parser already knows is impossible.
        //
        // A later task may relax either rule if it finds a legitimate no-pid
        // path, with a documented reason.
        if (haveMonitor)
        {
            a.mode = Args::Mode::Monitor;
            if (a.sessionPath.empty()) return { std::nullopt, "--monitor needs --session" };
            if (!a.envelopePath.empty()) return { std::nullopt, "--monitor takes no envelope path" };
            if (a.pid == 0) return { std::nullopt, "--monitor 0 is not a host pid" };
        }
        else
        {
            if (a.envelopePath.empty()) return { std::nullopt, "missing <report.arcdiag> (or --monitor <pid>)" };
            if (a.pid == 0) return { std::nullopt, "missing --pid <n> (the host this report came from)" };
        }
        return { a, "" };
    }
}
