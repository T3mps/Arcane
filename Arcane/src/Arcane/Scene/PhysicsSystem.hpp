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
//   3. STEP -- world->Step(m_fixedDt). Physics advances one fixed tick.
//
//   4. WRITE-BACK -- for each tracked entity write:
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
// archetype-stable order), no wall-clock, /fp:precise (workspace rule), no
// per-step heap allocation (only entity-add/remove touches the map).
//
// Header-only: the simulation Registry is owned by the host module; systems
// that touch it must instantiate in that module (see SystemSchedulers.hpp).

#include <Arcane/Physics/Fixture.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Shapes.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>

#include <Astra/Entity/Entity.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <glm/vec2.hpp>

#include <cassert>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Arcane
{
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
        std::unique_ptr<Physics::PhysicsWorld>                 world;
        std::unordered_map<Astra::Entity, Physics::BodyHandle> entityToBody;

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
    // MakeFixtureDef: build a Core::FixtureDef from a scene-layer Fixture desc.
    // -------------------------------------------------------------------------
    // Helper shared by the body-primary and AddFixture paths. Inline to keep
    // PhysicsSystem.hpp header-only.
    inline Physics::FixtureDef MakeFixtureDef(const Fixture& f)
    {
        Physics::FixtureDef fd;

        // Shape geometry.
        switch (f.kind)
        {
        case Physics::ShapeKind::Circle:
            fd.shape = Physics::MakeCircle(f.radius);
            break;
        case Physics::ShapeKind::Capsule:
            fd.shape = Physics::MakeCapsule(f.halfLen, f.radius);
            break;
        case Physics::ShapeKind::Aabb:
            fd.shape = Physics::MakeAabb(f.halfW, f.halfH);
            break;
        case Physics::ShapeKind::Polygon:
            // Polygon authored verts are out of scope (Fixture carries no vertex
            // array). Assert in Debug; fall back to circle so the entity doesn't
            // silently disappear from the simulation.
            assert(false && "PhysicsSystem: ShapeKind::Polygon not supported");
            fd.shape = Physics::MakeCircle(f.radius);
            break;
        }

        // Local transform.
        fd.localPos   = Physics::Vec2(f.localPos.x, f.localPos.y);
        fd.localAngle = static_cast<Physics::Real>(f.localAngle);

        // Material.
        fd.density     = static_cast<Physics::Real>(f.density);
        fd.friction    = static_cast<Physics::Real>(f.friction);
        fd.restitution = static_cast<Physics::Real>(f.restitution);

        // Collision filter.
        fd.categoryBits = f.categoryBits;
        fd.maskBits     = f.maskBits;

        fd.isSensor = f.isSensor;

        return fd;
    }

    // -------------------------------------------------------------------------
    // PhysicsSystem (M6 Physics-v2 T6)
    // -------------------------------------------------------------------------
    struct PhysicsSystem
        : Astra::SystemTraits<Astra::Reads<Collider2D>,
                              Astra::Writes<LocalTransform, PhysicsBodyRef, RigidBody2D>>
    {
        // fixedDt: the fixed timestep (seconds) forwarded to PhysicsWorld::Step.
        // Determinism contract: callers MUST pass the same constant every tick.
        // The 60 Hz RunLoop uses 1.0/60.0; tests use kDt = 1.0f/60.0f.
        explicit PhysicsSystem(float fixedDt) noexcept
            : m_fixedDt(fixedDt) {}

        void operator()(Astra::Registry& reg)
        {
            PhysicsResource* res = reg.GetResource<PhysicsResource>();
            if (!res || !res->world) return;

            Physics::PhysicsWorld& world        = *res->world;
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
                    if (ref.handle != Physics::kInvalidBody &&
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

                    Physics::BodyDef def;
                    def.type     = rb.type;
                    def.position = Physics::Vec2(lt.position.x, lt.position.y);

                    // Translate fixture[0] shape descriptor -> Core::Shape.
                    switch (fx0.kind)
                    {
                    case Physics::ShapeKind::Circle:
                        def.shape = Physics::MakeCircle(fx0.radius);
                        break;
                    case Physics::ShapeKind::Capsule:
                        def.shape = Physics::MakeCapsule(fx0.halfLen, fx0.radius);
                        break;
                    case Physics::ShapeKind::Aabb:
                        def.shape = Physics::MakeAabb(fx0.halfW, fx0.halfH);
                        break;
                    case Physics::ShapeKind::Polygon:
                        assert(false && "PhysicsSystem: ShapeKind::Polygon not supported");
                        def.shape = Physics::MakeCircle(fx0.radius);
                        break;
                    }

                    // Material + filter from fixture[0].
                    def.isSensor      = fx0.isSensor;
                    def.restitution   = static_cast<Physics::Real>(fx0.restitution);
                    def.friction      = static_cast<Physics::Real>(fx0.friction);
                    def.density       = static_cast<Physics::Real>(fx0.density);

                    // Body-level dynamics from RigidBody2D.
                    def.linearDamping = rb.linearDamping;
                    def.fixedRotation = rb.fixedRotation;
                    def.bullet        = rb.bullet;

                    // Optional mass override: RigidBody2D.mass > 0 beats density-derived.
                    if (rb.mass > 0.0f)
                        def.mass = rb.mass;

                    Physics::BodyHandle handle = world.AddBody(def);

                    // ---- ADDITIONAL FIXTURES (fixtures[1..N-1]) ----
                    // AddBody already installed fixture[0] as the primary shape.
                    // Call AddFixture for each subsequent fixture so the body has
                    // one physics fixture per authored Fixture descriptor.
                    for (std::size_t i = 1; i < col.fixtures.size(); ++i)
                    {
                        Physics::FixtureDef fd = MakeFixtureDef(col.fixtures[i]);
                        world.AddFixture(handle, fd);
                    }

                    // Authored velocity applied after AddBody so we call SetVelocity
                    // on a live handle (also wakes sleeping Dynamic bodies).
                    if (rb.velocity.x != 0.0f || rb.velocity.y != 0.0f)
                        world.SetVelocity(handle, Physics::Vec2(rb.velocity.x, rb.velocity.y));

                    ref.handle           = handle;
                    entityToBody[entity] = handle;
                });
            }

            // ------------------------------------------------------------------
            // PASS 3: STEP -- advance the world by one fixed timestep.
            // ------------------------------------------------------------------
            world.Step(m_fixedDt);

            // ------------------------------------------------------------------
            // PASS 4: WRITE-BACK -- propagate post-step poses to LocalTransform.
            // TransformPropagationSystem (ordered after this) then reads
            // LocalTransform and derives WorldTransform.
            // ------------------------------------------------------------------
            {
                auto view = reg.CreateView<PhysicsBodyRef, LocalTransform, RigidBody2D>();
                view.ForEach([&](Astra::Entity   /*entity*/,
                                 PhysicsBodyRef& ref,
                                 LocalTransform& lt,
                                 RigidBody2D&    rb)
                {
                    if (ref.handle == Physics::kInvalidBody) return;
                    if (!world.IsValid(ref.handle))          return;

                    const Physics::Vec2 pos = world.Position(ref.handle);
                    lt.position = glm::vec2(pos.x, pos.y);
                    lt.rotation = world.GetAngle(ref.handle);

                    // Mirror post-step velocity back into RigidBody2D for Dynamic
                    // bodies so authored velocity field stays consistent with physics.
                    // Kinematic velocity is authored and never written back: the solver
                    // does not modify it, so rb.velocity retains its authored value.
                    if (rb.type == Physics::BodyType::Dynamic)
                    {
                        const Physics::Vec2 vel = world.Velocity(ref.handle);
                        rb.velocity = glm::vec2(vel.x, vel.y);
                    }
                });
            }
        }

    private:
        float m_fixedDt;   // fixed 60 Hz timestep; determinism contract: constant per run
    };

} // namespace Arcane
