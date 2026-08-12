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
#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Guid.hpp>

#include <cstdint>
#include <filesystem>
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

        // GPU-progress stall that counts as a GPU hang (GpuHeartbeat below).
        //
        // The rule fires only while the render path is DEMONSTRABLY ALIVE and
        // the GPU-progress counter has stopped moving -- it disarms outright
        // when publishing stops, because a frozen counter nobody is publishing
        // is a host that is not rendering (minimized), not a stalled GPU. The
        // case that makes it reachable is the swapchain's frame-slot wait,
        // which polls and republishes rather than blocking (see
        // Render/GpuInstrumentation.hpp, GpuFrameSlot) precisely so a wedged
        // GPU is visible as "still waiting, still not retiring".
        //
        // TIGHTER than hangSeconds on purpose, and this is the whole reason
        // both numbers exist -- though NOT as a race between two live rules.
        // The frame-slot wait keeps the main-thread beat flowing, so the hang
        // rule cannot fire during it at all; it only becomes possible once the
        // wait gives up polling and parks, and then only hangSeconds after
        // that. 8 < 12 is what puts the GPU verdict INSIDE the polling window,
        // so a GPU that stopped retiring is named while the evidence is still
        // being published, rather than surfacing much later as a main-thread
        // hang it is not.
        //
        // What it does NOT buy: a GPU-section in the report. The provider runs
        // for EVERY report kind (see WriteReportImpl), so a plain `hang` already
        // carries markers/DRED/device-fault. What the kind changes is the CLAIM
        // the report makes about the cause -- "gpu-stall" vs "hang" -- and which
        // of the two a reader should believe.
        std::uint32_t gpuStallSeconds = 8;

        bool installCrashHandler = true;
        bool startHangWatchdog   = true;
    };

    // Registers the CALLING thread as the main thread and arms both triggers.
    // Call once, early in main(), beside Log::Init(). Idempotent.
    ARCANE_API void Install(const Config& cfg);

    // Disarms both triggers and joins the watchdog. Idempotent; safe to skip
    // (the process exiting is also fine). Does NOT reset Config::dumpDir --
    // a dumpDir retargeted live (RetargetDumpDir, below) is host state, not
    // arming state, and must survive a Shutdown/Install cycle the same way
    // appName does.
    ARCANE_API void Shutdown() noexcept;

    // Switches WHERE reports land, live -- no Shutdown()/Install() cycle
    // needed (GPU crash diagnostics arc, Task 8; F-6 in the seam-facts
    // survey). A host calls this immediately after whatever per-project state
    // it already retargets on the same event -- the editor's
    // RetargetLayoutIni (ArcaneEditor/src/App/EditorApp.cpp) and its
    // ArcaneRuntime equivalent -- with `<project>/Saved/Diagnostics` on
    // project open. An empty path reverts to Config::dumpDir's own default
    // (empty => "<exe dir>/diagnostics", see the Config comment above) --
    // ReportDir() already implements that fallback for an empty string, so
    // this never re-derives it; it is exactly what a project-less
    // convergence (a failed project switch, or no --project at all) passes.
    //
    // Thread-safe against a concurrent watchdog/crash report: takes the same
    // lock WriteReportImpl holds for its own g_cfg.dumpDir read (via
    // ReportDir()), so a live retarget can never race a report already
    // mid-write.
    ARCANE_API void RetargetDumpDir(const std::filesystem::path& dir);

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

    // -------------------------------------------------------------------
    // GPU-progress watchdog (GPU crash diagnostics arc, Task 7)
    // -------------------------------------------------------------------

    // "The GPU is still retiring work." One relaxed atomic store, called once
    // per frame by the render path with a MONOTONE count of GPU-side sync
    // points the device has actually passed (Arcane::GpuFrameProgress derives
    // it from an nvrhi event-query chain -- see Render/GpuInstrumentation.hpp).
    //
    // Deliberately NOT folded into Heartbeat(): that one says only that the
    // main thread is alive, which is exactly what a GPU hang can leave true.
    // A frame loop that keeps ticking while the GPU has stopped retiring
    // anything is invisible to the hang watchdog and is the case this exists
    // for. Like Heartbeat(), the FIRST call arms the trigger -- a host that
    // never renders (a test, a headless tool) gets silence, not a spurious
    // report gpuStallSeconds after boot.
    ARCANE_API void GpuHeartbeat(std::uint64_t fenceValue) noexcept;

    // "The render path is still alive and still watching the SAME counter."
    // Refreshes the freshness stamp GpuHeartbeat sets, without changing the
    // value and without arming the rule (a host that has never published a
    // value stays silent no matter how often this is called).
    //
    // Exists for exactly one caller shape: a render path that is BLOCKED
    // waiting for the GPU to retire work, and therefore cannot publish a new
    // value -- but is emphatically not idle. Without it, the frame-slot wait
    // would look identical to a minimized host (frozen counter, no publisher)
    // and the GPU rule would disarm on the one case it exists to catch. See
    // Render/GpuInstrumentation.hpp, GpuFrameSlot::WaitAndReset.
    ARCANE_API void GpuHeartbeatRefresh() noexcept;

    // The pure staleness rule the GPU watchdog runs, extracted so the part
    // that can actually be WRONG -- one report per stall, re-armed on progress
    // -- is testable without threads, timers, or a GPU. (The 2026-08-11
    // hosted-CI flake was a watchdog test timed against a deadline; here the
    // clock is a parameter, so the cases are exact instead of generous.)
    //
    // Not thread-safe and not meant to be: one instance lives on the watchdog
    // thread and is polled only from there.
    class ARCANE_API ProgressStallRule
    {
    public:
        // `stallSeconds` -- how long the counter must sit unchanged before the
        // stall is real. Values <= 0 make every repeat poll a stall.
        explicit ProgressStallRule(double stallSeconds) noexcept
            : m_stallSeconds(stallSeconds) {}

        // One observation. `value` is the latest counter, `nowSeconds` a
        // monotone clock reading in the caller's own epoch. Returns true on
        // EXACTLY the poll that should write a report; false on every other,
        // including every subsequent poll of the same stall. A changed value
        // re-arms and restarts the stall clock from THIS poll.
        [[nodiscard]] bool Poll(std::uint64_t value, double nowSeconds) noexcept;

        // How long the current value has been unchanged, for the report's
        // reason string. Zero before the first Poll().
        [[nodiscard]] double StalledSeconds(double nowSeconds) const noexcept;

        // Back to the never-polled state. The watchdog calls this when the
        // producer stops publishing altogether (a minimized host renders no
        // frames): the counter is frozen, but nobody is claiming otherwise, and
        // resuming must start a fresh stall clock rather than inherit the age of
        // a gap during which nothing was expected to move.
        void Reset() noexcept;

    private:
        double        m_stallSeconds;
        std::uint64_t m_value    = 0;
        double        m_since    = 0.0;
        bool          m_seeded   = false;   // a first Poll only seeds; it never reports
        bool          m_reported = false;   // this stall already produced its one report
    };

    // -------------------------------------------------------------------
    // GPU-section provider seam (GPU crash diagnostics arc, Task 4)
    // -------------------------------------------------------------------
    // Installed by a GPU crash backend (Task 5 = D3D12, Task 6 = Vulkan) so
    // WriteReport can fill an .arcdiag envelope's gpu-side fields without
    // Base/Diagnostics knowing anything about NVRHI/D3D12/Vulkan. Called at
    // most once per report, with an envelope that already carries
    // guid/kind/timestamp/appName/phase/buildInfo/cpuThreadSummary: the
    // provider fills `envelope`'s queues/fault/activeLayers (and, if it
    // wrote its own <reportStem>.gpudump, envelope.siblingGpuDump) and
    // appends human-readable text to `humanText`, which WriteReport folds
    // into the .txt report as a "=== GPU ===" block. `reportStem` is the
    // same base path (no extension) that mints the .txt/.dmp/.arcdiag
    // siblings (F-6b) -- e.g. a provider writes `<reportStem>.gpudump`.
    using GpuSectionProvider = void (*)(Diag::Envelope& envelope,
                                         std::string& humanText,
                                         const std::filesystem::path& reportStem,
                                         void* user);

    // Install (or replace) the process-wide GPU-section provider. Last
    // writer wins, mirroring the structured-diagnostics Sink slot below.
    ARCANE_API void SetGpuSectionProvider(GpuSectionProvider provider, void* user) noexcept;

    // Uninstall it. Idempotent; safe to call with none installed.
    ARCANE_API void ClearGpuSectionProvider() noexcept;

    // -------------------------------------------------------------------
    // Report-written hook (GPU crash diagnostics arc, Task 9)
    // -------------------------------------------------------------------
    // Fired once, at the very end of WriteReportImpl -- after every sibling
    // (.dmp/.txt/.arcdiag, F-6b) has finished writing and the report count/
    // log echo above are done -- with the path of the .arcdiag that was
    // just written. Exists so a host can register the new report as an
    // asset (AssetRegistry::AddFile, via Project::RegisterAsset/
    // Runtime::RegisterCreatedAsset -- F-7's single-asset call) and surface
    // it in Problems, without polling ReportCount() and re-deriving the
    // path itself.
    //
    // Runs on WHATEVER thread called WriteReportImpl: the watchdog thread
    // for hang/gpu-stall (WatchdogMain -- SURVIVABLE, the process keeps
    // running afterward) or the faulting thread for a crash (about to
    // terminate -- registering an asset at that point is moot: there is no
    // next frame left for a host to drain a queue into). The hook still
    // fires uniformly for every report kind, the same way GpuSectionProvider
    // runs for every report kind above -- it is the CONSUMER's job to
    // recognize the crash case is moot, not this seam's.
    //
    // A hook that touches anything not itself thread-safe (an AssetRegistry
    // has no lock of its own -- see AssetRegistry.hpp/.cpp; ImGui state;
    // ...) MUST marshal to its own safe thread first. This seam does no
    // marshaling itself, mirroring GpuSectionProvider immediately above.
    using ReportWrittenHook = void (*)(const std::filesystem::path& diagPath, void* user);

    // Install (or replace) the process-wide report-written hook. Last
    // writer wins, mirroring GpuSectionProvider -- one call per host
    // lifetime is the expected shape.
    ARCANE_API void SetReportWrittenHook(ReportWrittenHook hook, void* user) noexcept;

    // Uninstall it. Idempotent; safe to call with none installed.
    ARCANE_API void ClearReportWrittenHook() noexcept;
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
