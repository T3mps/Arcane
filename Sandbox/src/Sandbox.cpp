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

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Jobs/ArcaneWorkScheduler.hpp>  // bridges ITaskExecutor -> Mosaic::IWorkScheduler
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>      // PhysicsSystem + PhysicsResource
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>

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
    // Gravity for the sandbox world (+Y down, world unit == METER; MKS). This matches
    // the engine WorldDef default (g = +10 m/s^2); the sandbox keeps the named constant
    // because the HUD gravity slider round-trips through it (Configure(kGravityY)).
    // (The fixed timestep itself is owned by SandboxApp now -- Task 8's sandbox-owned
    // physics step scales it by the HUD time-scale; see SandboxApp::FixedUpdate.)
    constexpr float kGravityY = 10.0f;

    Arcane::EngineContext*       g_ctx = nullptr;
    Arcane::Sandbox::SandboxApp  g_app{};
    Astra::Entity                g_root{};

    // Persistent IWorkScheduler bridge for the engine executor: outlives each
    // world minted below (the world stores a raw scheduler pointer). Re-pointed
    // per-call from g_ctx->taskExecutor (null -> the world's serial default).
    Arcane::ArcaneWorkScheduler  g_scheduler{};

    // Install the PhysicsResource (the PhysicsWorld + entity<->body map) on the registry
    // if it isn't already present. PhysicsSystem reads this resource each fixed step.
    void EnsurePhysicsResource(Astra::Registry& reg)
    {
        // Astra snapshot format v2 round-trips resources: RestoreRegistry now reinstalls a
        // (no-op-serialized) world-less PhysicsResource shell, so a bare presence check would
        // skip re-establishing the world after a Stop. Rebuild whenever the world is missing.
        if (auto* res = reg.GetResource<Arcane::PhysicsResource>(); res && res->world) return;
        Manifold2D::Physics::WorldDef wd;
        wd.gravityY = kGravityY;   // MKS: +10 m/s^2 (the WorldDef default; the rest inherit it)
        auto world = std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd);
        // Phase D1: wire executor (g_ctx is always valid here; LoadState is called after Init).
        g_scheduler.SetExecutor(g_ctx ? g_ctx->taskExecutor : nullptr);
        world->SetExecutor(&g_scheduler);
        reg.SetResource(Arcane::PhysicsResource{ std::move(world), {} });
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
        Arcane::Log::InstallMosaicSink();
        Arcane::Assert::InstallMosaicHandler();
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
        creg->ReRegisterComponent<Arcane::Transform>();
        creg->ReRegisterComponent<Arcane::WorldTransform>();
        creg->ReRegisterComponent<Arcane::PreviousTransform>();
        creg->ReRegisterComponent<Arcane::SpriteRenderer>();
        creg->ReRegisterComponent<Arcane::PostProcess>();
        creg->ReRegisterComponent<Arcane::RigidBody2D>();
        creg->ReRegisterComponent<Arcane::Collider2D>();
        creg->ReRegisterComponent<Arcane::PhysicsBodyRef>();

        // 3. Register systems (functors in this module).
        //    fixedUpdate: EMPTY of engine systems. The PHYSICS STEP is the sandbox's
        //                 RunLoop plugin-fixed hook (SandboxApp::FixedUpdate); RunLoop now
        //                 owns pause/single-step/time-scale and GATES this whole phase, so
        //                 nothing that must run while paused may live here.
        //    update:      TransformPropagationSystem. Propagation runs once per frame -- the
        //                 update phase always runs, even paused -- AFTER the fixed steps, so
        //                 both a stepped body and a body authored while frozen get their
        //                 Transform -> WorldTransform derived for the render phase.
        //    render: sprites first, then the physics-debug overlay on top.
        auto& sch = ctx->engine->Schedulers();
        sch.update.AddSystem<Arcane::TransformPropagationSystem>();
        sch.render.AddSystem<Arcane::RenderSubmissionSystem>();
        sch.render.AddSystem<Arcane::Sandbox::PhysicsDebugRenderSystem>();
        sch.render.AddSystem<Arcane::Sandbox::PolygonDraftRenderSystem>();

        // 4. Build the scene (fresh boot only; a hot-reload-with-state restores it).
        //    BuildInitialScene mints its OWN fresh PhysicsResource (via the clean-rebuild
        //    path it shares with SetScene/Reset), so no pre-EnsurePhysicsResource is needed
        //    on the build path; the LoadState path below still calls EnsurePhysicsResource.
        Astra::Registry& reg = ctx->engine->Registry();
        g_app.Configure(kGravityY);   // gravity for every (re)built PhysicsResource world
        // Phase D1: wire the real task executor so each (re)built PhysicsWorld is ready
        // to parallelize the solver once later tasks enable it (null -> serial default).
        g_app.SetExecutor(ctx ? ctx->taskExecutor : nullptr);
        // Slice B: hand the Runtime to the app so the narrowphase inspector can build its
        // Minkowski-inset OffscreenCanvas from the host's device + ShaderLibrary (Runtime
        // render-resources bridge). Null device in a headless host -> the inset is skipped.
        g_app.SetRuntime(ctx->engine);
        // Epic 04: forward the HUD's pause/single-step/time-scale to the engine RunLoop
        // (the one clock that gates the fixed phase). The app holds this pointer and
        // delegates its sim-time methods to it.
        g_app.SetLoop(&ctx->engine->Loop());
        if (!reg.GetResource<Arcane::SceneRoot>())
            g_app.BuildInitialScene(reg);
        CacheRoot(reg);
        return true;
    }

    GAME_API void GamePlugin_Shutdown() { g_root = {}; }

    GAME_API void GamePlugin_FixedUpdate(double dt)
    {
        // The pause-gated sim step. RunLoop (which owns pause/single-step/time-scale) calls
        // this ONLY for the fixed steps that actually run; SandboxApp::FixedUpdate just drives
        // the PhysicsSystem step at the canonical dt. All per-frame authoring/interaction is
        // in Update. Input is unused here now (interaction moved to Update) but kept in the
        // signature for the hook contract.
        g_app.FixedUpdate(g_ctx->engine->Registry(), dt, g_ctx->engine->Input());
    }

    GAME_API void GamePlugin_Update(double dt, double alpha)
    {
        // The every-frame hook (runs even while paused): the SceneControl pump, the Task-7
        // mouse-interaction layer (spawn/drag/throw + pan/zoom), the inspector, polygon
        // authoring, a mint-only pass while frozen, and wheel-zoom -- everything that must
        // keep working while the sim is paused. Input() is the per-host-frame snapshot.
        g_app.Update(g_ctx->engine->Registry(), dt, alpha, g_ctx->engine->Input());

        // Push the plugin-owned camera to the engine BEFORE render: SetRenderContext
        // (called by the host after this Update phase) writes it into RenderContext2D,
        // so RenderSubmissionSystem + DrawPhysicsDebug apply the SAME world*scale+offset
        // transform (sprites + debug overlay pan/zoom together). Loom stays camera-agnostic.
        //
        // We push WorldToScreenScale() (== kPixelsPerMeter * zoom), NOT the raw zoom: the
        // engine's `zoom` field means world->SCREEN scale (it is unit-agnostic), so the
        // pixelsPerMeter fold must ride in here or the render path draws meter content at
        // 1 px/m -- a thumbnail -- while the mouse (which goes through the full ppm camera)
        // disagrees with the visual by 100x. This is the single seam that folds ppm into
        // the render path; the mouse path folds it via WorldToScreen/ScreenToWorld.
        const Arcane::Sandbox::Camera& cam = g_app.Cam();
        g_ctx->engine->SetCamera(cam.offset, cam.WorldToScreenScale());
    }

    // ABI v2: the host calls this between ImGuiLayer BeginFrame and Render. Task 8 draws
    // the Sandbox control panel (scene / sim / spawn / debug / stats) via Hud::Draw. The
    // plugin adopted the host's ImGui context in Init, so these ImGui:: calls land in the
    // host's single GImGui. Guarded by a current context so a headless host (no ImGuiLayer,
    // null imguiContext in Init) skips the draw instead of asserting.
    GAME_API void GamePlugin_DrawUI()
    {
        if (!ImGui::GetCurrentContext()) return;   // headless host: no UI surface
        if (!g_ctx) return;
        g_app.DrawUI(g_ctx->engine->Registry());
    }

    GAME_API void GamePlugin_SaveState(Astra::BinaryWriter& w)
    {
        // Persist the root entity ID explicitly -- resources (SceneRoot, PhysicsResource)
        // are not included in the registry binary snapshot. PhysicsResource is transient
        // runtime state re-established by PhysicsSystem after restore; SceneRoot is
        // re-set from this saved entity ID in LoadState.
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
