#pragma once

// Shared so the plugin module and the test module use the SAME C++ type for Pulse
// (one type name -> one TypeContext id). A per-TU anonymous-namespace struct would be
// a distinct type per module; do not rely on anonymous-namespace name-hash equality.

#include <Astra/Reflection/Reflection.hpp>
#include <Astra/Registry/Registry.hpp>

namespace Arcane::HotReloadTest
{
    struct Pulse { int ticks = 0; };

    ASTRA_REFLECT_TYPE(Pulse)
        ASTRA_REFLECT_FIELD(Pulse, ticks)
    ASTRA_END_REFLECT_TYPE()

    // Role-masked probe systems (spec 2026-09-15 s4, s9). Defined in the SHARED header
    // for the same reason Pulse is: Astra keys a system by a hash of its type NAME, so
    // the test exe's HasSystem<ServerOnlyTick>() resolves the system the PLUGIN
    // registered. Each stamps a distinct counter so behaviour, not just presence, is
    // observable per world.
    struct RoleCounters { int serverTicks = 0; int clientTicks = 0; };
    ASTRA_REFLECT_TYPE(RoleCounters)
        ASTRA_REFLECT_FIELD(RoleCounters, serverTicks)
        ASTRA_REFLECT_FIELD(RoleCounters, clientTicks)
    ASTRA_END_REFLECT_TYPE()
    struct ServerOnlyTick { void operator()(Astra::Registry& r) { r.CreateView<RoleCounters>().ForEach([](Astra::Entity, RoleCounters& c) { ++c.serverTicks; }); } };
    struct ClientOnlyTick { void operator()(Astra::Registry& r) { r.CreateView<RoleCounters>().ForEach([](Astra::Entity, RoleCounters& c) { ++c.clientTicks; }); } };

    // The use-after-unload probe (PluginHostTest, "[hotreload][typecontext]"). A
    // plain, NON-reflected resource -- exactly Arcane::SceneRoot's shape -- and NOT
    // a component: the plugin's OnInit SetResource<ProbeResource> is meant to be
    // this type's FIRST resolve anywhere in the process, so the identity Astra
    // records for it belongs to the PLUGIN image.
    //
    // NO TEST-EXE CODE MAY RESOLVE ProbeResource BEFORE THE PLUGIN DOES. TypeID<>,
    // GetResource<>, SetResource<>, RegisterComponent<> and views all resolve it;
    // one stray host-side use anywhere in ArcaneTests would make the EXE the first
    // registrar and the pin would silently stop pinning anything. It is deliberately
    // absent from every other case, and its one case resolves it only AFTER Unload().
    struct ProbeResource { int value = 0; };
}
