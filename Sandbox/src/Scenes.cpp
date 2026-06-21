// Sandbox scene builders. Task 4: one scene ("Playground") -- a static floor + 2
// static walls + 5 dynamic bodies (mixed circle/box colliders) that fall and settle
// onto the floor under gravity. Every body carries a SpriteRenderer so it also draws
// as a tinted quad through RenderSubmissionSystem; the physics-debug overlay draws the
// collider outlines on top.
//
// The body-building idiom mirrors PhysicsSystemTest.cpp: an entity with
// LocalTransform + WorldTransform + RigidBody2D + Collider2D (one Fixture per shape via
// MakeAabb/MakeCircle authored as a Fixture descriptor) + PhysicsBodyRef. PhysicsSystem
// consumes that on the first fixedUpdate, creates the PhysicsWorld body, and writes the
// post-step pose back into LocalTransform.
//
// Coordinate convention (matches the physics tests): +Y is DOWN, world unit == canvas
// pixel. The canvas is 1280x720; the floor sits low on screen and bodies drop onto it.

#include "Scenes.hpp"

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <array>

namespace Arcane::Sandbox
{
    namespace
    {
        // ---- small authoring helpers -------------------------------------------------

        // Create a body entity parented under `root`, with a single-fixture collider and
        // a sprite. The sprite size is derived from the half-extents so the quad roughly
        // matches the collider. Returns the created entity.
        Astra::Entity MakeBox(Astra::Registry& reg, Astra::Entity root,
                              glm::vec2 pos, glm::vec2 halfExtents,
                              Physics::BodyType type, glm::vec4 tint,
                              float restitution = 0.1f)
        {
            Astra::Entity e = reg.CreateEntity();

            Arcane::LocalTransform lt; lt.position = pos;
            reg.AddComponent<Arcane::LocalTransform>(e, lt);
            reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});

            Arcane::RigidBody2D rb;
            rb.type = type;
            // AABB shapes are axis-aligned by definition; the PhysicsWorld asserts that a
            // dynamic body with an AABB fixture is fixedRotation (it cannot rotate). Static
            // bodies never rotate, so setting it unconditionally on box bodies is correct.
            rb.fixedRotation = true;
            reg.AddComponent<Arcane::RigidBody2D>(e, rb);

            Arcane::Collider2D col;
            {
                Arcane::Fixture fx;
                fx.kind        = Physics::ShapeKind::Aabb;
                fx.halfW       = halfExtents.x;
                fx.halfH       = halfExtents.y;
                fx.density     = 1.0f;
                fx.friction    = 0.4f;
                fx.restitution = restitution;
                col.fixtures.push_back(fx);
            }
            reg.AddComponent<Arcane::Collider2D>(e, col);
            reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});

            Arcane::SpriteRenderer sp;
            sp.size = halfExtents * 2.0f;   // full extent (size is base pixel size pre-scale)
            sp.tint = tint;
            reg.AddComponent<Arcane::SpriteRenderer>(e, sp);

            reg.SetParent(e, root);
            return e;
        }

        Astra::Entity MakeCircle(Astra::Registry& reg, Astra::Entity root,
                                 glm::vec2 pos, float radius,
                                 Physics::BodyType type, glm::vec4 tint,
                                 float restitution = 0.2f)
        {
            Astra::Entity e = reg.CreateEntity();

            Arcane::LocalTransform lt; lt.position = pos;
            reg.AddComponent<Arcane::LocalTransform>(e, lt);
            reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});

            Arcane::RigidBody2D rb;
            rb.type = type;
            reg.AddComponent<Arcane::RigidBody2D>(e, rb);

            Arcane::Collider2D col;
            {
                Arcane::Fixture fx;
                fx.kind        = Physics::ShapeKind::Circle;
                fx.radius      = radius;
                fx.density     = 1.0f;
                fx.friction    = 0.4f;
                fx.restitution = restitution;
                col.fixtures.push_back(fx);
            }
            reg.AddComponent<Arcane::Collider2D>(e, col);
            reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});

            Arcane::SpriteRenderer sp;
            sp.size = glm::vec2(radius * 2.0f);   // bounding box of the circle
            sp.tint = tint;
            reg.AddComponent<Arcane::SpriteRenderer>(e, sp);

            reg.SetParent(e, root);
            return e;
        }

        // ---- scene 0: "Playground" ---------------------------------------------------
        //
        // Static floor (a wide thin box) + 2 static walls (tall thin boxes) framing a
        // pit, with 5 dynamic bodies (3 boxes + 2 circles) dropped in from above. Under
        // gravity the dynamics fall and settle onto the floor between the walls.
        void BuildPlayground(Astra::Registry& reg)
        {
            // SceneRoot: anchors the hierarchy; no physics on it.
            Astra::Entity root = reg.CreateEntity();
            reg.AddComponent<Arcane::LocalTransform>(root, Arcane::LocalTransform{});
            reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

            // --- statics ---------------------------------------------------------------
            const glm::vec4 kStatic{ 0.30f, 0.32f, 0.38f, 1.0f };

            // Floor: wide thin box near the bottom of the 1280x720 canvas. Top surface
            // at y ~= 620.
            MakeBox(reg, root, glm::vec2(640.0f, 640.0f), glm::vec2(420.0f, 20.0f),
                    Physics::BodyType::Static, kStatic);

            // Left wall + right wall: tall thin boxes framing the drop zone.
            MakeBox(reg, root, glm::vec2(240.0f, 480.0f), glm::vec2(20.0f, 160.0f),
                    Physics::BodyType::Static, kStatic);
            MakeBox(reg, root, glm::vec2(1040.0f, 480.0f), glm::vec2(20.0f, 160.0f),
                    Physics::BodyType::Static, kStatic);

            // --- dynamics: 5 bodies, mixed box/circle colliders -----------------------
            // Spread across the drop zone so they stack/scatter on the floor.
            MakeBox(reg, root, glm::vec2(520.0f, 120.0f), glm::vec2(28.0f, 28.0f),
                    Physics::BodyType::Dynamic, glm::vec4(0.90f, 0.45f, 0.30f, 1.0f));
            MakeBox(reg, root, glm::vec2(640.0f, 60.0f),  glm::vec2(34.0f, 22.0f),
                    Physics::BodyType::Dynamic, glm::vec4(0.40f, 0.70f, 0.95f, 1.0f));
            MakeBox(reg, root, glm::vec2(760.0f, 100.0f), glm::vec2(24.0f, 24.0f),
                    Physics::BodyType::Dynamic, glm::vec4(0.55f, 0.85f, 0.45f, 1.0f));

            MakeCircle(reg, root, glm::vec2(580.0f, 200.0f), 26.0f,
                       Physics::BodyType::Dynamic, glm::vec4(0.95f, 0.80f, 0.30f, 1.0f));
            MakeCircle(reg, root, glm::vec2(700.0f, 160.0f), 30.0f,
                       Physics::BodyType::Dynamic, glm::vec4(0.85f, 0.40f, 0.80f, 1.0f));
        }

        // The static scene table. Task 4: one entry.
        constexpr std::array<SceneDef, 1> kScenes = {{
            SceneDef{ "Playground", &BuildPlayground },
        }};
    }

    std::span<const SceneDef> SceneRegistry()
    {
        return std::span<const SceneDef>(kScenes.data(), kScenes.size());
    }
}
