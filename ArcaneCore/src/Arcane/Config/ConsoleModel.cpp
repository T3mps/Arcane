#include <Arcane/Config/ConsoleModel.hpp>

namespace Arcane
{
    void ConsoleModel::Submit(CVarRegistry& registry, Permission permission)
    {
        if (m_input.empty()) return;
        m_lines.push_back(ConsoleLine{ "> " + m_input, true });
        const ExecResult result = registry.Execute(m_input, permission);
        registry.Publish();
        m_lines.push_back(ConsoleLine{ result.text, result.ok });
        m_input.clear();
    }

    std::vector<std::string> ConsoleModel::Complete(const CVarRegistry& registry) const
    {
        std::vector<std::string> matches;
        if (m_input.empty()) return matches;
        for (const CVarListEntry& entry : registry.List())
        {
            if (entry.name.rfind(m_input, 0) == 0) matches.push_back(entry.name);
        }
        return matches;
    }
}
