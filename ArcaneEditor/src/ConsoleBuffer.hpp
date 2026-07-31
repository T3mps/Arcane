#pragma once

#include <Arcane/Util/FunctionRef.hpp>

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

namespace Arcane::Editor
{
    // A bounded FIFO of log lines. Push appends; when full the oldest line is
    // dropped. Guarded by its own mutex (2026-07-31 review, Important 2): since
    // project_open/editor_lock's Worker-thread boot stages started logging
    // through Arcane::Log::Engine() (Critical 1 fix), Push can now be called
    // from a worker thread while the UI reads ForEach/Size on the main thread
    // that pumps ImGui. That overlap is latent today -- the sink's own
    // callback_sink_mt mutex serializes concurrent Push callers against each
    // other, and nothing currently draws the Console panel while a boot/switch
    // worker is alive -- but the safety argument had silently moved from a
    // checkable invariant ("no worker logs") to an uncheckable one ("workers
    // only log when nobody is drawing the Console"). This mutex makes the
    // invariant checkable again instead of relying on that timing coincidence.
    class ConsoleBuffer
    {
    public:
        explicit ConsoleBuffer(std::size_t capacity) : m_capacity(capacity ? capacity : 1) {}

        void Push(std::string line);
        [[nodiscard]] std::size_t Size() const noexcept;
        [[nodiscard]] std::size_t Capacity() const noexcept { return m_capacity; }
        void ForEach(Arcane::FunctionRef<void(const std::string&)> fn) const;

    private:
        std::size_t             m_capacity;
        std::deque<std::string> m_lines;
        mutable std::mutex      m_mutex;
    };
}
