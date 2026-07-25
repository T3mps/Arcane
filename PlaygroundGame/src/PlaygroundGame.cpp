// PlaygroundGame: the M4 parent/child sprite scene behind the plugin ABI. The engine
// scene components/systems (Arcane/Scene/*) are header-only and instantiate in THIS
// module; the engine Registry/schedulers/RunLoop live in Arcane.dll via the Runtime.

#include "GameApi.hpp"

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/BinaryReader.hpp>

#include <glm/glm.hpp>
#include <imgui.h>   // ABI v2: adopt the host's ImGui context/allocators (imported from Arcane.dll)

#include <cstddef>
#include <vector>

namespace
{
    Arcane::EngineContext* g_ctx = nullptr;
    Astra::Entity          g_root{};
    Astra::Entity          g_orbiter{};
    Astra::Entity          g_moon{};
    double                 g_time = 0.0;

    void BuildScene(Astra::Registry& reg)
    {
        Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
        reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});

        Astra::Entity orbiter = reg.CreateEntity();
        Arcane::Transform ot; ot.position = glm::vec2(640.0f, 360.0f);
        reg.AddComponent<Arcane::Transform>(orbiter, ot);
        reg.AddComponent<Arcane::WorldTransform>(orbiter, Arcane::WorldTransform{});
        Arcane::SpriteRenderer os; os.size = glm::vec2(48.0f); os.tint = glm::vec4(0.9f, 0.7f, 0.2f, 1.0f);
        reg.AddComponent<Arcane::SpriteRenderer>(orbiter, os);
        reg.SetParent(orbiter, root);

        Astra::Entity moon = reg.CreateEntity();
        Arcane::Transform mt; mt.position = glm::vec2(80.0f, 0.0f);
        reg.AddComponent<Arcane::Transform>(moon, mt);
        reg.AddComponent<Arcane::WorldTransform>(moon, Arcane::WorldTransform{});
        Arcane::SpriteRenderer ms; ms.size = glm::vec2(20.0f); ms.tint = glm::vec4(0.4f, 0.8f, 1.0f, 1.0f);
        reg.AddComponent<Arcane::SpriteRenderer>(moon, ms);
        reg.SetParent(moon, orbiter);

        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    }

    // Rebuild the SceneRoot resource from the root entity and cache all handles.
    // Called after BuildScene (resource already set) or after RestoreRegistry
    // (resource was not serialized; caller supplies the root entity ID).
    void CacheHandlesFrom(Astra::Registry& reg, Astra::Entity root)
    {
        g_root = root; g_orbiter = {}; g_moon = {};
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
        reg.GetRelations(g_root).ForEachDescendant([&](Astra::Entity e, size_t depth) {
            if (depth == 1) g_orbiter = e;
            else if (depth == 2) g_moon = e;
        });
    }

    void CacheHandles(Astra::Registry& reg)
    {
        g_root = {}; g_orbiter = {}; g_moon = {};
        if (auto* sr = reg.GetResource<Arcane::SceneRoot>())
            CacheHandlesFrom(reg, sr->entity);
    }
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

        // ABI v2: adopt the host's ImGui context + allocators so DrawUI (called by the
        // host between ImGuiLayer BeginFrame/Render) draws into the host's single GImGui.
        // Null in a headless host (no ImGuiLayer) -> skip; PlaygroundGame's DrawUI is a no-op.
        if (ctx->imguiContext)
        {
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx->imguiContext));
            ImGui::SetAllocatorFunctions(
                reinterpret_cast<ImGuiMemAllocFunc>(ctx->imguiAlloc),
                reinterpret_cast<ImGuiMemFreeFunc>(ctx->imguiFree),
                ctx->imguiUserData);
        }

        auto creg = ctx->engine->Components();
        creg->ReRegisterComponent<Arcane::Transform>();   // 2. descriptors -> this module
        creg->ReRegisterComponent<Arcane::WorldTransform>();
        creg->ReRegisterComponent<Arcane::SpriteRenderer>();
        creg->ReRegisterComponent<Arcane::PostProcess>();

        auto& sch = ctx->engine->Schedulers();                 // 3. register systems (functors in this module)
        sch.fixedUpdate.AddSystem<Arcane::TransformPropagationSystem>();
        sch.render.AddSystem<Arcane::RenderSubmissionSystem>();

        Astra::Registry& reg = ctx->engine->Registry();
        if (!reg.GetResource<Arcane::SceneRoot>())             // build only on a fresh boot
            BuildScene(reg);
        CacheHandles(reg);
        return true;
    }

    GAME_API void GamePlugin_Shutdown() { g_root = {}; g_orbiter = {}; g_moon = {}; }

    GAME_API void GamePlugin_FixedUpdate(double dt)
    {
        g_time += dt;
        if (auto* lt = g_ctx->engine->Registry().GetComponent<Arcane::Transform>(g_orbiter))
            lt->rotation = static_cast<float>(g_time);   // orbit; propagation runs after, in the engine phase
    }

    GAME_API void GamePlugin_Update(double, double) {}

    // ABI v2: the host calls this between ImGuiLayer BeginFrame and Render. The orbit
    // fixture draws no UI -- a no-op that proves the entry point is exported and callable.
    GAME_API void GamePlugin_DrawUI() {}

    GAME_API void GamePlugin_SaveState(Astra::BinaryWriter& w)
    {
        // Persist the root entity ID explicitly -- resources are not included in the
        // registry binary snapshot, so LoadState must re-set SceneRoot after restore.
        const uint64_t rootRaw = static_cast<uint64_t>(g_root);
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

        // SceneRoot is a resource; resources are not serialized in the registry snapshot.
        // Re-register it from the saved entity ID and re-cache all handles.
        CacheHandlesFrom(g_ctx->engine->Registry(), savedRoot);
        g_time = 0.0;   // sceneTime is transient; orbit resumes from the restored rotation
        return true;
    }
}
