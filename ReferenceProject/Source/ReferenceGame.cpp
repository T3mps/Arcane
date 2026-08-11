// ReferenceGame: ReferenceProject's game module -- the minimal end-to-end proof of
// the product flow. The SCENE is data (Content/scenes/main.arcscene, loaded by
// the host through the manifest's bootScene); this module's whole job is the
// plugin lifecycle: pin the shared TypeContext, register the engine systems
// that make a data scene tick and render, and round-trip the registry for
// hot reload. It owns no component types (the engine roster is registered by
// Runtime's ctor; a module that DID own types would register them through its
// own RAII Astra::ComponentModule -- see HotReloadPlugin.cpp for that shape).

#include "GameApi.hpp"

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/BinaryReader.hpp>

#include <imgui.h>   // ABI v2: adopt the host's ImGui context/allocators (imported from ArcaneClient.dll)

#include <cstdint>
#include <tuple>
#include <vector>

namespace
{
    Arcane::EngineContext* g_ctx = nullptr;
}

extern "C"
{
    GAME_API uint32_t GamePlugin_ABIVersion() { return Arcane::kGamePluginABIVersion; }

    GAME_API bool GamePlugin_Init(Arcane::EngineContext* ctx)
    {
        Astra::SetTypeContext(ctx->typeContext);   // 1. shared context in THIS module
        Arcane::Log::InstallMosaicSink();
        Arcane::Assert::InstallMosaicHandler();
        g_ctx = ctx;

        // ABI v2: adopt the host's ImGui context + allocators so DrawUI draws into
        // the host's single GImGui. Null in a headless host -> skip.
        if (ctx->imguiContext)
        {
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx->imguiContext));
            ImGui::SetAllocatorFunctions(
                reinterpret_cast<ImGuiMemAllocFunc>(ctx->imguiAlloc),
                reinterpret_cast<ImGuiMemFreeFunc>(ctx->imguiFree),
                ctx->imguiUserData);
        }

        // 2. Register the engine systems (functors instantiate in THIS module):
        // propagation makes the scene's parent/child transforms real each fixed
        // step, submission draws it. The boot scene itself is loaded by the HOST
        // (HostBoot::BootScene, after this Init returns) -- nothing here builds
        // entities in code.
        auto& sch = ctx->engine->Schedulers();
        std::ignore = sch.fixedUpdate.AddSystem<Arcane::TransformPropagationSystem>();
        std::ignore = sch.render.AddSystem<Arcane::RenderSubmissionSystem>();
        return true;
    }

    GAME_API void GamePlugin_Shutdown() { g_ctx = nullptr; }

    GAME_API void GamePlugin_FixedUpdate(double) {}

    GAME_API void GamePlugin_Update(double, double) {}

    // Called between the host's ImGuiLayer BeginFrame and Render. No UI yet --
    // a no-op that proves the entry point is exported and callable.
    GAME_API void GamePlugin_DrawUI() {}

    GAME_API void GamePlugin_SaveState(Astra::BinaryWriter& w)
    {
        // Persist the scene-root entity id explicitly -- resources are not part
        // of the registry snapshot, so LoadState must re-set SceneRoot after the
        // restore. Resolved lazily from the live resource: the boot scene loads
        // AFTER Init, so no cached handle exists to trust here.
        Astra::Registry& reg = g_ctx->engine->Registry();
        const auto* sr = reg.GetResource<Arcane::SceneRoot>();
        const uint64_t rootRaw =
            sr ? static_cast<uint64_t>(sr->entity) : static_cast<uint64_t>(Astra::Entity::Invalid());
        w(rootRaw);

        auto snap = g_ctx->engine->SnapshotRegistry();
        if (snap.IsErr())
        {
            // Snapshot failed: write a zero-length blob so LoadState fails cleanly
            // (RestoreRegistry rejects an empty frame) instead of masking the loss.
            w(static_cast<uint64_t>(0));
            return;
        }
        const std::vector<std::byte>& blob = *snap.GetValue();
        w(static_cast<uint64_t>(blob.size()));
        w.WriteBytes(blob.data(), blob.size());
    }

    GAME_API bool GamePlugin_LoadState(Astra::BinaryReader& r)
    {
        uint64_t rootRaw = 0; r(rootRaw);
        const Astra::Entity savedRoot(static_cast<Astra::Entity::StorageType>(rootRaw));

        uint64_t n = 0; r(n);
        std::vector<std::byte> blob(static_cast<size_t>(n));
        r.ReadBytes(blob.data(), static_cast<size_t>(n));
        if (r.HasError()) return false;
        if (!g_ctx->engine->RestoreRegistry(blob)) return false;

        // SceneRoot is a resource; the snapshot does not carry it. Re-register it
        // from the saved entity id (ids/versions survive Save/Load round-trips).
        if (savedRoot.IsValid())
            g_ctx->engine->Registry().SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{savedRoot});
        return true;
    }
}
