#pragma once
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Protocol.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

// What the host publishes about itself to the dispatcher and the job table;
// Block() is the s6.3 `host` block every envelope carries.
struct ARC_REMOTE_API HostGate {
    std::atomic<bool> minimized{false};
    std::atomic<bool> readOnly{false};
    std::atomic<std::uint64_t> frame{0};
    std::atomic<std::int64_t> heartbeatUnixMs{0};
    std::string state = "ready";
    mutable std::mutex stateMutex; // State() and Block() are const and lock it
    std::string world;
    std::uint32_t pid = 0;
    std::string kind = "editor";
    std::uint32_t catalog = 0;
    std::uint32_t pluginGeneration = 0;
    std::string State() const;
    void SetState(std::string s);
    HostBlock Block() const; // fills HostBlock from these fields
};

class CommandRegistry; // Registry.hpp
struct Call;           // Registry.hpp
struct CommandResult;  // Registry.hpp

// The host main thread's half of the remote surface (spec s4.3, s7.7). Each
// Drain steps every running job once, then pops queued calls within the frame
// budget, applies the state gate, invokes, audits Drive commands and Completes
// each call's queue key (or binds it to the job the command armed). Main
// thread only: it reads the gate's plain fields and the job table's rows.
class ARC_REMOTE_API RemoteDispatcher {
public:
    // Calls jobs.SetQueue(&queue) (ruling F2). Creates auditPath's directory
    // and prunes its sibling out/ directory to the newest 200 files.
    RemoteDispatcher(RequestQueue& queue, CommandRegistry& registry, HostGate& gate,
                     JobTable& jobs, std::filesystem::path auditPath);
    RemoteDispatcher(const RemoteDispatcher&) = delete;
    RemoteDispatcher& operator=(const RemoteDispatcher&) = delete;

    // A positive argument wins; budgetMs <= 0 reads remote.server.frameBudgetMs
    // (itself <= 0 reads as kFrameBudgetMs) and budgetCommands == 0 means
    // kFrameBudgetCommands.
    void Drain(double budgetMs, std::uint32_t budgetCommands);
    std::uint32_t LastDrainRan() const; // commands invoked by the last Drain (gate refusals excluded)

private:
    void RunOne(std::uint64_t key, const Call& call, std::chrono::steady_clock::time_point queuedAt,
                double budgetMs);
    void Audit(const Call& call, const Envelope& result);
    void Publish(std::uint64_t key, CommandResult result);

    RequestQueue& m_queue;
    CommandRegistry& m_registry;
    HostGate& m_gate;
    JobTable& m_jobs;
    std::filesystem::path m_auditPath;
    std::uint32_t m_lastDrainRan = 0;
    std::set<std::string, std::less<>> m_slowWarned; // commands already warned for a slow Invoke
    bool m_auditWarned = false;
};

} // namespace Arcane::Remote

#endif

namespace Arcane::Remote
{

// Declared in every configuration: Dist's registry refuses these Dev cvars and
// the handles read the declared defaults.
ARC_REMOTE_API double FrameBudgetMsSetting();     // published remote.server.frameBudgetMs
ARC_REMOTE_API std::uint32_t QueueDepthSetting(); // published remote.server.queueDepth; HostSession (Task 11) sizes its RequestQueue with it

} // namespace Arcane::Remote
