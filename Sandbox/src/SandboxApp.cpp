// SandboxApp implementation. Task 4: build scene 0; per-frame hooks are no-ops (the
// engine systems do the physics + render work). See SandboxApp.hpp for the design notes.

#include "SandboxApp.hpp"
#include "Scenes.hpp"

#include <Astra/Registry/Registry.hpp>

namespace Arcane::Sandbox
{
    void SandboxApp::BuildInitialScene(Astra::Registry& reg)
    {
        const std::span<const SceneDef> scenes = SceneRegistry();
        if (scenes.empty()) return;
        if (m_sceneIndex >= scenes.size()) m_sceneIndex = 0;
        scenes[m_sceneIndex].build(reg);
    }

    void SandboxApp::FixedUpdate(double /*dt*/)
    {
        // No per-frame mutation in Task 4. PhysicsSystem (registered in fixedUpdate)
        // advances the PhysicsWorld and writes back into LocalTransform;
        // TransformPropagationSystem derives WorldTransform. Interaction comes in Task 6.
    }

    void SandboxApp::Update(double /*dt*/, double /*alpha*/)
    {
        // No variable-rate work in Task 4. Camera + HUD arrive in Tasks 6-8.
    }
}
