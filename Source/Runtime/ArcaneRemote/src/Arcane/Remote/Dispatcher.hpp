#pragma once
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Protocol.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
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

} // namespace Arcane::Remote

#endif
