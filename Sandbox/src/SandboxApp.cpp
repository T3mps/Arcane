// SandboxApp implementation. Owns the scene roster index + the clean-rebuild path
// (Clear + fresh PhysicsResource + builder) and pumps the SceneControl side channel each
// fixed step. The engine systems do the physics + render work; see SandboxApp.hpp for the
// design notes.

#include "SandboxApp.hpp"
#include "Scenes.hpp"

#include <Arcane/Input/InputSnapshot.hpp>       // InputSnapshot (fed to the interaction layer)
#include <Arcane/Physics/PhysicsWorld.hpp>      // PhysicsWorld + WorldDef (fresh world per scene)
#include <Arcane/Scene/PhysicsSystem.hpp>       // PhysicsResource

#include <Astra/Registry/Registry.hpp>

#include <memory>
#include <span>

namespace Arcane::Sandbox
{
    namespace
    {
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

        // 6. Publish the new index for the SceneControl side channel.
        PublishControl(reg);
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

    void SandboxApp::FixedUpdate(Astra::Registry& reg, double dt,
                                 const Arcane::InputSnapshot& input)
    {
        // Pump the SceneControl side channel BEFORE the engine fixedUpdate scheduler runs
        // (RunLoop calls this plugin hook first), so a rebuild lands on a clean registry,
        // never mid-PhysicsSystem. A reset takes precedence over a switch; both are one-shot
        // (cleared after consumption).
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

        // Task 7: run the mouse-interaction layer on the (possibly just-rebuilt) scene,
        // still BEFORE the engine fixedUpdate scheduler (PhysicsSystem). Spawned entities
        // are materialized by this step's CREATE pass; a grabbed body's drive velocity is
        // set so it integrates this step; camera pan/zoom is in place before Update pushes
        // the camera. Guarded by a live PhysicsWorld (always present after a (re)build).
        if (PhysicsResource* phys = reg.GetResource<PhysicsResource>(); phys && phys->world)
            m_interaction.Tick(reg, *phys->world, m_camera, input, static_cast<float>(dt));

        // PhysicsSystem (registered in fixedUpdate) then advances the PhysicsWorld and
        // writes back into LocalTransform; TransformPropagationSystem derives WorldTransform.
    }

    void SandboxApp::Update(double /*dt*/, double /*alpha*/)
    {
        // No variable-rate work yet. Camera + HUD arrive in later tasks.
    }
}
