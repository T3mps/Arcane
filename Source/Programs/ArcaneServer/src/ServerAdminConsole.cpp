#include "ServerAdminConsole.hpp"

#include <Arcane/Base/Log.hpp>

#include <iostream>
#include <thread>
#include <utility>

namespace Arcane::Server
{
    namespace
    {
        // A UTF-8 byte-order mark is not part of a command: Windows
        // PowerShell's pipe and a file saved by Notepad start stdin with one.
        constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

        std::string_view Trim(std::string_view s)
        {
            if (s.starts_with(kUtf8Bom)) s.remove_prefix(kUtf8Bom.size());
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
                s.remove_suffix(1);
            return s;
        }

        std::string_view NextToken(std::string_view& rest)
        {
            rest = Trim(rest);
            const std::size_t gap = rest.find_first_of(" \t");
            const std::string_view token = rest.substr(0, gap);
            rest = gap == std::string_view::npos ? std::string_view{} : Trim(rest.substr(gap + 1));
            return token;
        }
    }

    std::optional<RemoteCVarRequest> ParseAdminLine(std::string_view line, std::string_view callerId)
    {
        std::string_view rest = Trim(line);
        if (rest.empty() || rest.front() == '#') return std::nullopt;
        const std::string_view first = NextToken(rest);
        RemoteCVarRequest request;
        request.callerId = std::string(callerId);
        if (first == "get" || first == "explain" || first == "list")
        {
            request.op = std::string(first);
            request.name = std::string(NextToken(rest));
            return request;
        }
        if (first == "set")
        {
            request.op = "set";
            request.name = std::string(NextToken(rest));
            request.value = std::string(rest);
            return request;
        }
        request.name = std::string(first);
        if (rest.empty()) request.op = "get";
        else { request.op = "set"; request.value = std::string(rest); }
        return request;
    }

    AdminConsole::AdminConsole(RemoteCVarService& service, std::string callerId)
        : m_service(&service), m_callerId(std::move(callerId))
    {
    }

    std::string AdminConsole::Submit(std::string_view line)
    {
        const std::string_view trimmed = Trim(line);
        if (trimmed == "help" || trimmed == "?") return "OK " + std::string(kAdminHelp);
        const std::optional<RemoteCVarRequest> request = ParseAdminLine(line, m_callerId);
        if (!request) return {};
        const RemoteCVarResponse response = m_service->Handle(*request);
        std::string out = response.ok ? "OK" : "ERR";
        if (!response.text.empty())
        {
            out += response.text.find('\n') == std::string::npos ? " " : "\n";
            out += response.text;
        }
        return out;
    }

    StdinLines::StdinLines() : m_state(std::make_shared<State>()) {}

    void StdinLines::Start()
    {
        if (m_started) return;
        m_started = true;
        std::thread([state = m_state] {
            std::string line;
            while (std::getline(std::cin, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::lock_guard<std::mutex> lock(state->mutex);
                state->lines.push_back(std::move(line));
                line.clear();
            }
        }).detach();
    }

    std::vector<std::string> StdinLines::Drain()
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        std::vector<std::string> out(std::make_move_iterator(m_state->lines.begin()),
                                     std::make_move_iterator(m_state->lines.end()));
        m_state->lines.clear();
        return out;
    }

    bool ApplyDedicatedServerDefaults(CVarRegistry& registry)
    {
        const auto explained = registry.Explain("server.cheatsAllowed");
        if (!explained) return false;
        for (const CVarHistoryRecord& h : explained->history)
            if (h.by >= SetBy::Project) return false;   // the project, the user or the command line decided
        return registry.Set(registry.Find("server.cheatsAllowed"), CVarValue::Bool(false), SetBy::Project,
                            "dedicated-host", CVarContext::ServerAdmin) == SetResult::Applied;
    }

    void LogAuditRecord(const CVarAuditRecord& record, void*)
    {
        ARC_INFO("cvar-audit {} {} -> {} by {} ({})", record.name, record.oldValue.empty() ? "\"\"" : record.oldValue,
                 record.newValue, record.callerId, record.verdict == PolicyVerdict::Deny ? "denied" : "applied");
    }
}
