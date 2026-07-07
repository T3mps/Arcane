// Sandbox scene builders. NINE scenes, each showcasing one Physics v2 capability:
//   0 Playground      -- floor + 2 walls + mixed dynamics (the original sampler).
//   1 Box stack       -- a tall stack of boxes settling stably.
//   2 Pyramid         -- a triangular pile (friction / stress).
//   3 Joint chain     -- revolute + weld + distance + prismatic links.
//   4 Rotation drop   -- polygon boxes released at nonzero angles, settling flat.
//   5 CCD bullet      -- a fast bullet body fired at a thin wall (no tunnelling).
//   6 Compound bodies -- multi-fixture bodies with an off-origin COM that tip.
//   7 Mixed shapes    -- circles + capsules + polygons interacting in a bowl.
//   8 Stress test     -- brutal churn: kStressBodyCount mixed-shape dynamics (boxes,
//                        circles, capsules, n-gons, compounds) churned by ONE top-mounted
//                        kinematic balloon-whisk in a narrow bowl arena. Never settles.
//                        Procedural + seeded (reproducible).
//
// OUTLINE-UNIFY (Item A): the Sandbox draws EVERY body as an OUTLINE through the single
// canonical DrawPhysicsDebug overlay (collider outline + rich COM/velocity/orientation
// overlays). The builders therefore attach NO SpriteRenderer to physics bodies -- the
// filled-quad RenderSubmissionSystem path is unused by the sandbox now (it stays in the
// engine for other consumers). LARGER SCALE (Item C): geometry is sized up ~1.8x and the
// arenas spread past the 1280x720 canvas; the camera (interaction layer) zooms to fit.
//
// TWO BODY-AUTHORING PATHS (both feed the SAME PhysicsWorld owned by PhysicsResource):
//
//   (A) ASTRA COMPONENTS -- the default. An entity carries LocalTransform + WorldTransform +
//       RigidBody2D + Collider2D (a Fixture list) + PhysicsBodyRef. PhysicsSystem creates
//       the body on the first fixedUpdate and writes the post-step pose back into
//       LocalTransform. Used by scenes 0,1,2,5,6,8 and the circle/capsule bodies of 7.
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
//       Both paths render identically: the physics-debug overlay (PhysicsDebugRenderSystem ->
//       DrawPhysicsDebug) draws EVERY world body's outline (rotation-aware), regardless of how
//       the body was authored -- which is exactly the capability scenes 3/4/7/8 show off.
//
// Coordinate convention (matches the physics tests): +Y is DOWN, world unit == METER
// (MKS; the sandbox camera maps world->screen at 100 px/m). Floors sit low (larger +Y)
// and bodies drop onto them. All geometry below is authored in meters (bodies 0.1-10 m).

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
#include <numbers>
#include <random>
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

            // OUTLINE-UNIFY (Item A): no SpriteRenderer. Every sandbox body is
            // drawn by the canonical DrawPhysicsDebug outline overlay (collider
            // outline + rich COM/velocity/orientation overlays), not a filled
            // quad -- so the showcase reads consistently. `tint` is retained in
            // the signature for call-site compatibility but no longer authors a
            // sprite (the overlay colors by body type / island).
            (void)tint;

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

            // OUTLINE-UNIFY (Item A): no SpriteRenderer -- drawn by DrawPhysicsDebug.
            (void)tint;

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

            // OUTLINE-UNIFY (Item A): no SpriteRenderer -- drawn by DrawPhysicsDebug.
            (void)tint;

            reg.SetParent(e, root);
            return e;
        }

        // Path-A static floor (a wide thin box). Returns the floor entity. `topY`
        // is the y of the floor's TOP surface (bodies rest there).
        // LARGER SCALE (Item C): the default floor is wider + lower so the bigger
        // scenes have room to spread; the camera (next agent) zooms to fit.
        Astra::Entity MakeFloor(Astra::Registry& reg, Astra::Entity root,
                                float centerX = 6.4f, float topY = 8.0f,
                                float halfW = 8.8f, float halfH = 0.36f)
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

        // World-direct DYNAMIC regular convex n-gon (path B). `sides` in [3..8]; verts
        // placed on a circle of `radius` starting at `angle` radians. Density + friction
        // tuned for the stress scene (flowing, light mass). Sibling of WorldTriangle.
        Physics::BodyHandle WorldNgon(Physics::PhysicsWorld& w, glm::vec2 pos,
                                      float radius, int sides, float angle,
                                      float density = 0.08f)
        {
            sides = (sides < 3) ? 3 : (sides > 8 ? 8 : sides);
            std::vector<Physics::Vec2> verts;
            verts.reserve(static_cast<std::size_t>(sides));
            const float step = 2.0f * static_cast<float>(std::numbers::pi) / static_cast<float>(sides);
            for (int k = 0; k < sides; ++k)
            {
                const float a = angle + k * step;
                verts.push_back(Physics::Vec2(radius * std::cos(a), radius * std::sin(a)));
            }
            Physics::BodyDef def;
            def.type        = Physics::BodyType::Dynamic;
            def.position    = Physics::Vec2(pos.x, pos.y);
            def.shape       = Physics::MakePolygon(verts);
            def.density     = Physics::Real(density);
            def.friction    = Physics::Real(0.35f);
            def.restitution = Physics::Real(0.05f);
            return w.AddBody(def);
        }

        // World-direct KINEMATIC balloon-whisk agitator. ONE kinematic body at `center`
        // built from kLoops teardrop "wire" loops radiating at equal angular increments
        // (like a real balloon-whisk cage), plus a small central hub circle. Each loop
        // is approximated by thin segment-box polygons whose verts are expressed in
        // BODY-LOCAL frame (relative to `center`). The whisk spins at `omega` rad/s.
        // With radius=5.6 and omega=1.9 the tip speed is ~10.64 m/s; at 60 Hz each
        // wire advances ~0.177 m per step, below wire diameter 0.30 m -- no tunnelling.
        Physics::BodyHandle Whisk(Physics::PhysicsWorld& w, glm::vec2 center,
                                  float radius, float omega)
        {
            constexpr int   kLoops    = 6;
            constexpr float kWireHalf = 0.15f;  // half-thickness of each wire segment (m)
                                                // (thicker -> pushes harder + tunnel-safe at high omega)
                                                // tip speed 10.64 m/s -> 0.177 m/step < diameter 0.30 m

            const float R    = radius;
            const float W    = R * 0.30f;       // teardrop loop half-width
            const float hubR = R * 0.12f;       // central hub radius

            // First fixture is the hub circle (placed at the body origin = center).
            Physics::BodyDef def;
            def.type     = Physics::BodyType::Kinematic;
            def.position = Physics::Vec2(center.x, center.y);
            def.shape    = Physics::MakeCircle(Physics::Real(hubR));
            def.friction = Physics::Real(0.3f);
            Physics::BodyHandle h = w.AddBody(def);

            // Lambda: add one thin-box segment as a fixture (verts already in local frame).
            auto AddSegment = [&](glm::vec2 P, glm::vec2 Q)
            {
                glm::vec2 d = Q - P;
                const float len = std::sqrt(d.x * d.x + d.y * d.y);
                if (len < 0.01f) return;   // degenerate (< 1 cm) -- skip
                d.x /= len; d.y /= len;
                const glm::vec2 perp{ -d.y, d.x };

                // Extend ends by kWireHalf so adjacent segments overlap and close gaps.
                const glm::vec2 P0 = P - d * kWireHalf;
                const glm::vec2 Q0 = Q + d * kWireHalf;

                const std::vector<Physics::Vec2> verts = {
                    Physics::Vec2(P0.x + perp.x * kWireHalf, P0.y + perp.y * kWireHalf),
                    Physics::Vec2(Q0.x + perp.x * kWireHalf, Q0.y + perp.y * kWireHalf),
                    Physics::Vec2(Q0.x - perp.x * kWireHalf, Q0.y - perp.y * kWireHalf),
                    Physics::Vec2(P0.x - perp.x * kWireHalf, P0.y - perp.y * kWireHalf),
                };
                Physics::FixtureDef fd;
                fd.shape       = Physics::MakePolygon(verts);
                fd.friction    = Physics::Real(0.3f);
                fd.restitution = Physics::Real(0.0f);
                w.AddFixture(h, fd);
            };

            const float kTau = 2.0f * static_cast<float>(std::numbers::pi);

            for (int k = 0; k < kLoops; ++k)
            {
                const float phi = k * (kTau / static_cast<float>(kLoops));
                const float cp  = std::cos(phi);
                const float sp  = std::sin(phi);

                // Rotate a 2D point by phi.
                auto Rot = [&](float x, float y) -> glm::vec2
                {
                    return { x * cp - y * sp, x * sp + y * cp };
                };

                // Teardrop outline points in the body frame (unrotated, along local +x).
                // H->T1->T2->TIP->B2->B1->H (6 segments, closing the loop).
                const glm::vec2 H   = Rot(hubR,         0.0f);
                const glm::vec2 T1  = Rot(R * 0.45f,  -W);
                const glm::vec2 T2  = Rot(R * 0.85f,  -W * 0.55f);
                const glm::vec2 TIP = Rot(R,            0.0f);
                const glm::vec2 B2  = Rot(R * 0.85f,  +W * 0.55f);
                const glm::vec2 B1  = Rot(R * 0.45f,  +W);

                AddSegment(H,   T1);
                AddSegment(T1,  T2);
                AddSegment(T2,  TIP);
                AddSegment(TIP, B2);
                AddSegment(B2,  B1);
                AddSegment(B1,  H);
            }

            w.SetAngVelSlot(h.index, Physics::Real(omega));
            return h;
        }

        // =============================================================================
        // scene 0: "Playground" (the Task-4 sampler)
        // =============================================================================
        void BuildPlayground(Astra::Registry& reg)
        {
            Astra::Entity root = MakeRoot(reg);

            // LARGER SCALE (Item C): a wide deep drop-zone framed by tall walls.
            // The next agent adds zoom, so the world geometry can spread well
            // past the 1280x720 canvas -- bigger reads better at zoom-to-fit.
            // Floor + 2 walls framing the arena.
            MakeBox(reg, root, glm::vec2(6.4f, 8.2f), glm::vec2(7.6f, 0.36f),
                    Physics::BodyType::Static, kStatic);
            MakeBox(reg, root, glm::vec2(-0.8f, 5.6f), glm::vec2(0.36f, 3.0f),
                    Physics::BodyType::Static, kStatic);
            MakeBox(reg, root, glm::vec2(13.6f, 5.6f), glm::vec2(0.36f, 3.0f),
                    Physics::BodyType::Static, kStatic);

            // 5 dynamics: 3 boxes + 2 circles (meter-scale variety preserved).
            MakeBox(reg, root, glm::vec2(4.4f, 1.2f), glm::vec2(0.54f, 0.54f),
                    Physics::BodyType::Dynamic, kOrange);
            MakeBox(reg, root, glm::vec2(6.4f, 0.4f),  glm::vec2(0.66f, 0.42f),
                    Physics::BodyType::Dynamic, kBlue);
            MakeBox(reg, root, glm::vec2(8.6f, 0.9f),  glm::vec2(0.46f, 0.46f),
                    Physics::BodyType::Dynamic, kGreen);
            MakeCircle(reg, root, glm::vec2(5.4f, 2.6f), 0.50f,
                       Physics::BodyType::Dynamic, kGold);
            MakeCircle(reg, root, glm::vec2(7.6f, 2.0f), 0.58f,
                       Physics::BodyType::Dynamic, kMagenta);
        }

        // =============================================================================
        // scene 1: "Box stack" -- a vertical stack of boxes settling stably.
        // =============================================================================
        void BuildBoxStack(Astra::Registry& reg)
        {
            Astra::Entity root = MakeRoot(reg);
            MakeFloor(reg, root);

            // LARGER SCALE (Item C): a taller stack of bigger boxes.
            constexpr int   kCount   = 8;
            constexpr float kHalf    = 0.52f;   // box half-extent (m)
            constexpr float kGap     = 0.03f;   // small seating gap between boxes (m)
            constexpr float kTopSurf = 8.0f;    // floor top (matches MakeFloor)
            constexpr float kCenterX = 6.4f;

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

            // LARGER SCALE (Item C): a taller, wider pyramid of bigger boxes.
            constexpr int   kRows    = 6;
            constexpr float kHalf    = 0.46f;
            constexpr float kSpacing = 2.0f * kHalf + 0.03f;  // column pitch (m)
            constexpr float kTopSurf = 8.0f;
            constexpr float kCenterX = 6.4f;

            // Row r (0 = bottom) has (kRows - r) boxes, centered, stacked upward.
            for (int r = 0; r < kRows; ++r)
            {
                const int   count = kRows - r;
                const float rowY  = kTopSurf - kHalf - r * (2.0f * kHalf + 0.02f);
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
            // A static "ceiling" anchor body (drawn as an outline by the overlay,
            // like every other body now). LARGER SCALE (Item C): wider + thicker.
            MakeBox(reg, root, glm::vec2(6.4f, 0.9f), glm::vec2(5.6f, 0.18f),
                    Physics::BodyType::Static, kStatic);

            Physics::PhysicsWorld* w = World(reg);
            if (!w) return;

            // Meter-scale bobs + drops + spacing (variety preserved).
            constexpr float kBobR = 0.28f;

            // --- REVOLUTE pendulum: a static hub + a bob pinned at the hub, swinging. ------
            const Physics::BodyHandle hub = WorldStaticBox(*w, glm::vec2(3.6f, 1.3f),
                                                           glm::vec2(0.14f, 0.14f));
            const Physics::BodyHandle bob = WorldCircle(*w, glm::vec2(3.6f, 3.6f), kBobR);
            {
                Physics::JointDef jd;
                jd.kind   = Physics::JointKind::Revolute;
                jd.a      = hub;
                jd.b      = bob;
                jd.anchor = Physics::Vec2(3.6f, 1.3f);
                w->AddJoint(jd);
            }

            // --- DISTANCE link: a second bob hangs from the same hub at a fixed length. -----
            const Physics::BodyHandle distBob = WorldCircle(*w, glm::vec2(5.2f, 3.4f), kBobR);
            {
                Physics::JointDef jd;
                jd.kind   = Physics::JointKind::Distance;
                jd.a      = hub;
                jd.b      = distBob;
                jd.length = Physics::Real(2.3);
                w->AddJoint(jd);
            }

            // --- WELD pair: a static post + a body rigidly welded to it (stays put). --------
            const Physics::BodyHandle post   = WorldStaticBox(*w, glm::vec2(8.2f, 1.3f),
                                                              glm::vec2(0.14f, 0.14f));
            const Physics::BodyHandle welded = WorldCircle(*w, glm::vec2(8.9f, 1.3f), kBobR);
            {
                Physics::JointDef jd;
                jd.kind   = Physics::JointKind::Weld;
                jd.a      = post;
                jd.b      = welded;
                jd.anchor = Physics::Vec2(8.9f, 1.3f);
                w->AddJoint(jd);
            }

            // --- PRISMATIC slider: a body constrained to a horizontal axis off the post. ----
            const Physics::BodyHandle slider = WorldCircle(*w, glm::vec2(10.0f, 1.3f), kBobR);
            {
                Physics::JointDef jd;
                jd.kind = Physics::JointKind::Prismatic;
                jd.a    = post;
                jd.b    = slider;
                jd.axis = Physics::Vec2(1.0f, 0.0f);   // horizontal slide axis
                w->AddJoint(jd);
            }
            // Nudge the slider along its axis so it visibly slides (no perp drift under gravity).
            // Authored as mass * delta-v (protocol rule 3), NOT a /100 of the old px impulse:
            // the px impulse (8000) on a ~2463-unit-mass bob implied dv ~ 3.3 px/s, which at
            // meters is ~0.033 m/s -- invisible. kNudgeDv gives a clearly visible slide, well
            // under the 400 m/s cap. WorldCircle uses density 1, so mass = 1 * pi * r^2.
            constexpr Physics::Real kNudgeDv = Physics::Real(1.5);   // target slide speed (m/s)
            const Physics::Real     kPi      = static_cast<Physics::Real>(std::numbers::pi);
            const Physics::Real     bobMass  = Physics::Real(1) * kPi * kBobR * kBobR;  // density 1
            w->ApplyImpulse(slider, Physics::Vec2(bobMass * kNudgeDv, Physics::Real(0)));
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

            Physics::PhysicsWorld* w = World(reg);
            if (!w) return;

            // World-direct static floor (the real collision surface). LARGER SCALE
            // (Item C): a wide low floor the tilted polygons settle flat on. The
            // overlay draws it (and every body) as an outline.
            WorldStaticBox(*w, glm::vec2(6.4f, 8.2f), glm::vec2(8.2f, 0.36f));

            // A few tilted polygon boxes at staggered heights + release angles
            // (angles in radians, unchanged; the tumble reads at meter scale).
            WorldPolygonBox(*w, glm::vec2(4.2f, 1.6f), glm::vec2(0.58f, 0.58f), 0.6f);
            WorldPolygonBox(*w, glm::vec2(6.2f, 0.8f), glm::vec2(0.70f, 0.42f), -0.9f);
            WorldPolygonBox(*w, glm::vec2(8.2f, 2.0f), glm::vec2(0.52f, 0.52f), 1.2f);
            WorldPolygonBox(*w, glm::vec2(6.4f, 3.2f), glm::vec2(0.80f, 0.34f), -0.4f);
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

            // A THIN tall static wall on the right -- the thing the bullet must not
            // tunnel. LARGER SCALE (Item C): taller wall, further away, still thin.
            MakeBox(reg, root, glm::vec2(11.2f, 6.6f), glm::vec2(0.09f, 1.5f),
                    Physics::BodyType::Static, kStatic);

            // The bullet: a fast box fired right at the wall. fixedRotation (Aabb) +
            // bullet=true (enables the GJK-TOI CCD clamp) + a high +X authored velocity.
            Astra::Entity e = reg.CreateEntity();
            Arcane::LocalTransform lt; lt.position = glm::vec2(1.2f, 6.6f);
            reg.AddComponent<Arcane::LocalTransform>(e, lt);
            reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});

            Arcane::RigidBody2D rb;
            rb.type          = Physics::BodyType::Dynamic;
            rb.fixedRotation = true;
            rb.bullet        = true;                     // CCD: speculative + sweep clamp
            rb.velocity      = glm::vec2(70.0f, 0.0f);   // ~70 m/s: per-step travel 1.17 m >>
                                                         // 0.18 m wall -> tunnels without CCD
            reg.AddComponent<Arcane::RigidBody2D>(e, rb);

            Arcane::Collider2D col;
            {
                Arcane::Fixture fx;
                fx.kind        = Physics::ShapeKind::Aabb;
                fx.halfW       = 0.18f;
                fx.halfH       = 0.18f;
                fx.density     = 4.0f;     // heavy: keeps momentum into the wall
                fx.friction    = 0.3f;
                fx.restitution = 0.0f;
                col.fixtures.push_back(fx);
            }
            reg.AddComponent<Arcane::Collider2D>(e, col);
            reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});

            // OUTLINE-UNIFY (Item A): no SpriteRenderer -- the bullet is drawn by
            // DrawPhysicsDebug (its velocity ray makes the fast shot read clearly).
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
                    // LARGER SCALE (Item C): bigger lobes + a bigger COM offset.
                    Arcane::Fixture core;            // light central lobe at the origin
                    core.kind = Physics::ShapeKind::Circle;
                    core.radius = 0.34f; core.density = 0.5f; core.friction = 0.5f;
                    col.fixtures.push_back(core);

                    Arcane::Fixture heavy;           // heavy lobe offset to one side
                    heavy.kind = Physics::ShapeKind::Circle;
                    heavy.radius = 0.42f; heavy.density = 4.0f; heavy.friction = 0.5f;
                    heavy.localPos = glm::vec2(heavySign * 0.74f, 0.0f);   // off-COM mass
                    col.fixtures.push_back(heavy);
                }
                reg.AddComponent<Arcane::Collider2D>(e, col);
                reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});
                reg.SetParent(e, root);

                // OUTLINE-UNIFY (Item A): no per-fixture sprites. DrawPhysicsDebug
                // draws the body's collider outline + a COM marker (which makes
                // the off-origin compound COM visible -- the whole point of this
                // scene), so the body reads correctly without filled discs.
                (void)tint;
            };

            makeLopsided(glm::vec2(4.6f, 2.4f),  1.0f, kOrange);  // tips right
            makeLopsided(glm::vec2(8.4f, 1.8f), -1.0f, kMagenta); // tips left
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

            // Bowl floor + two tall side walls (Aabb statics). LARGER SCALE (Item C):
            // a wide deep bowl so the mixed pile has room to interact.
            MakeBox(reg, root, glm::vec2(6.4f, 8.2f), glm::vec2(5.6f, 0.36f),
                    Physics::BodyType::Static, kStatic);
            MakeBox(reg, root, glm::vec2(1.2f, 6.8f), glm::vec2(0.36f, 1.8f),
                    Physics::BodyType::Static, kStatic);
            MakeBox(reg, root, glm::vec2(11.6f, 6.8f), glm::vec2(0.36f, 1.8f),
                    Physics::BodyType::Static, kStatic);

            // Path-A round bodies (circles + capsules, meter-scale variety).
            MakeCircle(reg, root, glm::vec2(5.2f, 1.4f), 0.44f,
                       Physics::BodyType::Dynamic, kGold);
            MakeCircle(reg, root, glm::vec2(7.6f, 0.9f), 0.36f,
                       Physics::BodyType::Dynamic, kBlue);
            MakeCapsule(reg, root, glm::vec2(6.0f, 2.6f), 0.56f, 0.28f,
                        Physics::BodyType::Dynamic, kTeal);
            MakeCapsule(reg, root, glm::vec2(8.2f, 3.2f), 0.44f, 0.24f,
                        Physics::BodyType::Dynamic, kGreen);

            // Path-B polygons (a tilted box + a triangle) sharing the same world.
            if (Physics::PhysicsWorld* w = World(reg))
            {
                WorldStaticBox(*w, glm::vec2(6.4f, 8.2f), glm::vec2(5.6f, 0.36f));
                WorldPolygonBox(*w, glm::vec2(5.8f, 4.0f), glm::vec2(0.54f, 0.40f), 0.5f);
                WorldTriangle(*w, glm::vec2(7.2f, 4.6f), 0.54f);
            }
        }

        // =============================================================================
        // scene 8: "Stress test" -- brutal churn (kStressBodyCount mixed shapes).
        // =============================================================================
        // A WIDE BOWL arena with kStressBodyCount procedurally-generated dynamic
        // bodies (boxes, circles, capsules, n-gons, compounds), continuously churned
        // by ONE top-mounted kinematic balloon-whisk so the pile NEVER settles.
        // All world-direct (path B) for the complex shapes; path-A Astra components
        // for the simpler box/circle/capsule bodies. Both paths share the same world
        // and render through the canonical DrawPhysicsDebug outline overlay.
        //
        // BOWL SHAPE (meters, /100 of the old px arena):
        //   Center x = 6.4 m. Inner width ~17.2 m (left inner x=-2.2, right inner x=15.0).
        //   Two angled bottom-corner fillets (45-deg static boxes) slide bodies toward
        //   center so the single whisk reaches all contents.
        //
        // WHISK: ONE kinematic balloon-whisk body (kStressWhiskCount=1), center at
        //   (6.4, 3.6) m, radius 5.6 m, omega 1.9 rad/s.
        //   Tip speed = 1.9 * 5.6 = 10.64 m/s; per step at 60 Hz = 10.64/60 ~ 0.177 m.
        //   Wire half-thickness kWireHalf=0.15 -> diameter 0.30 m > 0.177 m -- no tunnelling.
        //
        // STABILITY DESIGN:
        //   pitch = kMaxBodyHalf*2 + margin  => no deep initial overlap (no explosion)
        //   whisk tip speed ~10.64 m/s at 60 Hz => < wire diameter 0.30 m per step
        //   low density (0.07-0.08) + low friction (0.35) => mass flows, churns
        //   seeded std::mt19937 => identical layout every build (deterministic tests)
        //
        // `bodyCount` is the number of procedural dynamic bodies. The registered
        // roster scene + Loom perf showcase pass kStressBodyCount (10000); the
        // unit-test stability check passes kStressStabilityBodyCount (1200) via the
        // public BuildStressTestN forwarder so its spawn column fits the test bound.
        // The arena/whisk/seed are identical regardless of count.
        void BuildStressTestImpl(Astra::Registry& reg, int bodyCount)
        {
            Astra::Entity root = MakeRoot(reg);

            // ---- tuning constants (anon-ns only; knobs exposed via Scenes.hpp) --------
            constexpr unsigned int kStressSeed  = 0xCAFEBABEu;  // seeded RNG
            constexpr float kWhiskRadius        = 5.6f;          // balloon-whisk cage radius (m)
            constexpr float kWhiskOmega         = 1.9f;          // rad/s (CCW) -- hard churn
            // ^ tip speed = omega*radius = 1.9*5.6 = 10.64 m/s (~0.177 m/60Hz step), under
            //   the wire diameter (2*kWireHalf=0.30) so the cage still cannot tunnel.
            constexpr float kWhiskCenterX       = 6.4f;
            constexpr float kWhiskCenterY       = 3.6f;          // upper-bowl, hangs into pile

            // Body size range (half-extent / radius / etc.) in meters.
            constexpr float kMinBodyHalf = 0.18f;
            constexpr float kMaxBodyHalf = 0.32f;

            // Grid pitch: >= max diameter + margin so there is no deep initial overlap.
            constexpr float kPitch = kMaxBodyHalf * 2.0f + 0.18f; // 0.82 m

            // Bowl arena bounds (meters, /100 of the old px arena).
            constexpr float kFloorY     = 8.8f;      // floor top surface y
            constexpr float kFloorHalfW = 8.8f;      // covers inner half-width + wall overlap
            constexpr float kFloorHalfH = 0.4f;
            constexpr float kWallHalfW  = 0.5f;      // thick walls
            constexpr float kWallHalfH  = 10.0f;     // tall enough to hold the pile
            constexpr float kArenaLeft  = -2.2f;     // inner left edge (x)
            constexpr float kArenaRight = 15.0f;     // inner right edge (x)
            constexpr float kArenaInnerW = kArenaRight - kArenaLeft;  // 17.2 m
            constexpr float kArenaCenterX = (kArenaLeft + kArenaRight) * 0.5f; // 6.4 m

            // Grid spawn parameters: cols span the bowl inner width.
            const int   kCols        = std::max(1, static_cast<int>(kArenaInnerW / kPitch));
            constexpr float kSpawnBottom = kFloorY - kPitch; // first row y (above floor)
            const float kSpawnLeft   = kArenaLeft;

            // ---- arena: floor + 2 tall walls + 2 angled corner fillets -------------
            Physics::PhysicsWorld* w = World(reg);
            if (!w) return;

            // Floor.
            WorldStaticBox(*w, glm::vec2(kArenaCenterX, kFloorY + kFloorHalfH),
                           glm::vec2(kFloorHalfW, kFloorHalfH));

            // Left and right walls (outer face flush with inner edge of bowl).
            WorldStaticBox(*w, glm::vec2(kArenaLeft - kWallHalfW, kFloorY - kWallHalfH),
                           glm::vec2(kWallHalfW, kWallHalfH));
            WorldStaticBox(*w, glm::vec2(kArenaRight + kWallHalfW, kFloorY - kWallHalfH),
                           glm::vec2(kWallHalfW, kWallHalfH));

            // Angled bottom-corner fillets (~45 degrees) to funnel bodies toward center.
            // Each is a static rotated box: half-extents 2.5 x 0.36 m, pivoted 45 deg inward.
            // Verts are in BODY-LOCAL frame (relative to the body position) per the
            // MakePolygon convention (same as WorldPolygonBox above).
            {
                constexpr float kFiltHalfL = 2.5f;   // half-length along the slope (m)
                constexpr float kFiltHalfT = 0.36f;  // half-thickness (m)
                constexpr float kFiltAngle = static_cast<float>(std::numbers::pi) * 0.25f; // 45 deg

                // Helper: build one fillet body. `cx/cy` = body center (world). `angle` = rotation.
                auto MakeFillet = [&](float cx, float cy, float angle)
                {
                    const float ca = std::cos(angle);
                    const float sa = std::sin(angle);
                    // Rotated box corners in LOCAL body frame.
                    const std::vector<Physics::Vec2> v = {
                        Physics::Vec2((-kFiltHalfL)*ca - (-kFiltHalfT)*sa,
                                      (-kFiltHalfL)*sa + (-kFiltHalfT)*ca),
                        Physics::Vec2(( kFiltHalfL)*ca - (-kFiltHalfT)*sa,
                                      ( kFiltHalfL)*sa + (-kFiltHalfT)*ca),
                        Physics::Vec2(( kFiltHalfL)*ca - ( kFiltHalfT)*sa,
                                      ( kFiltHalfL)*sa + ( kFiltHalfT)*ca),
                        Physics::Vec2((-kFiltHalfL)*ca - ( kFiltHalfT)*sa,
                                      (-kFiltHalfL)*sa + ( kFiltHalfT)*ca),
                    };
                    Physics::BodyDef fd;
                    fd.type     = Physics::BodyType::Static;
                    fd.position = Physics::Vec2(cx, cy);
                    fd.shape    = Physics::MakePolygon(v);
                    fd.friction = Physics::Real(0.4f);
                    w->AddBody(fd);
                };

                // Bottom-left fillet: tilts up-right (+45 deg). Center ~ (kArenaLeft+1.44, kFloorY-1.44).
                MakeFillet(kArenaLeft  + 1.44f, kFloorY - 1.44f,  kFiltAngle);
                // Bottom-right fillet: tilts up-left (-45 deg). Center ~ (kArenaRight-1.44, kFloorY-1.44).
                MakeFillet(kArenaRight - 1.44f, kFloorY - 1.44f, -kFiltAngle);
            }

            // ---- whisk: ONE top-mounted kinematic balloon-whisk (kStressWhiskCount=1) --
            Whisk(*w, glm::vec2(kWhiskCenterX, kWhiskCenterY), kWhiskRadius, kWhiskOmega);

            // ---- procedural body generator: one loop, seeded RNG -------------------
            std::mt19937 rng(kStressSeed);

            // Distributions.
            std::uniform_real_distribution<float> distJitter(-kPitch * 0.25f, kPitch * 0.25f);
            std::uniform_real_distribution<float> distSize(kMinBodyHalf, kMaxBodyHalf);
            std::uniform_real_distribution<float> distAngle(0.0f, 6.2832f);
            std::uniform_int_distribution<int>    distTint(0, 5);
            std::uniform_int_distribution<int>    distWeight(0, 99);
            std::uniform_int_distribution<int>    distNSides(0, 2); // {3,5,6}

            const glm::vec4 palette[6] = { kOrange, kBlue, kGreen, kGold, kMagenta, kTeal };
            const int kNGonSides[3]   = { 3, 5, 6 };

            for (int i = 0; i < bodyCount; ++i)
            {
                // 1. Grid position + jitter.
                const int   row = i / kCols;
                const int   col = i % kCols;
                const float jx  = distJitter(rng);
                const float jy  = distJitter(rng);
                const float px  = kSpawnLeft + (col + 0.5f) * kPitch + jx;
                const float py  = kSpawnBottom - row * kPitch + jy;

                // 2. Random properties.
                const float       sz    = distSize(rng);
                const glm::vec4   tint  = palette[distTint(rng)];
                const float       angle = distAngle(rng);

                // 3. Weighted shape pick: Box 25, Circle 20, Capsule 20, Polygon 25, Compound 10.
                const int w_val = distWeight(rng);
                const glm::vec2 pos(px, py);

                if (w_val < 25)
                {
                    // Box (path-A Aabb). MakeBox takes halfExtents.
                    // Mix some long/thin and some square-ish boxes.
                    const float hw = sz;
                    const float hh = sz * (0.5f + 0.5f * (static_cast<float>(i % 3) / 2.0f));
                    MakeBox(reg, root, pos, glm::vec2(hw, hh),
                            Physics::BodyType::Dynamic, tint,
                            /*restitution=*/0.05f, /*friction=*/0.35f, /*density=*/0.07f);
                }
                else if (w_val < 45)
                {
                    // Circle (path-A).
                    MakeCircle(reg, root, pos, sz,
                               Physics::BodyType::Dynamic, tint,
                               /*restitution=*/0.1f, /*density=*/0.07f);
                }
                else if (w_val < 65)
                {
                    // Capsule (path-A). halfLen ~ sz*0.6, radius ~ sz*0.4.
                    const float halfLen = sz * 0.6f;
                    const float radius  = sz * 0.4f;
                    MakeCapsule(reg, root, pos, halfLen, radius,
                                Physics::BodyType::Dynamic, tint);
                }
                else if (w_val < 90)
                {
                    // N-gon (world-direct polygon, path B).
                    const int sides = kNGonSides[distNSides(rng)];
                    WorldNgon(*w, pos, sz, sides, angle, /*density=*/0.08f);
                }
                else
                {
                    // Compound (world-direct 2-fixture body: a circle + offset lobe).
                    // The off-origin lobe shifts the COM so the body tips and tumbles.
                    Physics::BodyDef cdef;
                    cdef.type        = Physics::BodyType::Dynamic;
                    cdef.position    = Physics::Vec2(pos.x, pos.y);
                    cdef.shape       = Physics::MakeCircle(Physics::Real(sz * 0.55f));
                    cdef.density     = Physics::Real(0.07f);
                    cdef.friction    = Physics::Real(0.35f);
                    cdef.restitution = Physics::Real(0.05f);
                    Physics::BodyHandle ch = w->AddBody(cdef);

                    // Offset lobe: smaller circle at (+sz*0.8, 0) in body frame.
                    Physics::FixtureDef lfd;
                    lfd.shape       = Physics::MakeCircle(Physics::Real(sz * 0.35f));
                    lfd.localPos    = Physics::Vec2(sz * 0.8f, 0.0f);
                    lfd.density     = Physics::Real(0.07f);
                    lfd.friction    = Physics::Real(0.35f);
                    lfd.restitution = Physics::Real(0.05f);
                    w->AddFixture(ch, lfd);

                    (void)tint; // outline-only; tint unused
                }
            }
        }

        // The registered roster / Loom-perf entry: full kStressBodyCount churn.
        void BuildStressTest(Astra::Registry& reg)
        {
            BuildStressTestImpl(reg, kStressBodyCount);
        }

        // The static scene table -- the process-wide roster (9 scenes; the 9th is
        // the Item-B stress test).
        constexpr std::array<SceneDef, 9> kScenes = {{
            SceneDef{ "Playground",      &BuildPlayground   },
            SceneDef{ "Box stack",       &BuildBoxStack     },
            SceneDef{ "Pyramid",         &BuildPyramid      },
            SceneDef{ "Joint chain",     &BuildJointChain   },
            SceneDef{ "Rotation drop",   &BuildRotationDrop },
            SceneDef{ "CCD bullet",      &BuildCcdBullet    },
            SceneDef{ "Compound bodies", &BuildCompound     },
            SceneDef{ "Mixed shapes",    &BuildMixedShapes  },
            SceneDef{ "Stress test",     &BuildStressTest   },
        }};
    }

    std::span<const SceneDef> SceneRegistry()
    {
        return std::span<const SceneDef>(kScenes.data(), kScenes.size());
    }

    // Public forwarder onto the file-local BuildStressTestImpl so a test (or any
    // caller) can build the stress scene at a chosen body count without touching
    // the registered scene 8 / Loom perf knob (which stays at kStressBodyCount).
    void BuildStressTestN(Astra::Registry& reg, int bodyCount)
    {
        BuildStressTestImpl(reg, bodyCount);
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

    Astra::Entity SpawnCapsule(Astra::Registry& reg, Astra::Entity root,
                               glm::vec2 pos, float radius,
                               Physics::BodyType type, glm::vec4 tint,
                               float density)
    {
        // Upright pill: radius = end-cap radius, halfLen = radius (VERTICAL segment).
        // total height = 2*halfLen + 2*radius = 4*radius (clean 2:1 pill).
        // MakeCapsule takes a HORIZONTAL segment (-halfLen,0)-(+halfLen,0), so an
        // upright pill is authored with the segment along Y; the fixture convention
        // here matches MakeCapsule's (halfLen, radius) signature.
        // OUTLINE-UNIFY: no SpriteRenderer -- drawn by DrawPhysicsDebug.
        (void)tint;

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
            fx.halfLen     = radius;   // upright pill: segment half-length == end-cap radius
            fx.radius      = radius;
            fx.density     = density;
            fx.friction    = 0.4f;
            fx.restitution = 0.1f;
            col.fixtures.push_back(fx);
        }
        reg.AddComponent<Arcane::Collider2D>(e, col);
        reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});

        reg.SetParent(e, ResolveRoot(reg, root));
        return e;
    }
}
