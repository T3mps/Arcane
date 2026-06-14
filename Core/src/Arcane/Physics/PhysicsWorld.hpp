#pragma once

// PhysicsWorld: the 2D-physics orchestration core (M6, Task P1.8).
//
// PORT NOTE: a faithful port of the KINEMATIC subset of
// Client/src/physics/PhysicsWorld.lua. The Lua module is the whole engine
// (statics + kinematics + dynamics + solver + joints + islands + CCD +
// raycast). P1.8 ports ONLY:
//   * Body SoA storage + handle/free-list/generation + AddBody/RemoveBody/
//     IsValid (ports PhysicsWorld.lua addBody/removeBody/handleValid).
//   * Step stages 1 + 7 for KINEMATIC bodies only: prevX/Y snapshot +
//     kinematic velocity integration + mover-broadphase AABB update, then
//     ContactManager::Step for events. (Lua step() stage 1 kinematic branch +
//     the trailing contacts:step.)
//   * QueryAABB(rect) -> body handles (linear scan; ports queryAABB).
//   * Static bodies in a staticList (not the mover broadphase) + an optional
//     TileGrid for tile statics + _StaticCandidates (ports staticList /
//     _staticCandidates).
//
// DELIBERATELY NOT PORTED (deferred -- see PORT BOUNDARY below):
//   * Dynamic velocity integration (gravity / linear damping).
//   * The solver call, dynamic position integration (stage 4), island sleep
//     (stage 5), bullet CCD clamp, joints.
//   * raycast / shapeCast / lineOfSight (P1.9).
// Dynamic bodies are ACCEPTED + stored (BodyType::Dynamic) but are NEVER
// integrated or solved in P1.8. They are also registered in the mover
// broadphase so the ContactManager emits mover-mover events for them.
// NOTE: dynamic-vs-static-BODY events are KINEMATIC-ONLY (faithful to
// ContactManager.lua:150 -- the solver owns dynamic-vs-static response in
// P2.1). The force/impulse/sleep machinery arrives in
// P2.1 ("extends PhysicsWorld for Dynamic bodies"); the SoA carries the few
// dynamics fields it needs cheaply (angle/awake) but the solver-only fields
// (mass/inertia/damping/etc.) are intentionally left for P2.1 to add.
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
        // BodyDef: parameters for AddBody (ports the Lua addBody opts).
        // ----------------------------------------------------------------
        struct BodyDef
        {
            BodyType type = BodyType::Kinematic;
            Vec2     position{ Real(0), Real(0) };
            Shape    shape{}; // required; stored by value per slot
            bool     isSensor      = false;
            bool     eventsEnabled = true;
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

            [[nodiscard]] Vec2 Velocity(BodyHandle h) const noexcept;

            // Set velocity (Kinematic + Dynamic accept it; Static ignores).
            void SetVelocity(BodyHandle h, Vec2 v);

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

            std::uint32_t              m_count = 0; // high-water slot count
            std::vector<std::uint32_t> m_free;      // recycled slot stack

            // ---- broadphase + statics --------------------------------------
            std::unique_ptr<IBroadphase> m_moverBroadphase;
            std::vector<std::uint32_t>   m_staticList; // slot indices of statics
            std::unique_ptr<TileGrid>    m_tileGrid;   // optional tile statics

            bool m_eventsEnabled = true;

            // ---- contacts --------------------------------------------------
            ContactManager m_contacts;

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
        };

    } // namespace Physics
} // namespace Arcane
