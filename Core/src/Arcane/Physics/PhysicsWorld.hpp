#pragma once

// PhysicsWorld: the 2D-physics orchestration core (M6, Task P1.8).
//
// PORT NOTE: a faithful port of the KINEMATIC subset of
// Client/src/physics/PhysicsWorld.lua. The Lua module is the whole engine
// (statics + kinematics + dynamics + solver + joints + islands + CCD +
// raycast). P1.8 ports ONLY:
//   * Body SoA storage + handle/free-list/generation + AddBody/RemoveBody/
//     IsValid (ports PhysicsWorld.lua addBody/removeBody/handleValid).
//   * Step stages 1 and 5 (P1.8 scope only) for KINEMATIC bodies: prevX/Y
//     snapshot + kinematic velocity integration + mover-broadphase AABB update
//     (stage 1), then ContactManager::Step for events (stage 5). (Lua step()
//     stage 1 kinematic branch + the trailing contacts:step. Stages 2-4 were
//     added in P2.2/P2.4; see the PORTED IN P2.2 note below.)
//   * QueryAABB(rect) -> body handles (linear scan; ports queryAABB).
//   * Static bodies in a staticList (not the mover broadphase) + an optional
//     TileGrid for tile statics + _StaticCandidates (ports staticList /
//     _staticCandidates).
//
// PORTED IN P2.1 (extends the P1.8 kinematic subset):
//   * Dynamic velocity integration (gravity + linear damping) and dynamic
//     position integration (semi-implicit Euler).
//   * Dynamics SoA (m_invMass, m_invInertia, m_angle, m_angVel, m_rest,
//     m_fric, m_linDamp, m_sleepTimer, m_awake, m_bullet).
//   * Body dynamics accessors: ApplyImpulse, Wake, IsAwake, GetAngle/SetAngle.
//   * BodyDef dynamics params: density, mass override, restitution, friction,
//     linearDamping, fixedRotation, bullet.
//
// PORTED IN P2.2 (SoftStep solver):
//   * Step stages restructured to 1-5 (see Step() in PhysicsWorld.cpp):
//       stage 1: prev snapshot + kinematic integrate
//       stage 2: solver contact generation (UpdateContacts + EmitContactConstraints / Part A)
//       stage 3: Soft Step solve (Part B)
//       stage 4: island sleep bookkeeping (P2.4)
//       stage 5: contacts:step (events + gating + deferred flush)
//
// PORTED IN P2.4 (island sleep):
//   * Island::UpdateSleep wired at stage 4; sleep-island logic active.
//
// DELIBERATELY NOT PORTED (still deferred -- see PORT BOUNDARY below):
//   * Joints (P2.5).
//   * Bullet CCD clamp (P3.1 -- m_bullet stored now, clamp deferred).
//   * raycast / shapeCast / lineOfSight (P1.9).
// Dynamic bodies are ACCEPTED + stored (BodyType::Dynamic) and are
// integrated by Step (gravity + damping + position). They are also registered
// in the mover broadphase so the ContactManager emits mover-mover events.
// NOTE: dynamic-vs-static-BODY collision RESPONSE still requires the solver
// (P2.2); only kinematics events fire in P2.1 (faithful to
// ContactManager.lua:150 -- the solver owns dynamic response).
//
// DETERMINISM (port + modernize): Step iterates slots by INDEX (never map
// order); the mover broadphase emits SORTED pairs; the ContactManager sorts
// its End events; no wall-clock; no fast-math (the workspace builds /fp:precise
// and forbids /fp:fast). Zero steady-state allocation in Step after warmup:
// the SoA vectors only grow, the broadphase pools its nodes, and the
// ContactManager reuses its pair map + scratch buffers.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.
// No SDL3/NVRHI/Batcher2D/ImGui, no iso/Map/world coupling. Compiles both
// /MD (Arcane.dll) and static-CRT/C++20 (project ArcaneCore, server flavor).
// namespace Arcane::Physics, Core style.

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <Arcane/Util/BitSet.hpp>
#include <Arcane/Util/FunctionRef.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Fixture.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp>
#include <Arcane/Physics/Broadphase/DynamicTree.hpp> // FixtureBroadphaseTree() debug accessor
#include <Arcane/Physics/Broadphase/Passability.hpp>
#include <Arcane/Physics/Broadphase/TileGrid.hpp>
#include <Arcane/Physics/Broadphase/SpatialGrid.hpp>
#include <Arcane/Physics/ContactManager.hpp>
#include <Arcane/Physics/Contact.hpp>            // ContactPool (collision-rebuild Phase 3)
#include <Arcane/Physics/Solver/Solver.hpp>      // ISolver + ContactConstraint pool type
#include <Arcane/Physics/Joints/Joint.hpp>       // Joint base + JointDef (P2.5)
#include <Arcane/Physics/Island.hpp>             // Island::Island registry struct + constants (Phase A)

namespace Arcane
{
    namespace Physics
    {
        // ----------------------------------------------------------------
        // BroadphaseKind: which mover broadphase the world builds.
        // ----------------------------------------------------------------
        //
        // PORT + MODERNIZE: the Lua selected the mover broadphase by string
        // ("hash" | "sap" | "tree"), defaulting to SpatialHash. The C++ engine
        // defaults to DynamicTree (the P1.6 default-broadphase decision); the
        // other two stay selectable behind IBroadphase.
        enum class BroadphaseKind : std::uint8_t
        {
            Tree = 0, // DynamicTree (default)
            Hash = 1, // SpatialHash
            Sap  = 2, // SweepAndPrune
        };

        // ----------------------------------------------------------------
        // SolverKind: which constraint solver the world installs (P2.3 A/B).
        // ----------------------------------------------------------------
        //
        // SoftStep (default) is the Box2D-v3 TGS Soft modernization centerpiece
        // (P2.2). Baumgarte is the retained PGS oracle -- a faithful port of the
        // Lua SequentialImpulse (P2.3). Both implement ISolver and satisfy the
        // same stability invariants on the same scenes (the A/B cross-check);
        // running both guards against solver-specific bugs. Parallel to
        // BroadphaseKind: the world picks one in its constructor.
        enum class SolverKind : std::uint8_t
        {
            SoftStep  = 0, // Box2D-v3 TGS Soft (default)
            Baumgarte = 1, // Lua SequentialImpulse PGS oracle (A/B cross-check)
        };

        // ----------------------------------------------------------------
        // BodyDef: parameters for AddBody (ports the Lua addBody opts).
        // ----------------------------------------------------------------
        struct BodyDef
        {
            BodyType type = BodyType::Kinematic;
            Vec2     position{ Real(0), Real(0) };
            Shape    shape{}; // required; stored by value per slot
            bool     isSensor      = false;
            bool     eventsEnabled = true;

            // ---- dynamics (P2.1) -- ignored for Static/Kinematic ------------
            //
            // PORT of the Lua addBody dynamics opts (PhysicsWorld.lua:232-250).
            // On a Dynamic body AddBody computes mass + rotational inertia via
            // Shape::ComputeMass(density) (NOT a re-ported massProps -- the P1.1
            // ComputeMass was verified equivalent). invMass/invInertia are
            // derived from those; Static/Kinematic keep invMass = invInertia = 0
            // (they never integrate or solve).
            Real density        = Real(1);     // mass = density * shape area
            // Optional mass override: if > 0 it REPLACES the computed mass and
            // scales the inertia by (mass / computedMass) (ports lines 241-243).
            // <= 0 means "use the density-derived mass".
            Real mass           = Real(0);
            Real restitution    = Real(0);     // bounciness (solver, P2.2)
            Real friction       = Real(0.4);   // Lua default 0.4 (line 233)
            Real linearDamping  = Real(0);     // per-step velocity decay
            // Per-body sleep speed gate (px/s). < 0 inherits WorldDef::sleepThreshold.
            // A body is idle when |v| + |w|*maxExtent < sleepThreshold (Box2D v3).
            Real sleepThreshold = Real(-1);
            bool fixedRotation  = false;       // invInertia forced to 0
            bool bullet         = false;       // CCD clamp (P3); stored now

            // ---- primary fixture filter + local transform (T6 fix) ----------
            //
            // AddBody's auto-fixture (the back-compat path that creates one
            // fixture from the BodyDef's shape) was previously created with
            // hardcoded categoryBits=1 / maskBits=0xFFFFFFFF and localPos=(0,0)
            // / localAngle=0, silently discarding authored values.
            //
            // These fields carry the PRIMARY fixture's filter + local transform
            // so they flow through to the auto-fixture in AddBody.  They are
            // consistent with the other primary-fixture scalars already in BodyDef
            // (shape, density, friction, restitution, isSensor).
            //
            // MASS PATH UNCHANGED: adding these fields does NOT change
            // invMass/invInertia computation.  The legacy mass path reads
            // def.shape + def.density and ignores localPos/localAngle (correct:
            // compound-COM for an offset primary fixture is a deferred task).
            // Existing dynamics tests therefore stay byte-for-byte green.
            std::uint32_t categoryBits = 1u;           // collision category
            std::uint32_t maskBits     = 0xFFFFFFFFu;  // collision mask
            Vec2          localPos     { Real(0), Real(0) }; // body-frame offset
            Real          localAngle   = Real(0);            // body-frame rotation
        };

        // ----------------------------------------------------------------
        // WorldDef: parameters for the world constructor (ports PhysicsWorld.new
        // opts -- the P1.8 subset).
        // ----------------------------------------------------------------
        //
        // Optional tile statics: if `passability` is non-null the world owns a
        // TileGrid over it (cellSize/origin describe the grid). The seam must
        // outlive the world. With no passability source the world simply has no
        // tile spans (the P1.8 event tests place bodies at plain coords and
        // need no TileGrid).
        struct WorldDef
        {
            BroadphaseKind             broadphase  = BroadphaseKind::Tree;
            Real                       hashCellSize = Real(64); // for Hash
            const IPassabilitySource*  passability = nullptr;   // optional tile statics
            Real                       tileCellSize = Real(1);
            Vec2                       tileOrigin{ Real(0), Real(0) };

            // ---- dynamics config (P2.1) -------------------------------------
            //
            // Global gravity (world units / s^2), applied to awake Dynamic
            // bodies in Step's velocity-integrate stage (ports the Lua
            // gravityX/gravityY, default 0). The top-down overworld runs at
            // gravity 0; the dynamics tests set e.g. gravityY = 400.
            Real gravityX = Real(0);
            Real gravityY = Real(0);

            // ---- Soft Step solver config (P2.2; Box2D v3 TGS Soft) ----------
            //
            // substepCount: dt is split into this many sub-steps; each sub-step
            // integrates gravity + position and runs one biased solve + one
            // relax pass (the v3 sub-stepping that makes a moderate contact
            // hertz appear rigid). Box2D v3 default is 4.
            //
            // contactHertz / contactDampingRatio feed b2MakeSoft to derive the
            // soft (biasRate, massScale, impulseScale) per contact. A high hertz
            // (clamped to 0.25*substepCount/dt so it never out-runs the
            // sub-step rate) + the small sub-step makes rigid contacts behave
            // rigidly while staying stable. Box2D v3 defaults: 30 Hz, zeta 10.
            //
            // restitutionThreshold: approach speed (world units/s) below which
            // restitution is suppressed (resting micro-bounces are killed).
            // contactPushMaxVelocity: clamp on the soft penetration push-out
            // bias velocity so deep overlaps recover without exploding.
            std::uint32_t substepCount         = 4u;
            Real          contactHertz         = Real(30);
            Real          contactDampingRatio  = Real(10);
            Real          restitutionThreshold = Real(20);  // Lua REST_VEL = 20
            Real          contactPushMaxVelocity = Real(300);

            // Sleep speed gate (px/s): a body is idle when its combined speed
            // |v| + |w|*maxExtent < sleepThreshold (Box2D v3 b2FinalizeBodiesTask).
            // Box2D b2DefaultBodyDef is 0.05 m/s (~4.5 px/s at the sandbox's ~90 px/m).
            // Set to 8 empirically: the gravity-900 soft solver's residual jitter floor
            // for a settled pile is ~7 px/s (measured on the scene-8 no-whisk repro;
            // 6 fails to sleep, 7 sleeps, 8 = +1 margin -- see the never-settle findings
            // doc). Per-body override via BodyDef::sleepThreshold (>= 0). The maxExtent
            // weighting handles body size; gravity scaling is left to this constant.
            Real          sleepThreshold = Real(8);

            // ---- solver selection (P2.3 A/B cross-check) --------------------
            //
            // Which ISolver the world installs (parallel to `broadphase`).
            // SoftStep (default) is the P2.2 TGS Soft solver; Baumgarte is the
            // P2.3 retained PGS oracle (Lua SequentialImpulse port). Both satisfy
            // the same stability invariants on the same scenes.
            SolverKind    solverKind = SolverKind::SoftStep;

            // Baumgarte-only: velocity iterations per Step (the Lua w.velIters,
            // default 8). Ignored by SoftStep (which iterates by substepCount).
            std::uint32_t velIters = 8u;
        };

        class Body; // forward decl (Body.hpp); ergonomic view over a handle.

        // ----------------------------------------------------------------
        // RaycastHit: PhysicsWorld::Raycast result (ports raycast's returned
        // table). Either a CELL hit (isCell == true; cellX/cellY valid; body is
        // kInvalidBody) or a BODY hit (isCell == false; body valid; cellX/cellY
        // unused). t in [0,1] is the parametric fraction along the ray; point is
        // the world-space hit position (lerp of start..end by t).
        // ----------------------------------------------------------------
        struct RaycastHit
        {
            Real       t = Real(0);              // parametric fraction in [0,1]
            Vec2       point{ Real(0), Real(0) }; // world-space hit position
            bool       isCell = false;           // true: cell hit; false: body hit
            int        cellX = 0;                // valid iff isCell
            int        cellY = 0;                // valid iff isCell
            BodyHandle body = kInvalidBody;      // valid iff !isCell
        };

        // ----------------------------------------------------------------
        // RaycastOpts: Raycast modifiers (ports the raycast opts table).
        // ----------------------------------------------------------------
        struct RaycastOpts
        {
            // tallOnly (the LOS rule): cells block only when they block SIGHT
            // (BlocksSight), not merely movement (IsSolid). LOW obstacles let a
            // ray through; TALL obstacles stop it. Ports opts.tallOnly.
            bool tallOnly = false;
            // cellsOnly: skip the body tests entirely (only the cell DDA runs).
            // Ports opts.cellsOnly. LineOfSight uses { tallOnly, cellsOnly }.
            bool cellsOnly = false;
        };

        // ----------------------------------------------------------------
        // ShapeCastHit: PhysicsWorld::ShapeCast result (ports Cast.shapeCast's
        // returned table { t, x, y, nx, ny, dist, body? }). `point` is the
        // moving shape's CENTER at the time of impact; `normal` is the push-back
        // direction; `distance` is the surface distance at the stopping point;
        // `body` is the hit mover/static body handle (kInvalidBody for a tile
        // span). t in [0,1] is the parametric fraction along the cast delta.
        // ----------------------------------------------------------------
        struct ShapeCastHit
        {
            Real       t = Real(0);
            Vec2       point{ Real(0), Real(0) }; // shape CENTER at TOI
            Vec2       normal{ Real(0), Real(0) };
            Real       distance = Real(0);
            BodyHandle body = kInvalidBody; // valid for body hits; kInvalidBody for spans
        };

        // ----------------------------------------------------------------
        // ShapeCastOpts: ShapeCast modifiers (ports Cast.shapeCast opts).
        // ----------------------------------------------------------------
        struct ShapeCastOpts
        {
            // Include non-static (kinematic/dynamic) bodies as cast obstacles.
            // Ports opts.movers (statics + tile spans are ALWAYS included).
            bool movers = false;
            // Skip this body when casting (e.g. the caster itself). Ports
            // opts.exclude (a body handle, kInvalidBody = exclude nothing).
            BodyHandle exclude = kInvalidBody;
        };

        // A body slot NOT in the awake-set (static, kinematic, sleeping, or dead).
        // Sentinel stored in m_awakeIndex[slot] when the slot is not a member of
        // m_awakeBodies. Must not collide with any real dense position index.
        static constexpr std::uint32_t kNotAwake = 0xFFFFFFFFu;

        // A body slot NOT in the kinematic-set (static, dynamic, or dead). Sentinel
        // stored in m_kinematicIndex[slot] when the slot is not a member of
        // m_kinematicBodies. Mirrors kNotAwake for the kinematic solver-set (Phase C).
        static constexpr std::uint32_t kNotKinematic = 0xFFFFFFFFu;

        // ----------------------------------------------------------------
        // PhysicsWorld: the SoA body store + Step pipeline (kinematic subset).
        // ----------------------------------------------------------------
        //
        // The CANONICAL surface is handle-based (Position/Velocity/etc. take a
        // BodyHandle) because events carry BodyHandles. Body.hpp wraps a
        // {world, handle} pair for ergonomic call sites; it forwards to these.
        class PhysicsWorld
        {
        public:
            explicit PhysicsWorld(const WorldDef& def = {});
            ~PhysicsWorld();

            PhysicsWorld(const PhysicsWorld&)            = delete;
            PhysicsWorld& operator=(const PhysicsWorld&) = delete;

            // ---- body lifecycle (ports addBody / removeBody / handleValid) --

            // Create a body; returns a handle (index + generation). Reuses a
            // free slot or appends; bumps the slot's generation (live slots
            // start at generation 1). Static -> staticList; Kinematic/Dynamic ->
            // mover broadphase.
            BodyHandle AddBody(const BodyDef& def);

            // Destroy a body. Invalidates the handle (generation bump), removes
            // it from the broadphase / staticList, drops its contact pairs, and
            // recycles the slot. No-op for a stale/invalid handle.
            void RemoveBody(BodyHandle h);

            // True iff h refers to a live slot at the same generation (ports
            // handleValid).
            [[nodiscard]] bool IsValid(BodyHandle h) const noexcept;

            // ---- fixture lifecycle (v2 Task 4; additive over the body API) --
            //
            // Add a fixture to a live body.  Creates a fixture slot (reuses a
            // free slot or appends), links it to the body, and re-aggregates the
            // body's mass / COM / inertia.  Returns a stable FixtureHandle.
            // AddBody back-compat calls this internally for the first fixture.
            // Setup-time: may allocate (vector growth); no alloc inside Step.
            FixtureHandle AddFixture(BodyHandle bh, const FixtureDef& def);

            // Remove a fixture.  Unlinks it from the body, recycles the slot
            // (bumps the generation), and re-aggregates the body's mass.
            // No-op for a stale / invalid handle.
            void DropFixture(FixtureHandle fh);

            // True iff fh refers to a live fixture slot at the same generation.
            [[nodiscard]] bool IsValid(FixtureHandle fh) const noexcept;

            // Number of fixtures attached to a body (0 if the handle is stale).
            [[nodiscard]] std::uint32_t FixtureCount(BodyHandle bh) const noexcept;

            // ---- fixture-inspection accessors (tests + T5 wiring) -----------

            // Fixture world-space center position:
            //   bodyPos + R(bodyAngle) * fixture.localPos
            [[nodiscard]] Vec2 GetFixtureWorldPos(FixtureHandle fh) const noexcept;

            // Fixture world-space angle:
            //   bodyAngle + fixture.localAngle
            [[nodiscard]] Real GetFixtureWorldAngle(FixtureHandle fh) const noexcept;

            // Collision filter accessors (read-only; needed by tests and future
            // filter-query systems). Return the default (1 / 0xFFFFFFFF) for a
            // stale or out-of-range handle.
            [[nodiscard]] std::uint32_t GetFixtureCategory(FixtureHandle fh) const noexcept;
            [[nodiscard]] std::uint32_t GetFixtureMask(FixtureHandle fh) const noexcept;

            // Return the FixtureHandle for the n-th fixture (0-based) attached to
            // body bh, in insertion order.  Returns kInvalidFixture when bh is
            // stale, n >= FixtureCount(bh), or the slot has been recycled.
            // Primary use: tests that need the handle for fixture[0] without
            // having to call AddFixture explicitly.
            [[nodiscard]] FixtureHandle GetBodyFixture(BodyHandle bh,
                                                       std::uint32_t n) const noexcept;

            // ---- body mass / COM accessors (tests + solver T5) --------------

            // Aggregated body mass (sum of fixture masses).  0 for
            // Static/Kinematic (they are never integrated).
            [[nodiscard]] Real GetBodyMass(BodyHandle bh) const noexcept;

            // Body center of mass in the body-local frame (weighted centroid of
            // all fixtures).  Consumed by the compound-COM dynamics: the solver
            // integrates/rotates the body about its world COM (WorldCom(pos,
            // angle, localCenter)) and GenerateContacts measures contact anchors
            // from it.  Returns (0,0) for Static/Kinematic and single-fixture
            // bodies (for which COM == origin, so the COM path is a no-op).
            [[nodiscard]] Vec2 GetLocalCenter(BodyHandle bh) const noexcept;

            // Body rotational inertia about the COM.  0 for Static/Kinematic or
            // when fixedRotation is true.
            [[nodiscard]] Real GetBodyInertia(BodyHandle bh) const noexcept;

            // ---- handle-based accessors (the canonical surface) -------------

            [[nodiscard]] Vec2 Position(BodyHandle h) const noexcept;

            // Teleport: prev snaps too so DrawPosition does not lerp-smear
            // across the jump (ports Body:setPosition). Updates the broadphase
            // for movers.
            void SetPosition(BodyHandle h, Vec2 p);

            // Move WITHOUT snapping prev: updates posX/posY + the mover
            // broadphase AABB but leaves prevX/prevY untouched, so the render
            // boundary's prev->pos lerp still spans this tick's motion.
            //
            // PORT NOTE: this is the seam CharacterController::SlideMove writes
            // through (the Lua slideMove sets w.posX[i]/w.posY[i] directly +
            // moverHash:update, NOT via Body:setPosition, exactly to keep prev
            // managed by Step -- see the integration contract in
            // CharacterController.hpp). SetPosition is a TELEPORT (snaps prev);
            // MovePosition is a step-managed-prev move. Static bodies no-op the
            // broadphase update (they are not in the mover broadphase) but their
            // position is still written; the CC only ever drives movers.
            void MovePosition(BodyHandle h, Vec2 p);

            [[nodiscard]] Vec2 Velocity(BodyHandle h) const noexcept;

            // Set velocity (Kinematic + Dynamic accept it; Static ignores).
            // On a Dynamic body this WAKES it (clears the sleep timer) -- ports
            // setVelocity (PhysicsWorld.lua:98-104, line 102).
            void SetVelocity(BodyHandle h, Vec2 v);

            // Set angular velocity (Kinematic + Dynamic accept it; Static ignores).
            // Mirrors SetVelocity: a Dynamic body is WAKED (sleep timer cleared).
            void SetAngularVelocity(BodyHandle h, Real w);

            // Resolve a body handle to its SoA slot index (the handle's index field).
            // Callers must hold a valid handle; returns the raw index unconditionally.
            [[nodiscard]] std::uint32_t SlotOf(BodyHandle h) const noexcept { return h.index; }

            // ---- dynamics accessors (P2.1; Dynamic-only effects) ------------

            // Apply a linear impulse at the body center: velocity += i * invMass.
            // Dynamic only; wakes the body. No-op otherwise. Ports applyImpulse
            // (PhysicsWorld.lua:107-117, the no-point branch).
            void ApplyImpulse(BodyHandle h, Vec2 impulse);

            // Apply a linear impulse at world point p: the linear part as above,
            // plus angVel += cross(p - center, i) * invInertia. Dynamic only;
            // wakes the body. Ports applyImpulse (the px,py branch).
            void ApplyImpulse(BodyHandle h, Vec2 impulse, Vec2 worldPoint);

            // Wake a Dynamic body (awake = 1, sleepTimer = 0). No-op for
            // Static/Kinematic. Ports Body:wake (line 120).
            void Wake(BodyHandle h);

            // True iff the body is awake (Static/Kinematic report awake; they
            // never sleep). Ports Body:isAwake (line 119).
            [[nodiscard]] bool IsAwake(BodyHandle h) const noexcept;

            // Orientation (radians). Integrated from angVel for Dynamic bodies
            // in Step; identity for Static/Kinematic. Ports getAngle/setAngle
            // (lines 121-122).
            [[nodiscard]] Real GetAngle(BodyHandle h) const noexcept;
            void SetAngle(BodyHandle h, Real angle);

            // Render-boundary lerp between prev and current step positions
            // (ports Body:drawPosition).
            [[nodiscard]] Vec2 DrawPosition(BodyHandle h, Real alpha) const noexcept;

            [[nodiscard]] const Shape* GetShape(BodyHandle h) const noexcept;
            [[nodiscard]] BodyType     GetType(BodyHandle h) const noexcept;
            [[nodiscard]] bool         IsSensor(BodyHandle h) const noexcept;

            // ---- events ----------------------------------------------------

            // Install / replace the contact listener (ports onContact). Called
            // AFTER all step state has settled (deferred delivery).
            void OnContact(ContactManager::Listener fn);

            // Per-body event gate (ports _setBodyEvents). true->false: Disarm
            // (drop, no synthetic end). false->true: Rearm (fresh begin for
            // currently-overlapping pairs, level-triggered).
            void SetBodyEvents(BodyHandle h, bool on);

            // World-level event gate (ports setEventsEnabled). on->off: Disarm
            // all. off->on: Rearm all overlapping.
            void SetEventsEnabled(bool on);

            [[nodiscard]] bool EventsEnabled() const noexcept { return m_eventsEnabled; }

            // ---- joints (P2.5; ports PhysicsWorld.lua addJoint/removeJoint) -
            //
            // AddJoint builds a concrete Joint from the def (MakeJoint, the Lua
            // Joints.make factory) + WAKES its bodies (the Lua def.a/b:wake), so a
            // sleeping captive rejoins the solve, and returns a borrowed Joint*
            // (the world owns it; valid until RemoveJoint / RemoveBody-of-a-member
            // / world destruction). RemoveJoint drops it (the Lua table.remove).
            // RemoveBody auto-removes joints referencing the destroyed body (Lua
            // lines 281-286). The joints are solved inside BOTH solvers' velocity
            // loops (the soft-constraint formulation: sub-stepping softens them).
            //
            // Returns nullptr for an unknown JointKind (never throws).
            Joint* AddJoint(const JointDef& def);

            // Remove a joint by pointer (no-op if not found). Ports removeJoint.
            void RemoveJoint(Joint* j);

            // Live joint count (test/inspection hook; the harness asserts
            // removeJoint drops it). Ports `#w.joints`.
            [[nodiscard]] std::size_t JointCount() const noexcept { return m_joints.size(); }

            // ---- step (kinematic subset) -----------------------------------

            // Advance the world by dt: prev snapshot + KINEMATIC velocity
            // integration + mover-broadphase update, then ContactManager::Step
            // (events + gating + deferred flush). NO dynamics solving.
            void Step(Real dt);

            // Phase D1: inject the task executor the solver parallelizes over.
            // nullptr -> the world's owned SerialTaskExecutor (deterministic default).
            void SetExecutor(ITaskExecutor* exec) noexcept { m_executor = exec; }
            [[nodiscard]] ITaskExecutor* Executor() const noexcept
            {
                return m_executor ? m_executor : &m_serialExecutor;   // always valid; move-safe
            }

            // ---- queries ---------------------------------------------------

            // All live bodies whose shape-AABB intersects box. `out` is cleared
            // then filled with handles. Returns out.size(). Linear scan (ports
            // queryAABB). Index-ordered -> deterministic.
            int QueryAABB(const Aabb2& box, std::vector<BodyHandle>& out) const;

            // Raycast from `from` to `to` (PORT of raycast). Tests SOLID CELLS
            // (Cartesian DDA over the world's TileGrid lattice, if any) and BODY
            // shapes (AABB via the exact slab test; circle/capsule/polygon via a
            // GJK zero-radius point-cast), keeping the NEAREST hit (a body beats
            // a farther cell and vice versa). Returns std::nullopt for a miss or
            // a degenerate (zero-length) ray.
            //
            // opts.tallOnly  -> cells block only when BlocksSight (the LOS rule).
            // opts.cellsOnly -> skip body tests (cell DDA only).
            // With no TileGrid the cell pass is skipped (body tests still run).
            [[nodiscard]] std::optional<RaycastHit>
            Raycast(const Vec2& from, const Vec2& to,
                    const RaycastOpts& opts = {}) const;

            // Line-of-sight (PORT of lineOfSight): true iff NO sight-blocking
            // (TALL) cell lies between `from` and `to`. Equivalent to
            // Raycast(from, to, { tallOnly = true, cellsOnly = true }) == none.
            // A degenerate (zero-length) segment has no LOS (matches the Lua's
            // `if not x1 then return false`).
            // With no TileGrid, always returns true (cell-only test, no cells to
            // block).
            [[nodiscard]] bool LineOfSight(const Vec2& from, const Vec2& to) const;

            // Cast `shape` from `pos` by `delta` against tile spans + non-sensor
            // static bodies (+ optional movers via opts), keeping the NEAREST
            // time of impact (PORT of Cast.shapeCast; reuses the P1.4 GJK
            // conservative-advancement primitive). Returns std::nullopt for a
            // miss or a degenerate (zero-length) delta. The hit's `point` is the
            // shape CENTER at TOI.
            //
            // T7 Part B/C: `movingAngle` is the orientation the moving `shape` is
            // carried at during the sweep (the conservative-advancement holds the
            // angle fixed -- a TRANSLATIONAL sweep, matching the M6 contract). It
            // defaults to 0 so every existing caller stays byte-identical; the
            // BulletSweep (CCD) path passes the bullet fixture's real world angle.
            // Obstacle bodies are also swept with their fixtures' composed real
            // world transforms (rotation + fixture aware, T7).
            [[nodiscard]] std::optional<ShapeCastHit>
            ShapeCast(const Shape& shape, const Vec2& pos, const Vec2& delta,
                      const ShapeCastOpts& opts = {},
                      Real movingAngle = Real(0)) const;

            // All live bodies whose shape OVERLAPS `shape` at transform `xf`
            // (NEW -- composed, no direct Lua method). Uses a broadphase
            // candidate pass narrowed by the unified rotation + fixture-aware
            // Collide (pointCount > 0; T7). Includes statics + movers;
            // self-overlap of the query shape against a body at the same spot
            // counts. `out` is cleared then filled with handles, index-ordered ->
            // deterministic. Returns out.size().
            // NOTE: sensor bodies ARE included; callers wanting non-sensor
            // overlaps must filter via IsSensor() (unlike ShapeCast, which skips
            // sensors).
            int OverlapShape(const Shape& shape, const Transform& xf,
                             std::vector<BodyHandle>& out) const;

            // ---- ergonomic view --------------------------------------------

            // A Body view over a handle (Body.hpp). Cheap, copyable. Valid only
            // while the handle is.
            [[nodiscard]] Body GetBody(BodyHandle h) noexcept;

            // ---- internals consumed by ContactManager (port seam) ----------
            //
            // ContactManager reads the SoA directly (the Lua manager reached
            // into world.shape/posX/.../staticList/moverHash). These mirror that
            // access without exposing the raw vectors to general callers.

            [[nodiscard]] std::uint32_t Count()   const noexcept { return m_count; }
            [[nodiscard]] bool Alive(std::uint32_t i) const noexcept { return m_alive[i] != 0; }
            [[nodiscard]] bool SensorSlot(std::uint32_t i) const noexcept { return m_sensor[i] != 0; }
            [[nodiscard]] bool EvtOn(std::uint32_t i)  const noexcept { return m_evtOn[i] != 0; }
            [[nodiscard]] BodyType TypeSlot(std::uint32_t i) const noexcept
            {
                return static_cast<BodyType>(m_btype[i]);
            }
            [[nodiscard]] const Shape& ShapeSlot(std::uint32_t i) const noexcept { return m_shape[i]; }
            [[nodiscard]] Vec2 PosSlot(std::uint32_t i) const noexcept
            {
                return Vec2(m_posX[i], m_posY[i]);
            }
            // Body angle for slot i (T5: ContactManager ShapesOverlap uses it
            // so the event overlap test is consistent with the rotation-aware
            // GenerateContacts path).
            [[nodiscard]] Real AngleSlot(std::uint32_t i) const noexcept
            {
                return m_angle[i];
            }
            // Local-frame center of mass for slot i (compound-COM dynamics).
            // The COM in the body's LOCAL frame, aggregated from the body's
            // fixtures (single-fixture bodies and all static/kinematic bodies
            // have (0,0)). The solver lifts this to world space via
            // WorldCom(PosSlot(i), AngleSlot(i), LocalCenterSlot(i)) so a
            // compound body rotates about its COM, not its origin.
            [[nodiscard]] Vec2 LocalCenterSlot(std::uint32_t i) const noexcept
            {
                return Vec2(m_localCenterX[i], m_localCenterY[i]);
            }
            // Rotation + fixture-aware overlap test between two body SLOTS (T7
            // Part A). Iterates every fixture of body a against every fixture of
            // body b, composing each fixture's world Transform (bodyPos/angle ∘
            // fixtureLocal) and running the unified rotation-aware Collide; true
            // on the FIRST fixture-pair with a contact point. Falls back to the
            // legacy single shape (m_shape, real m_angle) for a body with no
            // fixtures (mirrors GenerateContacts' fallback). Sensor fixtures are
            // NOT skipped here -- event gating must detect sensor overlaps
            // (sensor-ness is applied later in ContactManager::Emit). The fixture
            // SoA is PRIVATE, so ContactManager (a separate TU) calls THIS rather
            // than reaching into the arrays. Replaces the rotation-blind
            // single-shape CollideShapes path that ContactManager used in T5.
            [[nodiscard]] bool SlotsOverlap(std::uint32_t a,
                                            std::uint32_t b) const;
            // Build the BodyHandle for slot i (index + its current generation).
            // Used by the ContactManager to fill event payloads.
            [[nodiscard]] BodyHandle HandleOf(std::uint32_t i) const noexcept
            {
                return BodyHandle{ i, m_gen[i] };
            }
            [[nodiscard]] const std::vector<std::uint32_t>& StaticList() const noexcept
            {
                return m_staticList;
            }
            // Per-FIXTURE mover broadphase accessor (collision-rebuild Phase 2,
            // Task 1). One proxy per live fixture of a Dynamic/Kinematic body,
            // keyed by fixture slot. Sole mover broadphase as of Task 3.
            [[nodiscard]] const IBroadphase& FixtureBroadphase() const noexcept
            {
                return *m_fixtureBroadphase;
            }
            // Non-const overload: required by consumers that call UpdatePairs
            // (Task 5 -- the incremental move-buffer drain). UpdatePairs is
            // NON-CONST (it mutates m_pairSet/m_moved/m_removed); callers with
            // a non-const PhysicsWorld& (e.g. UpdateContacts) use this overload
            // so the non-const method resolves without a cast.
            [[nodiscard]] IBroadphase& FixtureBroadphase() noexcept
            {
                return *m_fixtureBroadphase;
            }

            // ---- read-only debug-visualization accessors (Slice A) ----------
            //
            // These are PRESENTATION-FREE read paths consumed by the render-side
            // physics debug overlay -- presentation code must NOT enter Core. All
            // are read-only; iteration order is for DISPLAY only and no sim path
            // depends on it.

            // The concrete mover-broadphase DynamicTree, for ForEachLeaf
            // enumeration -- or nullptr when the world was built with a non-Tree
            // mover broadphase (Hash / Sap), which lack the tree's fat/tight leaf
            // structure. POINTER form (not a reference) precisely because the
            // broadphase is selectable: the caller (and the test) null-checks
            // before dereferencing. The default WorldDef builds a DynamicTree, so
            // the default world always returns non-null.
            [[nodiscard]] const DynamicTree* FixtureBroadphaseTree() const noexcept
            {
                return dynamic_cast<const DynamicTree*>(m_fixtureBroadphase.get());
            }

            // The per-shape static-body index (one proxy per static body slot).
            [[nodiscard]] const SpatialGrid& StaticGrid() const noexcept
            {
                return m_staticGrid;
            }

            // The dynamic/kinematic body tile-residency index.
            [[nodiscard]] const SpatialGrid& ResidencyGrid() const noexcept
            {
                return m_residencyGrid;
            }

            // Visit each ContactConstraint generated in the LAST Step (the
            // world-owned pool, valid until the next Step). Read-only; in pool
            // order. ForEachContactConstraint's visit count equals
            // ActiveContactCount().
            void ForEachContactConstraint(
                FunctionRef<void(const ContactConstraint&)> fn) const
            {
                for (const ContactConstraint& cc : m_contactConstraints)
                {
                    fn(cc);
                }
            }

            // Map a fixture slot to its owning body slot (Phase 2, Task 2).
            // Its sole caller -- ContactManager::Step's fixture-pair -> body-pair
            // dedup -- was deleted in Phase 4 (events now derive from the pool's
            // already-body-keyed touch-state), so this is currently UNUSED. Kept
            // as a public fixture->body map; a candidate for Phase-5 dead-code
            // removal.
            [[nodiscard]] std::uint32_t BodyOfFixture(std::uint32_t fi) const noexcept
            {
                return m_fxBody[fi];
            }

            // Re-run the REAL narrowphase on two fixtures and record an opt-in
            // NarrowphaseTrace (debug-viz Slice B inspector seam, Task 3).
            //
            // Composes each fixture's world transform with the SAME
            // ComposeFixtureXf the Step path's GenerateContacts uses, calls
            // out.Clear(), then invokes Collide(shapeA, xfA, shapeB, xfB,
            // /*specMargin*/0, &out) so the returned manifold reproduces the
            // Step's manifold for these two fixtures EXACTLY (hard-contact, no
            // speculative margin), while `out` captures the intermediate
            // narrowphase geometry (SAT axes / GJK / EPA / MPR snapshots).
            //
            // PURE: reads world state, writes only `out`; mutates no simulation
            // state. Returns an empty manifold (and a Cleared trace) for a stale
            // / invalid handle. Intended ONLY for the editor's physics inspector;
            // the Step path never calls this (it passes no trace to Collide, so
            // the simulation narrowphase stays byte-identical).
            [[nodiscard]] Manifold DebugCollide(FixtureHandle a, FixtureHandle b,
                                                NarrowphaseTrace& out) const;

            // Test/oracle helper: parallel arrays of (live mover fixture slot,
            // world AABB) -- the exact set m_fixtureBroadphase indexes.
            void LiveFixtureAabbs(std::vector<std::uint32_t>& fxOut,
                                  std::vector<Aabb2>& boxOut) const;

            // ---- internals consumed by the Soft Step solver (P2.2 seam) -----
            //
            // The solver reads/writes the dynamics SoA through these slot
            // accessors (mirroring the ContactManager seam above) so the raw
            // vectors stay private. Velocity + angVel are mutated every solve
            // iteration; position + angle are committed once per Step at the
            // solver's FinalizePositions. All are inline -> the per-iteration
            // call cost vanishes in an optimized build.
            [[nodiscard]] bool AwakeSlot(std::uint32_t i) const noexcept { return m_awake[i] != 0; }

            // ---- awake-set (Phase B) ----------------------------------------
            // Read-only view of the dense awake-dynamic body slot list (tasks 3/4
            // reroute the hot Step loops onto this; here it is maintained but not
            // yet iterated by any loop -- behavior is byte-identical to Task 1).
            [[nodiscard]] const std::vector<std::uint32_t>& AwakeBodies() const noexcept { return m_awakeBodies; }
            // Visit each awake dynamic slot (replaces for(i=0..count) if(awake&&dynamic)).
            template <typename Fn> void ForEachAwake(Fn&& fn) const { for (const std::uint32_t s : m_awakeBodies) { fn(s); } }

            // ---- dense solverIndex helpers (Phase C, Task 2) ----------------
            // The lane-wide solve re-homes its body-state scratch onto a DENSE
            // solverCount-sized SoA (no per-world-slot holes): awake dynamics take
            // [0, AwakeCount()) at AwakeIndexOf(slot); kinematics take
            // [AwakeCount(), AwakeCount()+KinematicCount()) at
            // AwakeCount()+KinematicIndexOf(slot); statics/spans/padding map to the
            // shared zero DUMMY tail at solverCount. These inline accessors let the
            // solver + the ContactConstraintSimd packer compute solverIndex without
            // friending PhysicsWorld internals. AwakeIndexOf(slot) returns kNotAwake
            // for a non-awake-dynamic slot; KinematicIndexOf(slot) returns
            // kNotKinematic for a non-kinematic slot -- the packer's kinematic-vs-
            // static B gate uses the kNotKinematic sentinel to tell a real kinematic
            // B (real dense row -> authored-velocity push) from a static B (dummy).
            [[nodiscard]] std::uint32_t AwakeIndexOf(std::uint32_t slot) const noexcept { return m_awakeIndex[slot]; }
            [[nodiscard]] std::uint32_t KinematicIndexOf(std::uint32_t slot) const noexcept { return m_kinematicIndex[slot]; }
            [[nodiscard]] std::uint32_t AwakeCount() const noexcept { return static_cast<std::uint32_t>(m_awakeBodies.size()); }
            [[nodiscard]] std::uint32_t KinematicCount() const noexcept { return static_cast<std::uint32_t>(m_kinematicBodies.size()); }
            // Raw per-slot index-array pointers so the packer (ContactConstraintSimd::
            // Build/PackLane) can map a body slot -> solverIndex without depending on
            // PhysicsWorld.hpp (it takes the arrays as plain pointers). The arrays are
            // per-world-slot-sized (== Count()), so a bodyA/bodyB world slot indexes
            // them directly. Stable for the duration of a Step (no resize mid-solve).
            [[nodiscard]] const std::uint32_t* AwakeIndexData() const noexcept { return m_awakeIndex.data(); }
            [[nodiscard]] const std::uint32_t* KinematicIndexData() const noexcept { return m_kinematicIndex.data(); }
            // Awake-set maintenance (called at create/sleep/wake/remove seams).
            // Both are idempotent and are PUBLIC so Island.cpp can reach them at
            // the sleep seam without coupling Island.cpp to a PhysicsWorld private.
            void AddToAwakeSet(std::uint32_t slot) noexcept;
            void RemoveFromAwakeSet(std::uint32_t slot) noexcept;

            // ---- kinematic solver-set (Phase C) -----------------------------
            // Read-only view of the dense KINEMATIC body slot list (the solver's
            // read-only B-endpoint working set). Task 2 reroutes the solver onto
            // this; here it is maintained but not yet iterated by any loop --
            // behavior is byte-identical. Unlike the awake-set, kinematics never
            // sleep, so this set is maintained ONLY at create/remove (no sleep/wake
            // path exists for it).
            [[nodiscard]] const std::vector<std::uint32_t>& KinematicBodies() const noexcept { return m_kinematicBodies; }
            // Visit each live kinematic slot (mirrors ForEachAwake for kinematics).
            template <typename Fn> void ForEachKinematic(Fn&& fn) const { for (const std::uint32_t s : m_kinematicBodies) { fn(s); } }
            // Kinematic-set maintenance (called at the AddBody/RemoveBody seams).
            // Both are idempotent; gate on BodyType::Kinematic.
            void AddToKinematicSet(std::uint32_t slot) noexcept;
            void RemoveFromKinematicSet(std::uint32_t slot) noexcept;

            // ---- persistent incremental contact coloring (Phase C, Task 4) ---
            //
            // A solver-relevant body-body contact is assigned a graph color ONCE at
            // create (AssignContactColor) and releases it at destroy
            // (ReleaseContactColor) -- the assign-at-create / release-at-destroy
            // replacement for the per-step greedy recolor. The invariant: no two
            // same-color contacts share a DYNAMIC body (a static/kinematic endpoint
            // never constrains coloring, mirroring ColorConstraints' aDyn/bDyn rule).
            //
            // CONSUMED by the solver (Task 5): the per-body color mask gates
            // AssignContactColor's lowest-free search, and the solver buckets
            // contacts by this persistent color instead of recoloring per step.
            // This persistent coloring is a DIFFERENT but equally-valid color
            // partition than the old per-step greedy one; because the colored solve
            // is Gauss-Seidel (color k's velocity updates feed color k+1), a
            // different valid partition is an INTENTIONAL re-baseline vs pre-Phase-C
            // main (different floats), NOT bit-identical. The contract that holds is
            // run-twice DETERMINISM + the behavioral [physics] suite (no exact
            // goldens) -- per the engine's re-baseline-numerics-on-purpose rule.
            // Public so the [phasec] coloring-validity test can call the
            // oracle/probes.

            // Assign the lowest free color to a NEW solver-relevant body-body
            // contact `id` between body slots `a`/`b`. `aDyn`/`bDyn` mark which
            // endpoints are dynamic (only dynamic endpoints constrain coloring +
            // occupy a per-body color bit). If no color in [0, kColorCount) is free
            // for both dynamic endpoints, the contact spills to overflow
            // (color == kInvalidColor). Caller passes the ORIENTED slots/dyn flags
            // computed in TryCreateContact.
            void AssignContactColor(std::uint32_t id, std::uint32_t a, std::uint32_t b,
                                    bool aDyn, bool bDyn);

            // Release contact `id`'s color back to its dynamic endpoints. No-op for
            // an uncolored (kInvalidColor) contact. Recomputes dyn-ness from the
            // contact's cached body slots (a body's type is fixed for its life), so
            // this is exact: the coloring invariant guarantees a body has at most
            // one contact per color, so clearing the bit on destroy frees it cleanly.
            void ReleaseContactColor(std::uint32_t id);

            // The persistent color of contact `id` (kInvalidColor if uncolored).
            // Read-only probe; not used by the Step path.
            [[nodiscard]] std::uint8_t ContactColorOf(std::uint32_t id) const;

            // Oracle: walk the live coloring and prove the invariant -- no DYNAMIC
            // body appears twice in one color, every listed contact is alive and
            // tagged with its color. Returns false on any violation. Used by the
            // [phasec] coloring-validity test (Debug + Release).
            [[nodiscard]] bool ValidatePersistentColoring() const;

            // Read-only probe: the total number of COLORED contacts (the sum of
            // m_colorContacts[k].size() over all colors). Lets the [phasec] coloring-
            // validity test prove the oracle did not trivially pass on an EMPTY
            // coloring (assert this > 0 after a settle that creates contacts). Not
            // used by the Step path.
            [[nodiscard]] std::size_t ColoredContactCount() const noexcept;

            [[nodiscard]] Real InvMassSlot(std::uint32_t i) const noexcept { return m_invMass[i]; }
            [[nodiscard]] Real InvInertiaSlot(std::uint32_t i) const noexcept { return m_invInertia[i]; }
            [[nodiscard]] Real RestSlot(std::uint32_t i) const noexcept { return m_rest[i]; }
            [[nodiscard]] Real FricSlot(std::uint32_t i) const noexcept { return m_fric[i]; }
            [[nodiscard]] Real LinDampSlot(std::uint32_t i) const noexcept { return m_linDamp[i]; }
            [[nodiscard]] Vec2 VelSlot(std::uint32_t i) const noexcept
            {
                return Vec2(m_velX[i], m_velY[i]);
            }
            [[nodiscard]] Real AngVelSlot(std::uint32_t i) const noexcept { return m_angVel[i]; }
            void SetVelSlot(std::uint32_t i, Vec2 v) noexcept
            {
                m_velX[i] = v.x;
                m_velY[i] = v.y;
            }
            void SetAngVelSlot(std::uint32_t i, Real w) noexcept { m_angVel[i] = w; }

            // ---- pull API for debug draw / inspection (P3.6) ----------------
            //
            // These are PRESENTATION-FREE read paths consumed by the Arcane.dll
            // render-side PhysicsDebugDraw -- presentation code must NOT enter
            // Core.  Both forward to Core-internal members without exposing raw
            // vectors to general callers.

            // Visit each contact pair in the begun state (begun == true) from
            // the LAST Step, passing the two body SLOT indices.  Read-only;
            // unordered iteration (unordered_map traversal -- acceptable for a
            // debug overlay).  Forwards to ContactManager::ForEachBegunPair.
            void ForEachContact(
                FunctionRef<void(std::uint32_t a,
                                 std::uint32_t b)> fn) const;

            // Island ROOT of slot i (debug/inspection, Phase A).
            // Phase A: reads m_islandId -- the persistent island id IS the root
            // (equal for all co-island members after merge, distinct across
            // islands). Non-members (static/kinematic or un-assigned) return a
            // high-bit-tagged slot that can never collide with a real (dense,
            // small) island id. Replaces the old per-step UF walk.
            [[nodiscard]] std::uint32_t IslandRootOf(std::uint32_t i) const noexcept;

            // ---- island registry management (Phase A) -----------------------
            // Mint or reuse an island id (empty members, not a split candidate).
            std::uint32_t AllocIsland();
            // Return an island id to the free list (clears members + flag).
            void          FreeIsland(std::uint32_t id) noexcept;
            // m_islandId[slot] (or kInvalidIsland). Inline -> zero call cost.
            [[nodiscard]] std::uint32_t IslandOf(std::uint32_t slot) const noexcept
            {
                return slot < m_islandId.size() ? m_islandId[slot] : Island::kInvalidIsland;
            }
            // Weighted union: relabel the smaller island's members into the larger,
            // free the smaller, return the survivor id. Pass two DISTINCT live ids.
            std::uint32_t MergeIslands(std::uint32_t idA, std::uint32_t idB);
            // Flag an island for the deferred split pass (no-op for kInvalidIsland).
            void          MarkSplitCandidate(std::uint32_t islandId) noexcept;
            // Rebuild one candidate island into 1+ connected components (fresh local
            // UF over its members' current touching pool contacts); clears the flag.
            void          SplitIsland(std::uint32_t islandId);
            // Remove contact `id` from both endpoints' m_bodyContacts (dyn-dyn
            // only; no-op for non-dyn-dyn or already-absent). Reads c BEFORE any
            // pool Destroy frees it.
            void DetachContactAdjacency(std::uint32_t id, const Contact& c) noexcept;
            // The canonical pooled-contact teardown: detach adjacency + release
            // the persistent color (while c still holds it) + pool.Destroy.
            void ReleaseAndDestroyContact(std::uint32_t id, const Contact& c) noexcept;
            // Wake every member of the body's island (set awake, reset timer).
            void          WakeIsland(std::uint32_t slot) noexcept;

            // ---- internals consumed by the Island sleep module (P2.4 seam) ---
            //
            // Island::UpdateSleep reads/writes the sleep-timer SoA + flips the
            // awake flag through these slot accessors (mirroring the solver seam
            // above) so the raw vectors stay private. SetAwakeSlot(i, false) +
            // zeroing the velocities is how an island goes to sleep; the
            // sleep-timer accumulates idle time per body. Inline -> the
            // per-body call cost vanishes in an optimized build.
            [[nodiscard]] Real SleepTimerSlot(std::uint32_t i) const noexcept { return m_sleepTimer[i]; }
            void SetSleepTimerSlot(std::uint32_t i, Real t) noexcept { m_sleepTimer[i] = t; }
            // Box2D-faithful sleep test inputs (Island::UpdateSleep): maxExtent is
            // the body's COM->farthest-point distance; sleepThreshold is the per-body
            // speed gate (a body is idle when |v| + |w|*maxExtent < sleepThreshold).
            [[nodiscard]] Real MaxExtentSlot(std::uint32_t i) const noexcept { return m_maxExtent[i]; }
            [[nodiscard]] Real SleepThresholdSlot(std::uint32_t i) const noexcept { return m_sleepThreshold[i]; }
            // WARNING (Phase B awake-set invariant): this writes ONLY the m_awake
            // flag -- it does NOT maintain the awake-set (m_awakeBodies). The sleep
            // seam pairs SetAwakeSlot(i,false) with RemoveFromAwakeSet(i) in
            // Island::UpdateSleep. To WAKE a body, do NOT call SetAwakeSlot(i,true)
            // as a primitive: use Wake()/WakeIsland() (which call AddToAwakeSet), or
            // pair an explicit AddToAwakeSet(i). A raw SetAwakeSlot(i,true) on a
            // slept dynamic would leave it awake-but-not-in-the-set: the solver
            // loops (ForEachAwake) would skip it (frozen) while EmitContactConstraints
            // (gating on m_awake) would still emit a constraint reading its stale
            // SoA velocity -- a silent desync. (No caller does this today.)
            void SetAwakeSlot(std::uint32_t i, bool on) noexcept
            {
                m_awake[i] = on ? std::uint8_t(1) : std::uint8_t(0);
            }
            // Snap the prev-position to the current position for slot i.
            // Called at the sleep seam (Island::UpdateSleep) so a body that just
            // fell asleep has prev==pos. Required because Stage 1 will only snap
            // AWAKE dynamics after the reroute: a sleeping body is never snapped
            // by Stage 1, so without this call its prev could drift from its
            // frozen pos and DrawPosition(alpha) would interpolate incorrectly.
            void SnapPrevToPos(std::uint32_t i) noexcept
            {
                m_prevX[i] = m_posX[i];
                m_prevY[i] = m_posY[i];
            }
            // Visit each LIVE island's member-slot list (Phase A sleep seam). A live
            // island has a non-empty member list; freed ids (empty) are skipped.
            // Iterated ascending island-id (deterministic). Const callback (sleep
            // mutates bodies through the slot accessors, not the island record).
            void ForEachIsland(
                FunctionRef<void(const std::vector<std::uint32_t>&)> fn) const
            {
                for (const Island::Island& isl : m_islands)
                {
                    if (!isl.bodies.empty())
                    {
                        fn(isl.bodies);
                    }
                }
            }
            // Commit final position/angle for slot i + refresh the mover
            // broadphase AABB (solver FinalizePositions). Dynamic-only call site.
            void CommitSlotPosition(std::uint32_t i, Vec2 p, Real angle) noexcept
            {
                m_posX[i]  = p.x;
                m_posY[i]  = p.y;
                m_angle[i] = angle;
                // UpdateMoverProxies refreshes all per-fixture mover-broadphase
                // proxies and the body's residency grid in one call
                // (Phase 2, Task 3: replaces the inline triple).
                UpdateMoverProxies(i);
            }
            // Global gravity (solver reads it for the per-sub-step integrate).
            [[nodiscard]] Vec2 Gravity() const noexcept
            {
                return Vec2(m_gravityX, m_gravityY);
            }
            // Solver warm-start cache size (inspection/test hook). Routes through
            // the installed ISolver. Meaningful ONLY for a solver that keeps a
            // keyed cache (Baumgarte); SoftStep relocated warm-start onto the
            // persistent Contact's manifold points and reports 0 (the ISolver
            // default override).
            [[nodiscard]] std::size_t SolverWarmStartCacheSize() const noexcept
            {
                return m_solver ? m_solver->WarmStartCacheSize() : std::size_t(0);
            }

            // Solver overflow (un-colorable spill) count from the last Step
            // (inspection/test hook). Routes through the installed ISolver. Meaningful
            // ONLY for a coloring solver that spills past kColorCount (SoftStep);
            // Baumgarte colors nothing and reports 0 (the ISolver default). EXISTS so
            // the overflow-settle test can directly PROVE the scalar overflow path was
            // exercised instead of assuming it "by construction".
            [[nodiscard]] std::size_t SolverOverflowCount() const noexcept
            {
                return m_solver ? m_solver->LastOverflowCount() : std::size_t(0);
            }

            // Count of ContactConstraints fed to the solver in the last Step
            // (debug/inspection hook). As of Phase 3 Task 4 m_contactConstraints is
            // produced by EmitContactConstraints (walking the persistent pool + the
            // transient span scratch) each Step. Returns its size immediately after
            // Step() returns.
            // Use: test (d) asserts >= 2 for a compound body (one constraint per
            // fixture-pair contact against the floor).
            [[nodiscard]] std::size_t ActiveContactCount() const noexcept
            {
                return m_contactConstraints.size();
            }

            // Test/inspection hook (collision-rebuild Phase 3, Task 2): the live
            // count of the PERSISTENT contact pool, populated each Step by
            // UpdateContacts. As of Task 4 this pool IS the solver feed (walked by
            // EmitContactConstraints). Reflects the pool's lifecycle (survive across
            // steps, destroy on fat-box separation / body removal). Note: it counts
            // fixture<->fixture contacts only -- tile spans are transient (not pooled).
            [[nodiscard]] std::size_t DebugContactCount() const noexcept
            {
                return m_contactPool.Count();
            }

            // Test invariant: true iff m_bodyContacts exactly mirrors the live
            // dyn-dyn body contacts (every such contact's id appears once in BOTH
            // endpoints' lists, and every id in every list is a live dyn-dyn body
            // contact incident to that slot, with no duplicates).
            [[nodiscard]] bool DebugValidateBodyContacts() const;

            // Test/inspection hook (collision-rebuild Phase 4, Task 1): true iff the
            // persistent pool holds a contact whose body SLOTS match the unordered
            // pair (a, b). Scans the pool's cached body slots (c.bodyA / c.bodyB),
            // mirroring the DebugContactCount read path. Used by the event-union
            // tests to assert that sensor + kinematic pairs now create a pooled
            // contact (even though they do NOT reach the solver feed). Stale/dead
            // handles return false (they never match a live contact's body slots).
            [[nodiscard]] bool DebugHasContact(BodyHandle a, BodyHandle b) const
            {
                if (!IsValid(a) || !IsValid(b))
                {
                    return false;
                }
                const std::uint32_t sa = a.index;
                const std::uint32_t sb = b.index;
                bool found = false;
                m_contactPool.ForEach(
                    [&](std::uint32_t /*id*/, const Contact& c)
                {
                    if (!c.bIsBody)
                    {
                        return; // tile span (never a body-pair)
                    }
                    if ((c.bodyA == sa && c.bodyB == sb) ||
                        (c.bodyA == sb && c.bodyB == sa))
                    {
                        found = true;
                    }
                });
                return found;
            }

            // Oracle-gate hooks (collision-rebuild Phase 3, Task 3). Read-only.
            //
            // DebugEmitPoolConstraints: walk the persistent contact pool + transient
            // spans and emit the solver-feed ContactConstraint set (the live feed),
            // sorted into the deterministic canonical order. Does NOT touch sim state.
            // DebugCopyActiveConstraints: a copy of m_contactConstraints (which, as
            // of Task 4, is itself the EmitContactConstraints output). The two sets
            // are equal on a body-only scene (the Task-3 equivalence gate, now a feed
            // self-consistency check post-swap).
            void DebugEmitPoolConstraints(std::vector<ContactConstraint>& out) const
            {
                EmitContactConstraints(out);
            }
            void DebugCopyActiveConstraints(std::vector<ContactConstraint>& out) const
            {
                out = m_contactConstraints;
            }

            // Soft Step config (solver reads it in Prepare / the sub-step loop).
            [[nodiscard]] std::uint32_t SubstepCount() const noexcept { return m_substepCount; }
            [[nodiscard]] Real ContactHertz() const noexcept { return m_contactHertz; }
            [[nodiscard]] Real ContactDampingRatio() const noexcept { return m_contactDampingRatio; }
            [[nodiscard]] Real RestitutionThreshold() const noexcept { return m_restitutionThreshold; }
            [[nodiscard]] Real ContactPushMaxVelocity() const noexcept { return m_contactPushMaxVelocity; }
            // World default sleep gate (px/s); AddBody copies it onto a body whose
            // BodyDef::sleepThreshold is < 0 (the inherit sentinel).
            [[nodiscard]] Real SleepThresholdDefault() const noexcept { return m_sleepThresholdDefault; }

            // Baumgarte velocity-iteration count (Lua w.velIters; SoftStep
            // ignores it). Read by the Baumgarte solver in its Solve().
            [[nodiscard]] std::uint32_t VelIters() const noexcept { return m_velIters; }

            // World-space tight AABB of slot i. Exposed here (not just private)
            // so ContactManager can use it for the AABB pre-filter on the
            // kinematic-vs-static loop without an extra round-trip through
            // GetShape + a separate ComputeAABB call.
            [[nodiscard]] Aabb2 SlotAabb(std::uint32_t i) const noexcept;

            // Static collision candidates near a box: merged tile spans (into
            // spansOut) + overlapping static-body slots (into staticsOut). Ports
            // _staticCandidates. Both vectors are cleared then filled. With no
            // TileGrid spansOut is empty. (Wired for P1.9/P1.10; the P1.8 event
            // tests use plain bodies and need no tile spans.)
            // gridScratch: caller-supplied scratch for SpatialGrid::QueryAABB (was the
            // shared mutable m_staticGridScratch; now caller-owned so the query is
            // re-entrant for the parallel create-phase detect).
            void StaticCandidates(const Aabb2& box, std::vector<Aabb2>& spansOut,
                                  std::vector<std::uint32_t>& staticsOut,
                                  std::vector<std::uint32_t>& gridScratch) const;

            // First-class tile residency: dynamic/kinematic body slots register
            // their world AABB cell-occupancy, refreshed when their position
            // commits (same lifecycle path as the mover broadphase). Region/tile
            // queries serve gameplay (combat-sphere extraction).
            int Residents(const Aabb2& region, std::vector<std::uint32_t>& out) const
            { return m_residencyGrid.QueryAABB(region, out); }

        private:
            // ---- per-fixture broadphase helpers (Phase 2, Task 1) -------------
            //
            // UpdateMoverProxies(b): refreshes residency and all fixture proxies in
            // m_fixtureBroadphase for every live fixture owned by body b. Called at
            // every position-commit site so the fixture broadphase stays in lockstep
            // with residency automatically.
            void UpdateMoverProxies(std::uint32_t b);

            // Add / remove a single fixture proxy in m_fixtureBroadphase.
            // AddFixtureProxy skips Static bodies (they are not mover proxies).
            void AddFixtureProxy(std::uint32_t fi);
            void RemoveFixtureProxy(std::uint32_t fi);

            // World-space AABB for a single fixture slot fi (body transform ∘
            // fixture local transform → Shape::ComputeAABB).
            [[nodiscard]] Aabb2 FixtureAabb(std::uint32_t fi) const noexcept;

            // Grow all SoA arrays to hold at least `n` slots.
            void EnsureCapacity(std::uint32_t n);

            // PORT of Cast.rayVsBody: exact TOI of a RAY (a zero-radius circle
            // cast) against slot `idx`'s shape via the GJK conservative-
            // advancement primitive. `from` is the ray origin, `delta` its full
            // displacement (the ray spans from..from+delta). Returns the
            // parametric t in [0,1] or nullopt (no hit). Internal: used by
            // Raycast's round/poly body tests.
            [[nodiscard]] std::optional<Real>
            RayVsBody(std::uint32_t idx, const Vec2& from, const Vec2& delta) const;

            // Compose a fixture's WORLD transform = body transform ∘ fixture
            // local transform:
            //   worldPos   = bodyPos + R(bodyAngle) * localPos
            //   worldAngle = bodyAngle + localAngle
            // The single copy of the rotate+offset formula, shared by SlotAabb,
            // GenerateContacts' FixtureWorldXf lambda, SlotsOverlap (events),
            // the fixture-aware queries (Queries.cpp), and BulletSweep (CCD).
            // Static so it can be called wherever the SoA values are in hand.
            [[nodiscard]] static Transform ComposeFixtureXf(Vec2 bodyPos,
                                                            Real bodyAngle,
                                                            Vec2 localPos,
                                                            Real localAngle) noexcept;

            // ---- Fixture SoA (v2 Task 4; additive) ---------------------------
            //
            // Parallel arrays keyed by fixture slot index (free-list discipline
            // identical to the body SoA).  Each body slot owns a vector of
            // fixture slot indices (m_bodyFixtures) -- setup-time alloc is
            // acceptable; Step never touches this path until T5.
            //
            // m_fxBody:  owning body's SoA SLOT index (not generation-gated).
            // m_fxGen:   generation per fixture slot (0 = dead; live starts at 1).
            // m_fxCount / m_fxFree: high-water mark + recycled slot stack (mirrors
            //            the body SoA m_count / m_free discipline).
            std::vector<Shape>          m_fxShape;
            std::vector<Real>           m_fxLocalPosX, m_fxLocalPosY;
            std::vector<Real>           m_fxLocalAngle;
            std::vector<Real>           m_fxDensity;
            std::vector<Real>           m_fxFriction;
            std::vector<Real>           m_fxRestitution;
            std::vector<std::uint32_t>  m_fxFilterCat;
            std::vector<std::uint32_t>  m_fxFilterMask;
            std::vector<std::uint8_t>   m_fxSensor;
            std::vector<std::uint32_t>  m_fxBody;    // owning body slot
            std::vector<std::uint32_t>  m_fxGen;     // generation per fixture slot

            std::uint32_t              m_fxCount = 0;
            std::vector<std::uint32_t> m_fxFree;

            // Per-body fixture index list (keyed by body SoA slot index).
            // A vector of vectors: grows to m_count on first use; each inner
            // vector holds the fixture slot indices owned by that body slot.
            // Setup-time alloc only (AddFixture / DropFixture / RemoveBody).
            std::vector<std::vector<std::uint32_t>> m_bodyFixtures;

            // Per-body dyn-dyn contact adjacency (keyed by body SoA slot), the
            // Box2D b2ContactEdge analogue used by SplitIsland to walk only an
            // island's own contacts. Holds the POOL IDS of each dynamic body's
            // dyn-dyn body contacts (both endpoints carry the id). Maintained ONLY
            // at contact create + destroy; merge/split/sleep/wake never touch it
            // (body slots are stable -- only m_islandId changes). Mirrors the
            // m_bodyFixtures vector-of-vectors lifecycle.
            std::vector<std::vector<std::uint32_t>> m_bodyContacts;

            // Per-body localCenter (COM in body-local frame, aggregated over
            // fixtures).  STORED but not yet consumed by integration/solver (T5).
            // Default (0,0) for bodies with no fixtures or Static/Kinematic.
            std::vector<Real> m_localCenterX, m_localCenterY;

            // Per-body total mass (used by GetBodyMass / GetBodyInertia accessors
            // in the test surface; the solver reads m_invMass directly).
            std::vector<Real> m_bodyMass;

            // Per-body inertia about the COM (before forcing invInertia=0 for
            // fixedRotation; stored for the accessor).
            std::vector<Real> m_bodyInertia;

            // Per-body fixedRotation flag (needed by RecomputeBodyMass to force
            // invInertia = 0; mirrors the BodyDef flag stored at AddBody).
            std::vector<std::uint8_t> m_fixedRotation;

            // Per-body mass override (mirrors BodyDef::mass; 0 means "no override").
            // Set by AddBody when def.mass > 0.  RecomputeBodyMass honours it so
            // a subsequent AddFixture does not silently discard an explicit mass.
            std::vector<Real> m_massOverride;

            // Ensure the fixture SoA and per-body auxiliary arrays have at least
            // n fixture slots.
            // NOTE: seeds at 4 slots when empty (unlike the body EnsureCapacity
            // which grows from capacity()*2, yielding 1 slot on first body add).
            // The different seeds are intentional: fixture slots are allocated in
            // small batches (AddBody creates 1, AddFixture creates 1) so pre-seeding
            // 4 avoids a realloc on the very first fixture without disturbing the
            // body SoA growth timing that existing tests depend on.
            void EnsureFxCapacity(std::uint32_t n);

            // Ensure per-body arrays are large enough for body slot index `bodySlot`.
            // Called when a new body slot is activated (alongside EnsureCapacity).
            // Also seeds at 4 when empty (same rationale as EnsureFxCapacity above).
            void EnsureBodyAuxCapacity(std::uint32_t n);

            // Re-aggregate mass / COM / inertia for body slot `bodySlot` from its
            // current fixture list.  Updates m_invMass / m_invInertia / m_bodyMass
            // / m_localCenterX/Y / m_bodyInertia.  Static/Kinematic keep 0/0.
            // Respects m_massOverride[bodySlot]: if > 0 the density-derived total
            // mass is replaced by the override and inertia is scaled proportionally,
            // so a mass override set at AddBody is preserved through AddFixture.
            void RecomputeBodyMass(std::uint32_t bodySlot);

            // Recompute the cached COM->farthest-fixture-point distance (+radius)
            // used by the sleep velocity test (Box2D sim->maxExtent). Call after the
            // body's COM (m_localCenter) is finalized -- AddBody + RecomputeBodyMass.
            void RecomputeMaxExtent(std::uint32_t bodySlot);

            // Allocate and populate a fixture slot for `bodySlot` from `def`,
            // link it into m_bodyFixtures, and return the slot index.  Does NOT
            // call RecomputeBodyMass -- callers do that themselves (AddFixture
            // always recomputes; the AddBody back-compat path does NOT, keeping
            // the legacy invMass/invInertia path byte-for-byte unchanged).
            std::uint32_t AllocFixtureSlot(std::uint32_t bodySlot, const FixtureDef& def);

            // ---- SoA (port of the Lua FFI arrays; std::vector here) ---------
            std::vector<Real>          m_posX, m_posY;
            std::vector<Real>          m_prevX, m_prevY;
            std::vector<Real>          m_velX, m_velY;
            std::vector<std::uint8_t>  m_btype;   // BodyType
            std::vector<std::uint8_t>  m_evtOn;   // per-body event gate
            std::vector<std::uint8_t>  m_alive;
            std::vector<std::uint8_t>  m_sensor;
            std::vector<std::uint32_t> m_gen;     // generation per slot
            std::vector<Shape>         m_shape;   // by value (immutable, small)

            // ---- dynamics SoA (P2.1; zeros for Static/Kinematic) ------------
            //
            // PORT of the Lua dynamics arrays (PhysicsWorld.lua:159-163). For
            // Static/Kinematic bodies these stay at the defaults below (0 inverse
            // mass/inertia, awake=1) so they never integrate or solve -- the
            // existing kinematic/static behaviour is unchanged.
            std::vector<Real>          m_angle, m_angVel;     // orientation + rate
            std::vector<Real>          m_invMass, m_invInertia;
            std::vector<Real>          m_rest, m_fric;        // solver params (P2.2)
            std::vector<Real>          m_linDamp;             // velocity decay
            std::vector<Real>          m_sleepTimer;          // island sleep (P2.4)
            std::vector<Real>          m_maxExtent;           // body COM->farthest-point dist (+radius); sleep test
            std::vector<Real>          m_sleepThreshold;      // per-body sleep speed gate (px/s); see WorldDef/BodyDef
            std::vector<std::uint8_t>  m_awake;               // 1 = awake (integrates this step; P2.4 sleep clears to 0)
            std::vector<std::uint8_t>  m_bullet;              // CCD clamp (P3)

            // ---- persistent island registry (Phase A) -----------------------
            //
            // m_islandId[slot] is the body's persistent island id (Island::
            // kInvalidIsland for Static/Kinematic or an un-assigned dynamic slot).
            // m_islands is the id-indexed record pool; freed ids are recycled via
            // m_islandFree. Maintained incrementally at the lifecycle seams; the
            // per-step union-find (m_uf / UnionFindScratch) has been deleted (Task 6)
            // -- the persistent registry IS the sole island structure.
            std::vector<std::uint32_t>      m_islandId;   // per-body island id
            std::vector<Island::Island>     m_islands;    // id-indexed record pool
            std::vector<std::uint32_t>      m_islandFree; // recycled island ids

            // ---- awake-set (Phase B) ----------------------------------------
            // m_awakeBodies is the dense list of AWAKE DYNAMIC body slots (the
            // solver/integrate working set; Tasks 3/4 reroute the hot loops onto
            // it). m_awakeIndex[slot] is the slot's position in that list (or
            // kNotAwake). Maintained incrementally at create/sleep/wake/remove;
            // iteration order is NOT ascending-slot (append/swap-remove) -- safe
            // because the rerouted loops do only independent per-body work.
            std::vector<std::uint32_t> m_awakeBodies;
            std::vector<std::uint32_t> m_awakeIndex;   // per-body-slot position, or kNotAwake

            // ---- kinematic solver-set (Phase C) -----------------------------
            // m_kinematicBodies is the dense list of LIVE KINEMATIC body slots (the
            // solver's read-only B-endpoint working set; Task 2 reroutes onto it).
            // m_kinematicIndex[slot] is the slot's position in that list (or
            // kNotKinematic). Maintained incrementally ONLY at create/remove --
            // kinematics never sleep, so there is no sleep/wake seam. Iteration
            // order is NOT ascending-slot (append/swap-remove), which is safe
            // because the consumer does only independent per-body reads.
            std::vector<std::uint32_t> m_kinematicBodies;
            std::vector<std::uint32_t> m_kinematicIndex; // per-body-slot position, or kNotKinematic

            // Step scratch (zero steady-state alloc -- clear() keeps capacity).
            // m_pendingMerges: dynamic-dynamic touch-BEGIN body-pairs collected in
            // the UpdateContacts pass, sorted canonically then applied (determinism).
            // m_splitCandidates: island ids to rebuild this Step (quota-limited).
            std::vector<BroadphasePair>     m_pendingMerges;
            std::vector<std::uint32_t>      m_splitCandidates;

            // SplitIsland scratch: member body slot -> local DSU index, O(1).
            // All-sentinel; SplitIsland writes only its members then resets them,
            // so the tail stays sentinel. Sized in EnsureCapacity.
            static constexpr std::uint32_t kSplitLocalNone = 0xFFFFFFFFu;
            std::vector<std::uint32_t>      m_splitLocalIndex;

            // Narrowphase-MT scratch (gather -> parallel collide+flag -> serial apply).
            // m_npContacts: the gathered stable live-contact id list (Box2D's
            // contactSims gather). m_npStateBits: one BitSet per worker, each Resize'd
            // to the pool id capacity per step (Box2D's per-worker contactStateBitSet).
            std::vector<std::uint32_t> m_npContacts;
            std::vector<Arcane::BitSet> m_npStateBits;

            std::uint32_t              m_count = 0; // high-water slot count
            std::vector<std::uint32_t> m_free;      // recycled slot stack

            // ---- broadphase + statics --------------------------------------
            // Per-FIXTURE mover broadphase (collision-rebuild Phase 2, Task 1):
            // one proxy per live fixture of a Dynamic/Kinematic body, keyed by
            // fixture slot. Sole mover broadphase as of Task 3.
            std::unique_ptr<IBroadphase> m_fixtureBroadphase;
            std::vector<std::uint32_t>   m_staticList; // slot indices of statics
            std::unique_ptr<TileGrid>    m_tileGrid;   // optional tile statics

            // Per-shape static index (collision-rebuild Phase 1). Static BODIES
            // register their slot id here (keyed by body slot; statics are single
            // proxies today -- per-fixture proxies arrive in Phase 2). StaticCandidates
            // queries this instead of the O(dynamics*statics) m_staticList scan.
            // Tile size = a coarse default until the map's tile size is wired in.
            SpatialGrid m_staticGrid{ Real(64) }; // TODO(Phase 2): wire to the map's real tile size
            // Dedicated query scratch for the grid lookup inside StaticCandidates.
            // MUST NOT reuse m_scratchStatics: ShapeCast calls
            // StaticCandidates(..., m_scratchStatics) as the OUTPUT, so reusing it
            // for the grid query would alias and corrupt the result.
            mutable std::vector<std::uint32_t> m_staticGridScratch;

            // Dynamic + kinematic body tile-residency index (Phase 1, Task 5).
            // Tracks live cell-occupancy for movers as they move -- the gameplay
            // region-query index (combat-sphere seam). Kept in LOCKSTEP with
            // m_fixtureBroadphase: Insert on AddBody, Move on every broadphase Update
            // (SetPosition/MovePosition/kinematic-integrate/BulletSweep/CommitSlotPosition),
            // Remove on RemoveBody. Tile size matches m_staticGrid for consistency.
            SpatialGrid m_residencyGrid{ Real(64) }; // TODO(Phase 2): wire to the map's real tile size

            bool m_eventsEnabled = true;

            // ---- dynamics config (P2.1 + P2.2) -----------------------------
            // Global gravity applied to awake Dynamic bodies in Step.
            Real m_gravityX = Real(0);
            Real m_gravityY = Real(0);

            // Soft Step config (copied from WorldDef; read by the solver).
            std::uint32_t m_substepCount         = 4u;
            Real          m_contactHertz         = Real(30);
            Real          m_contactDampingRatio  = Real(10);
            Real          m_restitutionThreshold = Real(20);
            Real          m_contactPushMaxVelocity = Real(300);
            Real          m_sleepThresholdDefault  = Real(8);   // WorldDef::sleepThreshold

            // Baumgarte-only velocity iterations (copied from WorldDef.velIters).
            std::uint32_t m_velIters = 8u;

            // ---- contacts --------------------------------------------------
            ContactManager m_contacts;

            // ---- solver (P2.2 SoftStep / P2.3 Baumgarte; A/B via solverKind) -
            //
            // The installed constraint solver (polymorphic since P2.3 -- the
            // world holds an ISolver* chosen by WorldDef::solverKind) + the
            // WORLD-OWNED ContactConstraint pool. GenerateContacts (Part A) fills
            // m_contactConstraints with SOLVER-AGNOSTIC raw geometry; the solver
            // reads it and computes its own effective masses / bias. The pool
            // only grows (clear()+emplace each Step preserves capacity) -> zero
            // steady-state allocation in Step.
            std::unique_ptr<ISolver>       m_solver;
            std::vector<ContactConstraint> m_contactConstraints;

            // ---- task executor (Phase D1) -----------------------------------
            ITaskExecutor*         m_executor = nullptr;   // injected; null -> m_serialExecutor
            mutable SerialTaskExecutor m_serialExecutor;   // owned deterministic fallback

            // ---- broadphase per-worker scratch (Phase D2, Task 3) ----------
            //
            // Reused across steps (resize-to-WorkerCount on growth; clear per step).
            // Zero steady-state alloc after the first step that has N workers.
            //   m_bpMovedScratch  : snapshot of moved proxy ids from EvictTouchedAndCollectMoved.
            //   m_bpFindScratch[w]: per-worker canonical key accumulator for QueryProxyPairs.
            //   m_bpStackScratch[w]: per-worker descent stack for QueryProxyPairs.
            // The serial UpdatePairs wrapper (tests/oracle) is unchanged; only
            // UpdateContacts is switched to the parallel orchestration.
            static constexpr std::size_t kBroadphaseGrain = 64;
            // Grain for the create-phase parallel detect (stage-2b ParallelFor).
            // Below kCreateGrain awake bodies the ParallelFor degrades to serial on
            // worker 0 -> byte-identical to the pre-MT path with zero overhead.
            static constexpr std::size_t kCreateGrain      = 16;
            std::vector<std::uint32_t>              m_bpMovedScratch;
            // false-sharing of adjacent inner-vector control blocks is DEFERRED (Task 4 measured this stage DRAM/latency-bound, so it is masked; pad each per-worker entry to 64B if it ever dominates).
            std::vector<std::vector<std::uint64_t>> m_bpFindScratch;   // per-worker key buffers
            std::vector<std::vector<std::uint32_t>> m_bpStackScratch;  // per-worker descent stacks

            // ---- joints (P2.5) ---------------------------------------------
            //
            // The world OWNS the joint objects (ports the Lua self.joints list).
            // m_jointConstraints is the pooled JointConstraint array fed to the
            // SolverContext each Step (Joint* views into m_joints); rebuilt in
            // Step from the live joints -> the SolverContext stays the P2.1 shape.
            // Both only grow (clear() preserves capacity) -> zero steady-state
            // alloc in Step; AddJoint/RemoveJoint are setup-time (may alloc).
            std::vector<std::unique_ptr<Joint>> m_joints;
            std::vector<JointConstraint>        m_jointConstraints;

            // ---- persistent contact update (collision-rebuild Phase 3, Task 2) -
            //
            // UpdateContacts(dt): the ONE-PASS persistent-contact update, called
            // from Step each Step. As of Phase 3 Task 4 it is the SOLE narrowphase
            // for the solver feed (GenerateContacts is retired); EmitContactConstraints
            // then walks its output into m_contactConstraints. Three phases:
            //   1. CREATE: ensure a Contact for every solver-relevant fixture-pair
            //      (mover<->mover from the incremental fixture-pair set; mover<->
            //      static-BODY from StaticCandidates).
            //   2. UPDATE + DESTROY: one deterministic pass over the pool (ascending
            //      id) -- destroy on stale handle / fat-box separation, recompute the
            //      cached manifold otherwise (skipping both-asleep pairs).
            //   3. TILE SPANS (Task 4): rebuild the per-Step transient m_spanContacts
            //      scratch -- per awake dynamic body, StaticCandidates -> Collide each
            //      dynamic fixture vs each merged span. Spans are virtual fixtures, so
            //      they are NOT pooled; this scratch is cleared + refilled each Step.
            //      Mirrors the legacy GenerateContacts span path EXACTLY (the span
            //      shape/transform/margin + the (fixA, fixB=kInvalidSlot) ids) so the
            //      manifold + MixContactId + COM are identical (PhysicsTileGridTest).
            //
            // The fixture<->fixture create/update phases are SIDE-EFFECT-FREE on the
            // simulation (read body/fixture state + the broadphase, write ONLY
            // m_contactPool + m_cpPairs). The span phase additionally writes only
            // m_spanContacts/m_spanCenters (Step-local scratch) and mirrors
            // GenerateContacts' wake-of-asleep-dynamic rule for span candidates.
            void UpdateContacts(Real dt);

            // CREATE-pass helper: ensure a Contact for the fixture-pair (fa, fb),
            // applying the EXACT filters + orientation rule (A's body dynamic;
            // both-dynamic -> lower body slot is A; bodies + fixtures swap together).
            // Fills the body slots ONLY on a fresh create.
            void TryCreateContact(std::uint32_t fa, std::uint32_t fb);

            // CREATE-pass helper (Task 4): wake-on-contact for a mover<->mover
            // fixture-pair. Reproduces the wake rule the retired GenerateContacts
            // ran in its mover-mover loop (PhysicsWorld.lua:369-382): a sleeping
            // dynamic touched by an awake mover wakes (so the island re-forms). Uses
            // the pre-orientation body slots and the SAME alive/sensor/da-db gates
            // TryCreateContact applies, so exactly the pairs that produce a
            // constraint can wake. Mutates m_awake / m_sleepTimer only (no other sim
            // state). MUST run before the manifold update pass so a woken body's
            // contact is recomputed + emitted this Step.
            void WakeMoverPair(std::uint32_t fa, std::uint32_t fb);

            // ---- immediate lifecycle-seam contact destruction (Task 5) -------
            //
            // Destroy every persistent-pool contact that references a fixture /
            // body the moment it is removed, so a recycled slot never leaves a
            // stale contact even for the single Step before UpdateContacts' guard
            // (line ~1917) would reap it. Walk the pool by STORED slot (not by
            // liveness) -- the caller invokes these AFTER the fixture/body has
            // been marked dead. ContactPool::ForEach tolerates Destroy(id) of the
            // CURRENT id mid-walk (it iterates ascending ids checking m_alive[id];
            // Destroy only flips the flag + frees the id, never resizes the pool),
            // so destroying in-place is safe. Additive cleanup: the update-pass
            // guard stays as defense-in-depth, so the sim is byte-identical.
            void DestroyContactsForFixture(std::uint32_t fixtureSlot);
            void DestroyContactsForBody(std::uint32_t bodySlot);

            // True iff fixture handle h still refers to its original live slot.
            [[nodiscard]] bool FixtureSlotLive(FixtureHandle h) const noexcept;

            // True iff the two fixtures' FAT broadphase boxes (optionally grown by
            // an extra speculative margin) still overlap (a mover fixture's fat box
            // comes from the DynamicTree leaf; a static fixture's from its tight AABB
            // grown by the tree margin). The contact owns its destruction when this
            // is false. `extraMargin` widens the overlap test so a fast mover's
            // velocity-scaled speculative contact (whose closing distance this Step
            // exceeds the fixed tree margin) is NOT reaped before it can produce a
            // speculative manifold -- mirroring the retired GenerateContacts, which
            // had no fat-box gate at all and collided every velocity-padded candidate.
            [[nodiscard]] bool FatBoxesOverlap(const Contact& c,
                                               Real extraMargin = Real(0)) const noexcept;

            // True iff both bodies are asleep (a static/kinematic body counts as
            // asleep -- it never wakes a recompute on its own). Mirrors the awake
            // gate GenerateContacts uses to skip work.
            [[nodiscard]] bool BothAsleep(const Contact& c) const noexcept;

            // Recompute one contact's manifold + classify its state change for the
            // MT serial tail. Reads stable body transforms + the contact; writes ONLY
            // c.manifold / c.touching / c.npState (no pool/color/island mutation).
            // moveDt = the step dt; threshSq = the speculative-margin speed^2 gate.
            void UpdateOneContact(std::uint32_t id, Contact& c,
                                  Real moveDt, Real threshSq) noexcept;

            // ---- the persistent solver feed (Phase 3, Task 4) ----------------
            //
            // Walk the persistent contact pool (the const ForEach, ascending id)
            // AND the transient tile-span scratch (m_spanContacts, filled by
            // UpdateContacts) and build the ContactConstraint set fed to the solver.
            // Read-only: it writes ONLY `out` and touches NO sim state (not the
            // pool, not m_spanContacts). Each emitted constraint mirrors the legacy
            // GenerateContacts `emit` lambda field-for-field (body slots, inv
            // mass/inertia, normal/kind, friction = sqrt(fA*fB), restitution =
            // max(rA,rB), per-point COM-relative anchors, baseSeparation =
            // -separation, id = MixContactId). Skips a contact that is not touching
            // and applies the SAME awake-gate (skip when the dynamic A is asleep).
            //
            // CANONICAL ORDER (design Sec 7): after collecting all constraints the
            // walk is sorted by (bodyA, bodyB, fixtureA_slot, fixtureB_slot) -- the
            // deterministic order that makes the live solver feed run-twice-identical
            // regardless of pool/broadphase emission order. (The Task-3 oracle still
            // compares as a SET, so this ordering does not affect that gate.)
            void EmitContactConstraints(std::vector<ContactConstraint>& out) const;

            // ---- P3.1 CCD: bullet GJK-TOI clamp (Step stage 6) ---------------
            //
            // PORT of the Lua bullet clamp (PhysicsWorld.lua:313-320): for each
            // alive `isBullet` body, sweep its start-of-step position (prevX/prevY,
            // snapshotted in Step stage 1) to its post-integration position
            // (posX/posY) against STATICS ONLY (tile spans + non-sensor static
            // bodies -- NOT movers; bullets clamp against statics per the Lua + the
            // plan) via the conservative-advancement ShapeCast (P1.4/P1.9). If the
            // sweep hits at fraction t < 1 the body would tunnel a thin static, so
            // clamp the position to prev + delta * max(0, t - kBulletEpsilon) and
            // refresh the mover broadphase AABB. This is the discrete BACKUP that
            // catches what the speculative margin (GenerateContacts) misses --
            // primarily KINEMATIC bullets, which the solver never touches, but
            // also fast DYNAMIC bullets as a safety net. Deterministic (the
            // conservative-advancement cast is fixed-iteration, no wall-clock).
            // Runs AFTER the solver commits dynamic positions and BEFORE events
            // (so contact events + island sleep see the clamped position).
            void BulletSweep();

            // ---- query scratch (zero steady-state alloc) -------------------
            //
            // Pooled candidate buffers for ShapeCast / OverlapShape, mirroring
            // the Lua's module-local _spans / _statics. mutable so the const
            // query methods may reuse them (clear()+push_back preserves capacity;
            // no per-call heap traffic after warmup).
            //
            // NOT re-entrant. Queries are single-threaded AND must NOT be called
            // from within a contact callback (OnContact fires inside Step; a
            // nested query would overwrite these scratch buffers mid-traversal ->
            // silent wrong results).
            // Safe call sites: the game-update loop, CharacterController, CCD
            // pre-step.
            // If a future system needs a query inside a callback, switch to
            // thread_local or caller-supplied scratch.
            mutable std::vector<Aabb2>         m_scratchSpans;
            mutable std::vector<std::uint32_t> m_scratchStatics;

            // ---- persistent contact pool (collision-rebuild Phase 3, Task 2/4) --
            //
            // The durable fixture-pair contact pool, POPULATED each Step by
            // UpdateContacts and CONSUMED by EmitContactConstraints (Task 4 -- the
            // solver feed; GenerateContacts is retired). Survives across steps; the
            // create pass dedups, the update pass recomputes manifolds + destroys
            // dead pairs. m_cpPairs is the move-buffer scratch for the mover<->mover
            // create pass.
            ContactPool                 m_contactPool;
            std::vector<BroadphasePair> m_cpPairs;

            // ---- persistent incremental contact coloring (Phase C, Task 4) -----
            //
            // m_bodyColorMask[slot] is a 12-bit (one bit per color, kColorCount<32)
            // occupancy mask over the colors that body slot currently has a contact
            // in. Keyed by WORLD body slot; a removed/recycled slot resets to 0
            // (EnsureCapacity defaults new slots to 0; the RemoveBody leak-detector
            // asserts a removed body left mask 0). m_colorContacts[k] is the list of
            // contact ids assigned to color k (sized kColorCount in the ctor). These
            // are maintained at create/destroy and CONSUMED by the solver (Task 5):
            // m_bodyColorMask gates color assignment, m_colorContacts is the
            // solver's per-color bucket list. The persistent coloring is a DIFFERENT
            // but equally-valid color partition than the old per-step greedy one, so
            // the colored Gauss-Seidel solve is an INTENTIONAL re-baseline vs
            // pre-Phase-C main (different floats), NOT bit-identical. The contract is
            // run-twice DETERMINISM + the behavioral [physics] suite (no exact
            // goldens) -- per the engine's re-baseline-numerics-on-purpose rule.
            std::vector<std::uint32_t>              m_bodyColorMask;
            std::vector<std::vector<std::uint32_t>> m_colorContacts;

            // ---- per-step touched EVENT body-pairs (collision-rebuild Phase 4) --
            //
            // The deduped, sorted set of body-pairs that are EVENT-RELEVANT AND
            // TOUCHING this Step, derived from the persistent pool by UpdateContacts'
            // caller (Step stage 6) and consumed by ContactManager::Step to derive
            // Begin/Stay events. This REPLACES ContactManager's old second
            // broadphase walk + kinematic-static SlotsOverlap pass: the pool already
            // computed `touching` once this Step, so the events pass is a byproduct.
            // Built by walking the pool ascending-id (deterministic), pushing
            // {min(bodyA,bodyB), max(...)} for each eventRelevant+touching contact,
            // then sort + unique (a compound body's N^2 fixture-pairs dedup to ONE
            // body-pair, matching the old sorted-body-pair emission order). clear()
            // keeps capacity -> zero steady-state alloc.
            std::vector<BroadphasePair> m_touchedEventPairs;

            // Transient TILE-SPAN contacts (collision-rebuild Phase 3, Task 4).
            //
            // Tile spans are VIRTUAL/transient fixtures (a merged run of solid
            // cells, no fixture slot), so they are NOT pooled like fixture<->fixture
            // contacts. Instead UpdateContacts refills this per-Step scratch (cleared
            // at the top, rebuilt) by mirroring the legacy GenerateContacts span path
            // EXACTLY: per awake dynamic body, StaticCandidates -> for each span,
            // build the span shape + transform and Collide(dynFixture, span). Each
            // resulting Contact carries bIsBody=false, bodyB=kInvalidSlot, the
            // span's geometric center in spanCenter, and fixA = the dynamic fixture
            // slot / fixB = kInvalidSlot so MixContactId + the COM-relative anchors
            // match what GenerateContacts produced. EmitContactConstraints walks
            // BOTH m_contactPool (fixture<->fixture) AND this scratch (spans) into
            // the solver feed, then applies the canonical sort.
            //
            // Contact::a holds {fi, gen} for the dynamic fixture; Contact::b is the
            // default (invalid) handle. Contact::bodyA is the dynamic body slot;
            // bodyB is kInvalidSlot. The span's geometric center is stashed in a
            // PARALLEL vector (m_spanCenters) at the same index (Contact has no
            // span-center field). Step-only; zero steady-state alloc after warmup.
            std::vector<Contact> m_spanContacts;
            std::vector<Vec2>    m_spanCenters; // span geometric center, parallel to m_spanContacts

            // ---- Create-phase MT: deferred new-pair records (narrowphase-MT G2) --
            //
            // The detect pass (stage-2b indexed loop) emits one record per fixture
            // pair candidate instead of calling TryCreateContact inline.  The serial
            // create tail replays TryCreateContact in (awakeIndex ascending,
            // push-order) order -- reproducing the serial ForEachAwake create order
            // exactly, so the order-dependent AssignContactColor + the EnsurePair id
            // allocation are byte-identical.  awakeIndex = the index into
            // m_awakeBodies (NOT the body slot), which is the ForEachAwake visit
            // order.  Task 4 will make the detect loop parallel; this Task keeps it
            // serial so byte-identity can be verified first.
            struct NewPairRecord { std::uint32_t awakeIndex; std::uint32_t fiA; std::uint32_t fiB; };
            std::vector<NewPairRecord> m_newPairs;

            // Create-phase MT per-worker scratch (sized to WorkerCount() each step,
            // grow-only). Each worker uses ONLY its own [w] entry -> contention-free.
            // SpanEntry tags each transient span contact with the awakeIndex of the
            // body that produced it, so the serial tail can stable_sort back to
            // ForEachAwake order (k ascending, within-k push order preserved because
            // each k is handled by exactly one worker and ranges are disjoint).
            struct SpanEntry { std::uint32_t awakeIndex; Contact c; Vec2 center; };
            std::vector<std::vector<Aabb2>>         m_genSpansW;    // per-worker StaticCandidates spans out
            std::vector<std::vector<std::uint32_t>> m_genStaticsW;  // per-worker statics out
            std::vector<std::vector<std::uint32_t>> m_gridScratchW; // per-worker QueryAABB scratch
            std::vector<std::vector<SpanEntry>>     m_spanEntriesW; // per-worker span contacts (awakeIndex-tagged)
            std::vector<std::vector<NewPairRecord>> m_newPairsW;    // per-worker new-pair records
            // Serial span-merge buffer: the per-worker m_spanEntriesW are concatenated
            // here + stable_sorted by awakeIndex before being appended to m_spanContacts
            // (the span-side sibling of m_newPairs). Member-promoted so it reuses its
            // capacity each Step -- zero steady-state alloc, matching the pair-merge path.
            std::vector<SpanEntry>                  m_allSpans;

            // ---- EmitContactConstraints sort scratch (Phase 3, Task 4) -------
            //
            // The canonical-sort scratch consumed by EmitContactConstraints every
            // Step (parallel sort key per emitted constraint + the index
            // permutation + the permuted output staging). Promoted from per-Step
            // locals to persistent members so the hot solver-feed path holds the
            // module's zero-steady-state-alloc-after-warmup discipline: each is
            // .clear()-and-reused at the top of EmitContactConstraints (clear
            // preserves capacity -> no realloc once the scene's contact count
            // stabilizes). `mutable` because EmitContactConstraints stays `const`
            // (the oracle DebugEmitPoolConstraints hook is const) -- mutable cache
            // scratch behind a const read-only API is idiomatic.
            struct EmitSortKey
            {
                std::uint32_t bodyA, bodyB, fixA, fixB;
            };
            mutable std::vector<EmitSortKey>       m_emitKeys;
            mutable std::vector<std::size_t>       m_emitOrder;
            mutable std::vector<ContactConstraint> m_emitSorted;

        };

    } // namespace Physics
} // namespace Arcane
