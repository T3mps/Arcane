#include <Arcane/Config/ConsoleModel.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace Arcane
{
    namespace
    {
        // console.historySize and console.maxLines are built-ins on EVERY
        // registry (CVarRegistry's constructor), and the model serves whichever
        // registry its caller hands it, so this is a per-command lookup rather
        // than Settings<T>() (which reads the global registry only). The
        // defaults live in that one registration: no shadow copy here.
        std::int32_t BuiltinInt32(const CVarRegistry& registry, std::string_view name)
        {
            const auto v = registry.Get(registry.Find(name));
            return v && v->type == CVarType::Int32 ? v->AsInt32() : 0;
        }
    }

    void ConsoleModel::AppendLine(std::string text, bool ok)
    {
        Push(CVarRegistry::Get(), ConsoleLine{ std::move(text), ok });
    }

    void ConsoleModel::Push(const CVarRegistry& registry, ConsoleLine line)
    {
        m_lines.push_back(std::move(line));
        if (const std::int32_t max = BuiltinInt32(registry, "console.maxLines"); max > 0 && m_lines.size() > std::size_t(max))
            m_lines.erase(m_lines.begin(), m_lines.end() - max);
    }

    void ConsoleModel::Submit(CVarRegistry& registry, CVarContext ctx)
    {
        if (m_input.empty()) return;
        if (m_history.empty() || m_history.back() != m_input) m_history.push_back(m_input);
        if (const std::int32_t cap = BuiltinInt32(registry, "console.historySize"); cap > 0)
            while (m_history.size() > std::size_t(cap)) m_history.pop_front();
        m_historyPos = -1;
        m_draft.clear();
        Push(registry, ConsoleLine{ "> " + m_input, true });
        // NO Publish here: the console is a writer like any other, and the
        // frame driver's barrier is what makes an accepted set visible to
        // every reader of the next frame at once (spec 6.4).
        const ExecResult result = registry.Execute(m_input, ctx);
        Push(registry, ConsoleLine{ result.text, result.ok });
        m_input.clear();
    }

    std::vector<std::string> ConsoleModel::Complete(const CVarRegistry& registry, CVarContext ctx) const
    {
        std::vector<std::string> matches;
        if (m_input.empty()) return matches;
        for (const CVarListEntry& entry : registry.List(ctx))
            if (entry.name.rfind(m_input, 0) == 0) matches.push_back(entry.name);
        for (const CVarListEntry& entry : registry.ListCommands())
            if (entry.name.rfind(m_input, 0) == 0) matches.push_back(entry.name);
        std::sort(matches.begin(), matches.end());
        matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
        return matches;
    }

    bool ConsoleModel::CompleteInput(const CVarRegistry& registry, CVarContext ctx)
    {
        const std::vector<std::string> matches = Complete(registry, ctx);
        if (matches.empty()) return false;
        std::string next;
        if (matches.size() == 1)
            next = matches[0] + " ";
        else
        {
            next = matches[0];
            for (const std::string& m : matches)
            {
                std::size_t n = 0;
                while (n < next.size() && n < m.size() && next[n] == m[n]) ++n;
                next.resize(n);
            }
            std::string list;
            for (const std::string& m : matches) list += (list.empty() ? "" : "  ") + m;
            Push(registry, ConsoleLine{ list, true });
        }
        if (next == m_input) return false;
        m_input = next;
        return true;
    }

    bool ConsoleModel::HistoryPrev()
    {
        if (m_history.empty()) return false;
        if (m_historyPos < 0) { m_draft = m_input; m_historyPos = static_cast<int>(m_history.size()) - 1; }
        else if (m_historyPos > 0) --m_historyPos;
        else return false;
        m_input = m_history[static_cast<std::size_t>(m_historyPos)];
        return true;
    }

    bool ConsoleModel::HistoryNext()
    {
        if (m_historyPos < 0) return false;
        if (m_historyPos + 1 < static_cast<int>(m_history.size()))
            m_input = m_history[static_cast<std::size_t>(++m_historyPos)];
        else
        {
            m_historyPos = -1;
            m_input = m_draft;
        }
        return true;
    }
}
