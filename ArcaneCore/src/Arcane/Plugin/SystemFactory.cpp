#include <Arcane/Plugin/SystemFactory.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <algorithm>
#include <utility>

namespace Arcane
{
    const char* ToString(NetMode m) noexcept
    {
        switch (m)
        {
            case NetMode::Standalone:      return "Standalone";
            case NetMode::DedicatedServer: return "DedicatedServer";
            case NetMode::ListenServer:    return "ListenServer";
            case NetMode::Client:          return "Client";
        }
        return "Unknown";
    }

    void SystemFactoryTable::Add(SystemFactoryEntry e)
    {
        if (m_openOwner)
            e.owner = m_openOwner;
        m_entries.push_back(std::move(e));
    }

    void SystemFactoryTable::BeginOwner(const void* owner) noexcept
    {
        ClearOwner(owner);
        m_openOwner = owner;
    }

    void SystemFactoryTable::EndOwner() noexcept
    {
        m_openOwner = nullptr;
    }

    void SystemFactoryTable::ClearOwner(const void* owner) noexcept
    {
        std::erase_if(m_entries, [owner](const SystemFactoryEntry& e) { return e.owner == owner; });
    }

    std::size_t SystemFactoryTable::Size() const noexcept { return m_entries.size(); }

    std::span<const SystemFactoryEntry> SystemFactoryTable::Entries() const noexcept
    {
        return std::span<const SystemFactoryEntry>(m_entries.data(), m_entries.size());
    }

    std::size_t SystemFactoryTable::InstantiateInto(SystemSchedulers& into, NetMode mode) const
    {
        std::size_t n = 0;
        for (const SystemFactoryEntry& e : m_entries)
        {
            if (!RoleMatches(e.mask, mode) || !e.instantiate)
                continue;
            switch (e.phase)
            {
                case SystemPhase::FixedUpdate: e.instantiate(into.fixedUpdate); break;
                case SystemPhase::Update:      e.instantiate(into.update);      break;
                case SystemPhase::Render:      e.instantiate(into.render);      break;
            }
            ++n;
        }
        if (n != 0)
            ARC_TRACE("SystemFactories: instantiated {} module system(s) for NetMode {}", n, ToString(mode));
        return n;
    }
}
