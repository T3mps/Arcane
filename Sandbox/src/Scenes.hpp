#pragma once

// Sandbox scene registry. Each SceneDef is a named scene builder that populates a
// fresh Registry with physics + render entities and sets the SceneRoot resource.
// Task 4 ships ONE scene (scene 0 "Playground"); scenes 1-7 land in Task 5.
//
// The builders depend on the physics/scene components + PhysicsResource being
// registered already (Sandbox.cpp does that in Init before calling the builder),
// and they assume the Astra TypeContext has been installed in THIS module.

#include <span>

namespace Astra { class Registry; }

namespace Arcane::Sandbox
{
    // One scene: a human-readable name + a builder that fills the Registry.
    // The builder creates a SceneRoot entity, parents all scene content under it,
    // and calls reg.SetResource<Arcane::SceneRoot>(...) so the engine systems and
    // future hot-reload path can find the subtree that IS the scene.
    struct SceneDef
    {
        const char* name = nullptr;
        void (*build)(Astra::Registry& reg) = nullptr;
    };

    // The process-wide table of sandbox scenes. Task 4: one entry (scene 0).
    std::span<const SceneDef> SceneRegistry();
}
