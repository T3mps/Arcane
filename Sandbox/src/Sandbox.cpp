// Sandbox: a hot-reloadable physics-sandbox plugin behind the v2 game ABI. Task 4 is the
// skeleton + ONE scene (a "Playground": static floor + 2 walls + 5 dynamic bodies) running
// the REAL engine stack: PhysicsSystem drives the v2 PhysicsWorld in fixedUpdate,
// TransformPropagationSystem derives WorldTransform, RenderSubmissionSystem submits sprites
// in the render phase, and PhysicsDebugRenderSystem overlays the collider/contacts via
// DrawPhysicsDebug. Mirrors PlaygroundGame.cpp's structure (the reference v2 plugin).
//
// The engine scene components/systems (Arcane/Scene/*) are header-only and instantiate in
// THIS module; the engine Registry/schedulers/RunLoop live in Arcane.dll via the Runtime.

#include "GameApi.hpp"
#include "SandboxApp.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>      // PhysicsSystem + PhysicsResource
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Arcane/Physics/PhysicsWorld.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/BinaryReader.hpp>

#include <imgui.h>   // ABI v2: adopt the host's ImGui context/allocators (imported from Arcane.dll)

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace
{
    // Fixed timestep: the 60 Hz RunLoop ticks PhysicsSystem at this dt every step.
    // Determinism contract (PhysicsSystem): constant per run.
    constexpr float kFixedDt = 1.0f / 60.0f;

    // Gravity for the sandbox world (+Y down, world unit == canvas px). Tuned higher
    // than the physics-test default so bodies fall at a brisk, readable rate on screen.
    constexpr float kGravityY = 900.0f;

    Arcane::EngineContext*       g_ctx = nullptr;
    Arcane::Sandbox::SandboxApp  g_app{};
    Astra::Entity                g_root{};

    // Install the PhysicsResource (the PhysicsWorld + entity<->body map) on the registry
    // if it isn't already present. PhysicsSystem reads this resource each fixed step.
    void EnsurePhysicsResource(Astra::Registry& reg)
    {
        if (reg.GetResource<Arcane::PhysicsResource>()) return;
        Arcane::Physics::WorldDef wd;
        wd.gravityY = kGravityY;
        reg.SetResource(Arcane::PhysicsResource{
            std::make_unique<Arcane::Physics::PhysicsWorld>(wd),
            {}
        });
    }

    // Cache the SceneRoot entity from the resource (set by the scene builder or restored
    // by LoadState). Mirrors PlaygroundGame's CacheHandles.
    void CacheRoot(Astra::Registry& reg)
    {
        g_root = {};
        if (auto* sr = reg.GetResource<Arcane::SceneRoot>())
            g_root = sr->entity;
    }
}

extern "C"
{
    GAME_API uint32_t GamePlugin_ABIVersion() { return Arcane::kGamePluginABIVersion; }

    GAME_API bool GamePlugin_Init(Arcane::EngineContext* ctx)
    {
        Astra::SetTypeContext(ctx->typeContext);   // 1. shared context in THIS module
        g_ctx = ctx;

        // ABI v2: adopt the host's ImGui context + allocators so DrawUI (called by the
        // host between ImGuiLayer BeginFrame/Render) draws into the host's single GImGui.
        // Null in a headless host (no ImGuiLayer) -> skip; Task 4's DrawUI is a no-op.
        if (ctx->imguiContext)
        {
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx->imguiContext));
            ImGui::SetAllocatorFunctions(
                reinterpret_cast<ImGuiMemAllocFunc>(ctx->imguiAlloc),
                reinterpret_cast<ImGuiMemFreeFunc>(ctx->imguiFree),
                ctx->imguiUserData);
        }

        // 2. Re-register every component descriptor into THIS module so its function
        //    pointers (ctor/dtor/serialize) target the plugin's code, not the previously
        //    loaded image's. Scene + physics components both.
        auto creg = ctx->engine->Components();
        creg->ReRegisterComponent<Arcane::LocalTransform>();
        creg->ReRegisterComponent<Arcane::WorldTransform>();
        creg->ReRegisterComponent<Arcane::SpriteRenderer>();
        creg->ReRegisterComponent<Arcane::RigidBody2D>();
        creg->ReRegisterComponent<Arcane::Collider2D>();
        creg->ReRegisterComponent<Arcane::PhysicsBodyRef>();

        // 3. Register systems (functors in this module).
        //    fixedUpdate: PhysicsSystem steps the world BEFORE propagation derives
        //                 WorldTransform from the updated LocalTransform (the
        //                 Writes<LocalTransform> trait enforces that order).
        //    render: sprites first, then the physics-debug overlay on top.
        auto& sch = ctx->engine->Schedulers();
        sch.fixedUpdate.AddSystem<Arcane::PhysicsSystem>(kFixedDt);
        sch.fixedUpdate.AddSystem<Arcane::TransformPropagationSystem>();
        sch.render.AddSystem<Arcane::RenderSubmissionSystem>();
        sch.render.AddSystem<Arcane::Sandbox::PhysicsDebugRenderSystem>();

        // 4. Build the scene (fresh boot only; a hot-reload-with-state restores it).
        //    BuildInitialScene mints its OWN fresh PhysicsResource (via the clean-rebuild
        //    path it shares with SetScene/Reset), so no pre-EnsurePhysicsResource is needed
        //    on the build path; the LoadState path below still calls EnsurePhysicsResource.
        Astra::Registry& reg = ctx->engine->Registry();
        g_app.Configure(kGravityY);   // gravity for every (re)built PhysicsResource world
        if (!reg.GetResource<Arcane::SceneRoot>())
            g_app.BuildInitialScene(reg);
        CacheRoot(reg);
        return true;
    }

    GAME_API void GamePlugin_Shutdown() { g_root = {}; }

    GAME_API void GamePlugin_FixedUpdate(double dt)
    {
        // Pumps the SceneControl side channel (a pending scene switch/reset rebuilds here,
        // BEFORE the engine fixedUpdate scheduler); PhysicsSystem then does the stepping.
        g_app.FixedUpdate(g_ctx->engine->Registry(), dt);
    }

    GAME_API void GamePlugin_Update(double dt, double alpha)
    {
        g_app.Update(dt, alpha);

        // Push the plugin-owned camera to the engine BEFORE render: SetRenderContext
        // (called by the host after this Update phase) writes it into RenderContext2D,
        // so RenderSubmissionSystem + DrawPhysicsDebug apply the SAME world*zoom+offset
        // transform (sprites + debug overlay pan/zoom together). Loom stays camera-agnostic.
        const Arcane::Sandbox::Camera& cam = g_app.Cam();
        g_ctx->engine->SetCamera(cam.offset, cam.zoom);
    }

    // ABI v2: the host calls this between ImGuiLayer BeginFrame and Render. Task 4 draws
    // no UI -- a no-op that proves the entry point is exported and callable. HUD is Task 8.
    GAME_API void GamePlugin_DrawUI() {}

    GAME_API void GamePlugin_SaveState(Astra::BinaryWriter& w)
    {
        // Persist the root entity ID explicitly -- resources (SceneRoot, PhysicsResource)
        // are not included in the registry binary snapshot. PhysicsResource is transient
        // runtime state re-established by PhysicsSystem after restore; SceneRoot is
        // re-set from this saved entity ID in LoadState.
        const uint64_t rootRaw = static_cast<uint64_t>(g_root);
        w(rootRaw);

        const std::vector<std::byte> blob = g_ctx->engine->SnapshotRegistry();
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

        // Resources are not serialized in the registry snapshot. Re-establish them:
        // SceneRoot from the saved entity ID, PhysicsResource fresh (PhysicsSystem
        // rebuilds every body from RigidBody2D + Collider2D + PhysicsBodyRef on the next
        // fixedUpdate, exactly as WorldTransform is re-derived by propagation).
        Astra::Registry& reg = g_ctx->engine->Registry();
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{savedRoot});
        EnsurePhysicsResource(reg);
        CacheRoot(reg);
        return true;
    }
}
