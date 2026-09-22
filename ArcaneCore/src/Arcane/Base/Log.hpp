#pragma once

// Engine logger (Base module): console-sink spdlog logger named "Arcane".
// Deliberately separate from Core's Logger (Util/Logger.hpp), which serves
// engine-agnostic named-category logging for library code; this is the
// engine runtime's own console logger. File sinks and per-module categories
// can grow here when the engine needs them.
// spdlog is header-only in this workspace: each module has its OWN spdlog
// registry. Consumers must reach this logger via Engine() / the ARC_*
// macros -- spdlog::get("Arcane") in another module returns null.

#include <Arcane/Core/Api.hpp>

#include <spdlog/spdlog.h>

#include <Mosaic/Log.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace Arcane::Log
{
    ARCANE_CORE_API void Init(spdlog::level::level_enum level = spdlog::level::info);
    ARCANE_CORE_API void Shutdown();

    // Never returns null: lazily calls Init() with defaults if needed.
    ARCANE_CORE_API spdlog::logger* Engine();

    // Mosaic diagnostics: the log SINK that forwards Mosaic/Manifold2D/Astra
    // records into the engine logger (Engine()). Defined in Log.cpp so it lives
    // once, in Arcane.dll, routing every module's records to one spdlog instance.
    ARCANE_CORE_API Mosaic::LogSink MosaicSink() noexcept;

    // Install the sink into the CALLING module's Mosaic storage. Inline on
    // purpose: Mosaic's g_logSink is a per-module inline atomic, so each module
    // (Arcane.dll, ArcaneRuntime.exe, the plugin, tests) installs into its own copy.
    inline void InstallMosaicSink() noexcept { Mosaic::SetLogSink(MosaicSink(), nullptr); }

    // ------------------------------------------------------------------
    // File sink + backlog (spec S5.6, crash window plan 1 task 4).
    //
    // Attach a rotating file sink beside the stderr one. Whatever file
    // currently sits at `file` is rotated out of the way first: delete
    // <stem>.5.log, shift .4->.5 ... .1->.2, then <file> -> <stem>.1.log
    // (keep = 5); a fresh, truncated file is then opened at `file`. Safe to
    // call repeatedly with the same path (each call rotates again) or with a
    // different one (the old sink is simply detached first). Returns false
    // if Log::Init() has not run yet (no engine logger to attach to) or if
    // the file could not be opened.
    ARCANE_CORE_API bool AttachFileSink(const std::filesystem::path& file);
    // The path passed to the most recent successful AttachFileSink, or an
    // empty path when no file sink is attached.
    ARCANE_CORE_API std::filesystem::path FileSinkPath();

    // The backlog: the last kBacklogLines formatted lines the engine logger
    // produced, ring-buffered. Backed by a fixed static array -- no
    // allocation, ever, on any of the paths below.
    inline constexpr std::size_t kBacklogLines = 512;
    inline constexpr std::size_t kBacklogLineBytes = 512;

    // FreezeBacklog: a single atomic store. Safe to call from the FAULTING
    // thread (e.g. a SEH filter or signal handler) -- it takes no lock and
    // touches no memory the logger itself owns beyond the flag. Once frozen,
    // the backlog sink checks the flag before doing anything else and simply
    // declines to record further lines, so the ring stays exactly as the
    // faulting thread left it for the crash thread to read.
    ARCANE_CORE_API void FreezeBacklog() noexcept;
    // Test-only: undoes FreezeBacklog() so later, unrelated tests still get
    // backlog coverage. FreezeBacklog is process-global (one static ring for
    // the whole module), so a test that freezes it must unfreeze it again.
    ARCANE_CORE_API void UnfreezeBacklogForTests() noexcept;
    // min(total lines ever recorded, kBacklogLines). Lock-free, heap-free.
    ARCANE_CORE_API std::size_t BacklogLineCount() noexcept;
    // Copies line i (0 = oldest retained) into buf, NUL-free, and returns the
    // number of bytes copied (at most min(strlen(line), buf.size())). Reads
    // the ring directly with no lock of any kind -- a line concurrently being
    // written by another thread may come back torn (part old, part new
    // content); that is an accepted tradeoff for staying lock-free on the
    // crash path. Never allocates.
    ARCANE_CORE_API std::size_t BacklogLine(std::size_t i, std::span<char> buf) noexcept;

    // FlushFileSinkBounded: there must be NO thread creation on the crash
    // path, yet spdlog's file sink can only be flushed by a thread that is
    // willing to take its internal mutex -- one the faulting thread might
    // already hold mid-write. The fix is to never do that flush from the
    // caller's own thread. The first successful AttachFileSink() call (which
    // never happens on the crash path -- it happens during normal startup,
    // long before any crash) lazily starts one dedicated helper thread that
    // parks on a condition variable, waiting to be asked to flush. Calling
    // FlushFileSinkBounded computes a single deadline (now + timeoutMs) that
    // covers the WHOLE call: it takes the shared mutex with a bounded
    // try_lock_until (never an unbounded lock -- review fix round 1, finding
    // 3: even acquiring that mutex must not be able to block forever), then
    // signals the helper and waits on a SEPARATE completion condition
    // variable up to that same deadline. It returns true only if the helper
    // reports completion in time, and false on any timeout -- including
    // acquiring the mutex, or the helper itself being stuck on a mutex the
    // dying faulting thread held (in that case the helper thread leaks, but
    // the process is already on its way down). Returns false immediately,
    // with no wait, if no helper thread was ever started (AttachFileSink
    // never succeeded) or no file sink is attached. The caller-side
    // wait/signal uses only pre-constructed synchronization primitives and
    // never allocates. Log::Shutdown() stops and joins the helper thread;
    // a static-destruction guard in Log.cpp also calls it at process exit
    // so a joinable helper thread never reaches ~std::thread() (which would
    // std::terminate the process) even when nothing ever calls Shutdown().
    ARCANE_CORE_API bool FlushFileSinkBounded(std::uint32_t timeoutMs) noexcept;
}

#define ARC_TRACE(...)    ::Arcane::Log::Engine()->trace(__VA_ARGS__)
#define ARC_DEBUG(...)    ::Arcane::Log::Engine()->debug(__VA_ARGS__)
#define ARC_INFO(...)     ::Arcane::Log::Engine()->info(__VA_ARGS__)
#define ARC_WARN(...)     ::Arcane::Log::Engine()->warn(__VA_ARGS__)
#define ARC_ERROR(...)    ::Arcane::Log::Engine()->error(__VA_ARGS__)
#define ARC_CRITICAL(...) ::Arcane::Log::Engine()->critical(__VA_ARGS__)
