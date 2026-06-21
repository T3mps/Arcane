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

#include "Camera.hpp"                       // the sandbox 2D pan + zoom view
#include "Interaction.hpp"                  // mouse spawn/drag/throw + pan/zoom (Task 7)

#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>   // PhysicsResource (owns the PhysicsWorld)
#include <Arcane/Scene/SceneResources.hpp>  // RenderContext2D (holds the live batcher)

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <cstddef>
#include <cstdint>

namespace Arcane { struct InputSnapshot; }

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

    // -------------------------------------------------------------------------
    // SandboxDebugDraw: the HUD-controlled debug-overlay flags (Registry resource).
    // -------------------------------------------------------------------------
    // The HUD (Task 8) toggles the PhysicsDebugDrawOptions flags; PhysicsDebugRenderSystem
    // (a SEPARATE Astra system with no handle to SandboxApp) reads them through this
    // resource each render frame. SandboxApp owns the authoritative copy and publishes
    // it into this resource every (re)build + whenever the HUD edits a flag. POD, small,
    // identified by its fully-qualified type name (Astra cross-module type identity), so
    // a headless test can mirror it if needed. Defaults match the Task-7 overlay
    // (contacts on, AABBs off): a fresh build with no HUD looks exactly as before.
    struct SandboxDebugDraw
    {
        bool  drawContacts  = true;
        bool  drawAabbs     = false;
        float lineThickness = 1.0f;
    };

    // Render-phase overlay: collider outlines + contacts on top of the sprites.
    // Reads-only w.r.t. ECS (the side effect is the external batcher submission),
    // matching RenderSubmissionSystem. No SystemTraits component dependencies are
    // declared because it touches no component storage -- it reads resources and
    // submits to the batcher. (An empty-traits system runs single-threaded, which is
    // required: Batcher2D is not thread-safe, same as RenderSubmissionSystem.)
    //
    // The overlay flags (contacts / AABBs / line thickness) come from the
    // SandboxDebugDraw resource the HUD controls -- NOT hardcoded -- so a HUD
    // checkbox toggle reaches DrawPhysicsDebug. The resource may be absent (a
    // headless host that never built one); then the Task-7 defaults apply.
    struct PhysicsDebugRenderSystem
    {
        void operator()(Astra::Registry& reg)
        {
            RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
            if (!ctx || !ctx->batcher) return;

            PhysicsResource* phys = reg.GetResource<PhysicsResource>();
            if (!phys || !phys->world) return;

            PhysicsDebugDrawOptions opts;
            opts.cameraOffset = ctx->cameraOffset;   // same camera as the sprites...
            opts.zoom         = ctx->zoom;            // ...so the overlay lines up under pan + zoom.

            // HUD-controlled flags (default to the Task-7 overlay if no resource).
            if (const SandboxDebugDraw* dbg = reg.GetResource<SandboxDebugDraw>())
            {
                opts.drawContacts  = dbg->drawContacts;
                opts.drawAabbs     = dbg->drawAabbs;
                opts.lineThickness = dbg->lineThickness;
            }
            else
            {
                opts.drawContacts = true;
            }
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

        // Per-frame hooks. FixedUpdate (Task 8) is where the SANDBOX OWNS the physics
        // step: it pumps the SceneControl side channel (a pending reset/switch rebuilds
        // here, on a clean registry), runs the mouse-interaction layer on `input`, then
        // -- UNLESS PAUSED -- steps the PhysicsWorld via a PhysicsSystem invocation with a
        // CONTROLLABLE dt (time-scaled; a single-step request runs exactly one tick then
        // re-pauses). PhysicsSystem is NO LONGER in the engine fixedUpdate scheduler; only
        // TransformPropagationSystem stays there (RunLoop runs this plugin hook BEFORE that
        // scheduler, so physics still writes LocalTransform before propagation derives
        // WorldTransform -- the ordering the engine path guaranteed is preserved). The
        // render systems run every frame so a frozen scene still draws + propagates.
        void FixedUpdate(Astra::Registry& reg, double dt, const Arcane::InputSnapshot& input);
        void Update(double dt, double alpha);

        // Draw the ImGui control panel (Task 8). Called by GamePlugin_DrawUI (the host
        // calls it between ImGuiLayer BeginFrame/Render). Forwards to Hud::Draw. Defined
        // in SandboxApp.cpp (which includes Hud.hpp) so the header stays imgui-free.
        void DrawUI(Astra::Registry& reg);

        std::size_t CurrentScene() const noexcept { return m_sceneIndex; }

        // ---- HUD-bound sim controls (Task 8) ---------------------------------------
        // The HUD writes these; FixedUpdate reads them to gate/scale the physics step.

        // Pause gates the physics step (the scene still renders + propagates frozen).
        void SetPaused(bool p) noexcept { m_paused = p; }
        [[nodiscard]] bool IsPaused() const noexcept { return m_paused; }

        // Request exactly ONE physics tick on the next FixedUpdate, then re-pause.
        // Only meaningful while paused (a running sim steps every frame anyway).
        void RequestSingleStep() noexcept { m_singleStep = true; }

        // Time-scale multiplies the physics dt (1 = real time, 0.5 = half, 2 = double).
        // Clamped to a sane range so a huge scale cannot blow the solver up in one step.
        void  SetTimeScale(float s) noexcept;
        [[nodiscard]] float TimeScale() const noexcept { return m_timeScale; }

        // Gravity (world units/s^2, +Y down). PhysicsWorld has no runtime gravity setter
        // (gravity is baked at construction), so this only records the value; the HUD
        // pairs it with a scene reset (Reset(reg) / a SceneControl reset request), which
        // mints a FRESH world at the new gravity via the same clean-rebuild path
        // SetScene/Reset use. SetGravityY alone does not affect the live world.
        void  SetGravityY(float g) noexcept { m_gravityY = g; }
        [[nodiscard]] float GravityY() const noexcept { return m_gravityY; }

        // The spawn knobs the HUD edits (forwarded to the Interaction's SpawnConfig).
        [[nodiscard]] const SpawnConfig& SpawnConfig()    const noexcept { return m_interaction.SpawnCfg(); }
        [[nodiscard]] Sandbox::SpawnConfig& SpawnConfigMut()    noexcept { return m_interaction.SpawnCfg(); }

        // The debug-overlay flags the HUD toggles. SandboxApp owns the authoritative
        // copy; FixedUpdate publishes it into the SandboxDebugDraw resource each step so
        // PhysicsDebugRenderSystem (a separate system) reads the HUD's choices.
        [[nodiscard]] const SandboxDebugDraw& DebugOptions() const noexcept { return m_debug; }
        [[nodiscard]] SandboxDebugDraw&       DebugOptionsMut()     noexcept { return m_debug; }

        // The sandbox 2D camera (pan + zoom). The plugin pushes it to the engine each
        // frame via Runtime::SetCamera so RenderSubmissionSystem + DrawPhysicsDebug
        // apply the SAME transform (sprites + overlay move together).
        const Camera& Cam() const noexcept { return m_camera; }
        Camera&       Cam()       noexcept { return m_camera; }

    private:
        // Tear the registry down to bare resources + mint a fresh PhysicsResource, then run
        // the scene `index` builder. Shared by BuildInitialScene/SetScene/Reset.
        void RebuildScene(Astra::Registry& reg, std::size_t index);

        // Publish sceneCount + currentScene into the SceneControl resource (idempotent;
        // creates it if absent). Called after every (re)build.
        void PublishControl(Astra::Registry& reg) const;

        // Publish the HUD debug flags into the SandboxDebugDraw resource so the
        // render-phase overlay system reads them. Idempotent; creates it if absent.
        void PublishDebug(Astra::Registry& reg) const;

        std::size_t m_sceneIndex = 0;
        float       m_gravityY   = 0.0f;
        Camera      m_camera{};      // default identity; configured in RebuildScene
        Interaction m_interaction{}; // mouse spawn/drag/throw + pan/zoom (Task 7)

        // ---- sim controls (Task 8) -------------------------------------------------
        bool  m_paused     = false;
        bool  m_singleStep = false;   // one-shot: run exactly one tick then re-pause
        float m_timeScale  = 1.0f;    // physics dt multiplier (clamped in SetTimeScale)

        // ---- debug overlay flags (Task 8) ------------------------------------------
        SandboxDebugDraw m_debug{};   // contacts on / AABBs off by default (Task-7 look)
    };
}
