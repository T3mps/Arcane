#pragma once
// HostSession: one armed host's remote surface (spec s4.3, s4.4, s4.7). It owns
// the request queue, the loopback listener, the dispatcher, the job table, the
// host gate and the descriptor; the host program owns one session and calls
// Drain once per main-loop iteration, after its event pump and before its sim
// step. Everything here runs on the host main thread except the listener's own
// service thread, which reads only the atoms Drain publishes.
//
// Kept free of socket headers on purpose: the listener, its atoms, the queue
// and the dispatcher are held by pointer, so a host header that includes this
// one (EditorApp.hpp) never pulls in winsock.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Config/CVarTypes.hpp>
#include <Arcane/Core/NetMode.hpp>
#include <Arcane/Remote/Commands/CaptureCommands.hpp>
#include <Arcane/Remote/Commands/CompareCommands.hpp>
#include <Arcane/Remote/Commands/CvarCommands.hpp>
#include <Arcane/Remote/Commands/PlayCommands.hpp>
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/FrameStats.hpp>
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Remote/TickGate.hpp>
#include <Arcane/Remote/Worlds.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace Arcane
{
class DiagnosticStore;  // Arcane/Base/DiagnosticStore.hpp
}

namespace Arcane::Remote
{

class RequestQueue;     // Listener.hpp
class RemoteListener;   // Listener.hpp
struct ListenerAtoms;   // Listener.hpp

// Which processes may serve the remote surface (spec 0.4, s4.7): never a Dist
// build, never a process whose primary world is a Client, and never when the
// third input is false. Standalone, DedicatedServer and ListenServer arm when
// the other two inputs allow it. The editor passes "editor.remote.enable is
// published true, or --remote was given" (spec 0.4(b): the editor is armed by
// its cvar, default on; EditorApp::UpdateRemoteSession); runtime and server
// call Allow only under --remote and pass true.
struct ARC_REMOTE_API Arming {
    static bool Allow(NetMode mode, bool distBuild, bool editorEnableCvar);
};

class ARC_REMOTE_API HostSession {
public:
    struct Config {
        HostKind kind;
        CVarContext cvarContext;
        std::filesystem::path projectRoot;
        std::filesystem::path descriptorOverride;
        std::uint32_t idleExitSeconds = 600;
        bool readOnly = false;
        bool holdSim = false;          // Task 12: --remote && maxFrames == 0; Start leaves Tick() Idle
        std::vector<std::string> capabilities;
        // Appended by later tasks, kept last so the fields above keep their
        // positions. Each task that appends a field also wires, in Start, the
        // registration that reads it (ruling F12): Task 19 adds
        // `WorldTable worlds`; Task 22 adds `DiagnosticStore* problemStore`;
        // Tasks 27-29 add their host hook structs. Session-owned state (the
        // cvar command user, FrameStats) is a HostSession member added by the
        // task that needs it.
        //
        // Task 16 (host.quit / host.wait, Commands/SessionCommands.hpp), main
        // thread, each called with hookUser. Plain function pointers, so a
        // Client-module predicate reaches ArcaneRemote without a dependency
        // on it (R8-2). Null = the host has none.
        bool (*dirty)(void*) = nullptr;    // unsaved changes: host.quit answers host.busy{state:"modal"}
        // wait settled (spec s4.7, Task 30): the --settle producer conjunct
        // (ShaderCompiler::IsIdle, and on the editor the cook queue and the
        // thumbnail harvester; SettleProducersIdle). The byte-equal pair is
        // the host.wait job's own: two captures through `capture`'s host pair.
        bool (*producersIdle)(void*) = nullptr;
        void* hookUser = nullptr;
        // Task 19 (ruling F16): the host's worlds, read by world.list and every
        // world-taking command through CommandWorlds(). Start installs it with
        // SetCommandWorlds; Stop installs an empty table. fn is called on the
        // main thread only (Start and inside Drain). P0 hosts list one world,
        // {"main", ToString(mode), &registry}, with user = the host app.
        WorldTable worlds;
        // Task 22 (ruling F12): the store problems.list and problem.follow
        // read. The editor passes its Problems-panel store (already the
        // engine's diagnostics sink). Null (runtime, server): Start creates a
        // session-owned store and installs it as the engine sink; Stop
        // uninstalls it. Main thread only.
        DiagnosticStore* problemStore = nullptr;
        // Task 27 (ruling F12): capture.viewport / capture.window's hooks,
        // forwarding to the host's NriGraphContext capture ring (R8-2: this
        // module includes no render header). The editor installs six pointers
        // (host pair = viewport pair, ruling F20), the runtime its one target
        // three times; a server leaves all six null (capture.no-renderer).
        // An empty outDir is filled by Start with the session's artifact
        // directory (<remote dir>/out). Start installs them with
        // SetCaptureHooks + RegisterCaptureCommands(&m_jobs); Teardown clears both.
        CaptureHooks capture;
        // Task 28 (ruling F12): capture.compare's reference hooks. The editor
        // and the runtime install ResolveFn / BlessFn over ReferenceImages
        // (ResolveReference with the host's adapter set, BlessReference) with
        // their project root; a server leaves them null (it has no renderer,
        // so capture.no-renderer answers first). Start installs them with
        // SetCompareHooks + RegisterCompareCommand(&m_jobs), the same table
        // the capture commands use; Teardown clears both.
        CompareHooks compare;
        // Task 29 (ruling F12): play.start/stop/pause/resume/step. The editor
        // installs its PlaySession / RunLoop functions (editor = true,
        // undoReason "play-mode"); a runtime or server leaves them null and
        // Start points `gate` at Tick() (undoReason "runtime-host"). Start
        // also sets `stats` to the session's FrameStats and installs them with
        // SetPlayHooks + SetPlayGate(&Gate()) + RegisterPlayCommands(&m_jobs);
        // Teardown clears all three.
        PlayHooks play;
    };
    explicit HostSession(Config config);
    ~HostSession(); // Stop("destroyed") when still started
    HostSession(const HostSession&) = delete;
    HostSession& operator=(const HostSession&) = delete;

    // Sets the registry's host kind (remembering the previous one), starts the
    // listener on 127.0.0.1, publishes status + catalogue and writes the
    // descriptor; true when the session is live. A listener that could not
    // bind (Port() == 0 afterwards) or a descriptor that could not be written
    // is a failed start: logged, nothing left running, the host kind
    // restored, false returned, and the host carries on without remote (or,
    // for an open-ended headless run, exits 2: it has no other stop path). A
    // session object starts at most once: a second Start (while live, after
    // Stop, or after a failed Start) is refused with false and a log line;
    // a host that wants a new session makes a new object.
    bool Start();
    // One main-loop iteration's work: dispatcher.Drain within the frame budget,
    // then HostStatus and the catalogue republished into the listener atoms,
    // and the descriptor heartbeat touched once a second. Never sleeps.
    void Drain();
    // The host's whole per-iteration remote step, one spelling for every host:
    // notes a module reload when `pluginGeneration` (the host's own
    // m_plugin->Generation(), ruling F10) differs from the last one seen,
    // Drains, then checks for a remote quit and the idle timeout. The first
    // generation it sees is adopted silently (the module the host booted with). True means
    // the host should leave its loop now; the reason is latched (ExitReason).
    // This form notes no failed reload: it is the three-argument form with
    // the attempt count last seen and lastReloadOk true.
    bool DrainFrame(std::uint32_t pluginGeneration);
    // Every host's form (Task 30): also the host's PluginHost::ReloadAttempts()
    // and LastReloadOk(). A reload that failed or was refused leaves the
    // generation alone, so an attempt count that moved with lastReloadOk false
    // is noted as a failed reload (NoteReload ok=false), which answers a
    // pending `wait reload` with module.reload-failed. The first call adopts
    // the count silently, as it adopts the generation.
    bool DrainFrame(std::uint32_t pluginGeneration, std::uint64_t reloadAttempts, bool lastReloadOk);
    // The reason DrainFrame latched when it told the host to stop:
    // "remote-quit" or "remote-idle". Null when the loop did not end on the
    // remote surface. Latched at the break site, so a later Ctrl-C or a long
    // teardown is never relabelled.
    const char* ExitReason() const;
    // The host is leaving its loop: state "shutting-down" (spec s4.3) is published to /status
    // and to the descriptor at once, so a client polling a shutting-down host
    // does not read "ready". Stop follows later.
    void MarkStopping();
    // Stops the listener (parked calls answer host.died), deletes the
    // descriptor and restores the registry's host kind. Idempotent.
    void Stop(std::string exitReason);
    // idleExitSeconds > 0 and the last completed command (or Start, if none) is
    // older than that. 0 never expires. False before Start.
    bool IdleExpired() const;
    // Main thread, once per loop iteration, from the predicate the host's own
    // pump uses to skip a frame (the editor's IsMinimized()): publishes it as
    // the gate's `minimized`, so Drain refuses needsFrame commands with
    // host.busy{state:"minimized"} while the rest still run. A no-op before Start.
    void NoteMinimized(bool minimized);
    HostGate& Gate();
    const HostGate& Gate() const;       // ruling F9: WaitPoll reads it through a const session
    const Config& GetConfig() const;    // ruling F9: the const hooks accessor (dirty, producersIdle, hookUser, ...)
    // AM1: the fixed-tick hosts' run/pause/step gate. Start sets it from
    // Config::holdSim: held = not running and paused (Consume is Idle until a
    // play command), otherwise running (Consume is Run, the pre-remote loop).
    TickGate& Tick();
    // ruling F10: the host passes its own m_plugin->Generation(); HostSession holds no PluginHost
    void NoteReload(std::uint32_t generation, bool ok, std::string reason, std::uint64_t frame);
    // Task 30: how many reloads NoteReload has recorded (ok or not), and the
    // last one's outcome. host.wait reload records the count when it arms, so
    // a failure noted after that answers module.reload-failed (exit 6).
    std::uint64_t ReloadSerial() const;
    bool LastReloadOk() const;
    const std::string& LastReloadReason() const;
    std::uint64_t LastReloadFrame() const;
    std::uint16_t Port() const;          // 0 unless started
    const std::filesystem::path& DescriptorFile() const; // where Start wrote (or would write) the descriptor
    bool TakeQuit() const;              // host.quit sets it; Task 16
    void RequestQuit();

    // Task 26 (stats.frame): the session's one snapshot. The sim-advance
    // branch of each host calls NoteSimAdvance, and only that branch (idle,
    // minimized, held and render-only iterations do not). `frame` is the
    // host's own host.frame value after the advance, `dtSeconds` its frame dt,
    // `tickMs` the wall time of the advance; renderMs is the last value given
    // to NoteRenderMs (0 until a render pass completes; a server never gives
    // one). No-ops before Start.
    FrameStats& Stats();
    void NoteSimAdvance(std::uint64_t frame, double dtSeconds, double tickMs);
    void NoteRenderMs(double renderMs);

private:
    void PublishStatus(std::int64_t nowUnixMs);
    void PublishCatalogue();
    void Teardown(); // the shared tail of Stop and of a failed Start

    Config m_config;
    // Task 20 (ruling F12): the user of the cvar.* commands and console.exec (Task 21),
    // {CVarRegistry::Get(), config.cvarContext}; Start installs it, Teardown releases it.
    CvarCommandUser m_cvarUser;
    // Task 22: the store a runtime or server session owns (Config::problemStore
    // null); Start installs it as the engine sink, Teardown uninstalls it.
    std::unique_ptr<DiagnosticStore> m_ownedStore;
    FrameStats m_stats;       // Task 26: Start registers stats.frame on it, Teardown releases it
    double m_lastRenderMs = 0;
    HostGate m_gate;
    TickGate m_tick;
    JobTable m_jobs;
    std::unique_ptr<ListenerAtoms> m_atoms;
    std::unique_ptr<RequestQueue> m_queue;       // sized by Start (QueueDepthSetting)
    std::unique_ptr<RemoteDispatcher> m_dispatcher;
    std::unique_ptr<RemoteListener> m_listener;

    std::filesystem::path m_descriptorPath;
    std::string m_project;                       // projectRoot, UTF-8, as the descriptor and status carry it
    std::uint64_t m_processStart = 0;
    std::vector<std::string> m_capabilities;     // config.capabilities, sorted
    HostKind m_kindBefore = HostKind::Editor;    // restored by Stop (ruling S3-9)
    bool m_started = false;
    bool m_startAttempted = false;               // a session object starts at most once
    const char* m_exitReason = nullptr;          // latched by DrainFrame
    bool m_generationKnown = false;              // DrainFrame's first generation is the boot module
    bool m_descriptorWritten = false;            // Teardown deletes only a descriptor this session wrote

    bool m_reloadOk = true;
    std::string m_reloadReason;
    std::uint64_t m_reloadAtFrame = 0;
    std::uint64_t m_reloadSerial = 0;           // NoteReload calls, ok or not (Task 30)
    std::uint64_t m_reloadAttemptsSeen = 0;     // the host's PluginHost::ReloadAttempts() at the last DrainFrame
    bool m_reloadAttemptsKnown = false;         // the first DrainFrame adopts the count silently
    std::uint32_t m_publishedCatalogue = 0;
    bool m_cataloguePublished = false;

    std::chrono::steady_clock::time_point m_lastActivity{};
    std::chrono::steady_clock::time_point m_lastHeartbeat{};
    std::atomic<bool> m_quit{false};
};

} // namespace Arcane::Remote

#endif
