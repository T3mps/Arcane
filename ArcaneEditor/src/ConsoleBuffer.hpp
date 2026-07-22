#pragma once

#include <Arcane/Util/FunctionRef.hpp>

#include <cstddef>
#include <deque>
#include <string>

namespace Arcane::Editor
{
    // A bounded FIFO of log lines. Push appends; when full the oldest line is
    // dropped. Not thread-safe on its own -- the spdlog sink that feeds it holds
    // spdlog's sink mutex, and the UI reads it on the same (main) thread that
    // pumps ImGui, so pushes and reads never overlap in Arcane Editor's single-thread
    // frame. (If a worker ever logs, wrap Push in the sink's lock.)
    class ConsoleBuffer
    {
    public:
        explicit ConsoleBuffer(std::size_t capacity) : m_capacity(capacity ? capacity : 1) {}

        void Push(std::string line);
        [[nodiscard]] std::size_t Size() const noexcept { return m_lines.size(); }
        [[nodiscard]] std::size_t Capacity() const noexcept { return m_capacity; }
        void ForEach(Arcane::FunctionRef<void(const std::string&)> fn) const;

    private:
        std::size_t             m_capacity;
        std::deque<std::string> m_lines;
    };
}
