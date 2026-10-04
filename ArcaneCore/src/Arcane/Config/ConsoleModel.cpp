#include <Arcane/Config/ConsoleModel.hpp>

#include <algorithm>

namespace Arcane
{
    void ConsoleModel::Submit(CVarRegistry& registry, CVarContext ctx)
    {
        if (m_input.empty()) return;
        std::size_t cap = 64;
        if (const auto v = registry.Get(registry.Find("console.historySize")); v && v->type == CVarType::Int32 && v->AsInt32() > 0)
            cap = static_cast<std::size_t>(v->AsInt32());
        if (m_history.empty() || m_history.back() != m_input) m_history.push_back(m_input);
        while (m_history.size() > cap) m_history.pop_front();
        m_historyPos = -1;
        m_draft.clear();
        m_lines.push_back(ConsoleLine{ "> " + m_input, true });
        // NO Publish here: the console is a writer like any other, and the
        // frame driver's barrier is what makes an accepted set visible to
        // every reader of the next frame at once (spec 6.4).
        const ExecResult result = registry.Execute(m_input, ctx);
        m_lines.push_back(ConsoleLine{ result.text, result.ok });
        m_input.clear();
    }

    std::vector<std::string> ConsoleModel::Complete(const CVarRegistry& registry) const
    {
        std::vector<std::string> matches;
        if (m_input.empty()) return matches;
        for (const CVarListEntry& entry : registry.List())
            if (entry.name.rfind(m_input, 0) == 0) matches.push_back(entry.name);
        for (const CVarListEntry& entry : registry.ListCommands())
            if (entry.name.rfind(m_input, 0) == 0) matches.push_back(entry.name);
        std::sort(matches.begin(), matches.end());
        matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
        return matches;
    }

    bool ConsoleModel::CompleteInput(const CVarRegistry& registry)
    {
        const std::vector<std::string> matches = Complete(registry);
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
            m_lines.push_back(ConsoleLine{ list, true });
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
