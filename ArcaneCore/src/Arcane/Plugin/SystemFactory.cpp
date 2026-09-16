#include <Arcane/Plugin/SystemFactory.hpp>

#include <Arcane/Base/Assert.hpp>
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
        // THE OWNER KEY IS NOT OPTIONAL. `instantiate` is a std::function compiled
        // INTO a module image; the only thing that can drop it before that image
        // unmaps is PluginHost's ClearOwner(image base), and an entry with no owner
        // matches no ClearOwner call. Keeping one would leave a callable into freed
        // code in a PROCESS-LIFETIME table, and the very next Runtime ctor would
        // call it through InstantiateModuleSystems. Registration is therefore legal
        // ONLY inside the BeginOwner/EndOwner bracket PluginHost holds across a
        // module's Init (GameModule::RegisterSystem's documented window) -- a
        // lazy RegisterSystem from OnUpdate, or any future non-Init path, is a
        // programmer error: fatal in Debug, and the entry is DROPPED rather than
        // kept as a landmine.
        ARC_ASSERT(m_openOwner != nullptr,
                   "SystemFactoryTable::Add outside an open owner bracket -- register systems from "
                   "GameModule::OnInit (the owner key is the module image, and an unowned entry can "
                   "never be cleared before that image unmaps)");
        if (!m_openOwner)
        {
            ARC_ERROR("SystemFactories: dropped the registration of '{}' -- no module image owns it "
                      "(register systems from OnInit, spec 2026-09-15 s4)", e.name);
            return;
        }
        e.owner = m_openOwner;
        m_entries.push_back(std::move(e));
    }

    void SystemFactoryTable::BeginOwner(const void* owner) noexcept
    {
        // A null key would be un-clearable AND would collide with the next module's
        // null key (purging the wrong entries). PluginHost refuses registration for
        // an image with no known range rather than calling this; the guard makes the
        // table safe on its own terms.
        if (!owner)
        {
            m_openOwner = nullptr;
            return;
        }
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
