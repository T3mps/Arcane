#pragma once

// SandboxApp: owns the sandbox's per-run state (the current scene index) and the
// scene lifecycle -- build the initial scene, switch to another scene, and reset (rebuild)
// the current scene. The heavy lifting -- physics stepping, transform propagation, sprite +
// debug rendering -- is done by the engine systems the plugin registers in Init; SandboxApp
// just owns the scene roster index + the clean-rebuild path.
//
// SCENE SWITCH / RESET (Task 5):
//   Switching scenes must START CLEAN. The cleanest reset given the sandbox owns the entire
//   registry (every entity is scene content under SceneRoot) is:
//     1. reg.Clear()            -- destroy ALL entities (Clear leaves resources untouched).
//     2. replace PhysicsResource -- a FRESH PhysicsWorld + empty entity<->body map, so the
//                                   old world's bodies cannot leak into the new scene.
//     3. run the new scene builder -- it repopulates entities and re-sets SceneRoot.
//   Reset() rebuilds the CURRENT index the same way. Both clamp/wrap the index into
//   [0, SceneRegistry().size()). The actual UI/key that calls these is a later task (HUD =
//   Task 8); here they exist and are exercised by the smoke test through the SceneControl
//   side channel below.
//
// Also defines PhysicsDebugRenderSystem: a render-phase Astra system that pulls the live
// Batcher2D from the RenderContext2D resource (set by the host each frame via
// Runtime::SetRenderContext) and the PhysicsWorld from the PhysicsResource, then calls
// DrawPhysicsDebug to overlay collider outlines + contacts. It runs in the same render
// scheduler as RenderSubmissionSystem (registered AFTER it, so the overlay draws on top),
// which is exactly how the render phase is driven -- no bespoke render path.

#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>   // PhysicsResource (owns the PhysicsWorld)
#include <Arcane/Scene/SceneResources.hpp>  // RenderContext2D (holds the live batcher)

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <cstddef>
#include <cstdint>

namespace Arcane::Sandbox
{
    // -------------------------------------------------------------------------
    // SceneControl: the sandbox-private scene-switch side channel (Registry resource).
    // -------------------------------------------------------------------------
    // Lives as a Registry resource (NOT part of the engine PluginVTable ABI -- the 7-entry
    // vtable is untouched). The plugin publishes sceneCount + currentScene; an external
    // driver (the HUD in Task 8, the smoke test today) sets requestedScene / requestReset
    // and the plugin consumes the request in GamePlugin_FixedUpdate (which RunLoop runs
    // BEFORE the engine fixedUpdate scheduler, so the rebuild lands on a clean registry,
    // never mid-PhysicsSystem).
    //
    // POD, <= 64 bytes => ResourceStorage keeps it inline (SBO). Identified purely by its
    // fully-qualified type name (Astra cross-module type identity), so the smoke test can
    // mirror an identically-named struct in its own module and share the same slot.
    struct SceneControl
    {
        std::int32_t sceneCount     = 0;   // plugin -> driver: SceneRegistry().size()
        std::int32_t currentScene   = 0;   // plugin -> driver: index after the last (re)build
        std::int32_t requestedScene = -1;  // driver -> plugin: switch to this index (-1 = none)
        std::int32_t requestReset   = 0;   // driver -> plugin: rebuild current scene (0/1)
    };

    // Render-phase overlay: collider outlines + contacts on top of the sprites.
    // Reads-only w.r.t. ECS (the side effect is the external batcher submission),
    // matching RenderSubmissionSystem. No SystemTraits component dependencies are
    // declared because it touches no component storage -- it reads two resources and
    // submits to the batcher. (An empty-traits system runs single-threaded, which is
    // required: Batcher2D is not thread-safe, same as RenderSubmissionSystem.)
    struct PhysicsDebugRenderSystem
    {
        void operator()(Astra::Registry& reg)
        {
            RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
            if (!ctx || !ctx->batcher) return;

            PhysicsResource* phys = reg.GetResource<PhysicsResource>();
            if (!phys || !phys->world) return;

            PhysicsDebugDrawOptions opts;
            opts.cameraOffset = ctx->cameraOffset;
            opts.drawContacts = true;
            DrawPhysicsDebug(*phys->world, *ctx->batcher, opts);
        }
    };

    // Owns the sandbox run state. One per loaded plugin image (file-local g_app).
    class SandboxApp
    {
    public:
        // Record the gravity the (re)built PhysicsResource worlds use. Called once by the
        // plugin in Init before BuildInitialScene so SetScene/Reset can mint fresh worlds.
        void Configure(float gravityY) noexcept { m_gravityY = gravityY; }

        // Build the current scene index into `reg` from scratch (fresh PhysicsResource +
        // builder). Called by GamePlugin_Init on a fresh boot (after the components are
        // registered). The scene index is recorded so SetScene/Reset can switch later.
        void BuildInitialScene(Astra::Registry& reg);

        // Switch to scene `index` (clamped/wrapped into [0, SceneRegistry().size())):
        // destroy all current entities + recreate a FRESH PhysicsResource, then run the
        // target builder. The old world's bodies do NOT leak into the new scene.
        void SetScene(Astra::Registry& reg, std::uint32_t index);

        // Rebuild the CURRENT scene index from scratch (same clean path as SetScene).
        void Reset(Astra::Registry& reg);

        // Per-frame hooks. The engine systems do the work (PhysicsSystem steps the world in
        // fixedUpdate; the render systems submit in the render phase). FixedUpdate also
        // pumps the SceneControl side channel: a pending reset/switch is applied here, BEFORE
        // the engine fixedUpdate scheduler runs, so the rebuild is on a clean registry.
        void FixedUpdate(Astra::Registry& reg, double dt);
        void Update(double dt, double alpha);

        std::size_t CurrentScene() const noexcept { return m_sceneIndex; }

    private:
        // Tear the registry down to bare resources + mint a fresh PhysicsResource, then run
        // the scene `index` builder. Shared by BuildInitialScene/SetScene/Reset.
        void RebuildScene(Astra::Registry& reg, std::size_t index);

        // Publish sceneCount + currentScene into the SceneControl resource (idempotent;
        // creates it if absent). Called after every (re)build.
        void PublishControl(Astra::Registry& reg) const;

        std::size_t m_sceneIndex = 0;
        float       m_gravityY   = 0.0f;
    };
}
