#include <ConsoleBuffer.hpp>

namespace Arcane::Editor
{
    void ConsoleBuffer::Push(std::string line)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines.push_back(std::move(line));
        while (m_lines.size() > m_capacity) m_lines.pop_front();
    }

    std::size_t ConsoleBuffer::Size() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lines.size();
    }

    void ConsoleBuffer::ForEach(Arcane::FunctionRef<void(const std::string&)> fn) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const std::string& l : m_lines) fn(l);
    }
}
