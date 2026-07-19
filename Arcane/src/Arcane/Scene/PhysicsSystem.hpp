#pragma once

// PhysicsSystem (M6 Physics-v2 T6): Astra fixed-update system that drives the
// PhysicsWorld from the ECS and writes results back to LocalTransform components.
//
// Also defines PhysicsResource: the Registry resource (singleton) holding the
// PhysicsWorld and the entity<->BodyHandle map. PhysicsResource lives here
// (not in SceneResources.hpp) because it pulls in Core headers that game-plugin
// consumers (PlaygroundGame.dll, future Game.dll) cannot include directly --
// they link Arcane.dll and have no Core in their include path. Any code that
// uses PhysicsSystem already has Core in scope.
//
// Pass ordering inside operator() (each invocation = one fixed step):
//
//   1. DESTROY PASS -- walk entityToBody; for any entry whose entity is no
//      longer alive (reg.IsValid returns false) or no longer has RigidBody2D,
//      call world->RemoveBody(handle) and erase the map entry. Stale handles
//      are collected first to keep iteration safe.
//
//   2. CREATE/SYNC PASS -- for each entity with RigidBody2D + Collider2D +
//      PhysicsBodyRef + LocalTransform whose PhysicsBodyRef.handle == kInvalidBody:
//      - Build a BodyDef from RigidBody2D + fixtures[0] (shape + material).
//      - Call world->AddBody(def) to create the body with the primary fixture.
//      - For each subsequent fixture (fixtures[1..N-1]) call world->AddFixture
//        with a FixtureDef built from the Fixture descriptor (shape, localPos,
//        localAngle, material, filter, isSensor).
//      - Store the BodyHandle in PhysicsBodyRef and entityToBody.
//      Entities with an empty fixtures list are skipped (no body to create).
//
//   2.5 CAPTURE PREVIOUS POSES (Epic 04.2, opt-in) -- if PhysicsInterpBuffer is
//      present as a resource, snapshot every live body's PRE-STEP world pose
//      into it (indexed by PhysicsWorld body slot). Skipped when paused
//      (stepWorld=false). Consumed by DrawPhysicsDebug for the debug overlay.
//
//   3. STEP -- world->Step(m_fixedDt). Physics advances one fixed tick.
//
//   4. WRITE-BACK -- for each tracked entity:
//      - (Epic 04.2, opt-in) if the entity carries a PreviousTransform, stash
//        the about-to-be-overwritten LocalTransform pose into it first, so
//        RenderSubmissionSystem can lerp prev -> current by render alpha.
//      - Then write:
//        world->Position(handle) -> LocalTransform.position
//        world->GetAngle(handle) -> LocalTransform.rotation
//      (Also writes Velocity back into RigidBody2D.velocity for Dynamic bodies.)
//      TransformPropagationSystem (registered after this system) then derives
//      WorldTransform from the updated LocalTransform.
//
// Ordering guarantee: PhysicsSystem is registered in fixedUpdate BEFORE
// TransformPropagationSystem; the Writes<LocalTransform> trait creates the data
// dependency that the scheduler respects.
//
// Determinism contract: fixed dt, stable view iteration (Astra guarantees
// archetype-stable order), no wall-clock, /fp:precise (workspace rule). No
// per-step heap allocation on the entity<->body map (only entity-add/remove
// touches it); the opt-in PhysicsInterpBuffer's prev.resize(n) in PASS 2.5 is
// a no-op once its capacity settles at steady-state body count, but it is
// still called every step when that resource is present.
//
// Header-only: the simulation Registry is owned by the host module; systems
// that touch it must instantiate in that module (see SystemSchedulers.hpp).

#include <Manifold2D/Physics/Fixture.hpp>
#include <Manifold2D/Physics/PhysicsTypes.hpp>
#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Manifold2D/Physics/Shapes.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Entity/Entity.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <glm/vec2.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Arcane
{
    // Physics types were lifted to the standalone Manifold2D library (Phase 2).
    // Alias so the system code below reads Phys:: for the Manifold2D::Physics types.
    namespace Phys = Manifold2D::Physics;

    // -------------------------------------------------------------------------
    // PhysicsResource (M6 P3.3)
    // -------------------------------------------------------------------------
    // Transient Registry resource: owns the PhysicsWorld and the entity<->handle
    // map maintained by PhysicsSystem. Not reflected; not serialized
    // (Registry::Save excludes resources -- transient runtime state only).
    //
    // WHY unique_ptr<PhysicsWorld>: PhysicsWorld has a deleted copy ctor and no
    // move ctor. Wrapping it in unique_ptr makes PhysicsResource
    // nothrow-move-constructible (unique_ptr + unordered_map are both
    // noexcept-movable), satisfying the Astra Component concept required by
    // SetResource<T> / EmplaceResource<T>.
    //
    // Lives here (not SceneResources.hpp) because it pulls in Core headers that
    // game-plugin consumers cannot include (PlaygroundGame.dll has no Core in
    // its include path; it includes SceneResources.hpp directly).
    struct PhysicsResource
    {
        std::unique_ptr<Phys::PhysicsWorld>                 world;
        std::unordered_map<Astra::Entity, Phys::BodyHandle> entityToBody;

        // No-op serialization: PhysicsResource is a transient runtime resource.
        // Astra's ResourceStorage auto-calls RegisterComponent<T>, which
        // instantiates Serialize/Deserialize function pointers; these template
        // methods satisfy HasSerializeMethod so the compiler picks the custom
        // path instead of the trivially-copyable memcpy path (which would fail
        // to compile here). Registry::Save excludes all resources entirely, so
        // this code never executes at runtime. The no-op is the correct contract:
        // body handles are re-established by PhysicsSystem on the first
        // fixedUpdate after scene load, just as WorldTransform is re-derived by
        // TransformPropagationSystem.
        template<typename Archive>
        void Serialize(Archive& /*ar*/) {}
    };

    // -------------------------------------------------------------------------
    // MakeScaledShape: build a Manifold2D Shape from a Fixture descriptor scaled
    // by an authored LocalTransform.scale. Aabb scales per-axis exact; Circle uses
    // max(|sx|,|sy|) (a circle has no distinguished axis; max never shrinks below
    // the larger authored axis); Capsule scales its length by |sx| and radius by
    // |sy| (a scalar-radius capsule is approximate under non-uniform scale -- the
    // round caps stay circular; documented in the design spec). Uniform scale is
    // exact for every shape.
    // -------------------------------------------------------------------------
    inline Phys::Shape MakeScaledShape(const Fixture& f, glm::vec2 scale)
    {
        const float sx   = std::abs(scale.x);
        const float sy   = std::abs(scale.y);
        const float sMax = std::max(sx, sy);
        switch (f.kind)
        {
        case Phys::ShapeKind::Circle:  return Phys::MakeCircle(f.radius * sMax);
        case Phys::ShapeKind::Capsule: return Phys::MakeCapsule(f.halfLen * sx, f.radius * sy);
        case Phys::ShapeKind::Aabb:    return Phys::MakeAabb(f.halfW * sx, f.halfH * sy);
        case Phys::ShapeKind::Polygon:
            assert(false && "PhysicsSystem: ShapeKind::Polygon not supported");
            return Phys::MakeCircle(f.radius * sMax);
        }
        return Phys::MakeCircle(f.radius * sMax);
    }

    // Author-edit detection tolerances. pos in meters, rot in radians. Small: they
    // only guard against SetAngle->GetAngle normalization round-trip noise, not any
    // meaningful author nudge (a real gizmo/inspector edit is orders larger).
    inline constexpr float kAuthorPosEps = 1e-5f;
    inline constexpr float kAuthorRotEps = 1e-5f;

    // Shortest-arc absolute angle difference (radians).
    inline float AngleDelta(float a, float b)
    {
        constexpr float kPi  = 3.14159265358979323846f;
        constexpr float kTau = 6.28318530717958647692f;
        float d = a - b;
        while (d >  kPi) d -= kTau;
        while (d < -kPi) d += kTau;
        return std::abs(d);
    }

    // -------------------------------------------------------------------------
    // MakeFixtureDef: build a Core::FixtureDef from a scene-layer Fixture desc,
    // scaled by the entity's authored LocalTransform.scale (default identity).
    // -------------------------------------------------------------------------
    // Helper shared by the body-primary and AddFixture paths. Inline to keep
    // PhysicsSystem.hpp header-only.
    inline Phys::FixtureDef MakeFixtureDef(const Fixture& f,
                                           glm::vec2 scale = glm::vec2(1.0f, 1.0f))
    {
        Phys::FixtureDef fd;

        // Shape geometry (scaled).
        fd.shape = MakeScaledShape(f, scale);

        // Local transform. Offset scales per-axis with the entity's scale (signed,
        // so a mirrored scale mirrors the offset); localAngle is unaffected.
        fd.localPos   = Phys::Vec2(f.localPos.x * scale.x, f.localPos.y * scale.y);
        fd.localAngle = static_cast<Phys::Real>(f.localAngle);

        // Material.
        fd.density     = static_cast<Phys::Real>(f.density);
        fd.friction    = static_cast<Phys::Real>(f.friction);
        fd.restitution = static_cast<Phys::Real>(f.restitution);

        // Collision filter.
        fd.categoryBits = f.categoryBits;
        fd.maskBits     = f.maskBits;

        fd.isSensor = f.isSensor;

        return fd;
    }

    // -------------------------------------------------------------------------
    // RebuildScaledFixtures: rebuild every fixture of `bh` at (descriptor x scale),
    // preserving the body pose. Adds the new scaled fixtures BEFORE dropping the old
    // ones, so the body never transiently holds zero fixtures (sidesteps any
    // "body must keep >= 1 fixture" invariant). AddFixture / DropFixture recompute
    // body mass internally. The body is not moved, so pose is preserved.
    // -------------------------------------------------------------------------
    inline void RebuildScaledFixtures(Phys::PhysicsWorld& world, Phys::BodyHandle bh,
                                      const Collider2D& col, glm::vec2 scale)
    {
        // Capture current fixtures BEFORE adding new ones (their indices are stable
        // until we mutate; the new fixtures append after them).
        const std::uint32_t n = world.FixtureCount(bh);
        std::vector<Phys::FixtureHandle> old;
        old.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i)
            old.push_back(world.GetBodyFixture(bh, i));

        // Add authored fixtures at the new scaled dims.
        for (const Fixture& f : col.fixtures)
        {
            Phys::FixtureDef fd = MakeFixtureDef(f, scale);
            world.AddFixture(bh, fd);
        }

        // Drop the pre-rebuild fixtures.
        for (Phys::FixtureHandle fh : old)
            world.DropFixture(fh);
    }

    // -------------------------------------------------------------------------
    // PhysicsSystem (M6 Physics-v2 T6)
    // -------------------------------------------------------------------------
    struct PhysicsSystem
        : Astra::SystemTraits<Astra::Reads<Collider2D>,
                              Astra::Writes<LocalTransform, PreviousTransform, PhysicsBodyRef, RigidBody2D>>
    {
        // fixedDt: the fixed timestep (seconds) forwarded to PhysicsWorld::Step.
        // Determinism contract: callers MUST pass the same constant every tick.
        // The 60 Hz RunLoop uses 1.0/60.0; tests use kDt = 1.0f/60.0f.
        // stepWorld: when false, run the DESTROY/CREATE/WRITE-BACK passes but
        // SKIP world.Step -- a paused frame mints spawned bodies + reflects poses
        // without paying the (dt-independent) narrowphase + solve. Default true.
        explicit PhysicsSystem(float fixedDt, bool stepWorld = true) noexcept
            : m_fixedDt(fixedDt), m_stepWorld(stepWorld) {}

        void operator()(Astra::Registry& reg)
        {
            PhysicsResource* res = reg.GetResource<PhysicsResource>();
            if (!res || !res->world) return;

            Phys::PhysicsWorld& world        = *res->world;
            auto&                  entityToBody  = res->entityToBody;

            // ------------------------------------------------------------------
            // PASS 1: DESTROY -- remove body rows for dead or un-physicised entities.
            // Collect stale entries first; erase after to avoid iterator invalidation.
            //
            // Implicit assumption: a handle becomes invalid ONLY via entity death or
            // RigidBody2D removal. This guarantees the map cannot leak: no handle
            // escapes without an IsValid==false or HasComponent==false trigger that
            // causes removal here. The CREATE pass self-heals any stale handle by
            // overwriting the map entry when it calls AddBody for the same entity.
            // ------------------------------------------------------------------
            {
                std::vector<Astra::Entity> toRemove;
                for (auto& [entity, handle] : entityToBody)
                {
                    const bool dead   = !reg.IsValid(entity);
                    const bool noBody = !dead && !reg.HasComponent<RigidBody2D>(entity);
                    if (dead || noBody)
                    {
                        world.RemoveBody(handle);
                        toRemove.push_back(entity);
                    }
                }
                for (Astra::Entity e : toRemove)
                    entityToBody.erase(e);
            }

            // ------------------------------------------------------------------
            // PASS 2: CREATE/SYNC -- add body rows for new physics entities.
            // Iterate via a View (archetype-stable, deterministic) so order does
            // not depend on unordered_map hash/bucket layout.
            // ------------------------------------------------------------------
            {
                auto view = reg.CreateView<RigidBody2D, Collider2D, PhysicsBodyRef, LocalTransform>();
                view.ForEach([&](Astra::Entity   entity,
                                 RigidBody2D&    rb,
                                 Collider2D&     col,
                                 PhysicsBodyRef& ref,
                                 LocalTransform& lt)
                {
                    // Skip entities that already have a tracked live handle.
                    if (ref.handle != Phys::kInvalidBody &&
                        entityToBody.count(entity) &&
                        world.IsValid(ref.handle))
                    {
                        return;
                    }

                    // Skip entities with no fixtures (cannot build a body).
                    if (col.fixtures.empty())
                        return;

                    // ---- PRIMARY FIXTURE (fixtures[0]) ----
                    // Build the BodyDef from RigidBody2D dynamics params + fixture[0].
                    const Fixture& fx0 = col.fixtures[0];

                    Phys::BodyDef def;
                    def.type     = rb.type;
                    def.position = Phys::Vec2(lt.position.x, lt.position.y);

                    // Primary fixture shape, scaled by the authored LocalTransform.scale.
                    def.shape = MakeScaledShape(fx0, lt.scale);

                    // Material + filter + local transform from fixture[0].
                    // T6 fix: categoryBits / maskBits / localPos / localAngle were
                    // previously silently dropped because AddBody's auto-fixture
                    // used hardcoded defaults.  BodyDef now carries these fields
                    // and AddBody's auto-fixture reads them (see PhysicsWorld.cpp).
                    def.isSensor      = fx0.isSensor;
                    def.restitution   = static_cast<Phys::Real>(fx0.restitution);
                    def.friction      = static_cast<Phys::Real>(fx0.friction);
                    def.density       = static_cast<Phys::Real>(fx0.density);
                    def.categoryBits  = fx0.categoryBits;
                    def.maskBits      = fx0.maskBits;
                    def.localPos      = Phys::Vec2(fx0.localPos.x * lt.scale.x,
                                                   fx0.localPos.y * lt.scale.y);
                    def.localAngle    = static_cast<Phys::Real>(fx0.localAngle);

                    // Body-level dynamics from RigidBody2D.
                    def.linearDamping = rb.linearDamping;
                    def.fixedRotation = rb.fixedRotation;
                    def.bullet        = rb.bullet;

                    // Optional mass override: RigidBody2D.mass > 0 beats density-derived.
                    if (rb.mass > 0.0f)
                        def.mass = rb.mass;

                    Phys::BodyHandle handle = world.AddBody(def);

                    // ---- ADDITIONAL FIXTURES (fixtures[1..N-1]) ----
                    // AddBody already installed fixture[0] as the primary shape.
                    // Call AddFixture for each subsequent fixture so the body has
                    // one physics fixture per authored Fixture descriptor.
                    for (std::size_t i = 1; i < col.fixtures.size(); ++i)
                    {
                        Phys::FixtureDef fd = MakeFixtureDef(col.fixtures[i], lt.scale);
                        world.AddFixture(handle, fd);
                    }

                    // Authored velocity applied after AddBody so we call SetVelocity
                    // on a live handle (also wakes sleeping Dynamic bodies).
                    if (rb.velocity.x != 0.0f || rb.velocity.y != 0.0f)
                        world.SetVelocity(handle, Phys::Vec2(rb.velocity.x, rb.velocity.y));

                    ref.handle           = handle;
                    ref.appliedScale     = lt.scale;
                    entityToBody[entity] = handle;
                });
            }

            // ------------------------------------------------------------------
            // PASS 2.5: CAPTURE PREVIOUS POSES (Epic 04.2 render interpolation).
            // Snapshot every live body's PRE-STEP world pose into PhysicsInterpBuffer
            // (opt-in resource; skipped if absent). Captured before Step so prev ==
            // the step-N-1 pose; multiple steps/frame leave prev = second-to-last.
            // Gated on m_stepWorld: a paused/mint-only pass does not step, so the
            // buffer (and the render alpha) stay frozen -> a frozen scene renders
            // static. Iterates the WORLD (not ECS) so world-direct joint/polygon
            // bodies with no entity are covered too.
            // ------------------------------------------------------------------
            if (m_stepWorld)
            {
                if (PhysicsInterpBuffer* interp = reg.GetResource<PhysicsInterpBuffer>())
                {
                    const std::uint32_t n = world.Count();
                    interp->prev.resize(n);
                    for (std::uint32_t i = 0; i < n; ++i)
                    {
                        if (world.Alive(i))
                        {
                            const Phys::BodyHandle h = world.HandleOf(i);
                            const Phys::Vec2       p = world.PosSlot(i);
                            interp->prev[i] = InterpPose{
                                glm::vec2(static_cast<float>(p.x), static_cast<float>(p.y)),
                                static_cast<float>(world.GetAngle(h)),
                                h.generation };
                        }
                        else
                        {
                            interp->prev[i].generation = 0;   // dead slot never matches
                        }
                    }
                    interp->captured = true;
                }
            }

            // ------------------------------------------------------------------
            // PASS 3: STEP -- advance the world by one fixed timestep.
            // Skipped when paused (stepWorld=false): no narrowphase, no solve.
            // ------------------------------------------------------------------
            if (m_stepWorld)
                world.Step(m_fixedDt);

            // ------------------------------------------------------------------
            // PASS 3.5: AUTHOR RECONCILE (paused only). When the sim is frozen the
            // AUTHOR owns pos/rot: push a diverged LocalTransform edit into the body
            // BEFORE PASS 4 reflects the (now-matching) body pose back. Stateless --
            // a paused body cannot move itself, so the live body pose is the baseline
            // and any divergence is an author edit. Skipped while stepping (Play =
            // body owns pos/rot; PASS 4 drives lt as before). Scale handled in Task 3.
            // ------------------------------------------------------------------
            if (!m_stepWorld)
            {
                auto view = reg.CreateView<PhysicsBodyRef, LocalTransform, Collider2D, RigidBody2D>();
                view.ForEach([&](Astra::Entity   /*entity*/,
                                 PhysicsBodyRef&  ref,
                                 LocalTransform&  lt,
                                 Collider2D&      col,
                                 RigidBody2D&     /*rb*/)
                {
                    if (ref.handle == Phys::kInvalidBody) return;
                    if (!world.IsValid(ref.handle))       return;

                    // SCALE: rebuild fixtures when lt.scale changed. Exact compare --
                    // physics never writes scale, so appliedScale can't drift; this
                    // both detects the edit and suppresses per-frame re-rebuild. Runs
                    // before the pose branch (rebuild does not move the body).
                    if (lt.scale != ref.appliedScale)
                    {
                        RebuildScaledFixtures(world, ref.handle, col, lt.scale);
                        ref.appliedScale = lt.scale;
                    }

                    // POS/ROT: stateless author reconcile.
                    const Phys::Vec2 bp = world.Position(ref.handle);
                    const float      ba = static_cast<float>(world.GetAngle(ref.handle));
                    if (std::abs(lt.position.x - static_cast<float>(bp.x)) > kAuthorPosEps ||
                        std::abs(lt.position.y - static_cast<float>(bp.y)) > kAuthorPosEps ||
                        AngleDelta(lt.rotation, ba) > kAuthorRotEps)
                    {
                        // SetPosition + SetAngle are BOTH load-bearing for a moved STATIC
                        // body: SetPosition updates the pose but NOT the static broadphase
                        // tree; SetAngle re-registers it from the already-updated position.
                        // Keep SetAngle unconditional -- gating it (e.g. "skip when rotation
                        // unchanged") would leave a moved static collider with a stale proxy
                        // that never refreshes. Velocity is zeroed unconditionally ("don't
                        // fling on resume") -- for a Kinematic body with authored rb.velocity
                        // this is a SPEC #1 design consequence (velocity re-apply is a non-goal).
                        world.SetPosition(ref.handle, Phys::Vec2(lt.position.x, lt.position.y));
                        world.SetAngle(ref.handle, static_cast<Phys::Real>(lt.rotation));
                        world.SetVelocity(ref.handle, Phys::Vec2(0.0f, 0.0f));
                        world.SetAngularVelocity(ref.handle, static_cast<Phys::Real>(0));
                    }
                });
            }

            // ------------------------------------------------------------------
            // PASS 4: WRITE-BACK -- propagate post-step poses to LocalTransform.
            // TransformPropagationSystem (ordered after this) then reads
            // LocalTransform and derives WorldTransform.
            // ------------------------------------------------------------------
            {
                auto view = reg.CreateView<PhysicsBodyRef, LocalTransform, RigidBody2D>();
                view.ForEach([&](Astra::Entity   entity,
                                 PhysicsBodyRef& ref,
                                 LocalTransform& lt,
                                 RigidBody2D&    rb)
                {
                    if (ref.handle == Phys::kInvalidBody) return;
                    if (!world.IsValid(ref.handle))          return;

                    // Render interpolation (Epic 04.2): stash the pose we are about to
                    // overwrite as this entity's PREVIOUS step pose (opt-in via the
                    // PreviousTransform component), so RenderSubmissionSystem can lerp
                    // prev -> current by alpha. Captured before the write below, so prev
                    // == the step-N-1 pose (multiple steps/frame leave prev = second-to-
                    // last, matching the physics pose buffer).
                    // Gated on m_stepWorld -- mirrors the PASS 2.5 interp-buffer capture:
                    // on a paused/mint-only pass the world does not step, so lt does not
                    // change; capturing here would set prev==current and make a paused
                    // Path-B sprite snap to the current pose instead of holding the same
                    // sub-step-interpolated pose the debug overlay shows. Gating keeps
                    // prev frozen at the true step-(N-1) pose while paused.
                    if (m_stepWorld)
                    {
                        if (PreviousTransform* pt = reg.GetComponent<PreviousTransform>(entity))
                        {
                            pt->position = lt.position;
                            pt->rotation = lt.rotation;
                        }
                    }

                    const Phys::Vec2 pos = world.Position(ref.handle);
                    lt.position = glm::vec2(pos.x, pos.y);
                    lt.rotation = world.GetAngle(ref.handle);

                    // Mirror post-step velocity back into RigidBody2D for Dynamic
                    // bodies so authored velocity field stays consistent with physics.
                    // Kinematic velocity is authored and never written back: the solver
                    // does not modify it, so rb.velocity retains its authored value.
                    if (rb.type == Phys::BodyType::Dynamic)
                    {
                        const Phys::Vec2 vel = world.Velocity(ref.handle);
                        rb.velocity = glm::vec2(vel.x, vel.y);
                    }
                });
            }
        }

    private:
        float m_fixedDt;    // fixed 60 Hz timestep; determinism contract: constant per run
        bool  m_stepWorld;  // false on paused frames -> skip the solve
    };

} // namespace Arcane
