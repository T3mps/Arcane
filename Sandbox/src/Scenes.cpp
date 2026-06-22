// Sandbox scene builders. Task 5 fills out the roster to EIGHT scenes, each showcasing one
// Physics v2 capability:
//   0 Playground      -- floor + 2 walls + mixed dynamics (the Task-4 sampler).
//   1 Box stack       -- a tall stack of boxes settling stably.
//   2 Pyramid         -- a triangular pile (friction / stress).
//   3 Joint chain     -- revolute + weld + distance + prismatic links.
//   4 Rotation drop   -- polygon boxes released at nonzero angles, settling flat.
//   5 CCD bullet      -- a fast bullet body fired at a thin wall (no tunnelling).
//   6 Compound bodies -- multi-fixture bodies with an off-origin COM that tip.
//   7 Mixed shapes    -- circles + capsules + polygons interacting in a bowl.
//
// TWO BODY-AUTHORING PATHS (both feed the SAME PhysicsWorld owned by PhysicsResource):
//
//   (A) ASTRA COMPONENTS -- the default. An entity carries LocalTransform + WorldTransform +
//       RigidBody2D + Collider2D (a Fixture list) + PhysicsBodyRef + SpriteRenderer.
//       PhysicsSystem creates the body on the first fixedUpdate and writes the post-step
//       pose back into LocalTransform; RenderSubmissionSystem draws the sprite. Used by
//       scenes 0,1,2,5,6 and the circle/capsule bodies of 7.
//
//   (B) WORLD-DIRECT -- create the body straight on the PhysicsWorld in the builder. Needed
//       when the Astra Fixture descriptor cannot express the body:
//         * POLYGON shapes: the Fixture descriptor carries no vertex array, and PhysicsSystem
//           asserts/falls-back for ShapeKind::Polygon. A ROTATING box MUST be a polygon (a
//           dynamic Aabb body trips the engine's fixedRotation invariant), so scenes 4 + 7
//           author their polygons world-direct.
//         * JOINTS: PhysicsWorld::AddJoint needs the two BodyHandles, which only exist after
//           a body is created. World-direct bodies hand us the handle immediately, so the
//           joint scene (3) builds its chain + joints right here.
//       World-direct bodies have NO entity, so RenderSubmissionSystem does not draw them; the
//       physics-debug overlay (PhysicsDebugRenderSystem -> DrawPhysicsDebug) draws EVERY world
//       body's outline (rotation-aware), which is exactly the capability scenes 3/4/7 show off.
//
// Coordinate convention (matches the physics tests): +Y is DOWN, world unit == canvas pixel.
// The canvas is 1280x720; floors sit low on screen and bodies drop onto them.

#include "Scenes.hpp"

#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/Joints/Joints.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Shapes.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>     // PhysicsResource (the world the bodies live in)
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cmath>
#include <vector>

namespace Arcane::Sandbox
{
    namespace
    {
        // ---- shared palette ---------------------------------------------------------
        constexpr glm::vec4 kStatic  { 0.30f, 0.32f, 0.38f, 1.0f };
        constexpr glm::vec4 kOrange  { 0.90f, 0.45f, 0.30f, 1.0f };
        constexpr glm::vec4 kBlue    { 0.40f, 0.70f, 0.95f, 1.0f };
        constexpr glm::vec4 kGreen   { 0.55f, 0.85f, 0.45f, 1.0f };
        constexpr glm::vec4 kGold    { 0.95f, 0.80f, 0.30f, 1.0f };
        constexpr glm::vec4 kMagenta { 0.85f, 0.40f, 0.80f, 1.0f };
        constexpr glm::vec4 kTeal    { 0.35f, 0.80f, 0.78f, 1.0f };

        // =============================================================================
        // SceneRoot + small authoring helpers (path A: Astra components)
        // =============================================================================

        // Create the SceneRoot anchor entity + publish the SceneRoot resource. Every builder
        // calls this first; all scene content is parented under the returned root.
        Astra::Entity MakeRoot(Astra::Registry& reg)
        {
            Astra::Entity root = reg.CreateEntity();
            reg.AddComponent<Arcane::LocalTransform>(root, Arcane::LocalTransform{});
            reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
            return root;
        }

        // GENERIC per-fixture visuals -- the reusable way to give ANY body sprites
        // that MATCH its physics. Creates one CHILD sprite per collider fixture,
        // shaped + sized + placed to that fixture (circle -> disc, capsule ->
        // capsule, aabb -> rect), parented to the body so transform propagation
        // tracks each fixture's world pose and the (rotation-aware)
        // RenderSubmissionSystem turns it with the body. Add a fixture and its
        // sprite appears -- this is how multi-fixture / compound showpieces stay
        // visually correct. Polygon fixtures (no authored verts on the Astra
        // Fixture) are world-direct + debug-drawn, so they get no sprite here.
        void AttachFixtureVisuals(Astra::Registry& reg, Astra::Entity body,
                                  const std::vector<Arcane::Fixture>& fixtures,
                                  glm::vec4 tint)
        {
            for (const Arcane::Fixture& fx : fixtures)
            {
                Arcane::SpriteRenderer sp;
                sp.tint = tint;
                switch (fx.kind)
                {
                case Physics::ShapeKind::Circle:
                    sp.shape = Arcane::SpriteShape::Circle;
                    sp.size  = glm::vec2(fx.radius * 2.0f);
                    break;
                case Physics::ShapeKind::Capsule:
                    sp.shape = Arcane::SpriteShape::Capsule;
                    sp.size  = glm::vec2((fx.halfLen + fx.radius) * 2.0f, fx.radius * 2.0f);
                    break;
                case Physics::ShapeKind::Aabb:
                    sp.shape = Arcane::SpriteShape::Rect;
                    sp.size  = glm::vec2(fx.halfW * 2.0f, fx.halfH * 2.0f);
                    break;
                default:
                    continue;   // Polygon: world-direct + debug-drawn, no sprite
                }

                Astra::Entity child = reg.CreateEntity();
                Arcane::LocalTransform lt;
                lt.position = fx.localPos;
                lt.rotation = fx.localAngle;
                reg.AddComponent<Arcane::LocalTransform>(child, lt);
                reg.AddComponent<Arcane::WorldTransform>(child, Arcane::WorldTransform{});
                reg.AddComponent<Arcane::SpriteRenderer>(child, sp);
                reg.SetParent(child, body);
            }
        }

        // Path-A box (Aabb fixture). Aabb shapes are axis-aligned, so a dynamic Aabb body
        // MUST be fixedRotation (the engine asserts otherwise); static bodies never rotate.
        // The sprite size tracks the half-extents so the quad matches the collider.
        Astra::Entity MakeBox(Astra::Registry& reg, Astra::Entity root,
                              glm::vec2 pos, glm::vec2 halfExtents,
                              Physics::BodyType type, glm::vec4 tint,
                              float restitution = 0.1f, float friction = 0.4f,
                              float density = 1.0f)
        {
            Astra::Entity e = reg.CreateEntity();

            Arcane::LocalTransform lt; lt.position = pos;
            reg.AddComponent<Arcane::LocalTransform>(e, lt);
            reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});

            Arcane::RigidBody2D rb;
            rb.type = type;
            rb.fixedRotation = true;   // Aabb dynamic bodies cannot rotate (engine invariant)
            reg.AddComponent<Arcane::RigidBody2D>(e, rb);

            Arcane::Collider2D col;
            {
                Arcane::Fixture fx;
                fx.kind        = Physics::ShapeKind::Aabb;
                fx.halfW       = halfExtents.x;
                fx.halfH       = halfExtents.y;
                fx.density     = density;
                fx.friction    = friction;
                fx.restitution = restitution;
                col.fixtures.push_back(fx);
            }
            reg.AddComponent<Arcane::Collider2D>(e, col);
            reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});

            Arcane::SpriteRenderer sp;
            sp.size = halfExtents * 2.0f;   // full extent
            sp.tint = tint;
            reg.AddComponent<Arcane::SpriteRenderer>(e, sp);

            reg.SetParent(e, root);
            return e;
        }

        // Path-A circle (Circle fixture). Circles may rotate freely as Dynamic.
        Astra::Entity MakeCircle(Astra::Registry& reg, Astra::Entity root,
                                 glm::vec2 pos, float radius,
                                 Physics::BodyType type, glm::vec4 tint,
                                 float restitution = 0.2f, float density = 1.0f)
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
                fx.density     = density;
                fx.friction    = 0.4f;
                fx.restitution = restitution;
                col.fixtures.push_back(fx);
            }
            reg.AddComponent<Arcane::Collider2D>(e, col);
            reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});

            Arcane::SpriteRenderer sp;
            sp.shape = Arcane::SpriteShape::Circle;   // render the disc, not a square
            sp.size = glm::vec2(radius * 2.0f);
            sp.tint = tint;
            reg.AddComponent<Arcane::SpriteRenderer>(e, sp);

            reg.SetParent(e, root);
            return e;
        }

        // Path-A horizontal capsule (Capsule fixture). Segment (-halfLen,0)-(+halfLen,0)
        // inflated by `radius`. Dynamic capsules may rotate (the core is a 2-vert segment).
        Astra::Entity MakeCapsule(Astra::Registry& reg, Astra::Entity root,
                                  glm::vec2 pos, float halfLen, float radius,
                                  Physics::BodyType type, glm::vec4 tint)
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
                fx.kind        = Physics::ShapeKind::Capsule;
                fx.halfLen     = halfLen;
                fx.radius      = radius;
                fx.density     = 1.0f;
                fx.friction    = 0.4f;
                fx.restitution = 0.1f;
                col.fixtures.push_back(fx);
            }
            reg.AddComponent<Arcane::Collider2D>(e, col);
            reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});

            Arcane::SpriteRenderer sp;
            sp.shape = Arcane::SpriteShape::Capsule;  // rounded capsule, not a bar
            sp.size = glm::vec2((halfLen + radius) * 2.0f, radius * 2.0f);
            sp.tint = tint;
            reg.AddComponent<Arcane::SpriteRenderer>(e, sp);

            reg.SetParent(e, root);
            return e;
        }

        // Path-A static floor that fills the bottom of the canvas (a wide thin box). Returns
        // the floor entity. `topY` is the y of the floor's TOP surface (bodies rest there).
        Astra::Entity MakeFloor(Astra::Registry& reg, Astra::Entity root,
                                float centerX = 640.0f, float topY = 620.0f,
                                float halfW = 560.0f, float halfH = 20.0f)
        {
            return MakeBox(reg, root, glm::vec2(centerX, topY + halfH),
                           glm::vec2(halfW, halfH), Physics::BodyType::Static, kStatic);
        }

        // =============================================================================
        // World-direct helpers (path B)
        // =============================================================================

        // The live PhysicsWorld owned by the PhysicsResource the plugin installed before
        // calling the builder. Never null on the build path (the plugin mints it first).
        Physics::PhysicsWorld* World(Astra::Registry& reg)
        {
            PhysicsResource* res = reg.GetResource<PhysicsResource>();
            return (res && res->world) ? res->world.get() : nullptr;
        }

        // World-direct static box (4-vert polygon core under Aabb). Used as the floor/anchor
        // for the world-direct scenes so floor + dynamics share one creation path/timing.
        Physics::BodyHandle WorldStaticBox(Physics::PhysicsWorld& w, glm::vec2 pos,
                                           glm::vec2 halfExtents)
        {
            Physics::BodyDef def;
            def.type     = Physics::BodyType::Static;
            def.position = Physics::Vec2(pos.x, pos.y);
            def.shape    = Physics::MakeAabb(halfExtents.x, halfExtents.y);
            def.friction = Physics::Real(0.5);
            return w.AddBody(def);
        }

        // World-direct DYNAMIC convex polygon box at a NONZERO angle. A rotating box must be a
        // polygon (a dynamic Aabb trips the fixedRotation invariant); the polygon core lets it
        // settle flat under gravity. `angle` is the release orientation in radians.
        Physics::BodyHandle WorldPolygonBox(Physics::PhysicsWorld& w, glm::vec2 pos,
                                            glm::vec2 halfExtents, float angle,
                                            float density = 1.0f)
        {
            const float hw = halfExtents.x, hh = halfExtents.y;
            // CCW box corners in local space (the factory normalizes winding + bakes normals).
            const std::vector<Physics::Vec2> verts = {
                Physics::Vec2(-hw, -hh), Physics::Vec2( hw, -hh),
                Physics::Vec2( hw,  hh), Physics::Vec2(-hw,  hh),
            };

            Physics::BodyDef def;
            def.type     = Physics::BodyType::Dynamic;
            def.position = Physics::Vec2(pos.x, pos.y);
            def.shape    = Physics::MakePolygon(verts);
            def.density  = Physics::Real(density);
            def.friction = Physics::Real(0.5);
            def.restitution = Physics::Real(0.05);
            Physics::BodyHandle h = w.AddBody(def);
            w.SetAngle(h, Physics::Real(angle));   // release at a nonzero angle
            return h;
        }

        // World-direct DYNAMIC convex triangle (3-vert polygon) -- a second polygon shape for
        // the mixed-shapes scene so the unified core handles >4-gon-free convex shapes too.
        Physics::BodyHandle WorldTriangle(Physics::PhysicsWorld& w, glm::vec2 pos, float size)
        {
            const std::vector<Physics::Vec2> verts = {
                Physics::Vec2(-size, size), Physics::Vec2(size, size),
                Physics::Vec2(Physics::Real(0), -size),
            };
            Physics::BodyDef def;
            def.type     = Physics::BodyType::Dynamic;
            def.position = Physics::Vec2(pos.x, pos.y);
            def.shape    = Physics::MakePolygon(verts);
            def.density  = Physics::Real(1);
            def.friction = Physics::Real(0.5);
            return w.AddBody(def);
        }

        // World-direct DYNAMIC circle/capsule (path B) for scenes that mix world-direct
        // polygons with round bodies in the SAME creation pass (so all share creation timing).
        Physics::BodyHandle WorldCircle(Physics::PhysicsWorld& w, glm::vec2 pos, float r)
        {
            Physics::BodyDef def;
            def.type     = Physics::BodyType::Dynamic;
            def.position = Physics::Vec2(pos.x, pos.y);
            def.shape    = Physics::MakeCircle(Physics::Real(r));
            def.density  = Physics::Real(1);
            def.friction = Physics::Real(0.4);
            def.restitution = Physics::Real(0.15);
            return w.AddBody(def);
        }

        // =============================================================================
        // scene 0: "Playground" (the Task-4 sampler)
        // =============================================================================
        void BuildPlayground(Astra::Registry& reg)
        {
            Astra::Entity root = MakeRoot(reg);

            // Floor + 2 walls framing a drop zone.
            MakeBox(reg, root, glm::vec2(640.0f, 640.0f), glm::vec2(420.0f, 20.0f),
                    Physics::BodyType::Static, kStatic);
            MakeBox(reg, root, glm::vec2(240.0f, 480.0f), glm::vec2(20.0f, 160.0f),
                    Physics::BodyType::Static, kStatic);
            MakeBox(reg, root, glm::vec2(1040.0f, 480.0f), glm::vec2(20.0f, 160.0f),
                    Physics::BodyType::Static, kStatic);

            // 5 dynamics: 3 boxes + 2 circles.
            MakeBox(reg, root, glm::vec2(520.0f, 120.0f), glm::vec2(28.0f, 28.0f),
                    Physics::BodyType::Dynamic, kOrange);
            MakeBox(reg, root, glm::vec2(640.0f, 60.0f),  glm::vec2(34.0f, 22.0f),
                    Physics::BodyType::Dynamic, kBlue);
            MakeBox(reg, root, glm::vec2(760.0f, 100.0f), glm::vec2(24.0f, 24.0f),
                    Physics::BodyType::Dynamic, kGreen);
            MakeCircle(reg, root, glm::vec2(580.0f, 200.0f), 26.0f,
                       Physics::BodyType::Dynamic, kGold);
            MakeCircle(reg, root, glm::vec2(700.0f, 160.0f), 30.0f,
                       Physics::BodyType::Dynamic, kMagenta);
        }

        // =============================================================================
        // scene 1: "Box stack" -- a vertical stack of boxes settling stably.
        // =============================================================================
        void BuildBoxStack(Astra::Registry& reg)
        {
            Astra::Entity root = MakeRoot(reg);
            MakeFloor(reg, root);

            constexpr int   kCount   = 6;
            constexpr float kHalf    = 30.0f;   // box half-extent
            constexpr float kGap     = 2.0f;    // small seating gap between boxes
            constexpr float kTopSurf = 620.0f;  // floor top
            constexpr float kCenterX = 640.0f;

            // Stack from the floor up; box i center sits (2*half + gap) above the one below.
            for (int i = 0; i < kCount; ++i)
            {
                const float cy = kTopSurf - kHalf - i * (2.0f * kHalf + kGap);
                const glm::vec4 tint = (i % 2 == 0) ? kBlue : kOrange;
                MakeBox(reg, root, glm::vec2(kCenterX, cy), glm::vec2(kHalf, kHalf),
                        Physics::BodyType::Dynamic, tint, 0.0f, 0.6f);
            }
        }

        // =============================================================================
        // scene 2: "Pyramid" -- a triangular pile (friction / stress).
        // =============================================================================
        void BuildPyramid(Astra::Registry& reg)
        {
            Astra::Entity root = MakeRoot(reg);
            MakeFloor(reg, root);

            constexpr int   kRows    = 5;
            constexpr float kHalf    = 26.0f;
            constexpr float kSpacing = 2.0f * kHalf + 2.0f;  // column pitch
            constexpr float kTopSurf = 620.0f;
            constexpr float kCenterX = 640.0f;

            // Row r (0 = bottom) has (kRows - r) boxes, centered, stacked upward.
            for (int r = 0; r < kRows; ++r)
            {
                const int   count = kRows - r;
                const float rowY  = kTopSurf - kHalf - r * (2.0f * kHalf + 1.0f);
                const float startX = kCenterX - (count - 1) * 0.5f * kSpacing;
                for (int c = 0; c < count; ++c)
                {
                    const float cx = startX + c * kSpacing;
                    const glm::vec4 tint = ((r + c) % 2 == 0) ? kGreen : kTeal;
                    MakeBox(reg, root, glm::vec2(cx, rowY), glm::vec2(kHalf, kHalf),
                            Physics::BodyType::Dynamic, tint, 0.0f, 0.7f);
                }
            }
        }

        // =============================================================================
        // scene 3: "Joint chain" -- revolute + weld + distance + prismatic (path B).
        // =============================================================================
        // All bodies + joints are world-direct (joints need the BodyHandles up front). One
        // SceneRoot anchor entity is created so the scene has a root like every other scene;
        // the bodies render via the physics-debug overlay.
        void BuildJointChain(Astra::Registry& reg)
        {
            Astra::Entity root = MakeRoot(reg);
            // A decorative static "ceiling" sprite so RenderSubmissionSystem has something to
            // draw too (the moving jointed bodies render through the debug overlay).
            MakeBox(reg, root, glm::vec2(640.0f, 80.0f), glm::vec2(360.0f, 10.0f),
                    Physics::BodyType::Static, kStatic);

            Physics::PhysicsWorld* w = World(reg);
            if (!w) return;

            constexpr float kBobR = 16.0f;

            // --- REVOLUTE pendulum: a static hub + a bob pinned at the hub, swinging. ------
            const Physics::BodyHandle hub = WorldStaticBox(*w, glm::vec2(360.0f, 110.0f),
                                                           glm::vec2(8.0f, 8.0f));
            const Physics::BodyHandle bob = WorldCircle(*w, glm::vec2(360.0f, 230.0f), kBobR);
            {
                Physics::JointDef jd;
                jd.kind   = Physics::JointKind::Revolute;
                jd.a      = hub;
                jd.b      = bob;
                jd.anchor = Physics::Vec2(360.0f, 110.0f);
                w->AddJoint(jd);
            }

            // --- DISTANCE link: a second bob hangs from the same hub at a fixed length. -----
            const Physics::BodyHandle distBob = WorldCircle(*w, glm::vec2(440.0f, 220.0f), kBobR);
            {
                Physics::JointDef jd;
                jd.kind   = Physics::JointKind::Distance;
                jd.a      = hub;
                jd.b      = distBob;
                jd.length = Physics::Real(130);
                w->AddJoint(jd);
            }

            // --- WELD pair: a static post + a body rigidly welded to it (stays put). --------
            const Physics::BodyHandle post   = WorldStaticBox(*w, glm::vec2(760.0f, 110.0f),
                                                              glm::vec2(8.0f, 8.0f));
            const Physics::BodyHandle welded = WorldCircle(*w, glm::vec2(800.0f, 110.0f), kBobR);
            {
                Physics::JointDef jd;
                jd.kind   = Physics::JointKind::Weld;
                jd.a      = post;
                jd.b      = welded;
                jd.anchor = Physics::Vec2(800.0f, 110.0f);
                w->AddJoint(jd);
            }

            // --- PRISMATIC slider: a body constrained to a horizontal axis off the post. ----
            const Physics::BodyHandle slider = WorldCircle(*w, glm::vec2(880.0f, 110.0f), kBobR);
            {
                Physics::JointDef jd;
                jd.kind = Physics::JointKind::Prismatic;
                jd.a    = post;
                jd.b    = slider;
                jd.axis = Physics::Vec2(1.0f, 0.0f);   // horizontal slide axis
                w->AddJoint(jd);
            }
            // Nudge the slider along its axis so it visibly slides (no perp drift under gravity).
            w->ApplyImpulse(slider, Physics::Vec2(2500.0f, 0.0f));
        }

        // =============================================================================
        // scene 4: "Rotation drop" -- polygon boxes released at nonzero angles (path B).
        // =============================================================================
        // The Phase-A headline: rotation-aware narrowphase. Dynamic POLYGON boxes (NOT Aabb,
        // which would trip the fixedRotation invariant) are released tilted; they tumble and
        // settle flat on a wide floor. World-direct because the Astra Fixture descriptor
        // cannot author polygon verts.
        void BuildRotationDrop(Astra::Registry& reg)
        {
            Astra::Entity root = MakeRoot(reg);
            // Decorative floor sprite (the falling polygons render via the debug overlay).
            MakeBox(reg, root, glm::vec2(640.0f, 640.0f), glm::vec2(480.0f, 20.0f),
                    Physics::BodyType::Static, kStatic);

            Physics::PhysicsWorld* w = World(reg);
            if (!w) return;

            // World-direct static floor (the real collision surface) at the same place.
            WorldStaticBox(*w, glm::vec2(640.0f, 640.0f), glm::vec2(480.0f, 20.0f));

            // A few tilted polygon boxes at staggered heights + release angles.
            WorldPolygonBox(*w, glm::vec2(480.0f, 140.0f), glm::vec2(34.0f, 34.0f), 0.6f);
            WorldPolygonBox(*w, glm::vec2(620.0f, 90.0f),  glm::vec2(40.0f, 24.0f), -0.9f);
            WorldPolygonBox(*w, glm::vec2(760.0f, 160.0f), glm::vec2(30.0f, 30.0f), 1.2f);
            WorldPolygonBox(*w, glm::vec2(640.0f, 220.0f), glm::vec2(46.0f, 20.0f), -0.4f);
        }

        // =============================================================================
        // scene 5: "CCD bullet" -- a fast bullet fired at a thin wall (no tunnelling).
        // =============================================================================
        // A thin static wall + one fast bullet=true dynamic body fired at it. The speculative
        // margin + bullet sweep stop it at the wall instead of tunnelling through. Authored via
        // Astra components (RigidBody2D carries bullet + velocity; PhysicsSystem applies both).
        void BuildCcdBullet(Astra::Registry& reg)
        {
            Astra::Entity root = MakeRoot(reg);
            MakeFloor(reg, root);

            // A THIN tall static wall on the right -- the thing the bullet must not tunnel.
            MakeBox(reg, root, glm::vec2(960.0f, 540.0f), glm::vec2(6.0f, 90.0f),
                    Physics::BodyType::Static, kStatic);

            // The bullet: a small fast box fired right at the wall. fixedRotation (Aabb) +
            // bullet=true (enables the GJK-TOI CCD clamp) + a high +X authored velocity.
            Astra::Entity e = reg.CreateEntity();
            Arcane::LocalTransform lt; lt.position = glm::vec2(220.0f, 540.0f);
            reg.AddComponent<Arcane::LocalTransform>(e, lt);
            reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});

            Arcane::RigidBody2D rb;
            rb.type          = Physics::BodyType::Dynamic;
            rb.fixedRotation = true;
            rb.bullet        = true;                       // CCD: speculative + sweep clamp
            rb.velocity      = glm::vec2(6000.0f, 0.0f);   // fast enough to tunnel without CCD
            reg.AddComponent<Arcane::RigidBody2D>(e, rb);

            Arcane::Collider2D col;
            {
                Arcane::Fixture fx;
                fx.kind        = Physics::ShapeKind::Aabb;
                fx.halfW       = 10.0f;
                fx.halfH       = 10.0f;
                fx.density     = 4.0f;     // heavy: keeps momentum into the wall
                fx.friction    = 0.3f;
                fx.restitution = 0.0f;
                col.fixtures.push_back(fx);
            }
            reg.AddComponent<Arcane::Collider2D>(e, col);
            reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});

            Arcane::SpriteRenderer sp; sp.size = glm::vec2(20.0f); sp.tint = kGold;
            reg.AddComponent<Arcane::SpriteRenderer>(e, sp);
            reg.SetParent(e, root);
        }

        // =============================================================================
        // scene 6: "Compound bodies" -- multi-fixture bodies with an off-origin COM.
        // =============================================================================
        // A floor + a couple of two-fixture bodies: a centered box-ish circle + a heavy
        // offset circle, so the aggregate COM is off-origin and the body tips toward the heavy
        // side. Authored via Astra components (Collider2D fixture list -> AddBody + AddFixture).
        void BuildCompound(Astra::Registry& reg)
        {
            Astra::Entity root = MakeRoot(reg);
            MakeFloor(reg, root);

            // Helper: a dynamic body with a light central circle + a heavy offset circle.
            // The heavy lobe at +localX shifts the COM right, so the body tips that way.
            auto makeLopsided = [&](glm::vec2 pos, float heavySign, glm::vec4 tint)
            {
                Astra::Entity e = reg.CreateEntity();
                Arcane::LocalTransform lt; lt.position = pos;
                reg.AddComponent<Arcane::LocalTransform>(e, lt);
                reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});

                Arcane::RigidBody2D rb; rb.type = Physics::BodyType::Dynamic;  // free to rotate
                reg.AddComponent<Arcane::RigidBody2D>(e, rb);

                Arcane::Collider2D col;
                {
                    Arcane::Fixture core;            // light central lobe at the origin
                    core.kind = Physics::ShapeKind::Circle;
                    core.radius = 18.0f; core.density = 0.5f; core.friction = 0.5f;
                    col.fixtures.push_back(core);

                    Arcane::Fixture heavy;           // heavy lobe offset to one side
                    heavy.kind = Physics::ShapeKind::Circle;
                    heavy.radius = 22.0f; heavy.density = 4.0f; heavy.friction = 0.5f;
                    heavy.localPos = glm::vec2(heavySign * 40.0f, 0.0f);   // off-COM mass
                    col.fixtures.push_back(heavy);
                }
                reg.AddComponent<Arcane::Collider2D>(e, col);
                reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});
                reg.SetParent(e, root);

                // Per-fixture visuals: a disc for the light core + a disc for the
                // heavy lobe, each tracking its fixture (replaces the single flat
                // 124x44 rectangle that did not match the two-circle physics).
                AttachFixtureVisuals(reg, e, col.fixtures, tint);
            };

            makeLopsided(glm::vec2(520.0f, 200.0f),  1.0f, kOrange);  // tips right
            makeLopsided(glm::vec2(780.0f, 160.0f), -1.0f, kMagenta); // tips left
        }

        // =============================================================================
        // scene 7: "Mixed shapes" -- circles + capsules + polygons in a bowl.
        // =============================================================================
        // A floor + two angled walls form a shallow bowl; a pile of circles + capsules (path A)
        // and polygons (path B) drop in and interact, exercising the unified core+radius shape
        // model (every kind collides against every other through one narrowphase).
        void BuildMixedShapes(Astra::Registry& reg)
        {
            Astra::Entity root = MakeRoot(reg);

            // Bowl floor + two short angled-ish side walls (Aabb statics).
            MakeBox(reg, root, glm::vec2(640.0f, 640.0f), glm::vec2(360.0f, 20.0f),
                    Physics::BodyType::Static, kStatic);
            MakeBox(reg, root, glm::vec2(300.0f, 560.0f), glm::vec2(20.0f, 100.0f),
                    Physics::BodyType::Static, kStatic);
            MakeBox(reg, root, glm::vec2(980.0f, 560.0f), glm::vec2(20.0f, 100.0f),
                    Physics::BodyType::Static, kStatic);

            // Path-A round bodies.
            MakeCircle(reg, root, glm::vec2(560.0f, 120.0f), 24.0f,
                       Physics::BodyType::Dynamic, kGold);
            MakeCircle(reg, root, glm::vec2(700.0f, 90.0f),  20.0f,
                       Physics::BodyType::Dynamic, kBlue);
            MakeCapsule(reg, root, glm::vec2(620.0f, 200.0f), 30.0f, 16.0f,
                        Physics::BodyType::Dynamic, kTeal);
            MakeCapsule(reg, root, glm::vec2(760.0f, 240.0f), 24.0f, 14.0f,
                        Physics::BodyType::Dynamic, kGreen);

            // Path-B polygons (a tilted box + a triangle) sharing the same world.
            if (Physics::PhysicsWorld* w = World(reg))
            {
                WorldStaticBox(*w, glm::vec2(640.0f, 640.0f), glm::vec2(360.0f, 20.0f));
                WorldPolygonBox(*w, glm::vec2(600.0f, 300.0f), glm::vec2(30.0f, 22.0f), 0.5f);
                WorldTriangle(*w, glm::vec2(700.0f, 340.0f), 30.0f);
            }
        }

        // The static scene table -- the process-wide roster (8 scenes, Task 5).
        constexpr std::array<SceneDef, 8> kScenes = {{
            SceneDef{ "Playground",      &BuildPlayground   },
            SceneDef{ "Box stack",       &BuildBoxStack     },
            SceneDef{ "Pyramid",         &BuildPyramid      },
            SceneDef{ "Joint chain",     &BuildJointChain   },
            SceneDef{ "Rotation drop",   &BuildRotationDrop },
            SceneDef{ "CCD bullet",      &BuildCcdBullet    },
            SceneDef{ "Compound bodies", &BuildCompound     },
            SceneDef{ "Mixed shapes",    &BuildMixedShapes  },
        }};
    }

    std::span<const SceneDef> SceneRegistry()
    {
        return std::span<const SceneDef>(kScenes.data(), kScenes.size());
    }

    // -------------------------------------------------------------------------
    // Public spawn builders (Task 7). Thin forwarders onto the file-local Path-A
    // MakeBox/MakeCircle so the interaction layer reuses the EXACT body assembly
    // (no duplication). If `root` is null, resolve the SceneRoot resource entity
    // so a spawn always lands under the scene subtree.
    // -------------------------------------------------------------------------
    namespace
    {
        Astra::Entity ResolveRoot(Astra::Registry& reg, Astra::Entity root)
        {
            if (root != Astra::Entity{}) return root;
            if (auto* sr = reg.GetResource<Arcane::SceneRoot>()) return sr->entity;
            return root;   // null -> MakeBox/MakeCircle SetParent(e, null) is a no-op parent
        }
    }

    Astra::Entity SpawnBox(Astra::Registry& reg, Astra::Entity root,
                           glm::vec2 pos, glm::vec2 halfExtents,
                           Physics::BodyType type, glm::vec4 tint,
                           float density)
    {
        return MakeBox(reg, ResolveRoot(reg, root), pos, halfExtents, type, tint,
                       /*restitution=*/0.1f, /*friction=*/0.4f, density);
    }

    Astra::Entity SpawnCircle(Astra::Registry& reg, Astra::Entity root,
                              glm::vec2 pos, float radius,
                              Physics::BodyType type, glm::vec4 tint,
                              float density)
    {
        return MakeCircle(reg, ResolveRoot(reg, root), pos, radius, type, tint,
                          /*restitution=*/0.2f, density);
    }
}
