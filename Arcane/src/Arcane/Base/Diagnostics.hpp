#pragma once

// Engine diagnostics (Base module): the post-mortem capture seam, sibling to
// Log.hpp and Assert.hpp. Two triggers, ONE report path:
//
//   1. CRASH -- SetUnhandledExceptionFilter. An access violation or an escaped
//      C++ exception writes a report instead of dying mute.
//   2. HANG  -- a watchdog thread. The main thread publishes a Heartbeat(); if
//      it stops advancing for longer than Config::hangSeconds, the watchdog
//      writes the SAME report for a process that is still very much alive.
//
// (2) is the reason this module exists. Windows Error Reporting's LocalDumps
// covers (1) already and is a fine backstop, but WER only ever fires on process
// DEATH -- a wedged main thread ("Not Responding") produces nothing, anywhere,
// forever. Neither does a debugger help much when the defect is intermittent:
// it requires a human attached at the exact moment. The watchdog does not.
//
// A report is:
//   - a MINIDUMP (.dmp), written FIRST because MiniDumpWriteDump is the
//     battle-tested path and must land even if the text walk below wedges;
//   - a SYMBOLIZED TEXT stack (.txt) of EVERY thread, also echoed into the
//     engine log. "Every thread" is deliberate: a main thread blocked on a
//     worker names the wrong culprit, and the answer is the other stack.
//
// Reading it needs no debugger, which is the whole point -- the text file is
// the artifact you paste into a bug report.
//
// Windows-only today; every entry point compiles and no-ops elsewhere so the
// planned Linux port keeps linking.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Arcane::Diagnostics
{
    struct Config
    {
        // Names the report files. Set it per host ("ArcaneEditor").
        std::string appName = "Arcane";

        // Where reports land. Empty => "<exe dir>/diagnostics". Chosen over the
        // CWD on purpose: hosts are documented as cd-then-run, and a report that
        // lands wherever the user happened to be is a report nobody finds.
        std::string dumpDir;

        // Main-thread stall that counts as a hang. Generous by default: a cold
        // shader compile or a large project scan can legitimately block the main
        // thread for seconds, and a false report that cries wolf gets ignored.
        std::uint32_t hangSeconds = 12;

        bool installCrashHandler = true;
        bool startHangWatchdog   = true;
    };

    // Registers the CALLING thread as the main thread and arms both triggers.
    // Call once, early in main(), beside Log::Init(). Idempotent.
    ARCANE_API void Install(const Config& cfg);

    // Disarms both triggers and joins the watchdog. Idempotent; safe to skip
    // (the process exiting is also fine).
    ARCANE_API void Shutdown() noexcept;

    // "The main thread is alive." One relaxed atomic store -- cheap enough for
    // every frame, which is where it belongs. A hang is DEFINED as this not
    // being called, so it must sit on every path the main thread loops through:
    // the frame loop AND the boot/project-switch pump.
    ARCANE_API void Heartbeat() noexcept;

    // Optional label for what the main thread is currently doing (a boot stage
    // id, say). Reproduced verbatim at the top of a report -- it turns "it hung"
    // into "it hung in switch_plugin_load". Cheap but not free (takes a lock);
    // call it per phase, never per frame.
    ARCANE_API void SetPhase(std::string phase);

    // Writes a report immediately, whatever the process state. Returns the .txt
    // path, or an empty string if nothing could be written. Public because a
    // manual trigger is useful, and because it is how the self-test proves the
    // capture path works BEFORE an intermittent bug depends on it.
    ARCANE_API std::string WriteReport(const char* reason);

    // Reports written this process. The observable the watchdog test asserts on.
    [[nodiscard]] ARCANE_API std::uint32_t ReportCount() noexcept;
}

// =============================================================================
// Structured diagnostics: the publish/sink seam
// =============================================================================
//
// Distinct from the post-mortem capture above (crash/hang reports written to
// disk after the fact). This seam is for problems a USER must act on RIGHT
// NOW -- a broken material reference, an unresolved asset, a plugin load
// failure -- surfaced live in an editor Problems/Console panel. Deliberately
// separate from Arcane::Log (Base/Log.hpp) too: a log line is an event that
// happened, a diagnostic is an assertion about how things are right now and
// stops being true when the underlying problem is fixed.
//
// PUBLICATION GROUPS: a producer owns a `key` and republishes its ENTIRE set
// for that key; the consumer replaces that key's contents atomically. Retraction
// is not a special case -- it is Publish(key, {}) (or Clear(key)). Individual
// rows are never added or removed, so there is no reconciliation to get wrong.
//
// The sink slot lives once, in Arcane.dll (Diagnostics.cpp), and is
// mutex-guarded: producers publish from worker threads (the async-boot arc runs
// the asset-registry scan off the main thread).

namespace Arcane
{
    enum class DiagSeverity : std::uint8_t { Info, Warning, Error };

    enum class DiagScope : std::uint8_t { Project, Assets, Scene, Plugin, Material, Shader };

    // What clicking the row should do. A tagged union in spirit; only the
    // members belonging to `kind` are meaningful.
    struct DiagLocator
    {
        enum class Kind : std::uint8_t { None, Entity, Asset, File, GraphNode };

        Kind          kind   = Kind::None;
        std::uint64_t entity = 0;    // Kind::Entity
        Guid          asset;         // Kind::Asset
        std::string   file;          // Kind::File
        int           line   = 0;    // Kind::File
        int           col    = 0;    // Kind::File
        Guid          ownerAsset;    // Kind::GraphNode -- the owning material
        std::uint32_t nodeId = 0;    // Kind::GraphNode

        [[nodiscard]] static DiagLocator Entity(std::uint64_t id) noexcept
        {
            DiagLocator l; l.kind = Kind::Entity; l.entity = id; return l;
        }
        [[nodiscard]] static DiagLocator Asset(const Guid& id) noexcept
        {
            DiagLocator l; l.kind = Kind::Asset; l.asset = id; return l;
        }
        [[nodiscard]] static DiagLocator File(std::string path, int lineNo = 0, int colNo = 0)
        {
            DiagLocator l; l.kind = Kind::File; l.file = std::move(path);
            l.line = lineNo; l.col = colNo; return l;
        }
        [[nodiscard]] static DiagLocator GraphNode(const Guid& owner, std::uint32_t node) noexcept
        {
            DiagLocator l; l.kind = Kind::GraphNode; l.ownerAsset = owner; l.nodeId = node; return l;
        }
    };

    struct Diagnostic
    {
        DiagSeverity severity = DiagSeverity::Warning;
        DiagScope    scope    = DiagScope::Project;
        // Stable and dotted ("scene.component.unknown"). This is the identity a
        // future suppression UI, a docs link, or a lint rule id hangs off --
        // never localize it and never reword it casually.
        std::string  code;
        std::string  message;
        // Optional consequence line, rendered dimmed beneath the message.
        std::string  detail;
        DiagLocator  locator;
    };

    namespace Diagnostics
    {
        // Raw function pointer + user data, mirroring Mosaic::SetLogSink. Keeps
        // the DLL boundary free of std::function's allocator coupling.
        using Sink = void (*)(std::string_view key, std::span<const Diagnostic> diags, void* user);

        // Install (or clear, with nullptr) the process-wide sink. Last writer wins.
        ARCANE_API void SetSink(Sink sink, void* user) noexcept;

        // Clear the slot ONLY if it still holds exactly (sink, user); returns
        // whether it cleared. A stale consumer's teardown must not silently
        // unslot whoever registered AFTER it -- the slot is process-wide and
        // last-writer-wins, so an unconditional SetSink(nullptr, nullptr) from
        // an old owner's destructor would disconnect a live, unrelated one
        // (same stale-registration hazard as a dangling plugin descriptor).
        // Prefer this over SetSink(nullptr, nullptr) in any owner's teardown path.
        [[nodiscard]] ARCANE_API bool ClearSinkIfCurrent(Sink sink, void* user) noexcept;

        // Replace `key`'s entire diagnostic set. Safe with no sink installed.
        ARCANE_API void Publish(std::string_view key, std::span<const Diagnostic> diags);

        // Retract everything under `key`. Exactly Publish(key, {}).
        ARCANE_API void Clear(std::string_view key);
    }
}
