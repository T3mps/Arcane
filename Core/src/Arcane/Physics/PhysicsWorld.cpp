// PhysicsWorld.cpp -- the 2D-physics orchestration core, KINEMATIC subset
// (port of the kinematic path of PhysicsWorld.lua).
//
// See PhysicsWorld.hpp for the contract + the PORT BOUNDARY (what P1.8 ports
// vs what P2/P3 add). This TU implements Create/AddBody/RemoveBody/IsValid,
// the kinematic Step (prev snapshot + kinematic integration + broadphase
// update, then ContactManager::Step), QueryAABB, the two-granularity event
// gating glue (SetBodyEvents / SetEventsEnabled), and _staticCandidates.
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.

#include <Arcane/Physics/PhysicsWorld.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/Broadphase/DynamicTree.hpp>
#include <Arcane/Physics/Broadphase/SpatialHash.hpp>
#include <Arcane/Physics/Broadphase/SweepAndPrune.hpp>
#include <Arcane/Physics/Narrowphase/Gjk.hpp>
#include <Arcane/Physics/Narrowphase/Dispatch.hpp>

namespace Arcane
{
    namespace Physics
    {
        namespace
        {
            std::unique_ptr<IBroadphase> MakeBroadphase(const WorldDef& def)
            {
                // PORT + MODERNIZE: the Lua selected by string, defaulting to
                // SpatialHash; we default to DynamicTree (the P1.6 decision).
                switch (def.broadphase)
                {
                case BroadphaseKind::Hash:
                    return std::make_unique<SpatialHash>(def.hashCellSize);
                case BroadphaseKind::Sap:
                    return std::make_unique<SweepAndPrune>();
                case BroadphaseKind::Tree:
                default:
                    return std::make_unique<DynamicTree>();
                }
            }
        } // namespace

        PhysicsWorld::PhysicsWorld(const WorldDef& def)
            : m_moverBroadphase(MakeBroadphase(def))
        {
            // Optional tile statics: own a TileGrid over the passability seam if
            // one was provided (ports the Lua `tileGrid = map and TileGrid.new`).
            if (def.passability != nullptr)
            {
                m_tileGrid = std::make_unique<TileGrid>(*def.passability,
                                                        def.tileCellSize,
                                                        def.tileOrigin);
            }
        }

        PhysicsWorld::~PhysicsWorld() = default;

        void PhysicsWorld::EnsureCapacity(std::uint32_t n)
        {
            if (n <= m_posX.size())
            {
                return;
            }
            // Amortized growth: at least double the current capacity so that
            // a one-at-a-time fill (AddBody(count+1)) does not realloc all ~12
            // SoA vectors on every add.  Free-list / steady-state behaviour is
            // unchanged; only the initial fill is faster.
            const std::uint32_t next =
                std::max(n, static_cast<std::uint32_t>(m_posX.capacity() * 2));
            m_posX.resize(next);
            m_posY.resize(next);
            m_prevX.resize(next);
            m_prevY.resize(next);
            m_velX.resize(next);
            m_velY.resize(next);
            m_btype.resize(next);
            m_evtOn.resize(next);
            m_alive.resize(next);
            m_sensor.resize(next);
            m_gen.resize(next, 0u); // gen kept zero-filled (live slots start at 1)
            m_shape.resize(next);
        }

        Aabb2 PhysicsWorld::SlotAabb(std::uint32_t i) const noexcept
        {
            Transform xf{ Vec2(m_posX[i], m_posY[i]), Real(0) };
            return m_shape[i].ComputeAABB(xf);
        }

        BodyHandle PhysicsWorld::AddBody(const BodyDef& def)
        {
            // Reuse a free slot or append (ports addBody's free-list pop).
            std::uint32_t idx;
            if (!m_free.empty())
            {
                idx = m_free.back();
                m_free.pop_back();
            }
            else
            {
                idx = m_count;
                EnsureCapacity(m_count + 1);
                ++m_count;
            }

            m_posX[idx]  = def.position.x;
            m_posY[idx]  = def.position.y;
            m_prevX[idx] = def.position.x;
            m_prevY[idx] = def.position.y;
            m_velX[idx]  = Real(0);
            m_velY[idx]  = Real(0);
            m_btype[idx] = static_cast<std::uint8_t>(def.type);
            m_evtOn[idx] = def.eventsEnabled ? std::uint8_t(1) : std::uint8_t(0);
            m_sensor[idx] = def.isSensor ? std::uint8_t(1) : std::uint8_t(0);
            m_alive[idx]  = 1;
            // Bump generation (live slots start at 1; bump on BOTH add + remove
            // so stale handles never match). Ports gen[idx] = (gen[idx] or 0)+1.
            m_gen[idx] += 1u;
            m_shape[idx] = def.shape;

            if (def.type == BodyType::Static)
            {
                m_staticList.push_back(idx);
            }
            else
            {
                // Kinematic + Dynamic register in the mover broadphase.
                // Kinematic movers get begin/stay/end events via the broadphase
                // Pairs() stream AND via the kinematic-vs-static-body loop in
                // ContactManager::Step.  Dynamic movers get mover-mover events
                // via broadphase Pairs() only -- dynamic-vs-static-BODY events
                // are intentionally KINEMATIC-ONLY (faithful to
                // ContactManager.lua:150); the solver owns dynamic-vs-static
                // response, which arrives in P2.1.
                m_moverBroadphase->Update(idx, SlotAabb(idx));
            }
            return BodyHandle{ idx, m_gen[idx] };
        }

        void PhysicsWorld::RemoveBody(BodyHandle h)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t idx = h.index;
            m_alive[idx] = 0;
            m_gen[idx] += 1u; // invalidates the handle (stale-handle invariant)

            if (static_cast<BodyType>(m_btype[idx]) == BodyType::Static)
            {
                // staticList is an unsorted no-duplicate index bag -- swap-and-pop
                // is O(1) and order-independent (pair keys are canonically keyed
                // (min,max) so static-loop order does not affect determinism).
                for (std::size_t i = 0; i < m_staticList.size(); ++i)
                {
                    if (m_staticList[i] == idx)
                    {
                        m_staticList[i] = m_staticList.back();
                        m_staticList.pop_back();
                        break;
                    }
                }
            }
            else
            {
                m_moverBroadphase->Remove(idx);
            }

            m_contacts.DropBody(idx);
            m_shape[idx] = Shape{}; // release polygon storage
            m_free.push_back(idx);
        }

        bool PhysicsWorld::IsValid(BodyHandle h) const noexcept
        {
            // Ports handleValid: in-range index, matching generation, alive.
            // gen==0 is NEVER a live slot (AddBody bumps 0->1 on first use), so
            // the sentinel kInvalidBody{0,0} can never match a live body.
            return h.index < m_count && m_gen[h.index] == h.generation &&
                   m_alive[h.index] != 0;
        }

        Vec2 PhysicsWorld::Position(BodyHandle h) const noexcept
        {
            if (!IsValid(h))
            {
                return Vec2(Real(0), Real(0));
            }
            return Vec2(m_posX[h.index], m_posY[h.index]);
        }

        void PhysicsWorld::SetPosition(BodyHandle h, Vec2 p)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            // Teleport: prev snaps too (no lerp smear) -- ports setPosition.
            m_posX[i]  = p.x;
            m_posY[i]  = p.y;
            m_prevX[i] = p.x;
            m_prevY[i] = p.y;
            if (static_cast<BodyType>(m_btype[i]) != BodyType::Static)
            {
                m_moverBroadphase->Update(i, SlotAabb(i));
            }
        }

        Vec2 PhysicsWorld::Velocity(BodyHandle h) const noexcept
        {
            if (!IsValid(h))
            {
                return Vec2(Real(0), Real(0));
            }
            return Vec2(m_velX[h.index], m_velY[h.index]);
        }

        void PhysicsWorld::SetVelocity(BodyHandle h, Vec2 v)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            const BodyType bt = static_cast<BodyType>(m_btype[i]);
            // Kinematic + Dynamic accept velocity; Static ignores (ports
            // setVelocity). The Dynamic wake side-effect is P2.
            if (bt == BodyType::Kinematic || bt == BodyType::Dynamic)
            {
                m_velX[i] = v.x;
                m_velY[i] = v.y;
            }
        }

        Vec2 PhysicsWorld::DrawPosition(BodyHandle h, Real alpha) const noexcept
        {
            if (!IsValid(h))
            {
                return Vec2(Real(0), Real(0));
            }
            const std::uint32_t i = h.index;
            // Render-boundary lerp prev..curr (ports drawPosition).
            return Vec2(m_prevX[i] + (m_posX[i] - m_prevX[i]) * alpha,
                        m_prevY[i] + (m_posY[i] - m_prevY[i]) * alpha);
        }

        const Shape* PhysicsWorld::GetShape(BodyHandle h) const noexcept
        {
            if (!IsValid(h))
            {
                return nullptr;
            }
            return &m_shape[h.index];
        }

        BodyType PhysicsWorld::GetType(BodyHandle h) const noexcept
        {
            if (!IsValid(h))
            {
                return BodyType::Static;
            }
            return static_cast<BodyType>(m_btype[h.index]);
        }

        bool PhysicsWorld::IsSensor(BodyHandle h) const noexcept
        {
            return IsValid(h) && m_sensor[h.index] != 0;
        }

        void PhysicsWorld::OnContact(ContactManager::Listener fn)
        {
            m_contacts.SetListener(std::move(fn));
        }

        void PhysicsWorld::SetBodyEvents(BodyHandle h, bool on)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            const bool was = m_evtOn[i] != 0;
            m_evtOn[i] = on ? std::uint8_t(1) : std::uint8_t(0);
            // true->false: Disarm (drop, no synthetic end). false->true: Rearm
            // (fresh begin for overlapping). Ports _setBodyEvents.
            if (was && !on)
            {
                m_contacts.Disarm(i);
            }
            else if (on && !was)
            {
                m_contacts.Rearm(*this, i);
            }
        }

        void PhysicsWorld::SetEventsEnabled(bool on)
        {
            if (m_eventsEnabled == on)
            {
                return;
            }
            m_eventsEnabled = on;
            // on->off: Disarm all. off->on: Rearm all overlapping. Ports
            // setEventsEnabled.
            if (on)
            {
                m_contacts.Rearm(*this);
            }
            else
            {
                m_contacts.Disarm();
            }
        }

        void PhysicsWorld::Step(Real dt)
        {
            // Stage 1 (kinematic subset) + stage 7: prev snapshot + KINEMATIC
            // velocity integration + mover-broadphase update, by INDEX order
            // (deterministic; no map iteration, no wall-clock).
            //
            // Static: no integrate. Dynamic: NOT integrated in P1.8 (the
            // gravity/damping/solver/position-integration/sleep/CCD stages are
            // P2/P3 -- see PORT BOUNDARY).
            for (std::uint32_t i = 0; i < m_count; ++i)
            {
                if (m_alive[i] == 0)
                {
                    continue;
                }
                // [P2.1] dynamic velocity integrate (gravity/damping) goes here,
                // before broadphase update
                m_prevX[i] = m_posX[i];
                m_prevY[i] = m_posY[i];
                if (static_cast<BodyType>(m_btype[i]) == BodyType::Kinematic)
                {
                    m_posX[i] += m_velX[i] * dt;
                    m_posY[i] += m_velY[i] * dt;
                    m_moverBroadphase->Update(i, SlotAabb(i));
                }
            }

            // Events + gating + deferred flush (ports the trailing
            // contacts:step). prev/curr are published by the snapshot +
            // integration above; DrawPosition lerps between them.
            m_contacts.Step(*this);
            // [P2.1] dynamic stages go here: solver (contacts+joints) ->
            // dynamic position integrate -> island/sleep
        }

        int PhysicsWorld::QueryAABB(const Aabb2& box,
                                    std::vector<BodyHandle>& out) const
        {
            // Linear scan over alive slots, index-ordered (deterministic).
            // Ports queryAABB (the richer query suite is P1.9).
            out.clear();
            for (std::uint32_t i = 0; i < m_count; ++i)
            {
                if (m_alive[i] == 0)
                {
                    continue;
                }
                if (AabbOverlap(box, SlotAabb(i)))
                {
                    out.push_back(BodyHandle{ i, m_gen[i] });
                }
            }
            return static_cast<int>(out.size());
        }

        // --------------------------------------------------------------------
        // Raycast -- PORT of PhysicsWorld.lua raycast (502-593). The cell DDA is
        // REFORMULATED from iso (Iso.toCellF) to plain CARTESIAN using the
        // world's TileGrid lattice (cellSize/origin). The body slab + GJK
        // point-cast tests are coordinate-agnostic and port faithfully.
        // --------------------------------------------------------------------
        std::optional<RaycastHit> PhysicsWorld::Raycast(const Vec2& from,
                                                        const Vec2& to,
                                                        const RaycastOpts& opts) const
        {
            const Real x0 = from.x, y0 = from.y;
            const Real x1 = to.x,   y1 = to.y;
            const Real dx = x1 - x0, dy = y1 - y0;

            // Degenerate (zero-length) ray -> no hit (ports `if not x1` + the
            // len<eps guards; here the ray has no direction so nothing to test).
            if (dx == Real(0) && dy == Real(0))
            {
                return std::nullopt;
            }

            bool       haveBest = false;
            RaycastHit best{};

            // ---- cell DDA (Cartesian reformulation of the iso lattice) ------
            //
            // The Lua used Iso.toCellF (centered-on-integer iso cells) to map
            // world->fractional-cell. The C++ engine REFORMULATES to TileGrid's
            // plain Cartesian, MIN-CORNER cell mapping so the DDA stays
            // internally consistent with IsSolid / TileGrid::CellBox (cell C
            // spans world [origin + C*cellSize, origin + (C+1)*cellSize), cell
            // boundaries at INTEGER fractional-cell values, not integer +/- 0.5).
            // fractional cell = (world - origin) / cellSize; integer cell =
            // floor(f); boundaries at integers. This is the standard
            // Amanatides-Woo DDA. The Lua's centered +/-0.5 boundary offsets drop
            // out (they were an iso artifact); the traversal structure
            // (tMax/tDelta, step, bounded budget, blocked-on-visit) ports
            // faithfully. The reported cellX/cellY match the cell IsSolid /
            // CellBox index exactly.
            if (m_tileGrid)
            {
                const IPassabilitySource& src   = m_tileGrid->Source();
                const Real                cell   = m_tileGrid->CellSize();
                const Vec2                origin = m_tileGrid->Origin();
                const Real                invCell =
                    cell != Real(0) ? Real(1) / cell : Real(0);

                // World -> fractional cell (min-corner Cartesian lattice).
                const Real fx0 = (x0 - origin.x) * invCell;
                const Real fy0 = (y0 - origin.y) * invCell;
                const Real fx1 = (x1 - origin.x) * invCell;
                const Real fy1 = (y1 - origin.y) * invCell;
                const Real cdx = fx1 - fx0;
                const Real cdy = fy1 - fy0;

                int cx = static_cast<int>(std::floor(fx0));
                int cy = static_cast<int>(std::floor(fy0));
                const int endCx = static_cast<int>(std::floor(fx1));
                const int endCy = static_cast<int>(std::floor(fy1));

                const int stepX = cdx > Real(0) ? 1 : -1;
                const int stepY = cdy > Real(0) ? 1 : -1;
                const Real inf = std::numeric_limits<Real>::infinity();
                // First cell boundary crossing: for +step the next boundary is at
                // (cx+1); for -step it is at cx. Distance in t-space, then
                // tDelta = |1/cd| per full cell. (Standard Amanatides-Woo.)
                Real tMaxX = inf;
                if (cdx != Real(0))
                {
                    const Real nextX = (stepX > 0)
                        ? (static_cast<Real>(cx) + Real(1))
                        : static_cast<Real>(cx);
                    tMaxX = (nextX - fx0) / cdx;
                }
                Real tMaxY = inf;
                if (cdy != Real(0))
                {
                    const Real nextY = (stepY > 0)
                        ? (static_cast<Real>(cy) + Real(1))
                        : static_cast<Real>(cy);
                    tMaxY = (nextY - fy0) / cdy;
                }
                const Real tDeltaX = cdx != Real(0) ? std::abs(Real(1) / cdx) : inf;
                const Real tDeltaY = cdy != Real(0) ? std::abs(Real(1) / cdy) : inf;

                Real t = Real(0);
                const int budget = (src.Width() + src.Height()) * 2; // bounded
                for (int it = 0; it < budget; ++it)
                {
                    const bool blocked = opts.tallOnly ? src.BlocksSight(cx, cy)
                                                       : src.IsSolid(cx, cy);
                    if (blocked)
                    {
                        best.t      = t;
                        best.point  = Vec2(x0 + dx * t, y0 + dy * t);
                        best.isCell = true;
                        best.cellX  = cx;
                        best.cellY  = cy;
                        best.body   = kInvalidBody;
                        haveBest    = true;
                        break;
                    }
                    if (cx == endCx && cy == endCy)
                    {
                        break;
                    }
                    if (tMaxX < tMaxY)
                    {
                        t = tMaxX;
                        tMaxX += tDeltaX;
                        cx += stepX;
                    }
                    else
                    {
                        t = tMaxY;
                        tMaxY += tDeltaY;
                        cy += stepY;
                    }
                    if (t > Real(1))
                    {
                        break;
                    }
                }
            }

            // ---- body tests (coordinate-agnostic) ---------------------------
            if (opts.cellsOnly)
            {
                return haveBest ? std::optional<RaycastHit>(best) : std::nullopt;
            }

            for (std::uint32_t i = 0; i < m_count; ++i)
            {
                if (m_alive[i] == 0)
                {
                    continue;
                }
                const Shape& s = m_shape[i];
                if (s.kind == ShapeKind::Aabb)
                {
                    // Exact slab test (ports lines 554-582).
                    const Aabb2 box = SlotAabb(i);
                    const Real a0 = box.min.x, b0 = box.min.y;
                    const Real a1 = box.max.x, b1 = box.max.y;
                    Real tmin = Real(0), tmax = Real(1);
                    bool ok = true;
                    if (dx == Real(0))
                    {
                        if (x0 < a0 || x0 > a1) ok = false;
                    }
                    else
                    {
                        Real ta = (a0 - x0) / dx, tb = (a1 - x0) / dx;
                        if (ta > tb) std::swap(ta, tb);
                        if (ta > tmin) tmin = ta;
                        if (tb < tmax) tmax = tb;
                    }
                    if (ok)
                    {
                        if (dy == Real(0))
                        {
                            if (y0 < b0 || y0 > b1) ok = false;
                        }
                        else
                        {
                            Real ta = (b0 - y0) / dy, tb = (b1 - y0) / dy;
                            if (ta > tb) std::swap(ta, tb);
                            if (ta > tmin) tmin = ta;
                            if (tb < tmax) tmax = tb;
                        }
                    }
                    if (ok && tmin <= tmax && tmin <= Real(1) && tmax >= Real(0))
                    {
                        const Real t = std::max(Real(0), tmin);
                        if (!haveBest || t < best.t)
                        {
                            best.t      = t;
                            best.point  = Vec2(x0 + dx * t, y0 + dy * t);
                            best.isCell = false;
                            best.body   = BodyHandle{ i, m_gen[i] };
                            haveBest    = true;
                        }
                    }
                }
                else
                {
                    // Exact shape hit via GJK point-cast (rayVsBody): a ray is a
                    // zero-radius circle cast against this body's shape.
                    const std::optional<Real> t = RayVsBody(i, from, Vec2(dx, dy));
                    if (t && (!haveBest || *t < best.t))
                    {
                        best.t      = *t;
                        best.point  = Vec2(x0 + dx * *t, y0 + dy * *t);
                        best.isCell = false;
                        best.body   = BodyHandle{ i, m_gen[i] };
                        haveBest    = true;
                    }
                }
            }

            return haveBest ? std::optional<RaycastHit>(best) : std::nullopt;
        }

        // --------------------------------------------------------------------
        // RayVsBody -- PORT of Cast.rayVsBody. A ray (point) = zero-radius
        // circle cast against ONE body's shape via the GJK conservative-
        // advancement primitive. Returns the parametric t or nullopt.
        // --------------------------------------------------------------------
        std::optional<Real> PhysicsWorld::RayVsBody(std::uint32_t idx,
                                                    const Vec2& from,
                                                    const Vec2& delta) const
        {
            static const Shape ray = MakeCircle(Real(0)); // zero-radius circle
            const Transform start{ from, Real(0) };
            const Transform bodyXf{ Vec2(m_posX[idx], m_posY[idx]), Real(0) };
            // Qualified: the free GJK ShapeCast, not PhysicsWorld::ShapeCast.
            const ShapeCastResult r =
                ::Arcane::Physics::ShapeCast(ray, start, delta, m_shape[idx], bodyXf);
            if (!r.hit)
            {
                return std::nullopt;
            }
            return r.t;
        }

        // --------------------------------------------------------------------
        // LineOfSight -- PORT of lineOfSight (601-604). Only TALL (sight-
        // blocking) cells block; LOW obstacles are see-through.
        // --------------------------------------------------------------------
        bool PhysicsWorld::LineOfSight(const Vec2& from, const Vec2& to) const
        {
            // Degenerate -> no LOS (ports `if not x1 then return false`).
            if (from.x == to.x && from.y == to.y)
            {
                return false;
            }
            RaycastOpts opts;
            opts.tallOnly  = true;
            opts.cellsOnly = true;
            return !Raycast(from, to, opts).has_value();
        }

        // --------------------------------------------------------------------
        // ShapeCast -- PORT of Cast.shapeCast. Swept-AABB candidate collection
        // (tile spans + non-sensor statics + optional movers) cast against each
        // via the P1.4 GJK conservative-advancement primitive (ShapeCastPoly for
        // spans, ShapeCast for body shapes). Keeps the NEAREST t. The hit point
        // is the shape CENTER at TOI.
        // --------------------------------------------------------------------
        std::optional<ShapeCastHit> PhysicsWorld::ShapeCast(
            const Shape& shape, const Vec2& pos, const Vec2& delta,
            const ShapeCastOpts& opts) const
        {
            const Real len =
                std::sqrt(delta.x * delta.x + delta.y * delta.y);
            if (len < Real(1e-9))
            {
                return std::nullopt; // no travel (ports the Cast.lua guard)
            }

            // Swept AABB for candidate collection (+/-2 pad, ports lines 40-43).
            const Transform xf0{ pos, Real(0) };
            const Transform xf1{ Vec2(pos.x + delta.x, pos.y + delta.y), Real(0) };
            const Aabb2 a = shape.ComputeAABB(xf0);
            const Aabb2 b = shape.ComputeAABB(xf1);
            Aabb2 swept;
            swept.min = Vec2(std::min(a.min.x, b.min.x) - Real(2),
                             std::min(a.min.y, b.min.y) - Real(2));
            swept.max = Vec2(std::max(a.max.x, b.max.x) + Real(2),
                             std::max(a.max.y, b.max.y) + Real(2));

            StaticCandidates(swept, m_scratchSpans, m_scratchStatics);

            bool         haveBest = false;
            ShapeCastHit best{};
            auto consider = [&](const ShapeCastResult& r, BodyHandle body)
            {
                if (r.hit && (!haveBest || r.t < best.t))
                {
                    best.t        = r.t;
                    best.normal   = r.normal;
                    best.distance = r.distance;
                    best.body     = body;
                    haveBest      = true;
                }
            };

            // Tile spans (raw world-space AABB polys -> ShapeCastPoly). The span
            // is an axis-aligned box; its 4 CCW corners feed ShapeCastPoly.
            for (std::size_t i = 0; i < m_scratchSpans.size(); ++i)
            {
                const Aabb2& sp = m_scratchSpans[i];
                const Vec2 poly[4] = {
                    Vec2(sp.min.x, sp.min.y),
                    Vec2(sp.max.x, sp.min.y),
                    Vec2(sp.max.x, sp.max.y),
                    Vec2(sp.min.x, sp.max.y),
                };
                consider(ShapeCastPoly(shape, xf0, delta, poly, 4), kInvalidBody);
            }

            // Non-sensor static bodies.
            for (std::size_t i = 0; i < m_scratchStatics.size(); ++i)
            {
                const std::uint32_t idx = m_scratchStatics[i];
                if (m_sensor[idx] != 0)
                {
                    continue;
                }
                const Transform bodyXf{ Vec2(m_posX[idx], m_posY[idx]), Real(0) };
                consider(::Arcane::Physics::ShapeCast(shape, xf0, delta,
                                                      m_shape[idx], bodyXf),
                         BodyHandle{ idx, m_gen[idx] });
            }

            // Optional movers (kinematic/dynamic, non-sensor, not excluded).
            if (opts.movers)
            {
                for (std::uint32_t i = 0; i < m_count; ++i)
                {
                    if (m_alive[i] == 0 ||
                        static_cast<BodyType>(m_btype[i]) == BodyType::Static ||
                        m_sensor[i] != 0)
                    {
                        continue;
                    }
                    if (opts.exclude != kInvalidBody &&
                        opts.exclude.index == i &&
                        opts.exclude.generation == m_gen[i])
                    {
                        continue;
                    }
                    const Transform bodyXf{ Vec2(m_posX[i], m_posY[i]), Real(0) };
                    consider(::Arcane::Physics::ShapeCast(shape, xf0, delta,
                                                          m_shape[i], bodyXf),
                             BodyHandle{ i, m_gen[i] });
                }
            }

            if (!haveBest)
            {
                return std::nullopt;
            }
            // Position = shape CENTER at TOI (ports best.x/best.y).
            best.point = Vec2(pos.x + delta.x * best.t, pos.y + delta.y * best.t);
            return best;
        }

        // --------------------------------------------------------------------
        // OverlapShape -- NEW (composed; no direct Lua method). Body handles
        // whose shape overlaps the query shape at xf, narrowed by CollideShapes
        // (pointCount > 0). Includes statics + movers; index-ordered.
        // --------------------------------------------------------------------
        int PhysicsWorld::OverlapShape(const Shape& shape, const Transform& xf,
                                       std::vector<BodyHandle>& out) const
        {
            out.clear();
            // Broadphase candidate pass via the query shape's tight AABB, then a
            // narrowphase overlap test. We do a deterministic INDEX-order linear
            // scan over alive slots (the broadphase QueryAABB returns ids sorted
            // ascending too, but a direct index scan with an AABB pre-filter is
            // simpler, index-ordered, and matches QueryAABB's contract). The AABB
            // pre-filter is the broadphase narrowing predicate.
            const Aabb2 qbox = shape.ComputeAABB(xf);
            for (std::uint32_t i = 0; i < m_count; ++i)
            {
                if (m_alive[i] == 0)
                {
                    continue;
                }
                if (!AabbOverlap(qbox, SlotAabb(i)))
                {
                    continue; // broad reject
                }
                const Transform bodyXf{ Vec2(m_posX[i], m_posY[i]), Real(0) };
                const Manifold m =
                    CollideShapes(shape, xf, m_shape[i], bodyXf, Real(0));
                if (m.pointCount > 0)
                {
                    out.push_back(BodyHandle{ i, m_gen[i] });
                }
            }
            return static_cast<int>(out.size());
        }

        void PhysicsWorld::StaticCandidates(const Aabb2& box,
                                            std::vector<Aabb2>& spansOut,
                                            std::vector<std::uint32_t>& staticsOut) const
        {
            // Merged tile spans (TileGrid) + overlapping static-body slots
            // (staticList, index-ordered). Ports _staticCandidates. With no
            // TileGrid spansOut is empty.
            spansOut.clear();
            if (m_tileGrid)
            {
                m_tileGrid->Query(box, spansOut);
            }
            staticsOut.clear();
            for (std::uint32_t i = 0; i < m_staticList.size(); ++i)
            {
                const std::uint32_t idx = m_staticList[i];
                if (AabbOverlap(box, SlotAabb(idx)))
                {
                    staticsOut.push_back(idx);
                }
            }
        }

        Body PhysicsWorld::GetBody(BodyHandle h) noexcept
        {
            return Body(this, h);
        }

    } // namespace Physics
} // namespace Arcane
