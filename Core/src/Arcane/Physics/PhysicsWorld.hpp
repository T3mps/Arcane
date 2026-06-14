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
//       stage 2: solver contact generation (GenerateContacts / Part A)
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

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp>
#include <Arcane/Physics/Broadphase/Passability.hpp>
#include <Arcane/Physics/Broadphase/TileGrid.hpp>
#include <Arcane/Physics/ContactManager.hpp>
#include <Arcane/Physics/Solver/Solver.hpp>      // ISolver + ContactConstraint pool type
#include <Arcane/Physics/Joints/Joint.hpp>       // Joint base + JointDef (P2.5)

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
            bool fixedRotation  = false;       // invInertia forced to 0
            bool bullet         = false;       // CCD clamp (P3); stored now
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
            [[nodiscard]] std::optional<ShapeCastHit>
            ShapeCast(const Shape& shape, const Vec2& pos, const Vec2& delta,
                      const ShapeCastOpts& opts = {}) const;

            // All live bodies whose shape OVERLAPS `shape` at transform `xf`
            // (NEW -- composed, no direct Lua method). Uses a broadphase
            // candidate pass narrowed by CollideShapes (pointCount > 0). Includes
            // statics + movers; self-overlap of the query shape against a body at
            // the same spot counts. `out` is cleared then filled with handles,
            // index-ordered -> deterministic. Returns out.size().
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
            [[nodiscard]] const IBroadphase& MoverBroadphase() const noexcept
            {
                return *m_moverBroadphase;
            }

            // ---- internals consumed by the Soft Step solver (P2.2 seam) -----
            //
            // The solver reads/writes the dynamics SoA through these slot
            // accessors (mirroring the ContactManager seam above) so the raw
            // vectors stay private. Velocity + angVel are mutated every solve
            // iteration; position + angle are committed once per Step at the
            // solver's FinalizePositions. All are inline -> the per-iteration
            // call cost vanishes in an optimized build.
            [[nodiscard]] bool AwakeSlot(std::uint32_t i) const noexcept { return m_awake[i] != 0; }
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
            void SetAwakeSlot(std::uint32_t i, bool on) noexcept
            {
                m_awake[i] = on ? std::uint8_t(1) : std::uint8_t(0);
            }
            // Pooled union-find scratch for the island graph (ports the Lua
            // w._uf). Grown to m_count once per Step; reused -> zero steady-state
            // alloc. Exposed so Island::UpdateSleep builds the constraint graph
            // in the world's own buffer instead of allocating its own.
            // INTERNAL: only for Island::UpdateSleep -- do not call from general
            // code (it is stomped each Step).
            [[nodiscard]] std::vector<std::uint32_t>& UnionFindScratch() noexcept
            {
                return m_uf;
            }
            // Commit final position/angle for slot i + refresh the mover
            // broadphase AABB (solver FinalizePositions). Dynamic-only call site.
            void CommitSlotPosition(std::uint32_t i, Vec2 p, Real angle) noexcept
            {
                m_posX[i]  = p.x;
                m_posY[i]  = p.y;
                m_angle[i] = angle;
                m_moverBroadphase->Update(i, SlotAabb(i));
            }
            // Global gravity (solver reads it for the per-sub-step integrate).
            [[nodiscard]] Vec2 Gravity() const noexcept
            {
                return Vec2(m_gravityX, m_gravityY);
            }
            // Solver warm-start cache size (inspection/test hook -- the harness
            // asserts the cache stays bounded as transient contacts come + go).
            // Routes through the installed ISolver (both solvers report it).
            [[nodiscard]] std::size_t SolverWarmStartCacheSize() const noexcept
            {
                return m_solver ? m_solver->WarmStartCacheSize() : std::size_t(0);
            }

            // Soft Step config (solver reads it in Prepare / the sub-step loop).
            [[nodiscard]] std::uint32_t SubstepCount() const noexcept { return m_substepCount; }
            [[nodiscard]] Real ContactHertz() const noexcept { return m_contactHertz; }
            [[nodiscard]] Real ContactDampingRatio() const noexcept { return m_contactDampingRatio; }
            [[nodiscard]] Real RestitutionThreshold() const noexcept { return m_restitutionThreshold; }
            [[nodiscard]] Real ContactPushMaxVelocity() const noexcept { return m_contactPushMaxVelocity; }

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
            void StaticCandidates(const Aabb2& box, std::vector<Aabb2>& spansOut,
                                  std::vector<std::uint32_t>& staticsOut) const;

        private:
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
            std::vector<std::uint8_t>  m_awake;               // 1 = awake (integrates this step; P2.4 sleep clears to 0)
            std::vector<std::uint8_t>  m_bullet;              // CCD clamp (P3)

            std::uint32_t              m_count = 0; // high-water slot count
            std::vector<std::uint32_t> m_free;      // recycled slot stack

            // ---- broadphase + statics --------------------------------------
            std::unique_ptr<IBroadphase> m_moverBroadphase;
            std::vector<std::uint32_t>   m_staticList; // slot indices of statics
            std::unique_ptr<TileGrid>    m_tileGrid;   // optional tile statics

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

            // Build the dynamics ContactConstraint array for this Step (Part A):
            // for each awake dynamic body, manifolds vs tile spans + static
            // bodies (StaticCandidates) and vs mover-mover pairs involving a
            // dynamic (broadphase Pairs, narrowed). Orients A = dynamic.
            //
            // P3.1 speculative-contact CCD: the per-body speculative margin fed
            // to CollideShapes is VELOCITY-SCALED -- max(kSkin, |v| * dt) -- so a
            // fast mover's wall is seen pre-overlap and the SoftStep solver's
            // speculative bias (s > 0 -> bias = s * invSubDt) caps the per-sub-step
            // advance to the gap, stopping tunneling without a discrete clamp.
            // A slow/resting body's margin is just kSkin (resting UNCHANGED).
            void GenerateContacts(Real dt);

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

            // ---- contact-gen scratch (Step-only; zero steady-state alloc) ---
            //
            // Reused by GenerateContacts each Step: the dynamic body's near-AABB
            // span/static candidate lists + the broadphase mover-pair buffer.
            // Distinct from the query scratch above (GenerateContacts runs inside
            // Step's solver stage, not from a query/callback site).
            std::vector<Aabb2>          m_genSpans;
            std::vector<std::uint32_t>  m_genStatics;
            std::vector<BroadphasePair> m_genPairs;

            // ---- island sleep scratch (P2.4; Step-only; zero steady-state) ---
            //
            // Union-find parent array for the per-Step constraint graph (bodies =
            // nodes, contacts = edges). Sized to m_count once per Step and reused
            // (ports the Lua w._uf). Island::UpdateSleep owns its contents; it
            // lives here so the island pass allocates nothing after warmup.
            std::vector<std::uint32_t>  m_uf;
        };

    } // namespace Physics
} // namespace Arcane
