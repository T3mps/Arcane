#pragma once

// Sandbox scene registry. Each SceneDef is a named scene builder that populates a
// fresh Registry with physics + render entities and sets the SceneRoot resource.
// Task 4 ships ONE scene (scene 0 "Playground"); scenes 1-7 land in Task 5.
//
// The builders depend on the physics/scene components + PhysicsResource being
// registered already (Sandbox.cpp does that in Init before calling the builder),
// and they assume the Astra TypeContext has been installed in THIS module.

// ---- stress-scene knobs (exported so tests auto-scale with the constant) -----
// kStressBodyCount - THE single knob: change this to scale the brutal CHURN/PERF
//   scene (the registered scene 8, the ArcaneRuntime showcase). Procedurally fills a
//   20-column grid, so the spawn column is ceil(N/20) rows tall -- at 10000 that
//   is a ~410 m column (0.82 m pitch) whose TOP sits ~ -404 m (far above the bowl).
// kStressStabilityBodyCount - the count the *unit-test* stability check runs at.
//   DECOUPLED from the perf knob on purpose: the SandboxVisualsTest "stays
//   bounded" assertion uses a meter-scale spatial bound (kYMin = -50), and the
//   spawn column must fit inside it. 1200 bodies = 60 rows -> top row y ~ -40.4 m,
//   comfortably inside -50 (a body would have to gain real energy to escape),
//   so this check stays a true instability tripwire while the perf scene keeps
//   the full 10000. (See the geometry note in BuildStressTest.)
// kStressWhiskCount - number of kinematic balloon-whisk agitators (always 1).
namespace Arcane::Sandbox
{
    inline constexpr int kStressBodyCount          = 10000;
    inline constexpr int kStressStabilityBodyCount = 1200;
    inline constexpr int kStressWhiskCount         = 1;
}

#include <span>

#include <Manifold2D/Physics/PhysicsTypes.hpp>   // Phys::BodyType

#include <Astra/Entity/Entity.hpp>           // Astra::Entity (a using-alias -- can't fwd-declare)

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace Astra { class Registry; }

namespace Arcane::Sandbox
{
    // Physics types were lifted to the standalone Manifold2D library (Phase 2).
    namespace Phys = Manifold2D::Physics;

    // -------------------------------------------------------------------------
    // Public spawn builders (Task 7): the SAME Astra-component body authoring the
    // scene builders use (Transform + WorldTransform + RigidBody2D +
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
                           Phys::BodyType type, glm::vec4 tint,
                           float density = 1.0f);

    // Dynamic/static circle: a Circle-fixture body. Returns the created entity.
    // `density` scales the body mass (HUD spawn knob, Task 8); defaults to 1.
    Astra::Entity SpawnCircle(Astra::Registry& reg, Astra::Entity root,
                              glm::vec2 pos, float radius,
                              Phys::BodyType type, glm::vec4 tint,
                              float density = 1.0f);

    // Dynamic/static capsule: an upright pill body. `radius` is both the end-cap
    // radius AND the segment half-length (total height = 4*radius, a clean 2:1 pill).
    // Mirrors SpawnBox/SpawnCircle; uses Phys::MakeCapsule via a Capsule fixture.
    Astra::Entity SpawnCapsule(Astra::Registry& reg, Astra::Entity root,
                               glm::vec2 pos, float radius,
                               Phys::BodyType type, glm::vec4 tint,
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

    // The stress scene (scene 8) built at an arbitrary dynamic-body count, so the
    // unit-test stability check can run at kStressStabilityBodyCount while the
    // registered roster scene (and the ArcaneRuntime perf showcase) stays at the full
    // kStressBodyCount. Same arena/whisk/RNG seed as BuildStressTest -- only the
    // procedural body count differs. Requires the PhysicsResource world installed
    // (path-A bodies mint on the next PhysicsSystem fixedUpdate).
    void BuildStressTestN(Astra::Registry& reg, int bodyCount);
}
