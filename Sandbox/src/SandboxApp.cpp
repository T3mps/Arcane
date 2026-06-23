// SandboxApp implementation. Owns the scene roster index + the clean-rebuild path
// (Clear + fresh PhysicsResource + builder) and pumps the SceneControl side channel each
// fixed step. The engine systems do the physics + render work; see SandboxApp.hpp for the
// design notes.

#include "SandboxApp.hpp"
#include "Hud.hpp"
#include "Scenes.hpp"

#include <Arcane/Input/InputSnapshot.hpp>       // InputSnapshot (fed to the interaction layer)
#include <Arcane/Physics/PhysicsWorld.hpp>      // PhysicsWorld + WorldDef (fresh world per scene)
#include <Arcane/Scene/PhysicsSystem.hpp>       // PhysicsResource + PhysicsSystem (sandbox-owned step)

#include <Astra/Registry/Registry.hpp>

#include <algorithm>                            // std::clamp
#include <memory>
#include <span>

namespace Arcane::Sandbox
{
    namespace
    {
        // The sandbox fixed timestep (matches the 60 Hz RunLoop). The sandbox-owned
        // physics step (Task 8) scales THIS by the HUD time-scale per FixedUpdate.
        constexpr float kFixedDt = 1.0f / 60.0f;

        // Time-scale clamp: a huge scale in one discrete step would tunnel/explode the
        // solver; a tiny one is harmless. Keep it readable + stable.
        constexpr float kMinTimeScale = 0.05f;
        constexpr float kMaxTimeScale = 4.0f;

        // Install a FRESH PhysicsResource (new PhysicsWorld + empty entity<->body map),
        // replacing any existing one. SetResource overwrites the resource slot in place, so
        // the previous world (and all its bodies) is destroyed -- nothing leaks into the new
        // scene. Mirrors EnsurePhysicsResource in Sandbox.cpp but unconditionally replaces.
        void InstallFreshPhysicsResource(Astra::Registry& reg, float gravityY)
        {
            Arcane::Physics::WorldDef wd;
            wd.gravityY = gravityY;
            reg.SetResource(Arcane::PhysicsResource{
                std::make_unique<Arcane::Physics::PhysicsWorld>(wd),
                {}
            });
        }
    }

    void SandboxApp::SetTimeScale(float s) noexcept
    {
        m_timeScale = std::clamp(s, kMinTimeScale, kMaxTimeScale);
    }

    void SandboxApp::RebuildScene(Astra::Registry& reg, std::size_t index)
    {
        const std::span<const SceneDef> scenes = SceneRegistry();
        if (scenes.empty()) return;
        if (index >= scenes.size()) index %= scenes.size();   // wrap into range
        m_sceneIndex = index;

        // 1. Destroy ALL current scene entities. Registry::Clear() wipes entities +
        //    relationships + archetypes but LEAVES resources intact (RenderContext2D is set
        //    by the host each frame; PhysicsResource/SceneRoot are replaced below/by the
        //    builder). This is the cleanest reset since the sandbox owns every entity.
        reg.Clear();

        // 2. Mint a fresh PhysicsResource so the new scene's bodies start from an empty
        //    world + empty entity<->body map (no stale handles from the old scene).
        InstallFreshPhysicsResource(reg, m_gravityY);

        // 3. Run the target builder -- it repopulates entities and re-sets SceneRoot.
        scenes[m_sceneIndex].build(reg);

        // 4. Default the camera to the identity transform (offset (0,0), zoom 1). The
        //    scenes are authored directly in canvas-pixel space (world unit == canvas px,
        //    floors low on a 1280x720 canvas), so the identity already frames them on
        //    screen -- the same framing the smoke test relied on pre-camera. Pan/zoom
        //    INPUT (mouse drag + wheel) is Task 7; this task just makes the camera
        //    pipeline live + correct so a nonzero camera moves sprites + overlay together.
        m_camera = Camera{};

        // 5. Drop any active mouse grab: the old PhysicsWorld (and the grabbed body's
        //    handle) is gone -- a held grab must not carry into the fresh world.
        m_interaction.ClearGrab();

        // 6. Publish the new index for the SceneControl side channel + the HUD debug
        //    flags for the render-phase overlay (both survive Clear() as resources, but
        //    a fresh build re-establishes them so the very first frame is correct).
        PublishControl(reg);
        PublishDebug(reg);
    }

    void SandboxApp::PublishDebug(Astra::Registry& reg) const
    {
        SandboxDebugDraw* dbg = reg.GetResource<SandboxDebugDraw>();
        if (!dbg)
        {
            reg.SetResource(SandboxDebugDraw{});
            dbg = reg.GetResource<SandboxDebugDraw>();
        }
        if (!dbg) return;
        *dbg = m_debug;   // mirror the HUD-owned flags into the render-read resource
    }

    void SandboxApp::PublishControl(Astra::Registry& reg) const
    {
        SceneControl* ctrl = reg.GetResource<SceneControl>();
        if (!ctrl)
        {
            reg.SetResource(SceneControl{});
            ctrl = reg.GetResource<SceneControl>();
        }
        if (!ctrl) return;
        ctrl->sceneCount   = static_cast<std::int32_t>(SceneRegistry().size());
        ctrl->currentScene = static_cast<std::int32_t>(m_sceneIndex);
    }

    void SandboxApp::BuildInitialScene(Astra::Registry& reg)
    {
        RebuildScene(reg, m_sceneIndex);
    }

    void SandboxApp::SetScene(Astra::Registry& reg, std::uint32_t index)
    {
        RebuildScene(reg, static_cast<std::size_t>(index));
    }

    void SandboxApp::Reset(Astra::Registry& reg)
    {
        RebuildScene(reg, m_sceneIndex);
    }

    void SandboxApp::FixedUpdate(Astra::Registry& reg, double /*dt*/,
                                 const Arcane::InputSnapshot& input)
    {
        // Pump the SceneControl side channel BEFORE anything else (RunLoop calls this
        // plugin hook first), so a rebuild lands on a clean registry, never mid-step.
        // A reset takes precedence over a switch; both are one-shot (cleared on consume).
        if (SceneControl* ctrl = reg.GetResource<SceneControl>())
        {
            if (ctrl->requestReset != 0)
            {
                ctrl->requestReset = 0;
                ctrl->requestedScene = -1;   // a reset supersedes any pending switch
                Reset(reg);
            }
            else if (ctrl->requestedScene >= 0)
            {
                const std::uint32_t idx = static_cast<std::uint32_t>(ctrl->requestedScene);
                ctrl->requestedScene = -1;
                SetScene(reg, idx);
            }
        }

        // Keep the render-overlay resource in lockstep with the HUD-owned flags every
        // step (a HUD checkbox edit between frames reaches PhysicsDebugRenderSystem).
        PublishDebug(reg);

        PhysicsResource* phys = reg.GetResource<PhysicsResource>();
        if (!phys || !phys->world) return;

        // Task 7: the mouse-interaction layer runs on the (possibly just-rebuilt) scene
        // BEFORE the physics step -- spawned entities are materialized by the step's CREATE
        // pass; a grabbed body's drive velocity is set so it integrates this step; camera
        // pan/zoom is in place before Update pushes the camera. Interaction runs even while
        // PAUSED (spawn/grab/pan still respond; the step below just won't advance the sim).
        m_interaction.Tick(reg, *phys->world, m_camera, input, kFixedDt);

        // Mirror the in-progress polygon draft into the render-read resource so
        // PolygonDraftRenderSystem can draw the clicked vertices. Published ONLY when
        // shape == Polygon -- switching to Box/Circle/Capsule hides stray markers while
        // the in-progress points are retained internally for when Polygon is resumed.
        if (!reg.GetResource<PolygonDraftResource>())
            reg.SetResource(PolygonDraftResource{});
        if (m_interaction.IsPolygonMode())
            reg.GetResource<PolygonDraftResource>()->worldPoints = m_interaction.PolygonPoints();
        else
            reg.GetResource<PolygonDraftResource>()->worldPoints.clear();

        // ITEM 2: commit a HUD-requested polygon on the LIVE world (deferred out of the
        // render phase). World-direct -> it renders via DrawPhysicsDebug (no entity).
        // Runs even while paused (authoring geometry shouldn't need a running sim).
        if (m_requestPolygonSpawn)
        {
            m_requestPolygonSpawn = false;
            m_interaction.SpawnPolygon(*phys->world);
        }

        // ---- SANDBOX-OWNED PHYSICS STEP (Task 8 sim-control) -----------------------
        // PhysicsSystem is no longer in the engine fixedUpdate scheduler; the sandbox
        // drives it here so pause/single-step/time-scale can gate + scale the dt. RunLoop
        // runs this plugin hook BEFORE the engine fixedUpdate scheduler (which still holds
        // TransformPropagationSystem), so physics writes LocalTransform before propagation
        // derives WorldTransform -- the ordering the engine path guaranteed is preserved.
        //
        // PAUSED: skip the step (the scene stays frozen but still renders + propagates).
        // SINGLE-STEP: while paused, run exactly one tick then re-pause (one-shot flag).
        // TIME-SCALE: feed the PhysicsSystem a scaled dt (a larger/smaller fixed tick).
        const bool runThisStep = !m_paused || m_singleStep;
        if (!runThisStep)
        {
            // Frozen: mint a paused-spawned body + write back its pose, but DO
            // NOT step (skip the narrowphase + solve entirely -> pausing the dense
            // stress scene instantly recovers FPS).
            PhysicsSystem mintOnly(kFixedDt, /*stepWorld=*/false);
            mintOnly(reg);
            return;
        }

        const bool wasSingleStep = m_singleStep;
        m_singleStep = false;                       // consume the one-shot request

        const float stepDt = kFixedDt * m_timeScale;
        PhysicsSystem physics(stepDt);
        physics(reg);                               // CREATE/SYNC + Step(stepDt) + write-back

        if (wasSingleStep)
            m_paused = true;                        // re-pause after the single step
    }

    void SandboxApp::Update(double /*dt*/, double /*alpha*/,
                            const Arcane::InputSnapshot& input)
    {
        // Variable-rate (once-per-host-frame) work. Mouse-wheel zoom is consumed HERE
        // -- not in FixedUpdate's Tick -- because the wheel delta is a per-frame
        // accumulated impulse: the fixed-step Tick runs 0..N times per host frame, so
        // consuming it there drops notches (no fixed step that frame) or doubles them
        // (several). Update runs exactly once per host frame, matching the wheel's
        // sampling cadence, so the zoom is smooth. The camera is pushed to the engine
        // by the plugin immediately after this.
        m_interaction.ApplyWheelZoom(m_camera, input);
    }

    void SandboxApp::DrawUI(Astra::Registry& reg)
    {
        Hud::Draw(*this, reg);
    }
}
