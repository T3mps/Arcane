#include <Arcane/Remote/Dispatcher.hpp>

#if !defined(ARC_BUILD_DIST)

#include <utility>

namespace Arcane::Remote
{

std::string HostGate::State() const
{
    std::lock_guard lock(stateMutex);
    return state;
}

void HostGate::SetState(std::string s)
{
    std::lock_guard lock(stateMutex);
    state = std::move(s);
}

HostBlock HostGate::Block() const
{
    HostBlock block;
    block.kind = kind;
    block.pid = pid;
    block.world = world;
    block.frame = frame.load(std::memory_order_acquire);
    block.state = State();
    block.catalog = catalog;
    block.pluginGeneration = pluginGeneration;
    return block;
}

} // namespace Arcane::Remote

#endif
