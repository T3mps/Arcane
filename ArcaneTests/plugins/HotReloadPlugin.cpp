// Minimal hot-reload test plugin. Built into three DLLs from this one source:
//   HotReloadPluginV1  -> HOTRELOAD_STEP=1,  ABI = kGamePluginABIVersion
//   HotReloadPluginV2  -> HOTRELOAD_STEP=10, ABI = kGamePluginABIVersion
//   HotReloadPluginBad -> ABI = kGamePluginABIVersion + 999 (forces rollback)
// Built ON the SDK's ARCANE_GAME_MODULE (Arcane/Plugin/GameModule.hpp), so the
// [hotreload] suite is the macro's plugin test: the prologue (Pulse arrives
// through the ARCANE_COMPONENT drain), the base Save/LoadState round-trip plus
// this module's extras, the Shutdown order (the OnShutdown log line below),
// and the ABI-override seam the Bad build exists to trip.

#include "HotReloadShared.hpp"

#include <Arcane/Plugin/GameModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cstdint>

#ifndef HOTRELOAD_STEP
  #define HOTRELOAD_STEP 1
#endif
#ifndef HOTRELOAD_ABI_OFFSET
  #define HOTRELOAD_ABI_OFFSET 0
#endif

// One reflected component (shared header), registered through the drain the
// macro's Init performs -- the same path a wizard-made component takes.
ARCANE_COMPONENT(Arcane::HotReloadTest::Pulse)
ARCANE_COMPONENT(Arcane::HotReloadTest::RoleCounters)

namespace Arcane::HotReloadTest
{
    struct Module final : Arcane::GameModule
    {
        Astra::Entity pulse = Astra::Entity::Invalid();

        void CacheHandle()
        {
            pulse = Astra::Entity::Invalid();
            Registry().CreateView<Pulse>().ForEach([&](Astra::Entity e, Pulse&) { pulse = e; });
        }

        bool OnInit(Arcane::EngineContext&) override
        {
            bool exists = false;
            Registry().CreateView<Pulse>().ForEach([&](Astra::Entity, Pulse&) { exists = true; });
            if (!exists)
                Registry().CreateEntityWith(Pulse{0});   // fresh boot only
            CacheHandle();
            // The s4 contract: factories registered ONCE per DLL load, with an explicit
            // mask; each Runtime instantiates what its NetMode matches.
            RegisterSystem<ServerOnlyTick>(Arcane::RoleMask::Server, Arcane::SystemPhase::FixedUpdate);
            RegisterSystem<ClientOnlyTick>(Arcane::RoleMask::Client, Arcane::SystemPhase::FixedUpdate);
            return true;
        }

        void OnFixedUpdate(double) override
        {
            if (auto* p = Registry().GetComponent<Pulse>(pulse))
                p->ticks += (HOTRELOAD_STEP);            // V1: +1, V2: +10 (observably different code)
        }

        // The hook-order probe (PluginHostTest "OnShutdown runs while the
        // module's component handle is still open"): say whether this module's
        // ComponentModule handle is still open here -- i.e. the macro tore the
        // instance down BEFORE the handle. Logged, not stamped into the registry:
        // Unload() resets the registry right after, and Log::Engine() is the
        // DLL's one logger the test can attach a sink to.
        void OnShutdown() override
        {
            ARC_INFO("HotReloadPlugin: OnShutdown with handle {}",
                     static_cast<bool>(Components()) ? "open" : "closed");
        }

        // Extras AFTER the base's registry blob: the pulse entity id, so
        // OnLoadState can prove the base restored the entity it re-finds by view.
        void OnSaveState(Astra::BinaryWriter& w) override
        {
            w(static_cast<uint64_t>(pulse));
        }
        bool OnLoadState(Astra::BinaryReader& r) override
        {
            uint64_t saved = 0; r(saved);
            CacheHandle();
            return !r.HasError() && static_cast<uint64_t>(pulse) == saved;
        }
    };
}

ARCANE_GAME_MODULE_ABI(Arcane::HotReloadTest::Module,
                       ::Arcane::kGamePluginABIVersion + (HOTRELOAD_ABI_OFFSET))
