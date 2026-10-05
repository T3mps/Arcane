#pragma once

// ArcaneServer's local admin console (settings spec s9): each stdin line is
// one RemoteCVarService request in the ServerAdmin context -- the console a
// dedicated server's operator types into. Core-only, like the rest of this
// host. The pure halves (grammar, replies, dedicated defaults) are
// source-compiled into ArcaneTests; the stdin reader runs only in the exe.

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/RemoteCVarService.hpp>

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Server
{
    inline constexpr std::string_view kAdminHelp =
        "get <name> | set <name> <value> | list [prefix] | explain <name> | <name> | <name> <value>"
        " -- ServerAdmin context; Protected values are never shown";

    [[nodiscard]] std::optional<RemoteCVarRequest> ParseAdminLine(std::string_view line, std::string_view callerId);

    class AdminConsole
    {
    public:
        AdminConsole(RemoteCVarService& service, std::string callerId);
        // "OK <text>" / "ERR <text>" (multi-line text after "OK\n"); "" for a blank or comment line.
        [[nodiscard]] std::string Submit(std::string_view line);

    private:
        RemoteCVarService* m_service;
        std::string m_callerId;
    };

    // Lines from stdin, read on a detached thread. The state is shared, so the
    // reader may outlive this object (a getline blocked on an interactive
    // console at exit is ended by the process exit, not joined). EOF -- a
    // file, NUL, or the witness's closed pipe -- just ends the thread.
    class StdinLines
    {
    public:
        StdinLines();
        void Start();
        [[nodiscard]] std::vector<std::string> Drain();

    private:
        struct State
        {
            std::mutex mutex;
            std::deque<std::string> lines;
        };
        std::shared_ptr<State> m_state;
        bool m_started = false;
    };

    // A dedicated host refuses cheats unless the project or the operator opts in
    // (spec s3.2: server.cheatsAllowed "off for dedicated"). Sets the Project
    // rung to false only when no Project-or-stronger record exists. Returns
    // whether it set anything; the caller publishes.
    bool ApplyDedicatedServerDefaults(CVarRegistry& registry);

    // The service's audit sink for this host: one engine-log line per record.
    void LogAuditRecord(const CVarAuditRecord& record, void* user);
}
