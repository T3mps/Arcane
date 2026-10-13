#include <Arcane/Config/RemoteCVarService.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

namespace Arcane
{
    namespace
    {
        constexpr std::string_view kProtected = "<protected>";

        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
                s.remove_suffix(1);
            return s;
        }

        bool MatchesPrefix(std::string_view name, std::string_view prefix)
        {
            if (prefix.empty()) return true;
            if (!name.starts_with(prefix)) return false;
            if (prefix.back() == '.' || name.size() == prefix.size()) return true;
            return name[prefix.size()] == '.';
        }

        // Execute's bare read answers "<name> = <value>"; keep just the value.
        std::string StripPrefix(std::string_view name, const std::string& text)
        {
            const std::string prefix = std::string(name) + " = ";
            return text.starts_with(prefix) ? text.substr(prefix.size()) : text;
        }

        std::int64_t NowUnixMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        }
    }

    RemoteCVarService::RemoteCVarService(CVarRegistry& registry, CVarAuditFn sink, void* sinkUser) noexcept
        : m_registry(&registry), m_sink(sink), m_sinkUser(sinkUser)
    {
    }

    RemoteCVarService::Target RemoteCVarService::Resolve(std::string_view name) const
    {
        Target t;
        for (const CVarListEntry& c : m_registry->ListCommands())   // Hidden commands are not listed: unknown
            if (c.name == name) { t.kind = Kind::Command; t.flags = c.flags; return t; }
        const auto explained = m_registry->Explain(name);
        if (!explained || HasFlag(explained->flags, CVarFlags::Hidden)) return t;
        t.kind = Kind::CVar;
        t.flags = explained->flags;
        return t;
    }

    std::string RemoteCVarService::ReadValue(std::string_view name, const CVarCaller& caller)
    {
        const ExecResult r = m_registry->Execute(name, CVarContext::ServerAdmin, SetBy::Console, &caller);
        return r.ok ? StripPrefix(name, r.text) : std::string{};
    }

    void RemoteCVarService::Audit(std::string_view name, std::string_view callerId, std::string before,
                                  std::string after, PolicyVerdict verdict)
    {
        if (!m_sink) return;
        CVarAuditRecord record;
        record.name = name;
        record.callerId = callerId;
        record.oldValue = std::move(before);
        record.newValue = std::move(after);
        record.context = CVarContext::ServerAdmin;
        record.verdict = verdict;
        record.unixMs = NowUnixMs();
        m_sink(record, m_sinkUser);
    }

    RemoteCVarResponse RemoteCVarService::List(const std::string& prefix, const CVarCaller& caller)
    {
        std::vector<CVarListEntry> entries = m_registry->List(CVarContext::ServerAdmin);
        std::sort(entries.begin(), entries.end(),
                  [](const CVarListEntry& a, const CVarListEntry& b) { return a.name < b.name; });
        std::string text;
        std::size_t shown = 0;
        for (const CVarListEntry& e : entries)
        {
            if (!MatchesPrefix(e.name, prefix)) continue;
            const CVarHandle handle = m_registry->Find(e.name);
            if (!m_registry->CanRead(handle, CVarContext::ServerAdmin, &caller)) continue;
            std::string line;
            if (HasFlag(e.flags, CVarFlags::Protected))
            {
                line = e.name + " = " + std::string(kProtected);
            }
            else
            {
                const ExecResult r = m_registry->Execute(e.name, CVarContext::ServerAdmin, SetBy::Console, &caller);
                if (!r.ok) continue;   // not readable here (a policy)
                line = e.name + " = " + StripPrefix(e.name, r.text);
            }
            if (shown++ != 0) text += '\n';
            text += line;
        }
        if (shown == 0)
            return { true, prefix.empty() ? std::string("(no cvars)") : "(no cvars match '" + prefix + "')" };
        return { true, std::move(text) };
    }

    RemoteCVarResponse RemoteCVarService::RunCommand(const std::string& op, const std::string& name,
                                                     const std::string& args, const std::string& callerId,
                                                     const CVarCaller& caller, CVarFlags flags)
    {
        if (op == "explain") return { false, "'" + name + "' is a command, not a cvar" };
        if (!HasFlag(flags, CVarFlags::ServerCanExecute))
            return { false, "command '" + name + "' is not remotely executable (no ServerCanExecute)" };
        // Only op set runs a command. get on a flagged command used to Execute
        // it; the explain op is the remote history path, never cvar_explain.
        if (op != "set")
            return { false, "command '" + name + "' runs only via set" };
        if (name == "cvar_explain")
            return { false, "cvar_explain is not remotely executable; use the explain op" };
        const ExecResult r = m_registry->Execute(args.empty() ? name : name + " " + args, CVarContext::ServerAdmin,
                                                 SetBy::Console, &caller);
        Audit(name, callerId, {}, args, r.ok ? PolicyVerdict::Allow : PolicyVerdict::Deny);
        return { r.ok, r.text };
    }

    RemoteCVarResponse RemoteCVarService::Handle(const RemoteCVarRequest& request)
    {
        const std::string op{ Trim(request.op) };
        const std::string name{ Trim(request.name) };
        const std::string value{ Trim(request.value) };
        const CVarCaller caller{ request.callerId, 0, nullptr };

        if (op == "list") return List(name, caller);
        if (op != "get" && op != "set" && op != "explain")
            return { false, "unknown op '" + op + "' (get|set|list|explain)" };
        if (name.empty()) return { false, op + ": missing name" };

        const Target target = Resolve(name);
        if (target.kind == Kind::Unknown) return { false, "unknown cvar '" + name + "'" };
        if (target.kind == Kind::Command) return RunCommand(op, name, value, request.callerId, caller, target.flags);

        const bool isProtected = HasFlag(target.flags, CVarFlags::Protected);
        const std::string protectedText = name + " is protected: never readable remotely";

        if (op == "get")
        {
            if (isProtected) return { false, protectedText };
            const ExecResult r = m_registry->Execute(name, CVarContext::ServerAdmin, SetBy::Console, &caller);
            if (!r.ok) return { false, r.text };
            return { true, name + " = " + StripPrefix(name, r.text) };
        }

        if (op == "explain")
        {
            if (isProtected) return { false, protectedText };
            const ExecResult probe = m_registry->Execute(name, CVarContext::ServerAdmin, SetBy::Console, &caller);
            if (!probe.ok) return { false, probe.text };   // not readable in this context: no history either
            const ExecResult r = m_registry->Execute("cvar_explain " + name, CVarContext::ServerAdmin,
                                                     SetBy::Console, &caller);
            return { r.ok, r.text };
        }

        // set
        if (value.empty())
        {
            std::string before = isProtected ? std::string(kProtected) : ReadValue(name, caller);
            Audit(name, request.callerId, std::move(before), std::string{}, PolicyVerdict::Deny);
            return { false, "set " + name + ": missing value" };
        }
        std::string before = isProtected ? std::string(kProtected) : ReadValue(name, caller);
        const ExecResult r = m_registry->Execute(name + " " + value, CVarContext::ServerAdmin, SetBy::Console, &caller);
        std::string after;
        if (r.ok)
        {
            m_registry->Publish();
            after = isProtected ? std::string(kProtected) : ReadValue(name, caller);
        }
        else
        {
            after = isProtected ? std::string(kProtected) : value;
        }
        Audit(name, request.callerId, std::move(before), after, r.ok ? PolicyVerdict::Allow : PolicyVerdict::Deny);
        if (!r.ok) return { false, r.text };
        return { true, name + " = " + after };
    }
}
