#pragma once

// SandboxApp: owns the sandbox's per-run state (the current scene index) and the
// scene-build entry point. Task 4 is the skeleton: one scene, no camera, no HUD, no
// interaction (those are Tasks 5-8). The heavy lifting -- physics stepping, transform
// propagation, sprite + debug rendering -- is done by the engine systems the plugin
// registers in Init; SandboxApp just builds the initial scene and holds the index.
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

namespace Arcane::Sandbox
{
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
        // Build scene 0 into `reg`. Called by GamePlugin_Init on a fresh boot (after the
        // components + PhysicsResource are registered). The scene index is recorded so
        // later tasks can switch scenes without re-querying the registry.
        void BuildInitialScene(Astra::Registry& reg);

        // Per-frame hooks. Task 4: no per-frame mutation -- the engine systems do the
        // work (PhysicsSystem steps the world in fixedUpdate; the render systems submit
        // in the render phase). Kept as explicit no-ops so the wiring is visible and the
        // later tasks have an obvious seam.
        void FixedUpdate(double dt);
        void Update(double dt, double alpha);

        std::size_t CurrentScene() const noexcept { return m_sceneIndex; }

    private:
        std::size_t m_sceneIndex = 0;
    };
}
