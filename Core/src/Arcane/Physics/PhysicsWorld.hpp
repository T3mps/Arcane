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
// broadphase so the ContactManager can still emit events for them, matching
// the Lua's mover treatment. The force/impulse/sleep machinery arrives in
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
            // queryAABB; the richer query suite is P1.9). Index-ordered ->
            // deterministic.
            int QueryAABB(const Aabb2& box, std::vector<BodyHandle>& out) const;

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
            // World-space tight AABB of slot i.
            [[nodiscard]] Aabb2 SlotAabb(std::uint32_t i) const noexcept;

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

            // ---- pooled scratch (zero steady-state alloc) ------------------
            mutable std::vector<Aabb2>         m_spanScratch;
            mutable std::vector<std::uint32_t> m_staticScratch;
        };

    } // namespace Physics
} // namespace Arcane
