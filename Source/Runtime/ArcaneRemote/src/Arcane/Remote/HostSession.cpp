// RateLimiter.hpp first: its Logger.hpp defines SPDLOG_ACTIVE_LEVEL, which must
// precede the first spdlog include (Listener.cpp's rule; Listener.hpp reaches spdlog).
#include <Arcane/Net/RateLimiter.hpp>
#include <Arcane/Remote/HostSession.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Commands/CatalogCommands.hpp>
#include <Arcane/Remote/Commands/SessionCommands.hpp>
#include <Arcane/Remote/Commands/WorldCommands.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Http.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Core/AbiVersion.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Project/Project.hpp> // EditorLock::Self: this process's pid + creation stamp

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <utility>

namespace Arcane::Remote
{

namespace
{

ARC_CONSTANT("spec s4.4: the descriptor heartbeat is refreshed once a second (well under the 10 s hung threshold)")
constexpr auto kHeartbeatInterval = std::chrono::milliseconds(1000);

std::int64_t NowUnixMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string Utf8(const std::filesystem::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

} // namespace

bool Arming::Allow(NetMode mode, bool distBuild, bool editorEnableCvar)
{
    if (distBuild || !editorEnableCvar)
        return false;
    return mode != NetMode::Client;
}

HostSession::HostSession(Config config)
    : m_config(std::move(config))
{
}

HostSession::~HostSession()
{
    Stop("destroyed");
}

bool HostSession::Start()
{
    if (m_startAttempted) {
        ARC_ERROR("remote: this {} session was already started once; a host makes a new session to re-arm",
                  ToString(m_config.kind));
        return false;
    }
    m_startAttempted = true;

    CommandRegistry& registry = CommandRegistry::Get();
    m_kindBefore = registry.GetHostKind();
    registry.SetHostKind(m_config.kind); // ruling F-1; Stop restores m_kindBefore (ruling S3-9)
    // Ruling F12: the host's own remote commands register here, before the
    // gate reads the registry generation and the first catalogue is published.
    // Teardown releases the session commands; the catalogue commands hold no
    // pointer, so they stay registered for the process.
    RegisterSessionCommands(this, &m_jobs); // host.quit, host.wait (Task 16)
    RegisterCatalogCommands();              // catalog.describe, schema.type (Task 17, ruling F13)
    SetCommandWorlds(m_config.worlds);      // the one world table (Task 19, ruling F16); Teardown clears it
    RegisterWorldCommands();                // world.list (Task 19); reads CommandWorlds(), holds no pointer

    const EditorLock::Info self = EditorLock::Self();
    m_processStart = self.start;
    m_project = Utf8(m_config.projectRoot);
    m_capabilities = m_config.capabilities;
    std::sort(m_capabilities.begin(), m_capabilities.end());

    const bool projectless = m_config.projectRoot.empty();
    const std::filesystem::path remoteDir = DescriptorDir(m_config.projectRoot, projectless);
    if (!m_config.descriptorOverride.empty())
        m_descriptorPath = m_config.descriptorOverride;
    else if (projectless)
        m_descriptorPath = remoteDir.empty() ? std::filesystem::path()
                                             : remoteDir / (std::string(ToString(m_config.kind)) + "-" +
                                                            std::to_string(self.pid) + ".json");
    else
        m_descriptorPath = DescriptorPath(m_config.projectRoot, m_config.kind, self.pid);

    // The gate's plain fields are written here, on the main thread, before the
    // listener exists (Task 9 carry 4).
    m_gate.kind = ToString(m_config.kind);
    m_gate.pid = self.pid;
    m_gate.catalog = registry.Generation();
    m_gate.SetState("ready");
    // The s6.3 host block's `world`: the host's default world, the one a call
    // without --world resolves to (ResolveWorld with an empty name). A host
    // with several worlds, or none, has no default and leaves it empty.
    if (m_config.worlds.fn != nullptr) {
        const std::vector<WorldInfo> worlds = m_config.worlds.fn(m_config.worlds.user);
        if (worlds.size() == 1)
            m_gate.world = worlds.front().name;
    }
    const char* envReadOnly = std::getenv("ARC_REMOTE_READ_ONLY");
    const bool readOnly = m_config.readOnly || (envReadOnly != nullptr && std::strcmp(envReadOnly, "1") == 0);
    m_gate.readOnly.store(readOnly, std::memory_order_release);
    // AM1: an open-ended --remote run holds its sim until a play command
    // (Consume is Idle); a --frames N run keeps running so the scripted run
    // still finishes.
    m_tick.SetRunning(!m_config.holdSim);
    m_tick.SetPaused(m_config.holdSim);

    m_atoms = std::make_unique<ListenerAtoms>();
    m_queue = std::make_unique<RequestQueue>(QueueDepthSetting());
    m_dispatcher = std::make_unique<RemoteDispatcher>(*m_queue, registry, m_gate, m_jobs,
                                                      remoteDir.empty() ? std::filesystem::path()
                                                                        : remoteDir / "audit.ndjson");
    m_listener = std::make_unique<RemoteListener>(*m_queue, *m_atoms, MakeToken());
    m_listener->Start();
    if (m_listener->Port() == 0) {
        ARC_ERROR("remote: the loopback listener did not start; this host runs without remote");
        Teardown();
        return false;
    }

    const std::int64_t now = NowUnixMs();
    m_gate.heartbeatUnixMs.store(now, std::memory_order_release);
    PublishStatus(now);
    PublishCatalogue();

    Descriptor desc;
    desc.hostKind = m_config.kind;
    desc.pid = self.pid;
    desc.processStart = m_processStart;
    desc.port = m_listener->Port();
    desc.token = m_listener->Token();
    desc.project = m_project;
    desc.heartbeatUnixMs = now;
    desc.state = m_gate.State();
    desc.frame = m_gate.frame.load(std::memory_order_acquire);
    desc.abi = kGamePluginABIVersion; // ruling F27
    if (m_descriptorPath.empty() || !WriteDescriptorAtomic(m_descriptorPath, desc)) {
        ARC_ERROR("remote: could not write the descriptor {}; this host runs without remote",
                  Utf8(m_descriptorPath));
        Teardown();
        return false;
    }

    m_descriptorWritten = true;
    m_started = true;
    m_lastActivity = std::chrono::steady_clock::now();
    m_lastHeartbeat = m_lastActivity;
    ARC_INFO("remote: {} listening on 127.0.0.1:{} (descriptor {}){}", ToString(m_config.kind),
             m_listener->Port(), Utf8(m_descriptorPath), readOnly ? ", read-only" : "");
    return true;
}

void HostSession::Drain()
{
    if (!m_started)
        return;

    m_gate.catalog = CommandRegistry::Get().Generation();
    // A call already queued counts as activity even if the gate refuses it.
    const bool hadQueued = m_queue->Depth() > 0;
    m_dispatcher->Drain(FrameBudgetMsSetting(), kFrameBudgetCommands);
    const auto steadyNow = std::chrono::steady_clock::now();
    if (hadQueued || m_dispatcher->LastDrainRan() > 0)
        m_lastActivity = steadyNow;

    const std::int64_t now = NowUnixMs();
    m_gate.heartbeatUnixMs.store(now, std::memory_order_release);
    PublishStatus(now);
    PublishCatalogue();

    if (steadyNow - m_lastHeartbeat >= kHeartbeatInterval) {
        m_lastHeartbeat = steadyNow;
        TouchHeartbeat(m_descriptorPath, now, m_gate.State(), m_gate.frame.load(std::memory_order_acquire));
        m_jobs.Prune(now);
    }
}

bool HostSession::DrainFrame(std::uint32_t pluginGeneration)
{
    if (!m_started)
        return false;
    // The first generation seen is the module the host booted with, not a reload.
    if (!m_generationKnown) {
        m_gate.pluginGeneration = pluginGeneration;
        m_generationKnown = true;
    } else if (pluginGeneration != m_gate.pluginGeneration) {
        NoteReload(pluginGeneration, true, "reloaded", m_gate.frame.load(std::memory_order_acquire));
    }
    Drain();
    if (m_exitReason == nullptr) { // the first reason wins: it is what ended the loop
        if (TakeQuit())
            m_exitReason = "remote-quit";
        else if (IdleExpired())
            m_exitReason = "remote-idle";
    }
    return m_exitReason != nullptr;
}

const char* HostSession::ExitReason() const
{
    return m_exitReason;
}

void HostSession::MarkStopping()
{
    if (!m_started)
        return;
    m_gate.SetState("shutting-down");
    const std::int64_t now = NowUnixMs();
    PublishStatus(now);
    TouchHeartbeat(m_descriptorPath, now, m_gate.State(), m_gate.frame.load(std::memory_order_acquire));
}

void HostSession::Stop(std::string exitReason)
{
    if (!m_started)
        return;
    m_started = false;
    m_gate.SetState("shutting-down");
    PublishStatus(NowUnixMs());
    ARC_INFO("remote: {} stopping ({})", ToString(m_config.kind), exitReason);
    Teardown();
}

void HostSession::Teardown()
{
    if (m_listener)
        m_listener->Stop(); // answers every parked call host.died, joins the service thread
    m_listener.reset();
    m_dispatcher.reset();
    m_jobs.SetQueue(nullptr); // the queue goes next; no finish path may Complete into it
    m_queue.reset();
    m_atoms.reset();
    m_cataloguePublished = false;
    ReleaseSessionCommands(this); // a stopped session's commands answer host.unavailable
    // The world table names this host's objects (user = the app): a stopped
    // session leaves world-taking commands answering host.unavailable. Like
    // ReleaseSessionCommands, a table another session installed since is left alone.
    const WorldTable installed = CommandWorlds();
    if (installed.fn == m_config.worlds.fn && installed.user == m_config.worlds.user)
        SetCommandWorlds(WorldTable{});
    if (m_descriptorWritten) {
        std::error_code ec;
        std::filesystem::remove(m_descriptorPath, ec);
        m_descriptorWritten = false;
    }
    CommandRegistry::Get().SetHostKind(m_kindBefore); // ruling S3-9
}

bool HostSession::IdleExpired() const
{
    if (!m_started || m_config.idleExitSeconds == 0)
        return false;
    return std::chrono::steady_clock::now() - m_lastActivity >= std::chrono::seconds(m_config.idleExitSeconds);
}

void HostSession::NoteMinimized(bool minimized)
{
    if (!m_started)
        return;
    m_gate.minimized.store(minimized, std::memory_order_release);
}

HostGate& HostSession::Gate()
{
    return m_gate;
}

const HostGate& HostSession::Gate() const
{
    return m_gate;
}

const HostSession::Config& HostSession::GetConfig() const
{
    return m_config;
}

TickGate& HostSession::Tick()
{
    return m_tick;
}

void HostSession::NoteReload(std::uint32_t generation, bool ok, std::string reason, std::uint64_t frame)
{
    m_gate.pluginGeneration = generation; // main thread (Task 9 carry 4)
    m_generationKnown = true;
    m_reloadOk = ok;
    m_reloadReason = std::move(reason);
    m_reloadAtFrame = frame;
    if (m_started)
        PublishStatus(NowUnixMs());
}

std::uint16_t HostSession::Port() const
{
    return m_started && m_listener ? m_listener->Port() : 0;
}

const std::filesystem::path& HostSession::DescriptorFile() const
{
    return m_descriptorPath;
}

bool HostSession::TakeQuit() const
{
    return m_quit.load(std::memory_order_acquire);
}

void HostSession::RequestQuit()
{
    m_quit.store(true, std::memory_order_release);
}

void HostSession::PublishStatus(std::int64_t nowUnixMs)
{
    if (!m_atoms || !m_listener)
        return;
    auto status = std::make_shared<HostStatus>();
    status->hostKind = m_config.kind;
    status->pid = m_gate.pid;
    status->processStart = m_processStart;
    status->port = m_listener->Port();
    status->state = m_gate.State();
    status->frame = m_gate.frame.load(std::memory_order_acquire);
    status->world = m_gate.world;
    status->abi = kGamePluginABIVersion; // ruling F27: the descriptor's value
    status->project = m_project;
    status->catalogueGeneration = m_gate.catalog;
    status->pluginGeneration = m_gate.pluginGeneration;
    status->reloadOk = m_reloadOk;
    status->reloadReason = m_reloadReason;
    status->reloadAtFrame = m_reloadAtFrame;
    status->capabilities = m_capabilities;
    status->heartbeatUnixMs = nowUnixMs;
    status->minimized = m_gate.minimized.load(std::memory_order_acquire);
    m_atoms->status.store(std::move(status));
}

void HostSession::PublishCatalogue()
{
    if (!m_atoms)
        return;
    // The catalogue only changes with the registry's generation; rebuilding the
    // JSON (every command's schema) each frame would be waste.
    const CommandRegistry& registry = CommandRegistry::Get();
    const std::uint32_t generation = registry.Generation();
    if (m_cataloguePublished && generation == m_publishedCatalogue)
        return;
    m_atoms->catalog.store(std::make_shared<const nlohmann::json>(registry.Catalogue()));
    m_publishedCatalogue = generation;
    m_cataloguePublished = true;
}

} // namespace Arcane::Remote

#endif
