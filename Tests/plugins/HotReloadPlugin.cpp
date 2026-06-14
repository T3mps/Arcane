// Minimal hot-reload test plugin. Built into three DLLs from this one source:
//   HotReloadPluginV1  -> HOTRELOAD_STEP=1,  ABI = kGamePluginABIVersion
//   HotReloadPluginV2  -> HOTRELOAD_STEP=10, ABI = kGamePluginABIVersion
//   HotReloadPluginBad -> ABI = kGamePluginABIVersion + 999 (forces rollback)
// One reflected component (Pulse, shared header) + the seven entry points.
// SaveState/LoadState wrap the whole-registry snapshot via the engine Runtime.

#include "PluginExport.hpp"
#include "HotReloadShared.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginABI.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/BinaryReader.hpp>

#include <cstddef>
#include <vector>

#ifndef HOTRELOAD_STEP
  #define HOTRELOAD_STEP 1
#endif
#ifndef HOTRELOAD_ABI_OFFSET
  #define HOTRELOAD_ABI_OFFSET 0
#endif

using Arcane::HotReloadTest::Pulse;

namespace
{
    Arcane::EngineContext* g_ctx = nullptr;
    Astra::Entity          g_pulse{};

    void CacheHandle(Astra::Registry& reg)
    {
        g_pulse = {};
        reg.CreateView<Pulse>().ForEach([&](Astra::Entity e, Pulse&) { g_pulse = e; });
    }
}

extern "C"
{
    GAME_API uint32_t GamePlugin_ABIVersion()
    {
        return Arcane::kGamePluginABIVersion + (HOTRELOAD_ABI_OFFSET);
    }

    GAME_API bool GamePlugin_Init(Arcane::EngineContext* ctx)
    {
        Astra::SetTypeContext(ctx->typeContext);          // 1. shared context in THIS module
        g_ctx = ctx;
        ctx->engine->Components()->ReRegisterComponent<Pulse>();   // 2. descriptors -> this module

        Astra::Registry& reg = ctx->engine->Registry();
        bool exists = false;
        reg.CreateView<Pulse>().ForEach([&](Astra::Entity, Pulse&) { exists = true; });
        if (!exists)
            reg.CreateEntityWith(Pulse{0});               // fresh boot only
        CacheHandle(reg);
        return true;
    }

    GAME_API void GamePlugin_Shutdown() { g_pulse = {}; }

    GAME_API void GamePlugin_FixedUpdate(double)
    {
        if (auto* p = g_ctx->engine->Registry().GetComponent<Pulse>(g_pulse))
            p->ticks += (HOTRELOAD_STEP);                 // V1: +1, V2: +10 (observably different code)
    }

    GAME_API void GamePlugin_Update(double, double) {}

    GAME_API void GamePlugin_SaveState(Astra::BinaryWriter& w)
    {
        const std::vector<std::byte> blob = g_ctx->engine->SnapshotRegistry();
        w(static_cast<uint64_t>(blob.size()));
        w.WriteBytes(blob.data(), blob.size());
    }

    GAME_API bool GamePlugin_LoadState(Astra::BinaryReader& r)
    {
        uint64_t n = 0; r(n);
        std::vector<std::byte> blob(static_cast<size_t>(n));
        r.ReadBytes(blob.data(), static_cast<size_t>(n));
        if (r.HasError()) return false;
        if (!g_ctx->engine->RestoreRegistry(blob)) return false;
        CacheHandle(g_ctx->engine->Registry());
        return true;
    }
}
