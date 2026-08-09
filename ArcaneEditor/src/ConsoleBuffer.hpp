#pragma once

// A bounded FIFO of structured log entries. Push appends; when full the oldest
// entry is dropped.
//
// THREAD-SAFE by its own mutex. The original version relied on "pushes and reads
// never overlap in Arcane Editor's single-thread frame" and said outright: "If a
// worker ever logs, wrap Push in the sink's lock." The async-boot arc runs
// project open (and its asset-registry scan, which logs) on a worker thread, so
// that day has arrived -- the lock lives here rather than in the sink so every
// caller inherits it.

#include <ConsoleModel.hpp>

#include <Arcane/Util/FunctionRef.hpp>

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

namespace Arcane::Editor
{
    class ConsoleBuffer
    {
    public:
        explicit ConsoleBuffer(std::size_t capacity) : m_capacity(capacity ? capacity : 1) {}

        void Push(ConsoleEntry entry);
        void Clear();
        // Trims immediately when the new cap is smaller.
        void SetCapacity(std::size_t capacity);

        [[nodiscard]] std::size_t Size() const;
        [[nodiscard]] std::size_t Capacity() const;

        void ForEach(Arcane::FunctionRef<void(const ConsoleEntry&)> fn) const;
        // A copy, for the draw pass: CollapseConsole holds pointers into its
        // input, so the panel needs storage that cannot be evicted mid-frame by
        // a worker push.
        [[nodiscard]] std::vector<ConsoleEntry> Snapshot() const;

    private:
        mutable std::mutex      m_mutex;
        std::size_t             m_capacity;
        std::deque<ConsoleEntry> m_entries;
    };
}
