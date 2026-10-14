#include <Panels/ConsoleBuffer.hpp>

namespace Arcane::Editor
{
    void ConsoleBuffer::Push(ConsoleEntry entry)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Stamped under the same lock that orders the pushes, so seq order ==
        // display order even with worker-thread pushes. Never reset (not even
        // by Clear) -- see ConsoleEntry::seq.
        entry.seq = m_nextSeq++;
        m_entries.push_back(std::move(entry));
        while (m_entries.size() > m_capacity) m_entries.pop_front();
    }

    void ConsoleBuffer::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }

    void ConsoleBuffer::SetCapacity(std::size_t capacity)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_capacity = capacity ? capacity : 1;
        while (m_entries.size() > m_capacity) m_entries.pop_front();
    }

    std::size_t ConsoleBuffer::Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.size();
    }

    std::size_t ConsoleBuffer::Capacity() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_capacity;
    }

    void ConsoleBuffer::ForEach(Arcane::FunctionRef<void(const ConsoleEntry&)> fn) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const ConsoleEntry& e : m_entries) fn(e);
    }

    std::vector<ConsoleEntry> ConsoleBuffer::Snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return std::vector<ConsoleEntry>(m_entries.begin(), m_entries.end());
    }
}
