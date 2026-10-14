#include <Arcane/Remote/TickGate.hpp>

#if !defined(ARC_BUILD_DIST)

#include <limits>

namespace Arcane::Remote
{

void TickGate::SetRunning(bool running)
{
    std::lock_guard lock(m_mutex);
    m_running = running;
}

void TickGate::SetPaused(bool paused)
{
    std::lock_guard lock(m_mutex);
    m_paused = paused;
}

void TickGate::RequestSteps(std::uint32_t n)
{
    std::lock_guard lock(m_mutex);
    const std::uint32_t room = std::numeric_limits<std::uint32_t>::max() - m_steps;
    m_steps += n < room ? n : room;
}

TickDecision TickGate::Consume()
{
    std::lock_guard lock(m_mutex);
    if (m_running && !m_paused) {
        m_steps = 0; // a free run makes any pending single steps moot
        return TickDecision::Run;
    }
    if (m_steps > 0) {
        --m_steps;
        return TickDecision::Step;
    }
    return TickDecision::Idle;
}

bool TickGate::Running() const
{
    std::lock_guard lock(m_mutex);
    return m_running;
}

bool TickGate::Paused() const
{
    std::lock_guard lock(m_mutex);
    return m_paused;
}

std::uint32_t TickGate::StepsRemaining() const
{
    std::lock_guard lock(m_mutex);
    return m_steps;
}

} // namespace Arcane::Remote

#endif
