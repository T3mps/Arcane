#pragma once

// Sandbox scene registry. Each SceneDef is a named scene builder that populates a
// fresh Registry with physics + render entities and sets the SceneRoot resource.
// Task 4 ships ONE scene (scene 0 "Playground"); scenes 1-7 land in Task 5.
//
// The builders depend on the physics/scene components + PhysicsResource being
// registered already (Sandbox.cpp does that in Init before calling the builder),
// and they assume the Astra TypeContext has been installed in THIS module.

// ---- stress-scene knobs (exported so tests auto-scale with the constant) -----
// kStressBodyCount - THE single knob: change this to scale the brutal churn.
// kStressWhiskCount - number of kinematic balloon-whisk agitators (always 1).
namespace Arcane::Sandbox
{
    inline constexpr int kStressBodyCount = 10000;
    inline constexpr int kStressWhiskCount = 1;
}

#include <span>

#include <Arcane/Physics/PhysicsTypes.hpp>   // Physics::BodyType

#include <Astra/Entity/Entity.hpp>           // Astra::Entity (a using-alias -- can't fwd-declare)

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace Astra { class Registry; }

namespace Arcane::Sandbox
{
    // -------------------------------------------------------------------------
    // Public spawn builders (Task 7): the SAME Astra-component body authoring the
    // scene builders use (LocalTransform + WorldTransform + RigidBody2D +
    // Collider2D + PhysicsBodyRef + SpriteRenderer, parented under SceneRoot), but
    // callable from outside Scenes.cpp so the interaction layer can spawn at the
    // cursor without duplicating the body-assembly. PhysicsSystem mints the live
    // PhysicsWorld body on the next fixedUpdate (component-driven, no world-direct
    // create). `root` defaults to the SceneRoot resource entity when left null.
    // -------------------------------------------------------------------------

    // Dynamic/static box: an Aabb-fixture body. Returns the created entity.
    // `density` scales the body mass (HUD spawn knob, Task 8); defaults to 1.
    Astra::Entity SpawnBox(Astra::Registry& reg, Astra::Entity root,
                           glm::vec2 pos, glm::vec2 halfExtents,
                           Physics::BodyType type, glm::vec4 tint,
                           float density = 1.0f);

    // Dynamic/static circle: a Circle-fixture body. Returns the created entity.
    // `density` scales the body mass (HUD spawn knob, Task 8); defaults to 1.
    Astra::Entity SpawnCircle(Astra::Registry& reg, Astra::Entity root,
                              glm::vec2 pos, float radius,
                              Physics::BodyType type, glm::vec4 tint,
                              float density = 1.0f);

    // Dynamic/static capsule: an upright pill body. `radius` is both the end-cap
    // radius AND the segment half-length (total height = 4*radius, a clean 2:1 pill).
    // Mirrors SpawnBox/SpawnCircle; uses Physics::MakeCapsule via a Capsule fixture.
    Astra::Entity SpawnCapsule(Astra::Registry& reg, Astra::Entity root,
                               glm::vec2 pos, float radius,
                               Physics::BodyType type, glm::vec4 tint,
                               float density = 1.0f);

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
