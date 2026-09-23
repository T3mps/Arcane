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
                out = argv[++i];
                return true;
            };
            std::string v;
            if (s.rfind("--", 0) != 0)
            {
                if (!a.envelopePath.empty()) return { std::nullopt, "two positional arguments: '" + a.envelopePath + "' and '" + s + "'" };
                a.envelopePath = s;
                continue;
            }
            if (s == "--unattended") { a.unattended = true; continue; }
            if (s == "--respawned")  { a.respawned = true; continue; }
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
            else if (s == "--deadline")        { if (!ParseNumber(v, a.deadlineSeconds)) return { std::nullopt, "--deadline is not a number: " + v }; }
            else return { std::nullopt, "unknown argument: " + s };
        }
        if (haveMonitor)
        {
            a.mode = Args::Mode::Monitor;
            if (a.sessionPath.empty()) return { std::nullopt, "--monitor needs --session" };
            if (!a.envelopePath.empty()) return { std::nullopt, "--monitor takes no envelope path" };
        }
        else if (a.envelopePath.empty())
        {
            return { std::nullopt, "missing <report.arcdiag> (or --monitor <pid>)" };
        }
        return { a, "" };
    }
}
