#include <ConsoleBuffer.hpp>

namespace Arcane::Editor
{
    void ConsoleBuffer::Push(std::string line)
    {
        m_lines.push_back(std::move(line));
        while (m_lines.size() > m_capacity) m_lines.pop_front();
    }

    void ConsoleBuffer::ForEach(Arcane::FunctionRef<void(const std::string&)> fn) const
    {
        for (const std::string& l : m_lines) fn(l);
    }
}
