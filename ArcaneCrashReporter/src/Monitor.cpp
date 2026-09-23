#include "Monitor.hpp"
#include "FileText.hpp"
#include "MonitorRule.hpp"
#include "ReportView.hpp"
#include "ReporterShared.hpp"
#include "ReporterWindow.hpp"
#include "Win32Text.hpp"

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Platform/NativeWindow.hpp>

#include <Json.hpp>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace Arcane::Reporter
{
    namespace
    {
        // The host's session record (D15), re-read AFTER the host is gone.
        // PRESENCE is the whole verdict ("no clean exit recorded"); the
        // contents are only what the synthesized report needs, so a record
        // that exists but does not parse still counts as present, with every
        // field falling back to the command line or a default.
        struct Session
        {
            std::string app, product, logPath, reportDir, commandLine, recoveredEvent;
        };

        std::optional<Session> ReadSession(const std::filesystem::path& file)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(file, ec)) return std::nullopt;
            Session s;
            const auto doc = nlohmann::json::parse(Slurp(file), nullptr, /*allow_exceptions*/false);
            if (doc.is_object())
            {
                auto str = [&](const char* k) { return doc.contains(k) && doc[k].is_string() ? doc[k].get<std::string>() : std::string{}; };
                s.app = str("app"); s.product = str("product"); s.logPath = str("logPath");
                s.reportDir = str("reportDir"); s.commandLine = str("commandLine"); s.recoveredEvent = str("recoveredEvent");
            }
            if (s.app.empty())
            {
                // "<app>-pid<n>.session" -- the one field the file NAME carries.
                const std::string name = ToUtf8(file.stem().wstring());
                const std::size_t at = name.rfind("-pid");
                if (at != std::string::npos && at > 0) s.app = name.substr(0, at);
            }
            return s;
        }

        std::uint64_t FileTimeU64(const FILETIME& t)
        {
            return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        }

        // R100 + the hang-then-kill decision: has one of THIS host's reports
        // (name match: app prefix AND pid suffix -- never the reporter's own,
        // never another process of the same app) already told the story of
        // this death? Only a report written during this host's lifetime
        // (last write >= its creation time, so a same-pid report from an
        // earlier run in the same folder cannot count), and only a FATAL one
        // (MonitorRule.hpp: ReportSpeaksForExit).
        bool HostReportSpoke(const std::filesystem::path& dir, const std::string& app, std::uint32_t pid, std::uint64_t hostCreated)
        {
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            {
                if (!IsHostReportName(ToUtf8(e.path().filename().wstring()), app, pid)) continue;
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (hostCreated && GetFileAttributesExW(e.path().c_str(), GetFileExInfoStandard, &fad)
                    && FileTimeU64(fad.ftLastWriteTime) < hostCreated)
                    continue;
                const auto env = Diag::ReadFile(e.path());
                if (env && ReportSpeaksForExit(env->exitCode)) return true;
            }
            return false;
        }

        // An attended hang reporter for this host is still up and its host
        // never recovered: the D11 event (named in the record) is still open
        // -- only a live hang reporter holds it once the host is gone -- and
        // still unsignalled. MonitorRule.hpp's HangWindowOwnsExit decides
        // whether that window is what the user is looking at.
        bool UnrecoveredHangReporterAlive(const std::string& eventName)
        {
            if (eventName.empty()) return false;
            const HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, ToWide(eventName).c_str());
            if (!ev) return false;
            const bool unsignalled = WaitForSingleObject(ev, 0) == WAIT_TIMEOUT;
            CloseHandle(ev);
            return unsignalled;
        }

        // Diagnostics.cpp's stem stamp (local time, "YYYYMMDD-HHMMSS") and
        // the envelope's ISO-8601 UTC, from the same instant.
        void Stamps(std::string& compact, std::string& iso)
        {
            SYSTEMTIME local{}, utc{};
            GetLocalTime(&local);
            GetSystemTime(&utc);
            char a[32], b[32];
            std::snprintf(a, sizeof(a), "%04u%02u%02u-%02u%02u%02u",
                          local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
            std::snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                          utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
            compact = a; iso = b;
        }

        std::filesystem::path WithSuffix(const std::filesystem::path& stem, const wchar_t* suffix)
        {
            std::filesystem::path p = stem;   // R64: append to the PATH, never narrow through the ACP
            p += suffix;
            return p;
        }

        // Reap the record (UE :1408) -- and, R104(c), the "<record>.tmp" a
        // host leaves behind when it dies between writing the temp copy and
        // renaming it over the record (Diagnostics.cpp, WriteSessionRecord).
        // Both are no-ops when the host already cleaned up.
        void ReapRecord(const std::filesystem::path& record)
        {
            std::error_code ec;
            std::filesystem::remove(record, ec);
            std::filesystem::remove(WithSuffix(record, L".tmp"), ec);
        }

        // R98 / R33: the host's handle. The host passes an inheritable
        // duplicate of its own handle (--host-handle) through a one-entry
        // handle list; it is taken only if it really names the pid we were
        // told about. OpenProcess by pid is the FALLBACK for a line without
        // one (a hand-run monitor): it is the only path on which pid reuse
        // could matter, and it is opened HERE, at start, while the host is
        // presumably still alive.
        HANDLE OpenHost(const Args& a)
        {
            if (a.hostHandle != 0)
            {
                const HANDLE inherited = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(a.hostHandle));
                if (GetProcessId(inherited) == a.pid) return inherited;
                ARC_WARN("monitor: --host-handle {} does not name pid {} ({}); opening the pid instead",
                         a.hostHandle, a.pid, GetLastError());
            }
            return OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, a.pid);
        }
    }

    int RunMonitor(const Args& a)
    {
        HANDLE host = OpenHost(a);
        if (!host)
        {
            // Already gone before we could watch -- no exit code to name, and
            // no way to know whether its record will ever be deleted. Nothing
            // to say.
            ARC_WARN("monitor: cannot open host pid {} ({})", a.pid, GetLastError());
            return Arcane::Reporter::ExitCode::kOk;
        }

        // D16 (UE: WindowsPlatformCrashContext.cpp:643-657): the FIRST
        // instance relaunches itself and exits, so the watcher's parent is a
        // process that no longer exists and "End process tree" on the host
        // cannot take it along. The respawn inherits the SAME host handle,
        // the same way the host passed it (R98) -- one handle, via the handle
        // list. GetCommandLineW is the exact line this instance got; when the
        // handle came from the OpenProcess fallback its value is appended
        // (ParseArgs keeps the LAST --host-handle).
        if (!a.respawned)
        {
            std::wstring cmd = GetCommandLineW();
            const bool   fromFallback = a.hostHandle == 0 || reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(a.hostHandle)) != host;
            if (fromFallback)
                cmd += L" --host-handle " + std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(host)));
            cmd += L" --respawned";
            DWORD child = 0;
            if (SpawnDetachedInheriting(cmd, host, &child))
            {
                // D14 x D16: the host granted the foreground to THIS pid; the
                // respawn is the one that may show a window, so pass it on.
                AllowSetForegroundWindow(child);
                CloseHandle(host);
                return Arcane::Reporter::ExitCode::kOk;
            }
            // Respawn failed: watch from here rather than not at all.
            ARC_WARN("monitor: respawn failed ({}); watching from the first instance", GetLastError());
        }

        WaitForSingleObject(host, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(host, &code);
        FILETIME created{}, exited{}, kernel{}, user{};
        const std::uint64_t hostCreated = GetProcessTimes(host, &created, &exited, &kernel, &user) ? FileTimeU64(created) : 0;
        CloseHandle(host);

        // Judged NOW, after the exit: RetargetDumpDir rewrites the record
        // when a project opens, and Shutdown/atexit delete it. The process
        // object is signalled only after its DLLs detached, so an atexit
        // deletion has always landed by the time this line runs.
        const std::filesystem::path   recordPath = std::filesystem::path(ToWide(a.sessionPath));
        const std::optional<Session>  session    = ReadSession(recordPath);
        const std::string app       = !a.app.empty() ? a.app : (session && !session->app.empty() ? session->app : std::string("Arcane"));
        const std::string reportDirUtf8 = !a.reportDir.empty() ? a.reportDir : (session ? session->reportDir : std::string{});
        const std::filesystem::path reportDir = reportDirUtf8.empty() ? recordPath.parent_path()
                                                                      : std::filesystem::path(ToWide(reportDirUtf8));

        bool crashPathSpoke = false;
        if (session)
        {
            crashPathSpoke = HostReportSpoke(reportDir, app, a.pid, hostCreated)
                          || HangWindowOwnsExit(UnrecoveredHangReporterAlive(session->recoveredEvent), code);
        }
        std::error_code ec;
        if (ClassifyHostExit(session.has_value(), crashPathSpoke) == MonitorVerdict::Silent)
        {
            ReapRecord(recordPath);   // a no-op when the host already did
            return Arcane::Reporter::ExitCode::kOk;
        }

        // Synthesize (spec s5.8): envelope with the code named, the .txt
        // header, and the host's log tail as the backlog. No minidump, no
        // stack -- this process never saw the fault. The log tail is read
        // BEFORE this process installs its own Diagnostics below, so nothing
        // of ours can touch the host's log first.
        const std::string logPath  = !a.logPath.empty() ? a.logPath : session->logPath;
        const std::string product  = !a.product.empty() ? a.product : (session->product.empty() ? app : session->product);
        const std::string relaunch = !a.relaunch.empty() ? a.relaunch : session->commandLine;
        const std::string logTail  = logPath.empty() ? std::string{} : LastLines(Slurp(std::filesystem::path(ToWide(logPath))), 512);

        std::filesystem::create_directories(reportDir, ec);

        // This process's OWN Diagnostics (D13, R70), installed only now:
        // a monitor that has nothing to say -- the overwhelmingly common
        // case, every clean exit -- creates no log file and no crash thread,
        // and one that sleeps for hours beside the editor holds nothing
        // but a process handle. dumpDir = the report dir the RECORD names,
        // so if the monitor itself dies while writing, its report lands
        // beside the host's, where the user is already looking; its stem is
        // "ArcaneCrashReporter-...", which IsHostReportName never counts as
        // the host's (R100). spawnReporter stays false (R70: never true
        // here) and launchMonitor with it: a monitor never watches itself.
        Arcane::Diagnostics::Config diag;
        diag.appName           = "ArcaneCrashReporter";
        diag.productName       = "Arcane Crash Reporter";
        diag.dumpDir           = ToUtf8(reportDir.wstring());
        diag.unattended        = a.unattended;
        diag.spawnReporter     = false;   // R70
        diag.launchMonitor     = false;
        diag.startHangWatchdog = false;
        const ArmedDiagnostics armed(diag);

        std::string compact, iso;
        Stamps(compact, iso);
        // Diagnostics.cpp's "<app>-<stamp>-pid<n>" -- unless the host wrote a
        // (survivable) report in this same second, whose files this must not
        // overwrite: then "<app>-<stamp>-<k>-pid<n>", which keeps the app
        // prefix and pid suffix IsHostReportName matches on.
        std::filesystem::path stem = reportDir / ToWide(app + "-" + compact + "-pid" + std::to_string(a.pid));
        for (int k = 2; std::filesystem::exists(WithSuffix(stem, L".arcdiag"), ec) && k < 100; ++k)
            stem = reportDir / ToWide(app + "-" + compact + "-" + std::to_string(k) + "-pid" + std::to_string(a.pid));
        const std::filesystem::path txtPath = WithSuffix(stem, L".txt");
        const std::filesystem::path logCopy = WithSuffix(stem, L".log.txt");
        const std::filesystem::path envPath = WithSuffix(stem, L".arcdiag");

        Diag::Envelope e;
        e.guid         = Guid::Generate();
        e.kind         = "abnormal-exit";
        e.reason       = AbnormalExitReason(code);
        e.timestampUtc = iso;
        e.appName      = app;
        e.buildInfo    = Arcane::BuildInfo();
        e.logPath      = logPath;
        e.commandLine  = relaunch;
        e.exitCode     = static_cast<int>(code);
        e.siblingTxt   = ToUtf8(txtPath.wstring());

        char codeHex[16];
        std::snprintf(codeHex, sizeof(codeHex), "0x%08X", static_cast<unsigned>(code));
        bool ok = WriteText(logCopy, logTail);
        ok = WriteText(txtPath,
                       "=== Arcane diagnostic report (monitor) ===\n"
                       "reason      : " + e.reason + "\n"
                       "app         : " + app + "\n"
                       "pid         : " + std::to_string(a.pid) + "\n"
                       "exit code   : " + codeHex + " (" + std::to_string(code) + ")\n"
                       "time (UTC)  : " + iso + "\n"
                       "log         : " + logPath + "\n"
                       "minidump    : <none -- the monitor never saw the fault>\n\n"
                       "The host exited without recording a clean shutdown and without a report of its own:\n"
                       "a __fastfail, a /GS cookie failure, heap corruption, a stack overflow with no room\n"
                       "for SEH, or an external kill (spec s5.8). The log tail is in the .log.txt beside this.\n") && ok;
        ok = Diag::WriteFile(e, envPath) && ok;
        ReapRecord(recordPath);   // now that its story is told
        if (ok) ARC_WARN("monitor: {} exited abnormally ({}); report at {}", app, e.reason, ToUtf8(envPath.wstring()));
        else    ARC_ERROR("monitor: {} exited abnormally ({}); the report at {} could not be written in full",
                          app, e.reason, ToUtf8(stem.wstring()));
        if (a.unattended) return Arcane::Reporter::ExitCode::kOk;

        // The window, as for a crash (spec s5.8). R99: the buttons go through
        // ReporterMain's OnButton with no hang. R34: the presenter is declared
        // BEFORE the window, so the window -- destroyed first -- joins its
        // thread before anything the `[&]` callback reaches goes away.
        Args view         = a;
        view.envelopePath = ToUtf8(envPath.wstring());
        view.product      = product;
        view.relaunch     = relaunch;
        const ReportView finished = BuildReportView(e, view, nullptr, LastLines(logTail, 200));
        std::unique_ptr<ReporterWindow> ui;
        NativeWindow                    window;
        ui = std::make_unique<ReporterWindow>(finished, [&](int id) { OnButton(id, *ui, window, nullptr); });
        ui->SetView(finished);   // before Show: nothing to symbolize, so the first paint is the final one
        ui->Show(window, product);
        if (window.WasEverOpen()) window.Wait();
        return Arcane::Reporter::ExitCode::kOk;
    }
}
