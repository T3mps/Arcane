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
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Remote/TickGate.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

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
        // registration that reads it (ruling F12): Task 16 adds
        // `bool (*dirty)(void*)`, `bool (*settled)(void*)` and the `void* hookUser`
        // both receive; Task 19 adds `WorldTable worlds`; Task 22 adds
        // `DiagnosticStore* problemStore`; Tasks 27-29 add their host hook
        // structs. Session-owned state (the cvar command user, FrameStats)
        // is a HostSession member added by the task that needs it.
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
    bool DrainFrame(std::uint32_t pluginGeneration);
    // The reason DrainFrame latched when it told the host to stop:
    // "remote-quit" or "remote-idle". Null when the loop did not end on the
    // remote surface. Latched at the break site, so a later Ctrl-C or a long
    // teardown is never relabelled.
    const char* ExitReason() const;
    // The host is leaving its loop: state "stopping" is published to /status
    // and to the descriptor at once, so a client polling a shutting-down host
    // does not read "ready". Stop follows later.
    void MarkStopping();
    // Stops the listener (parked calls answer host.died), deletes the
    // descriptor and restores the registry's host kind. Idempotent.
    void Stop(std::string exitReason);
    // idleExitSeconds > 0 and the last completed command (or Start, if none) is
    // older than that. 0 never expires. False before Start.
    bool IdleExpired() const;
    HostGate& Gate();
    const HostGate& Gate() const;       // ruling F9: WaitPoll reads it through a const session
    const Config& GetConfig() const;    // ruling F9: the const hooks accessor (dirty, settled, hookUser, ...)
    // AM1: the fixed-tick hosts' run/pause/step gate. Start sets it from
    // Config::holdSim: held = not running and paused (Consume is Idle until a
    // play command), otherwise running (Consume is Run, the pre-remote loop).
    TickGate& Tick();
    // ruling F10: the host passes its own m_plugin->Generation(); HostSession holds no PluginHost
    void NoteReload(std::uint32_t generation, bool ok, std::string reason, std::uint64_t frame);
    std::uint16_t Port() const;          // 0 unless started
    const std::filesystem::path& DescriptorFile() const; // where Start wrote (or would write) the descriptor
    bool TakeQuit() const;              // host.quit sets it; Task 16
    void RequestQuit();

private:
    void PublishStatus(std::int64_t nowUnixMs);
    void PublishCatalogue();
    void Teardown(); // the shared tail of Stop and of a failed Start

    Config m_config;
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
    std::uint32_t m_publishedCatalogue = 0;
    bool m_cataloguePublished = false;

    std::chrono::steady_clock::time_point m_lastActivity{};
    std::chrono::steady_clock::time_point m_lastHeartbeat{};
    std::atomic<bool> m_quit{false};
};

} // namespace Arcane::Remote

#endif
