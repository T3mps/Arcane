#include <Arcane/Base/Diagnostics.hpp>

#include <Arcane/Base/DiagEnvelope.hpp>   // Diag::Envelope, WriteFile -- the .arcdiag sibling
#include <Arcane/Base/Engine.hpp>         // ExecutablePathUtf8(), BuildInfo()
#include <Arcane/Base/Log.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace Arcane::Diagnostics
{
namespace
{
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] std::int64_t NowTicks() noexcept
    {
        return Clock::now().time_since_epoch().count();
    }

    [[nodiscard]] double SecondsSince(std::int64_t ticks) noexcept
    {
        const Clock::duration d{NowTicks() - ticks};
        return std::chrono::duration<double>(d).count();
    }

    // Monotone seconds in an arbitrary epoch -- the clock ProgressStallRule is
    // polled with. Its epoch is irrelevant; only differences are used.
    [[nodiscard]] double NowSeconds() noexcept
    {
        return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    }

    // ---- shared state -----------------------------------------------------
    // Deliberately file-static rather than a class: the crash filter is a raw
    // C callback and the watchdog outlives any owner we could hand it.

    Config             g_cfg;
    std::atomic<bool>  g_installed{false};

    // Main-thread liveness. g_beatSeen gates the WHOLE hang trigger: a host
    // that never calls Heartbeat() (a headless tool, a test) must get silence,
    // not a spurious report hangSeconds after boot.
    std::atomic<std::int64_t> g_lastBeat{0};
    std::atomic<bool>         g_beatSeen{false};

    // GPU-side progress (Task 7). Same arming discipline as the pair above and
    // for the same reason: a host that never renders publishes nothing here and
    // must get silence. The VALUE is what matters (a monotone count of GPU sync
    // points passed) -- its staleness, not its age, is the signal, so unlike
    // g_lastBeat this is not a timestamp.
    std::atomic<std::uint64_t> g_gpuFence{0};
    std::atomic<bool>          g_gpuBeatSeen{false};
    std::atomic<std::int64_t>  g_lastGpuBeat{0};

    // How stale the last GpuHeartbeat CALL may be before the GPU rule stops
    // evaluating. A frozen counter means "the GPU stopped" only while somebody
    // is still publishing it; a minimized host renders no frames at all, and its
    // frozen counter must read as "no data", not as a stall. Two seconds is ~120
    // frames of grace at 60Hz.
    constexpr double kGpuBeatFreshnessCapSeconds = 2.0;

    // The gate has to trip BEFORE the stall threshold could, or a host that
    // published once and then stopped rendering would be reported as stalled in
    // the window between the two. Deriving the window from the configured
    // threshold makes that structural for ANY gpuStallSeconds instead of an
    // accident of the shipped default (which is 8, so this returns the 2s cap).
    [[nodiscard]] double GpuBeatFreshnessSeconds(double stallSeconds) noexcept
    {
        return std::min(kGpuBeatFreshnessCapSeconds, stallSeconds * 0.5);
    }

    std::mutex  g_phaseMutex;
    std::string g_phase;

    std::atomic<std::uint32_t> g_reportCount{0};

    // Serializes report writing. Two triggers can race (the watchdog fires while
    // the main thread faults); interleaved reports are worse than a late one.
    std::mutex g_reportMutex;

    // GPU-section provider slot (Task 5/6 install their backend here). A
    // separate mutex from g_reportMutex: WriteReportImpl only holds this one
    // long enough to copy the two pointers out, then calls the provider
    // unlocked -- an unknown callback must never run while holding a lock
    // another thread might need in order to install/clear it.
    std::mutex         g_gpuProviderMutex;
    GpuSectionProvider g_gpuProvider     = nullptr;
    void*              g_gpuProviderUser = nullptr;

    // DbgHelp is explicitly NOT thread-safe -- every Sym* call in the process
    // must be serialized, including ones the walk makes indirectly.
    std::mutex        g_symMutex;
    std::atomic<bool> g_symReady{false};

    std::thread       g_watchdog;
    std::atomic<bool> g_watchdogStop{false};

#if defined(_WIN32)
    DWORD  g_mainThreadId = 0;
    LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

    // A fault INSIDE the handler must not re-enter it forever.
    std::atomic<bool> g_inCrashHandler{false};
#endif

    [[nodiscard]] std::filesystem::path ReportDir()
    {
        std::error_code ec;
        std::filesystem::path dir;

        if (!g_cfg.dumpDir.empty())
        {
            dir = std::filesystem::path(g_cfg.dumpDir);
        }
        else
        {
            // Beside the exe, never the CWD: the hosts are documented as
            // cd-then-run, so a CWD-relative report is a report nobody finds.
            const std::string exe = ExecutablePathUtf8();
            dir = exe.empty() ? std::filesystem::path(".")
                              : std::filesystem::path(exe).parent_path();
            dir /= "diagnostics";
        }

        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    [[nodiscard]] std::string TimeStampForFilename()
    {
#if defined(_WIN32)
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04u%02u%02u-%02u%02u%02u",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return buf;
#else
        return "unknown-time";
#endif
    }

    // ISO-8601 UTC, for the .arcdiag envelope's timestampUtc -- distinct
    // from TimeStampForFilename's local-time, filename-safe stamp above. A
    // report a teammate opens in another timezone needs an unambiguous
    // instant, not the reporter's local clock.
    [[nodiscard]] std::string TimestampUtcIso8601()
    {
#if defined(_WIN32)
        SYSTEMTIME st{};
        GetSystemTime(&st);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return buf;
#else
        return "unknown-time";
#endif
    }

    [[nodiscard]] std::string CurrentPhase()
    {
        std::lock_guard lock(g_phaseMutex);
        return g_phase;
    }

    // Kind derivation for the .arcdiag envelope: substring match on the
    // REASON string, never the trigger site. Covers every phrasing
    // WriteReportImpl sees today -- "crash (unhandled exception)" (the
    // crash filter, below) and "hang (main thread has not ticked for ...)"
    // (WatchdogMain, below) -- plus the GPU vocabulary named in
    // docs/specs/2026-08-11-gpu-crash-diagnostics-design.md: the planned
    // GPU-progress watchdog (Task 7, not yet wired) reports exactly
    // "gpu-stall", and a device-removed capture (Task 5/6) is expected to
    // name "gpu-crash". A reason naming "gpu" always resolves to one of the
    // two gpu kinds rather than falling through to plain crash/hang, and it
    // is checked before "hang" so "gpu-stall" cannot misclassify as "hang".
    [[nodiscard]] std::string DeriveKind(const char* reason)
    {
        const std::string_view r = reason ? reason : "";
        if (r.find("gpu") != std::string_view::npos)
            return r.find("stall") != std::string_view::npos ? "gpu-stall" : "gpu-crash";
        if (r.find("hang") != std::string_view::npos)
            return "hang";
        return "crash";
    }

#if defined(_WIN32)
    // ---- symbols ----------------------------------------------------------

    void EnsureSymbols()
    {
        if (g_symReady.load(std::memory_order_acquire)) return;
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
        // TRUE == also enumerate already-loaded modules, so Arcane.dll and the
        // hosted plugin resolve without us tracking load events ourselves.
        if (SymInitialize(GetCurrentProcess(), nullptr, TRUE))
            g_symReady.store(true, std::memory_order_release);
    }

    [[nodiscard]] std::string DescribeThread(DWORD tid)
    {
        // GetThreadDescription is Win10 1607+. Looked up dynamically rather than
        // linked so this file does not carry an SDK floor of its own.
        using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        static GetThreadDescriptionFn fn = []() -> GetThreadDescriptionFn {
            if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
                return reinterpret_cast<GetThreadDescriptionFn>(
                    reinterpret_cast<void*>(GetProcAddress(k32, "GetThreadDescription")));
            return nullptr;
        }();
        if (!fn) return {};

        HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (!th) return {};

        std::string out;
        PWSTR desc = nullptr;
        if (SUCCEEDED(fn(th, &desc)) && desc)
        {
            if (const int n = WideCharToMultiByte(CP_UTF8, 0, desc, -1, nullptr, 0, nullptr, nullptr); n > 1)
            {
                out.resize(static_cast<std::size_t>(n) - 1);
                WideCharToMultiByte(CP_UTF8, 0, desc, -1, out.data(), n, nullptr, nullptr);
            }
            LocalFree(desc);
        }
        CloseHandle(th);
        return out;
    }

    // One symbolized frame line. Caller holds g_symMutex.
    [[nodiscard]] std::string DescribeFrame(unsigned index, DWORD64 pc)
    {
        char line[1024];

        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)]{};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = MAX_SYM_NAME;

        // Module name first: it is the one field that survives a total symbol
        // miss, and "which DLL" is often already the answer.
        std::string moduleName;
        if (const DWORD64 base = SymGetModuleBase64(GetCurrentProcess(), pc); base != 0)
        {
            IMAGEHLP_MODULE64 mod{};
            mod.SizeOfStruct = sizeof(mod);
            if (SymGetModuleInfo64(GetCurrentProcess(), base, &mod))
                moduleName = mod.ModuleName;
        }

        DWORD64 symDisp = 0;
        if (SymFromAddr(GetCurrentProcess(), pc, &symDisp, sym))
        {
            IMAGEHLP_LINE64 ln{};
            ln.SizeOfStruct = sizeof(ln);
            DWORD lineDisp = 0;
            if (SymGetLineFromAddr64(GetCurrentProcess(), pc, &lineDisp, &ln))
            {
                std::snprintf(line, sizeof(line), "  %02u  %s!%s + 0x%llx   [%s:%lu]",
                              index, moduleName.empty() ? "?" : moduleName.c_str(),
                              sym->Name, static_cast<unsigned long long>(symDisp),
                              ln.FileName ? ln.FileName : "?", ln.LineNumber);
            }
            else
            {
                std::snprintf(line, sizeof(line), "  %02u  %s!%s + 0x%llx",
                              index, moduleName.empty() ? "?" : moduleName.c_str(),
                              sym->Name, static_cast<unsigned long long>(symDisp));
            }
        }
        else
        {
            std::snprintf(line, sizeof(line), "  %02u  %s!0x%llx   <no symbol>",
                          index, moduleName.empty() ? "?" : moduleName.c_str(),
                          static_cast<unsigned long long>(pc));
        }
        return line;
    }

    // Walks one thread. `ctxIn` is the fault context when we have one (a crash);
    // otherwise null and we fetch it ourselves.
    //
    // The current thread is NEVER suspended -- suspending yourself never
    // resumes. For any other thread we suspend, copy, walk, and resume
    // immediately, one thread at a time: holding several suspended raises the
    // odds of wedging on a lock DbgHelp itself needs.
    [[nodiscard]] std::vector<std::string> WalkThread(DWORD tid, const CONTEXT* ctxIn)
    {
        std::vector<std::string> frames;

#if defined(_M_X64)
        const bool isSelf = (tid == GetCurrentThreadId());

        CONTEXT ctx{};
        HANDLE  th = nullptr;

        if (isSelf)
        {
            if (ctxIn) ctx = *ctxIn;
            else       RtlCaptureContext(&ctx);
            th = GetCurrentThread();
        }
        else
        {
            th = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME, FALSE, tid);
            if (!th)
            {
                frames.emplace_back("  <could not open thread>");
                return frames;
            }
            if (SuspendThread(th) == static_cast<DWORD>(-1))
            {
                CloseHandle(th);
                frames.emplace_back("  <could not suspend thread>");
                return frames;
            }
            ctx.ContextFlags = CONTEXT_FULL;
            if (!GetThreadContext(th, &ctx))
            {
                ResumeThread(th);
                CloseHandle(th);
                frames.emplace_back("  <could not read thread context>");
                return frames;
            }
        }

        STACKFRAME64 frame{};
        frame.AddrPC.Offset    = ctx.Rip;
        frame.AddrPC.Mode      = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode   = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode   = AddrModeFlat;

        {
            std::lock_guard lock(g_symMutex);
            EnsureSymbols();

            constexpr unsigned kMaxFrames = 96;
            for (unsigned i = 0; i < kMaxFrames; ++i)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), th, &frame, &ctx,
                                 nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                    break;
                if (frame.AddrPC.Offset == 0) break;
                frames.emplace_back(DescribeFrame(i, frame.AddrPC.Offset));
            }
        }

        if (!isSelf)
        {
            ResumeThread(th);
            CloseHandle(th);
        }
#else
        (void)tid; (void)ctxIn;
        frames.emplace_back("  <stack walking implemented for x64 only>");
#endif

        if (frames.empty())
            frames.emplace_back("  <no frames recovered>");
        return frames;
    }

    bool WriteMiniDump(const std::filesystem::path& path, EXCEPTION_POINTERS* ep)
    {
        HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;

        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers    = FALSE;

        // Not WithFullMemory: for a wedged GPU host that is hundreds of MB and
        // takes long enough that the user gives up. This set keeps every
        // thread's stack, the handle table (which lock is held -- the question a
        // deadlock actually asks), and memory the stacks point at.
        const auto type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithThreadInfo | MiniDumpWithHandleData |
            MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);

        const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                          type, ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(file);
        return ok != FALSE;
    }
#endif  // _WIN32

    // The one report path both triggers share.
    std::string WriteReportImpl(const char* reason, void* exceptionPointers)
    {
#if defined(_WIN32)
        std::lock_guard reportLock(g_reportMutex);

        auto* ep = static_cast<EXCEPTION_POINTERS*>(exceptionPointers);

        const std::filesystem::path dir   = ReportDir();
        const std::string           stamp = TimeStampForFilename();
        const std::string           base  = g_cfg.appName + "-" + stamp + "-pid" +
                                            std::to_string(GetCurrentProcessId());

        const std::filesystem::path dmpPath  = dir / (base + ".dmp");
        const std::filesystem::path txtPath  = dir / (base + ".txt");
        const std::filesystem::path diagPath = dir / (base + ".arcdiag");
        // Same stem, no extension -- what a GPU-section provider writes its
        // own <reportStem>.gpudump against (F-6b).
        const std::filesystem::path reportStem = dir / base;

        // Minidump FIRST, on purpose. The text walk below suspends threads and
        // calls DbgHelp in a process that is already misbehaving; if it wedges,
        // the .dmp is still on disk and still answers the question.
        const bool dumpOk = WriteMiniDump(dmpPath, ep);

        std::string out;
        out.reserve(16 * 1024);
        auto append = [&out](const std::string& s) { out += s; out += '\n'; };

        append("=== Arcane diagnostic report ===");
        append("reason      : " + std::string(reason ? reason : "unspecified"));
        append("app         : " + g_cfg.appName);
        append("pid         : " + std::to_string(GetCurrentProcessId()));
        append("main thread : " + std::to_string(g_mainThreadId));
        if (const std::string phase = CurrentPhase(); !phase.empty())
            append("phase       : " + phase);
        if (g_beatSeen.load(std::memory_order_acquire))
        {
            char b[64];
            std::snprintf(b, sizeof(b), "%.2f s", SecondsSince(g_lastBeat.load(std::memory_order_acquire)));
            append("since beat  : " + std::string(b));
        }
        append(std::string("minidump    : ") + (dumpOk ? dmpPath.string() : "<failed to write>"));

        if (ep && ep->ExceptionRecord)
        {
            char b[128];
            std::snprintf(b, sizeof(b), "code 0x%08lx at 0x%llx",
                          ep->ExceptionRecord->ExceptionCode,
                          reinterpret_cast<unsigned long long>(ep->ExceptionRecord->ExceptionAddress));
            append("exception   : " + std::string(b));
        }
        append("");

        // EVERY thread. A main thread parked on a worker's result names the
        // wrong culprit; the worker's stack is the answer, and we cannot tell
        // which is which before looking.
        const DWORD selfTid = GetCurrentThreadId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            const DWORD pid = GetCurrentProcessId();
            for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID != pid) continue;

                std::string title = "--- thread " + std::to_string(te.th32ThreadID);
                if (te.th32ThreadID == g_mainThreadId) title += " (MAIN)";
                if (te.th32ThreadID == selfTid)        title += " (reporter)";
                if (const std::string name = DescribeThread(te.th32ThreadID); !name.empty())
                    title += " \"" + name + "\"";
                title += " ---";
                append(title);

                // The fault context belongs to the thread that FAULTED, which is
                // the thread reporting. Handing it to any other thread's walk
                // would print one stack twice and lose a real one.
                const CONTEXT* ctx = nullptr;
                if (te.th32ThreadID == selfTid && ep) ctx = ep->ContextRecord;

                for (const std::string& f : WalkThread(te.th32ThreadID, ctx))
                    append(f);
                append("");
            }
            CloseHandle(snap);
        }
        else
        {
            append("<could not enumerate threads>");
        }

        // The .txt goes down BEFORE the GPU provider runs, not after: the
        // provider is now the single riskiest step in this function (a real
        // backend touches a just-removed/hung device -- GetDeviceRemovedReason,
        // DRED retrieval, a raw .gpudump write), and the whole point of the
        // "minidump first" comment above is that a report already on disk
        // survives whatever happens next. If this write fails, txtOk stays
        // false and siblingTxt records that honestly below instead of
        // claiming a file that doesn't exist.
        const bool txtOk = [&]() {
            if (FILE* f = nullptr; fopen_s(&f, txtPath.string().c_str(), "wb") == 0 && f)
            {
                std::fwrite(out.data(), 1, out.size(), f);
                std::fclose(f);
                return true;
            }
            return false;
        }();

        // .arcdiag envelope (GPU crash diagnostics arc, Task 4): ALWAYS
        // emitted, with or without a GPU backend installed -- a plain CPU
        // crash/hang report must be fully renderable by the future
        // CrashReportDocument on its own, not an empty shell waiting on
        // Task 5/6. guid is minted fresh here (never by Diag::WriteFile /
        // Diag::Parse) because Parse rejects a nil guid.
        Diag::Envelope envelope;
        envelope.guid      = Guid::Generate();
        envelope.kind      = DeriveKind(reason);
        envelope.timestampUtc = TimestampUtcIso8601();
        envelope.appName   = g_cfg.appName;
        envelope.phase     = CurrentPhase();
        envelope.buildInfo = BuildInfo();
        // Same content the .txt sibling's CPU portion carries -- the
        // symbolized all-thread walk above, snapshotted BEFORE any GPU
        // section is appended below so this field stays CPU-only (F-6b).
        envelope.cpuThreadSummary = out;

        GpuSectionProvider gpuProvider     = nullptr;
        void*              gpuProviderUser = nullptr;
        {
            std::lock_guard gpuLock(g_gpuProviderMutex);
            gpuProvider     = g_gpuProvider;
            gpuProviderUser = g_gpuProviderUser;
        }
        if (gpuProvider)
        {
            const std::size_t cpuLen = out.size();
            std::string gpuText;
            gpuProvider(envelope, gpuText, reportStem, gpuProviderUser);
            append("=== GPU ===");
            if (!gpuText.empty())
                append(gpuText);

            // Append-only: the CPU portion is already durable on disk from
            // the write above. Only the newly appended GPU slice goes out
            // here, so a provider that blocks or faults never re-risks the
            // CPU report that already succeeded.
            if (txtOk)
            {
                if (FILE* f = nullptr; fopen_s(&f, txtPath.string().c_str(), "ab") == 0 && f)
                {
                    std::fwrite(out.data() + cpuLen, 1, out.size() - cpuLen, f);
                    std::fclose(f);
                }
            }
        }

        // Sibling paths reflect what actually landed on disk, per
        // DiagEnvelope.hpp's contract ("Empty when a given sibling wasn't
        // produced") -- never claim a path Task 10's shell-open button would
        // fail to open. siblingGpuDump stays whatever the provider set
        // directly on `envelope` (empty if it wrote nothing).
        envelope.siblingTxt = txtOk  ? txtPath.string() : std::string{};
        envelope.siblingDmp = dumpOk ? dmpPath.string() : std::string{};
        Diag::WriteFile(envelope, diagPath);

        g_reportCount.fetch_add(1, std::memory_order_acq_rel);

        // Echo into the engine log too: the log is what the user already has
        // open, and a report nobody notices is a report that did not happen.
        ARC_ERROR("Diagnostics: {} -- report written\n{}", reason ? reason : "report", out);

        return txtPath.string();
#else
        (void)reason; (void)exceptionPointers;
        return {};
#endif
    }

#if defined(_WIN32)
    LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep)
    {
        if (g_inCrashHandler.exchange(true, std::memory_order_acq_rel))
            return EXCEPTION_EXECUTE_HANDLER;   // faulted inside ourselves; do not loop

        WriteReportImpl("crash (unhandled exception)", ep);

        if (g_prevFilter) return g_prevFilter(ep);
        return EXCEPTION_EXECUTE_HANDLER;
    }
#endif

    // ONE thread, TWO rules (Task 7). The GPU-progress rule is a sibling loop
    // body here rather than a second thread: it needs the same 250ms cadence,
    // the same debugger suppression, and the same report serialization as the
    // hang rule, and a second thread would buy nothing but a second way for the
    // two to interleave reports for what is usually one incident.
    void WatchdogMain()
    {
#if defined(_WIN32)
        SetThreadDescription(GetCurrentThread(), L"Arcane-HangWatchdog");
#endif
        const auto threshold = static_cast<double>(g_cfg.hangSeconds);

        // Which beat value we already reported on, so one stall yields one
        // report -- and a NEW stall later still yields another.
        std::int64_t reportedBeat = 0;
        bool         reported     = false;

        // The GPU rule's state, in the extracted+tested form. Same shape --
        // report once, re-arm on progress -- over a VALUE rather than an age.
        ProgressStallRule gpuRule(static_cast<double>(g_cfg.gpuStallSeconds));

        // Main-thread liveness rule. Unchanged in substance; lifted into a
        // lambda only so its early-outs stop being `continue`s that would also
        // skip the GPU rule below.
        const auto checkMainThreadBeat = [&]()
        {
            if (!g_beatSeen.load(std::memory_order_acquire)) return;

            const std::int64_t beat = g_lastBeat.load(std::memory_order_acquire);

            if (reported && beat != reportedBeat)
                reported = false;   // main thread moved again: re-arm

            if (reported) return;
            if (SecondsSince(beat) < threshold) return;

            char msg[160];
            std::snprintf(msg, sizeof(msg), "hang (main thread has not ticked for %.1fs)",
                          SecondsSince(beat));
            WriteReportImpl(msg, nullptr);

            reported     = true;
            reportedBeat = beat;
        };

        // GPU-progress rule. The signal it is built on is narrow and stated
        // plainly: the render path is STILL PUBLISHING a counter that has
        // STOPPED MOVING. Both halves matter --
        //
        //   - a frozen counter that nobody is publishing (a minimized host
        //     renders no frames) is no evidence at all, so the freshness gate
        //     below disarms rather than inventing a stall;
        //   - a wedged GPU that has also parked the main thread inside a
        //     blocking swapchain wait stops publishing too, and is therefore
        //     NOT this rule's case. That one belongs to the hang rule, whose
        //     all-thread walk shows the main thread parked in the wait -- which
        //     is the honest report. Trying to claim it here would mean calling
        //     every ordinary main-thread hang a GPU stall, since an ordinary
        //     hang also stops the render path.
        const double gpuThreshold = static_cast<double>(g_cfg.gpuStallSeconds);
        const double gpuFreshness = GpuBeatFreshnessSeconds(gpuThreshold);

        const auto checkGpuProgress = [&]()
        {
            if (!g_gpuBeatSeen.load(std::memory_order_acquire)) return;

            if (SecondsSince(g_lastGpuBeat.load(std::memory_order_acquire)) >= gpuFreshness)
            {
                // Not rendering. Drop the accumulated stall age so a host that
                // resumes starts a fresh clock instead of firing on the gap.
                gpuRule.Reset();
                return;
            }

            const std::uint64_t fence = g_gpuFence.load(std::memory_order_acquire);
            const double        now   = NowSeconds();
            if (!gpuRule.Poll(fence, now)) return;

            const double beatAge = g_beatSeen.load(std::memory_order_acquire)
                                 ? SecondsSince(g_lastBeat.load(std::memory_order_acquire))
                                 : -1.0;

            // "gpu-stall" is a CONTRACT, not prose: Diagnostics::DeriveKind
            // reads this string and only a lowercase "gpu" + "stall" yields the
            // .arcdiag kind that makes the GPU-section provider's output mean
            // "the GPU stopped" rather than "the GPU died".
            char msg[224];
            if (beatAge >= 0.0)
            {
                std::snprintf(msg, sizeof(msg),
                              "gpu-stall: GPU progress counter %llu has not advanced for %.1fs "
                              "(main thread last ticked %.1fs ago)",
                              static_cast<unsigned long long>(fence),
                              gpuRule.StalledSeconds(now), beatAge);
            }
            else
            {
                std::snprintf(msg, sizeof(msg),
                              "gpu-stall: GPU progress counter %llu has not advanced for %.1fs "
                              "(main thread never ticked)",
                              static_cast<unsigned long long>(fence),
                              gpuRule.StalledSeconds(now));
            }
            WriteReportImpl(msg, nullptr);
        };

        while (!g_watchdogStop.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));

#if defined(_WIN32)
            // A breakpoint stops the main thread by design -- and with it every
            // submission, so the GPU counter freezes too. Firing then would
            // train the user to ignore the one signal that matters. Hoisted
            // above both rules for that second reason.
            if (IsDebuggerPresent()) continue;
#endif

            // The two rules are disjoint by construction -- both signals are
            // published from the main thread, so "still publishing GPU
            // progress" and "has not ticked for hangSeconds" cannot be true at
            // once -- which is why running both every poll costs nothing and
            // cannot double-report one incident. GPU first anyway: the more
            // specific rule ahead of the general one.
            checkGpuProgress();
            checkMainThreadBeat();
        }
    }
}   // namespace

void Install(const Config& cfg)
{
    if (g_installed.exchange(true, std::memory_order_acq_rel)) return;

    g_cfg = cfg;

    // Arm from a CLEAN slate. Heartbeat()/GpuHeartbeat() set their seen-flags
    // unconditionally -- they are one relaxed store each and deliberately do
    // not test whether diagnostics are installed -- so a beat published before
    // this arming (or by a previous arming that has since shut down) would
    // otherwise be inherited here as a live, and by now ancient, timestamp. The
    // watchdog would then report a hang seconds after boot for something that
    // happened before it existed. Shutdown() clears these too; doing it on BOTH
    // edges is what makes the state per-arming rather than per-process.
    g_beatSeen.store(false, std::memory_order_release);
    g_gpuBeatSeen.store(false, std::memory_order_release);

#if defined(_WIN32)
    g_mainThreadId = GetCurrentThreadId();

    if (cfg.installCrashHandler)
        g_prevFilter = SetUnhandledExceptionFilter(&OnUnhandledException);
#endif

    if (cfg.startHangWatchdog)
    {
        g_watchdogStop.store(false, std::memory_order_release);
        g_watchdog = std::thread(&WatchdogMain);
    }

    ARC_INFO("Diagnostics armed (crash handler {}, hang watchdog {} @ {}s, gpu-stall @ {}s) -> {}",
             cfg.installCrashHandler ? "on" : "off",
             cfg.startHangWatchdog ? "on" : "off",
             cfg.hangSeconds,
             cfg.gpuStallSeconds,
             ReportDir().string());
}

void Shutdown() noexcept
{
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;

    g_watchdogStop.store(true, std::memory_order_release);
    if (g_watchdog.joinable()) g_watchdog.join();

#if defined(_WIN32)
    if (g_prevFilter)
    {
        SetUnhandledExceptionFilter(g_prevFilter);
        g_prevFilter = nullptr;
    }
    if (g_symReady.exchange(false, std::memory_order_acq_rel))
    {
        std::lock_guard lock(g_symMutex);
        SymCleanup(GetCurrentProcess());
    }
#endif

    // Beat state is per-arming: a later Install() must not inherit a stale
    // "already beating" flag from this one. Both triggers, for both reasons --
    // the GPU one would otherwise re-arm against a dead device's last counter.
    g_beatSeen.store(false, std::memory_order_release);
    g_gpuBeatSeen.store(false, std::memory_order_release);
}

void Heartbeat() noexcept
{
    g_lastBeat.store(NowTicks(), std::memory_order_release);
    // Release-store after the beat so the watchdog never sees "beating" with an
    // unset timestamp and fires on a zero.
    g_beatSeen.store(true, std::memory_order_release);
}

void GpuHeartbeat(std::uint64_t fenceValue) noexcept
{
    g_gpuFence.store(fenceValue, std::memory_order_release);
    g_lastGpuBeat.store(NowTicks(), std::memory_order_release);
    // Same ordering discipline as Heartbeat(): arm only after the value is
    // visible, so the watchdog never seeds its rule on a zero the render path
    // never published.
    g_gpuBeatSeen.store(true, std::memory_order_release);
}

void GpuHeartbeatRefresh() noexcept
{
    // Time only. Deliberately does NOT touch g_gpuFence -- there is no new
    // value, and that IS the signal -- and does NOT set g_gpuBeatSeen: a
    // refresh must never arm a rule that has no published value to judge.
    g_lastGpuBeat.store(NowTicks(), std::memory_order_release);
}

bool ProgressStallRule::Poll(std::uint64_t value, double nowSeconds) noexcept
{
    // A first observation only seeds. Reporting here would call every armed
    // watchdog's very first poll a stall.
    if (!m_seeded)
    {
        m_seeded = true;
        m_value  = value;
        m_since  = nowSeconds;
        return false;
    }

    if (value != m_value)
    {
        // Progress: re-arm, and restart the stall clock from HERE rather than
        // from when this value was first seen -- otherwise a counter that moved
        // once after a long quiet period would look instantly stale.
        m_value    = value;
        m_since    = nowSeconds;
        m_reported = false;
        return false;
    }

    if (m_reported) return false;                        // one stall, one report
    if (nowSeconds - m_since < m_stallSeconds) return false;

    m_reported = true;
    return true;
}

double ProgressStallRule::StalledSeconds(double nowSeconds) const noexcept
{
    return m_seeded ? nowSeconds - m_since : 0.0;
}

void ProgressStallRule::Reset() noexcept
{
    m_seeded   = false;
    m_reported = false;
    m_value    = 0;
    m_since    = 0.0;
}

void SetPhase(std::string phase)
{
    std::lock_guard lock(g_phaseMutex);
    g_phase = std::move(phase);
}

std::string WriteReport(const char* reason)
{
    return WriteReportImpl(reason, nullptr);
}

std::uint32_t ReportCount() noexcept
{
    return g_reportCount.load(std::memory_order_acquire);
}

void SetGpuSectionProvider(GpuSectionProvider provider, void* user) noexcept
{
    std::lock_guard lock(g_gpuProviderMutex);
    g_gpuProvider     = provider;
    g_gpuProviderUser = user;
}

void ClearGpuSectionProvider() noexcept
{
    std::lock_guard lock(g_gpuProviderMutex);
    g_gpuProvider     = nullptr;
    g_gpuProviderUser = nullptr;
}
}   // namespace Arcane::Diagnostics

// =============================================================================
// Structured diagnostics: the publish/sink seam
// =============================================================================
// Separate anonymous namespace + separate statics from the capture module
// above: this slot is the live Problems/Console feed, unrelated to crash/hang
// report state. Named g_sink* (not g_mutex) to keep grep-for-a-static
// unambiguous between the two halves of this file.

namespace Arcane::Diagnostics
{
namespace
{
    // One slot, in Arcane.dll. The mutex guards BOTH the slot and the
    // dispatch: a worker publishing while the main thread swaps sinks (at
    // editor shutdown) must never call a half-torn-down consumer.
    std::mutex g_sinkMutex;
    Sink       g_sink     = nullptr;
    void*      g_sinkUser = nullptr;
}   // namespace

void SetSink(Sink sink, void* user) noexcept
{
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    g_sink     = sink;
    g_sinkUser = user;
}

bool ClearSinkIfCurrent(Sink sink, void* user) noexcept
{
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    if (g_sink != sink || g_sinkUser != user)
        return false;
    g_sink     = nullptr;
    g_sinkUser = nullptr;
    return true;
}

void Publish(std::string_view key, std::span<const Diagnostic> diags)
{
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    if (g_sink)
        g_sink(key, diags, g_sinkUser);
}

void Clear(std::string_view key)
{
    Publish(key, {});
}
}   // namespace Arcane::Diagnostics
