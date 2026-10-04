// Minimal hot-reload test plugin. Built into four DLLs from this one source:
//   HotReloadPluginV1       -> HOTRELOAD_STEP=1,  ABI = kGamePluginABIVersion
//   HotReloadPluginV2       -> HOTRELOAD_STEP=10, ABI = kGamePluginABIVersion
//   HotReloadPluginBad      -> ABI = kGamePluginABIVersion + 999 (forces rollback)
//   HotReloadPluginInitFail -> HOTRELOAD_INIT_FAIL: automatic registration and
//        OnInit's manual registration both add factories before OnInit returns
//        false. The ABI is fine and the image loads
//        cleanly, so this is the one failure shape that gets as far as running a
//        module's registrations before the host has to unwind them -- the fixture
//        for PluginHost's secondary-init-failure teardown (final-review fix wave,
//        C1: the factories must be ClearOwner'ed before the image unmaps).
// Built ON the SDK's ARC_GAME_MODULE (Arcane/Plugin/GameModule.hpp), so the
// [hotreload] suite is the macro's plugin test: the prologue (Pulse arrives
// through the ARC_COMPONENT drain), the base Save/LoadState round-trip plus
// this module's extras, the Shutdown order (the OnShutdown log line below),
// and the ABI-override seam the Bad build exists to trip.

#include "HotReloadShared.hpp"

#include <Arcane/Plugin/GameModule.hpp>
#include <Arcane/Config/CVarDecl.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cstdint>
#include <string>

#ifndef HOTRELOAD_STEP
  #define HOTRELOAD_STEP 1
#endif
#ifndef HOTRELOAD_ABI_OFFSET
  #define HOTRELOAD_ABI_OFFSET 0
#endif

// One reflected component (shared header), registered through the drain the
// macro's Init performs -- the same path a wizard-made component takes.
ARC_COMPONENT(Arcane::HotReloadTest::Pulse)
ARC_COMPONENT(Arcane::HotReloadTest::RoleCounters)

// Deliberately pair the automatic path with ClientOnlyTick's manual OnInit
// path below. The same DLL therefore proves that both produce owned factories
// with identical role, reload, and teardown behavior.
ARC_SYSTEM(Arcane::HotReloadTest::ServerOnlyTick,
              Arcane::RoleMask::Server,
              Arcane::SystemPhase::FixedUpdate)

// The cvar-lifetime probes (settings spec s4.4; CVarModuleLifetimeTest):
//   - one Archive cvar whose default is this build's step;
//   - one command that answers with that step;
//   - one callback into this image on an ENGINE cvar.
// After an unload or a reload, a stale function pointer would answer with the
// OLD step, or call into unmapped code.
ARC_CVAR(cvar_hotReloadStep, "hotreload.step", std::int32_t, HOTRELOAD_STEP,
         .flags = ::Arcane::CVarFlags::Archive,
         .help = "The hot-reload fixture's build step (1 = V1, 10 = V2).");

namespace
{
    ::Arcane::CommandResult PingCommand(std::string_view, void*)
    {
        return { true, "step " + std::to_string(HOTRELOAD_STEP) };
    }

    void OnHistorySizeChanged(::Arcane::CVarHandle, void*)
    {
        ARC_INFO("HotReloadPlugin: console.historySize changed (step {})", HOTRELOAD_STEP);
    }
}

ARC_COMMAND("hotreload.ping", ::Arcane::CVarFlags::None, "Answers with the fixture's build step.", &PingCommand);

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
            {
                Registry().CreateEntityWith(Pulse{0});   // fresh boot only
#ifndef HOTRELOAD_INIT_FAIL
                // The use-after-unload probe (HotReloadShared.hpp). This is the FIRST
                // resolve of ProbeResource in the process -- the identity Astra records
                // for it is this DLL's. The [hotreload][typecontext] case then resolves
                // it from the exe AFTER this image unmaps. Excluded from the INIT_FAIL
                // build so that fixture's behaviour is unchanged (it exists for the
                // factory-teardown path and must not become a first registrar here).
                Registry().SetResource<ProbeResource>(ProbeResource{42});
#endif
            }
            CacheHandle();
            // The cross-module callback probe: PluginHost's CVarModuleScope
            // around Init tags it with this module, and the unload must drop it.
            ::Arcane::CVarRegistry::Get().AddCallback(::Arcane::CVarRegistry::Get().Find("console.historySize"),
                                                      &OnHistorySizeChanged, nullptr);
            // The s4 contract: factories register ONCE per DLL load, with an
            // explicit mask; each Runtime instantiates what its NetMode matches.
            // ServerOnlyTick arrived through ARC_SYSTEM before OnInit;
            // ClientOnlyTick stays manual as the constructor-aware control path.
            RegisterSystem<ClientOnlyTick>(Arcane::RoleMask::Client, Arcane::SystemPhase::FixedUpdate);
#ifdef HOTRELOAD_INIT_FAIL
            // AFTER both registrations, deliberately: this build exists to leave
            // entries in the process-lifetime SystemFactoryTable that point into an
            // image the host is about to unmap. See the header comment.
            ARC_INFO("HotReloadPlugin: OnInit refusing on purpose (HOTRELOAD_INIT_FAIL)");
            return false;
#else
            return true;
#endif
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
            // The path discriminator (HotReloadShared.hpp): count this run in a
            // transient resource no registry snapshot can carry.
            const LoadStateMarker* marker = Registry().GetResource<LoadStateMarker>();
            Registry().SetResource<LoadStateMarker>(LoadStateMarker{marker ? marker->loads + 1 : 1});
            return !r.HasError() && static_cast<uint64_t>(pulse) == saved;
        }
    };
}

ARC_GAME_MODULE_ABI(Arcane::HotReloadTest::Module,
                       ::Arcane::kGamePluginABIVersion + (HOTRELOAD_ABI_OFFSET))
