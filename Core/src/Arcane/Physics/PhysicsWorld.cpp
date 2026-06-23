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
#include <cassert>
#include <cmath>
#include <functional>

#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/Broadphase/DynamicTree.hpp>
#include <Arcane/Physics/Broadphase/SpatialHash.hpp>
#include <Arcane/Physics/Broadphase/SweepAndPrune.hpp>
#include <Arcane/Physics/Narrowphase/GeometryKernel.hpp> // AabbOverlap (QueryAABB)
#include <Arcane/Physics/Narrowphase/Collide.hpp>        // Collide (rotation-aware fixture-pair narrowphase: contacts T5, events/overlap T7)
#include <Arcane/Physics/Solver/SoftStep.hpp>            // SoftStep solver impl
#include <Arcane/Physics/Solver/Baumgarte.hpp>           // Baumgarte oracle impl (A/B)
#include <Arcane/Physics/Island.hpp>                     // island sleep pass (stage 4)
#include <Arcane/Physics/Joints/Joints.hpp>              // joint set + MakeJoint factory (P2.5)

namespace Arcane
{
    namespace Physics
    {
        namespace
        {
            // Combine the geometric feature id (from Collide) with the FIXTURE-PAIR
            // identity so two DIFFERENT fixture pairs on the SAME body pair get
            // DISTINCT, STABLE warm-start keys. The solver's warm-start cache is
            // keyed by this id; without the fixture mix two fixtures of a compound
            // body that hit the same feature of the other body produce the SAME id
            // and ALIAS in the cache (one fixture-pair's accumulated impulse would
            // seed the other's contact point at a different lever arm -> degraded
            // warm start / penetration resistance for every compound body). The
            // mix is a deterministic integer hash (no fp; stable per (base, fixA,
            // fixB) across steps -- the warm-start invariant only needs the same
            // physical contact to map to the SAME id each step, never a particular
            // value, so this is physics-neutral for any non-aliasing contact).
            //
            // Legacy single-shape bodies (no fixtures -> both indices kInvalidSlot)
            // pass the base id through UNCHANGED: there is exactly one geometric
            // pair per body pair so the base is already unique, and byte-identity
            // is preserved for the pre-fixture-era paths.
            [[nodiscard]] inline std::uint32_t MixContactId(std::uint32_t base,
                                                            std::uint32_t fixA,
                                                            std::uint32_t fixB) noexcept
            {
                if (fixA == kInvalidSlot && fixB == kInvalidSlot)
                {
                    return base;
                }
                // 64-bit MurmurHash3-style finalizer over (base, fixA, fixB).
                std::uint64_t h = static_cast<std::uint64_t>(base);
                h = (h ^ (static_cast<std::uint64_t>(fixA) + 0x9E3779B97F4A7C15ull))
                    * 0xFF51AFD7ED558CCDull;
                h = (h ^ (static_cast<std::uint64_t>(fixB) + 0xC2B2AE3D27D4EB4Full))
                    * 0xFF51AFD7ED558CCDull;
                h ^= h >> 33;
                return static_cast<std::uint32_t>(h);
            }
        } // namespace

        // Compose a fixture's WORLD transform = body transform ∘ fixture local
        // transform.  SlotAabb, GenerateContacts' FixtureWorldXf lambda,
        // SlotsOverlap, the fixture-aware queries, and BulletSweep all delegate
        // here so the rotate+offset formula lives in exactly ONE place. Promoted
        // from a TU-local static to a static member (T7) so Queries.cpp (a
        // separate TU) can compose fixture world transforms too.
        Transform PhysicsWorld::ComposeFixtureXf(Vec2 bodyPos, Real bodyAngle,
                                                 Vec2 localPos,
                                                 Real localAngle) noexcept
        {
            const Real bc = std::cos(bodyAngle);
            const Real bs = std::sin(bodyAngle);
            return Transform{
                Vec2(bodyPos.x + bc * localPos.x - bs * localPos.y,
                     bodyPos.y + bs * localPos.x + bc * localPos.y),
                bodyAngle + localAngle
            };
        }

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

            std::unique_ptr<ISolver> MakeSolver(const WorldDef& def)
            {
                // P2.3 A/B seam: pick the installed solver by WorldDef::solverKind
                // (default SoftStep -- the P2.2 TGS Soft solver; Baumgarte is the
                // retained PGS oracle that runs the same scenes for cross-check).
                switch (def.solverKind)
                {
                case SolverKind::Baumgarte:
                    return std::make_unique<Baumgarte>();
                case SolverKind::SoftStep:
                default:
                    return std::make_unique<SoftStep>();
                }
            }
        } // namespace

        PhysicsWorld::PhysicsWorld(const WorldDef& def)
            : m_fixtureBroadphase(MakeBroadphase(def))
            , m_gravityX(def.gravityX)
            , m_gravityY(def.gravityY)
            , m_substepCount(def.substepCount > 0u ? def.substepCount : 1u)
            , m_contactHertz(def.contactHertz)
            , m_contactDampingRatio(def.contactDampingRatio)
            , m_restitutionThreshold(def.restitutionThreshold)
            , m_contactPushMaxVelocity(def.contactPushMaxVelocity)
            , m_velIters(def.velIters > 0u ? def.velIters : 1u)
            , m_solver(MakeSolver(def))
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

            // Dynamics SoA (P2.1). Reals default-zero (no inverse mass/inertia ->
            // immovable like a static until AddBody fills a Dynamic slot); awake
            // defaults to 1 so an un-filled slot is never treated as a sleeper.
            // AddBody overwrites every field for the slot it returns, so these
            // fills only matter for the never-touched tail; keeping them
            // self-consistent avoids surprises if a future path scans the SoA.
            m_angle.resize(next, Real(0));
            m_angVel.resize(next, Real(0));
            m_invMass.resize(next, Real(0));
            m_invInertia.resize(next, Real(0));
            m_rest.resize(next, Real(0));
            m_fric.resize(next, Real(0));
            m_linDamp.resize(next, Real(0));
            m_sleepTimer.resize(next, Real(0));
            m_awake.resize(next, std::uint8_t(1));
            m_bullet.resize(next, std::uint8_t(0));
        }

        // ----------------------------------------------------------------
        // Fixture SoA helpers (v2 Task 4)
        // ----------------------------------------------------------------

        void PhysicsWorld::EnsureFxCapacity(std::uint32_t n)
        {
            if (n <= m_fxShape.size())
            {
                return;
            }
            // Amortised growth: at least double the current capacity.
            const std::uint32_t next =
                std::max(n, static_cast<std::uint32_t>(
                    m_fxShape.empty() ? 4u : m_fxShape.capacity() * 2u));

            m_fxShape.resize(next);
            m_fxLocalPosX.resize(next, Real(0));
            m_fxLocalPosY.resize(next, Real(0));
            m_fxLocalAngle.resize(next, Real(0));
            m_fxDensity.resize(next, Real(0));
            m_fxFriction.resize(next, Real(0));
            m_fxRestitution.resize(next, Real(0));
            m_fxFilterCat.resize(next, 1u);
            m_fxFilterMask.resize(next, 0xFFFFFFFFu);
            m_fxSensor.resize(next, std::uint8_t(0));
            m_fxBody.resize(next, 0u);
            m_fxGen.resize(next, 0u); // 0 = dead; live starts at 1
        }

        void PhysicsWorld::EnsureBodyAuxCapacity(std::uint32_t n)
        {
            if (n <= m_bodyFixtures.size())
            {
                return;
            }
            // Grow to match body SoA capacity.
            const std::uint32_t next =
                std::max(n, static_cast<std::uint32_t>(
                    m_bodyFixtures.empty() ? 4u : m_bodyFixtures.capacity() * 2u));

            m_bodyFixtures.resize(next);
            m_localCenterX.resize(next, Real(0));
            m_localCenterY.resize(next, Real(0));
            m_bodyMass.resize(next, Real(0));
            m_bodyInertia.resize(next, Real(0));
            m_fixedRotation.resize(next, std::uint8_t(0));
            m_massOverride.resize(next, Real(0));
        }

        void PhysicsWorld::RecomputeBodyMass(std::uint32_t bodySlot)
        {
            // Static and Kinematic bodies never integrate or solve; keep their
            // inverse mass/inertia at zero (the existing convention).
            const BodyType bt = static_cast<BodyType>(m_btype[bodySlot]);
            if (bt != BodyType::Dynamic)
            {
                m_invMass[bodySlot]    = Real(0);
                m_invInertia[bodySlot] = Real(0);
                m_bodyMass[bodySlot]   = Real(0);
                m_bodyInertia[bodySlot]= Real(0);
                m_localCenterX[bodySlot] = Real(0);
                m_localCenterY[bodySlot] = Real(0);
                return;
            }

            // Accumulate mass, density-weighted centroid, and inertia over
            // all fixtures attached to this body slot.
            //
            // Fix 2: collect each fixture's MassData ONCE into a local cache so
            // the two-pass aggregation (centroid + parallel-axis inertia) reads
            // each ComputeMass result from the cache rather than recomputing it.
            // This produces identical aggregation values + identical order; only
            // the per-fixture ComputeMass call is deduplicated.
            const std::vector<std::uint32_t>& fxList = m_bodyFixtures[bodySlot];

            // Cache: parallel arrays of (bodyLocalX, bodyLocalY, massData) for
            // each contributing fixture.  Stack-allocated vector (setup-time only).
            struct FxEntry
            {
                Real      bx, by;   // centroid in body-local space
                MassData  md;
            };
            std::vector<FxEntry> entries;
            entries.reserve(fxList.size());

            Real totalMass = Real(0);
            // Weighted centroid numerator (body-local space).
            Real cx = Real(0);
            Real cy = Real(0);

            // Single pass: compute each fixture's MassData once, accumulate mass
            // and density-weighted centroid, and store for the inertia pass.
            for (const std::uint32_t fi : fxList)
            {
                if (m_fxGen[fi] == 0u)
                {
                    continue; // dead slot in the list (should not happen; defensive)
                }
                const MassData md = m_fxShape[fi].ComputeMass(m_fxDensity[fi]);
                if (md.mass <= Real(0))
                {
                    continue; // degenerate / zero-density fixture
                }

                // The fixture's centroid in body-local space:
                //   bodyLocalCentroid = localPos + R(localAngle) * shapeCentroid
                // For our analytic test cases the shape centroid is (0,0) for
                // both circle and aabb, so localPos is the body-local centroid.
                // The general form is computed here for correctness.
                const Real la  = m_fxLocalAngle[fi];
                const Real lc  = std::cos(la);
                const Real ls  = std::sin(la);
                const Real scx = md.centroid.x;
                const Real scy = md.centroid.y;
                // Rotate shape centroid by localAngle, then offset by localPos.
                const Real bx = m_fxLocalPosX[fi] + lc * scx - ls * scy;
                const Real by = m_fxLocalPosY[fi] + ls * scx + lc * scy;

                totalMass += md.mass;
                cx += md.mass * bx;
                cy += md.mass * by;

                entries.push_back(FxEntry{ bx, by, md });
            }

            if (totalMass <= Real(0))
            {
                // No contributing fixtures (all degenerate).
                m_invMass[bodySlot]      = Real(0);
                m_invInertia[bodySlot]   = Real(0);
                m_bodyMass[bodySlot]     = Real(0);
                m_bodyInertia[bodySlot]  = Real(0);
                m_localCenterX[bodySlot] = Real(0);
                m_localCenterY[bodySlot] = Real(0);
                return;
            }

            // Body center of mass in body-local frame.
            const Real comX = cx / totalMass;
            const Real comY = cy / totalMass;

            // Second pass: sum inertia about the COM via parallel-axis theorem
            // using the cached MassData (no second ComputeMass call per fixture).
            //   I_total = sum_i (I_i + m_i * |centroid_i - COM|^2)
            Real totalInertia = Real(0);
            for (const FxEntry& e : entries)
            {
                // Parallel-axis shift: distance^2 from fixture centroid to COM.
                const Real dx = e.bx - comX;
                const Real dy = e.by - comY;
                totalInertia += e.md.inertia + e.md.mass * (dx * dx + dy * dy);
            }

            // Fix 1: if the body has a mass override (set at AddBody when
            // def.mass > 0), apply it now: replace the density-derived totalMass
            // and scale totalInertia proportionally, exactly mirroring the
            // AddBody override semantics (scale = override / computedMass).
            // This preserves the override through subsequent AddFixture calls so
            // compound bodies with an explicit mass keep the right dynamics.
            // Static/Kinematic never reach this branch (early return above).
            const Real massOvr = (bodySlot < m_massOverride.size())
                                     ? m_massOverride[bodySlot]
                                     : Real(0);
            if (massOvr > Real(0) && totalMass > Real(0))
            {
                const Real scale = massOvr / totalMass;
                totalInertia    *= scale;
                totalMass        = massOvr;
            }

            // Store the aggregated body state.
            m_bodyMass[bodySlot]     = totalMass;
            m_localCenterX[bodySlot] = comX;
            m_localCenterY[bodySlot] = comY;
            m_bodyInertia[bodySlot]  = totalInertia;

            m_invMass[bodySlot]    = Real(1) / totalMass;
            const bool fixedRot    = m_fixedRotation[bodySlot] != 0u;
            m_invInertia[bodySlot] = (fixedRot || totalInertia <= Real(0))
                                         ? Real(0)
                                         : Real(1) / totalInertia;
        }

        // ----------------------------------------------------------------
        // Fixture lifecycle: AddFixture / DropFixture / IsValid(FixtureHandle)
        // ----------------------------------------------------------------

        std::uint32_t PhysicsWorld::AllocFixtureSlot(std::uint32_t bodySlot,
                                                          const FixtureDef& def)
        {
            // Ensure per-body auxiliary arrays cover this body slot.
            EnsureBodyAuxCapacity(bodySlot + 1u);

            // Acquire a fixture slot (reuse from free-list or append).
            std::uint32_t fi;
            if (!m_fxFree.empty())
            {
                fi = m_fxFree.back();
                m_fxFree.pop_back();
            }
            else
            {
                fi = m_fxCount;
                EnsureFxCapacity(m_fxCount + 1u);
                ++m_fxCount;
            }

            // Populate the fixture slot fields.
            m_fxShape[fi]      = def.shape;
            m_fxLocalPosX[fi]  = def.localPos.x;
            m_fxLocalPosY[fi]  = def.localPos.y;
            m_fxLocalAngle[fi] = def.localAngle;
            m_fxDensity[fi]    = def.density;
            m_fxFriction[fi]   = def.friction;
            m_fxRestitution[fi]= def.restitution;
            m_fxFilterCat[fi]  = def.categoryBits;
            m_fxFilterMask[fi] = def.maskBits;
            m_fxSensor[fi]     = def.isSensor ? std::uint8_t(1) : std::uint8_t(0);
            m_fxBody[fi]       = bodySlot;
            m_fxGen[fi]       += 1u; // bump generation (dead=0, live starts at 1)

            // Link the slot to the body.
            m_bodyFixtures[bodySlot].push_back(fi);

            return fi;
        }

        // AddFixture: compound shapes + mass-override now compose correctly.
        // After allocating and linking the fixture slot, RecomputeBodyMass
        // re-aggregates the body mass from all fixtures (respecting any
        // m_massOverride set at AddBody), so a previously set mass override
        // is preserved rather than silently discarded.
        FixtureHandle PhysicsWorld::AddFixture(BodyHandle bh, const FixtureDef& def)
        {
            if (!IsValid(bh))
            {
                return kInvalidFixture;
            }
            const std::uint32_t bodySlot = bh.index;

            const std::uint32_t fi = AllocFixtureSlot(bodySlot, def);

            // Re-aggregate the body's mass / COM / inertia (mass override respected).
            RecomputeBodyMass(bodySlot);

            // A fixture added to a STATIC body grows its union AABB; movers
            // self-correct via the per-step broadphase Update, but statics never
            // re-register, so refresh the static grid here.
            if (static_cast<BodyType>(m_btype[bodySlot]) == BodyType::Static)
                m_staticGrid.Move(bodySlot, SlotAabb(bodySlot));

            // Register the new fixture in the per-fixture mover broadphase
            // (Phase 2, Task 1). AddFixtureProxy skips Static bodies.
            AddFixtureProxy(fi);

            return FixtureHandle{ fi, m_fxGen[fi] };
        }

        void PhysicsWorld::DropFixture(FixtureHandle fh)
        {
            if (!IsValid(fh))
            {
                return;
            }
            const std::uint32_t fi       = fh.index;
            const std::uint32_t bodySlot = m_fxBody[fi];

            // Unlink from the body's fixture list (swap-and-pop).
            std::vector<std::uint32_t>& list = m_bodyFixtures[bodySlot];
            for (std::size_t k = 0; k < list.size(); ++k)
            {
                if (list[k] == fi)
                {
                    list[k] = list.back();
                    list.pop_back();
                    break;
                }
            }

            // Remove the fixture proxy from the per-fixture mover broadphase
            // BEFORE bumping the generation (Phase 2, Task 1). The Remove call
            // only needs the slot id -- it does not read gen or body fields.
            RemoveFixtureProxy(fi);

            // Invalidate the slot (bump generation + clear shape storage).
            m_fxGen[fi] += 1u;
            m_fxShape[fi] = Shape{}; // release polygon vertex/normal storage

            // Recycle.
            m_fxFree.push_back(fi);

            // Re-aggregate the body's mass.
            RecomputeBodyMass(bodySlot);

            // Symmetric with AddFixture: dropping a fixture from a STATIC shrinks
            // its union AABB. Statics never re-register via Step, so refresh the
            // static grid here to keep its AABB tight (movers self-correct each step).
            if (static_cast<BodyType>(m_btype[bodySlot]) == BodyType::Static)
                m_staticGrid.Move(bodySlot, SlotAabb(bodySlot));
        }

        bool PhysicsWorld::IsValid(FixtureHandle fh) const noexcept
        {
            if (fh.generation == 0u)
            {
                return false;
            }
            if (fh.index >= m_fxCount)
            {
                return false;
            }
            return m_fxGen[fh.index] == fh.generation;
        }

        std::uint32_t PhysicsWorld::FixtureCount(BodyHandle bh) const noexcept
        {
            if (!IsValid(bh))
            {
                return 0u;
            }
            const std::uint32_t bodySlot = bh.index;
            if (bodySlot >= m_bodyFixtures.size())
            {
                return 0u;
            }
            return static_cast<std::uint32_t>(m_bodyFixtures[bodySlot].size());
        }

        Vec2 PhysicsWorld::GetFixtureWorldPos(FixtureHandle fh) const noexcept
        {
            if (!IsValid(fh))
            {
                return Vec2(Real(0), Real(0));
            }
            const std::uint32_t fi       = fh.index;
            const std::uint32_t bodySlot = m_fxBody[fi];

            const Real bodyAngle = m_angle[bodySlot];
            const Real c = std::cos(bodyAngle);
            const Real s = std::sin(bodyAngle);
            const Real lx = m_fxLocalPosX[fi];
            const Real ly = m_fxLocalPosY[fi];

            return Vec2(
                m_posX[bodySlot] + c * lx - s * ly,
                m_posY[bodySlot] + s * lx + c * ly);
        }

        Real PhysicsWorld::GetFixtureWorldAngle(FixtureHandle fh) const noexcept
        {
            if (!IsValid(fh))
            {
                return Real(0);
            }
            const std::uint32_t fi       = fh.index;
            const std::uint32_t bodySlot = m_fxBody[fi];
            return m_angle[bodySlot] + m_fxLocalAngle[fi];
        }

        std::uint32_t PhysicsWorld::GetFixtureCategory(FixtureHandle fh) const noexcept
        {
            if (!IsValid(fh))
            {
                return 1u; // default
            }
            return m_fxFilterCat[fh.index];
        }

        std::uint32_t PhysicsWorld::GetFixtureMask(FixtureHandle fh) const noexcept
        {
            if (!IsValid(fh))
            {
                return 0xFFFFFFFFu; // default
            }
            return m_fxFilterMask[fh.index];
        }

        FixtureHandle PhysicsWorld::GetBodyFixture(BodyHandle bh,
                                                    std::uint32_t n) const noexcept
        {
            if (!IsValid(bh))
            {
                return kInvalidFixture;
            }
            const std::uint32_t bodySlot = bh.index;
            if (bodySlot >= m_bodyFixtures.size())
            {
                return kInvalidFixture;
            }
            const std::vector<std::uint32_t>& list = m_bodyFixtures[bodySlot];
            if (n >= static_cast<std::uint32_t>(list.size()))
            {
                return kInvalidFixture;
            }
            const std::uint32_t fi = list[n];
            if (fi >= m_fxCount || m_fxGen[fi] == 0u)
            {
                return kInvalidFixture;
            }
            return FixtureHandle{ fi, m_fxGen[fi] };
        }

        Real PhysicsWorld::GetBodyMass(BodyHandle bh) const noexcept
        {
            if (!IsValid(bh))
            {
                return Real(0);
            }
            const std::uint32_t bodySlot = bh.index;
            if (bodySlot >= m_bodyMass.size())
            {
                return Real(0);
            }
            return m_bodyMass[bodySlot];
        }

        Vec2 PhysicsWorld::GetLocalCenter(BodyHandle bh) const noexcept
        {
            if (!IsValid(bh))
            {
                return Vec2(Real(0), Real(0));
            }
            const std::uint32_t bodySlot = bh.index;
            if (bodySlot >= m_localCenterX.size())
            {
                return Vec2(Real(0), Real(0));
            }
            return Vec2(m_localCenterX[bodySlot], m_localCenterY[bodySlot]);
        }

        Real PhysicsWorld::GetBodyInertia(BodyHandle bh) const noexcept
        {
            if (!IsValid(bh))
            {
                return Real(0);
            }
            const std::uint32_t bodySlot = bh.index;
            if (bodySlot >= m_bodyInertia.size())
            {
                return Real(0);
            }
            return m_bodyInertia[bodySlot];
        }

        Aabb2 PhysicsWorld::SlotAabb(std::uint32_t i) const noexcept
        {
            // T5: body AABB = union of its fixtures' rotation-aware world AABBs.
            // Each fixture's world transform is composed from the body transform +
            // the fixture's local transform:
            //   worldAngle = bodyAngle + fxLocalAngle
            //   worldPos   = bodyPos + R(bodyAngle) * fxLocalPos
            // (via GetFixtureWorldPos / GetFixtureWorldAngle).
            //
            // The mover broadphase is PER-FIXTURE (m_fixtureBroadphase; one proxy
            // per live fixture of a Dynamic/Kinematic body). SlotAabb returns the
            // union over all fixtures and is used for the static grid + residency
            // grid only. A body with zero live fixtures falls back to the legacy
            // single-shape AABB (back-compat for any body that bypasses AddFixture).
            //
            // For a body with no fixture list or an empty fixture list, fall
            // back to the legacy single-shape path (same as pre-T5 behaviour).
            if (i < m_bodyFixtures.size() && !m_bodyFixtures[i].empty())
            {
                const Real bodyAngle = m_angle[i];
                const Vec2 bodyPos(m_posX[i], m_posY[i]);

                // Sentinel: empty AABB that we expand on first fixture.
                Aabb2 unionAabb;
                unionAabb.min = Vec2( Real(1e30f),  Real(1e30f));
                unionAabb.max = Vec2(-Real(1e30f), -Real(1e30f));

                for (const std::uint32_t fi : m_bodyFixtures[i])
                {
                    if (fi >= m_fxCount || m_fxGen[fi] == 0u)
                    {
                        continue; // dead slot (defensive)
                    }

                    // Compose body transform + fixture local transform (shared helper).
                    const Transform xf = ComposeFixtureXf(
                        bodyPos, bodyAngle,
                        Vec2(m_fxLocalPosX[fi], m_fxLocalPosY[fi]),
                        m_fxLocalAngle[fi]);
                    const Aabb2 fxAabb = m_fxShape[fi].ComputeAABB(xf);

                    // Expand the union AABB.
                    unionAabb.min.x = std::min(unionAabb.min.x, fxAabb.min.x);
                    unionAabb.min.y = std::min(unionAabb.min.y, fxAabb.min.y);
                    unionAabb.max.x = std::max(unionAabb.max.x, fxAabb.max.x);
                    unionAabb.max.y = std::max(unionAabb.max.y, fxAabb.max.y);
                }

                // If we actually expanded the sentinel (at least one live fixture),
                // return the union; otherwise fall through to the legacy path.
                if (unionAabb.min.x <= unionAabb.max.x)
                {
                    return unionAabb;
                }
            }

            // Legacy fallback: single-shape AABB with the body's real angle.
            // (Also correct for bodies that have fixtures but they all died.)
            const Transform xf{ Vec2(m_posX[i], m_posY[i]), m_angle[i] };
            return m_shape[i].ComputeAABB(xf);
        }

        // ----------------------------------------------------------------
        // Per-fixture broadphase helpers (Phase 2, Task 1)
        // ----------------------------------------------------------------

        Aabb2 PhysicsWorld::FixtureAabb(std::uint32_t fi) const noexcept
        {
            const std::uint32_t b = m_fxBody[fi];
            const Transform xf = ComposeFixtureXf(
                Vec2(m_posX[b], m_posY[b]), m_angle[b],
                Vec2(m_fxLocalPosX[fi], m_fxLocalPosY[fi]), m_fxLocalAngle[fi]);
            return m_fxShape[fi].ComputeAABB(xf);
        }

        void PhysicsWorld::AddFixtureProxy(std::uint32_t fi)
        {
            const std::uint32_t b = m_fxBody[fi];
            if (static_cast<BodyType>(m_btype[b]) == BodyType::Static)
            {
                return; // statics are not in the mover fixture broadphase
            }
            m_fixtureBroadphase->Update(fi, FixtureAabb(fi));
        }

        void PhysicsWorld::RemoveFixtureProxy(std::uint32_t fi)
        {
            m_fixtureBroadphase->Remove(fi);
        }

        void PhysicsWorld::UpdateMoverProxies(std::uint32_t b)
        {
            // Residency index (combat-sphere seam): keep body-union AABB in sync.
            const Aabb2 bodyBox = SlotAabb(b);
            m_residencyGrid.Move(b, bodyBox);

            // Per-fixture proxies: refresh every live fixture of this body.
            const Vec2 pos(m_posX[b], m_posY[b]);
            const Real ang = m_angle[b];
            if (b < m_bodyFixtures.size())
            {
                for (const std::uint32_t fi : m_bodyFixtures[b])
                {
                    if (fi >= m_fxCount || m_fxGen[fi] == 0u)
                    {
                        continue; // dead slot (defensive)
                    }
                    const Transform xf = ComposeFixtureXf(
                        pos, ang,
                        Vec2(m_fxLocalPosX[fi], m_fxLocalPosY[fi]),
                        m_fxLocalAngle[fi]);
                    m_fixtureBroadphase->Update(fi, m_fxShape[fi].ComputeAABB(xf));
                }
            }
        }

        void PhysicsWorld::LiveFixtureAabbs(std::vector<std::uint32_t>& fxOut,
                                            std::vector<Aabb2>& boxOut) const
        {
            fxOut.clear(); boxOut.clear();
            // Enumerate the AUTHORITATIVE live mover-fixture set: each live
            // Dynamic/Kinematic body's m_bodyFixtures list (exactly what the
            // fixture broadphase indexes; dropped fixtures are unlinked from it).
            for (std::uint32_t b = 0; b < m_count; ++b)
            {
                if (m_alive[b] == 0u) continue;
                if (static_cast<BodyType>(m_btype[b]) == BodyType::Static) continue;
                for (const std::uint32_t fi : m_bodyFixtures[b])
                {
                    if (fi >= m_fxCount || m_fxGen[fi] == 0u) continue; // defensive
                    fxOut.push_back(fi);
                    boxOut.push_back(FixtureAabb(fi));
                }
            }
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

            // ---- dynamics state (P2.1) -- ports addBody (PhysicsWorld.lua:
            // 229-250). Static/Kinematic get the zero defaults (no inverse
            // mass/inertia -> never integrated/solved). A Dynamic body derives
            // mass + rotational inertia from Shape::ComputeMass(density).
            m_angle[idx]      = Real(0);
            m_angVel[idx]     = Real(0);
            m_invMass[idx]    = Real(0);
            m_invInertia[idx] = Real(0);
            m_rest[idx]       = def.restitution;
            m_fric[idx]       = def.friction;
            m_linDamp[idx]    = def.linearDamping;
            m_sleepTimer[idx] = Real(0);
            m_awake[idx]      = 1;
            m_bullet[idx]     = def.bullet ? std::uint8_t(1) : std::uint8_t(0);

            if (def.type == BodyType::Dynamic)
            {
                // A dynamic AABB must be fixedRotation: an axis-aligned box has
                // no meaningful orientation in this fixed-rotation engine
                // (ports the Lua assert, PhysicsWorld.lua:238).
                assert((def.shape.kind != ShapeKind::Aabb || def.fixedRotation) &&
                       "dynamic AABB shapes must be fixedRotation "
                       "(axis-aligned by definition)");

                // Mass + rotational inertia from Shape::ComputeMass(density)
                // (the P1.1 MassData -- verified equivalent to the Lua massProps,
                // so we do NOT re-port massProps). md.inertia is about the
                // shape's centroid, which matches the Lua's per-shape inertia.
                const MassData md = def.shape.ComputeMass(def.density);
                Real m       = md.mass;
                Real inertia = md.inertia;

                // Optional mass override: scale inertia by (mass/computedMass)
                // then use the override mass (ports lines 241-243). Guard against
                // a zero computed mass (degenerate shape) so the scale is safe.
                if (def.mass > Real(0))
                {
                    if (m > Real(0))
                    {
                        inertia = inertia * (def.mass / m);
                    }
                    m = def.mass;
                }

                m_invMass[idx]    = m > Real(0) ? Real(1) / m : Real(0);
                m_invInertia[idx] = (def.fixedRotation || inertia <= Real(0))
                                        ? Real(0)
                                        : Real(1) / inertia;
            }

            if (def.type == BodyType::Static)
            {
                m_staticList.push_back(idx);
                // SlotAabb uses the single-shape fallback here: m_bodyFixtures[idx]
                // is empty at this point (a fresh slot, or cleared by RemoveBody on
                // recycle), and the back-compat fixture created later in AddBody has
                // the SAME shape, so the registered AABB stays consistent.
                m_staticGrid.Insert(idx, SlotAabb(idx));
            }
            else
            {
                // Kinematic + Dynamic: register body-union AABB in residency index
                // (combat-sphere seam). Fixture proxies are added via AddFixtureProxy
                // below; the per-fixture broadphase (m_fixtureBroadphase) is the sole
                // mover broadphase as of Phase 2, Task 3.
                const Aabb2 moverBox = SlotAabb(idx);
                m_residencyGrid.Insert(idx, moverBox);
            }

            // ---- Back-compat fixture creation (v2 Task 4) -------------------
            //
            // AddBody creates exactly ONE fixture from the BodyDef's shape +
            // material so the fixture data model is always populated.
            // The legacy single-shape contact-gen path (GenerateContacts) reads
            // m_shape[] + m_rest[] + m_fric[] DIRECTLY and is UNCHANGED (Task 5
            // wires fixture-pair contact gen and retires that path).  So we
            // create the fixture record WITHOUT calling RecomputeBodyMass --
            // the already-correct m_invMass/m_invInertia derived above (which
            // correctly handles the optional mass override) must be preserved
            // byte-for-byte.
            //
            // EnsureBodyAuxCapacity + store fixedRotation + mass-override flags
            // (needed by RecomputeBodyMass if AddFixture is called later).
            EnsureBodyAuxCapacity(idx + 1u);
            m_bodyFixtures[idx].clear(); // fresh slot (may be recycled)
            m_fixedRotation[idx] = def.fixedRotation ? std::uint8_t(1) : std::uint8_t(0);
            // Record the mass override so RecomputeBodyMass (called by a later
            // AddFixture) honours it and does not silently drop it (Fix 1).
            m_massOverride[idx] = (def.mass > Real(0)) ? def.mass : Real(0);

            // Create the auto fixture record via AllocFixtureSlot (shared with
            // AddFixture -- single field-population site, safe for T5 to extend).
            // We do NOT call RecomputeBodyMass here; the legacy invMass/invInertia
            // path above is the one source of truth for the single-shape case.
            //
            // T6 fix: populate the auto-fixture from the new BodyDef filter +
            // local-transform fields so authored categoryBits / maskBits /
            // localPos / localAngle on the primary fixture flow through.
            // The mass/invMass path above is NOT changed: compound-COM for an
            // offset primary fixture is deferred; legacy dynamics tests stay
            // byte-for-byte green because invMass/invInertia are derived from
            // def.shape + def.density only (above), never from localPos/localAngle.
            {
                FixtureDef autoFd;
                autoFd.shape        = def.shape;
                autoFd.localPos     = def.localPos;
                autoFd.localAngle   = def.localAngle;
                autoFd.density      = def.density;
                autoFd.friction     = def.friction;
                autoFd.restitution  = def.restitution;
                autoFd.categoryBits = def.categoryBits;
                autoFd.maskBits     = def.maskBits;
                autoFd.isSensor     = def.isSensor;
                const std::uint32_t autoFi = AllocFixtureSlot(idx, autoFd); // no RecomputeBodyMass

                // Register the auto-fixture in the per-fixture mover broadphase
                // (Phase 2, Task 1). AddFixtureProxy skips Static bodies.
                // Static fixtures are not registered here (covered by m_staticGrid).
                AddFixtureProxy(autoFi);

                // Populate the body-mass accessors consistently with the legacy
                // path.  For Dynamic bodies the correct mass was computed above
                // (possibly with a mass override); for Static/Kinematic it is 0.
                // This keeps GetBodyMass/GetBodyInertia/GetLocalCenter correct
                // for the single-fixture case without going through RecomputeBodyMass.
                if (def.type == BodyType::Dynamic)
                {
                    const MassData md = def.shape.ComputeMass(def.density);
                    Real totalMass    = md.mass;
                    Real totalInertia = md.inertia;
                    if (def.mass > Real(0) && totalMass > Real(0))
                    {
                        const Real scale = def.mass / totalMass;
                        totalInertia    *= scale;
                        totalMass        = def.mass;
                    }
                    m_bodyMass[idx]      = totalMass;
                    m_bodyInertia[idx]   = totalInertia;
                    m_localCenterX[idx]  = md.centroid.x;
                    m_localCenterY[idx]  = md.centroid.y;
                }
                else
                {
                    m_bodyMass[idx]     = Real(0);
                    m_bodyInertia[idx]  = Real(0);
                    m_localCenterX[idx] = Real(0);
                    m_localCenterY[idx] = Real(0);
                }
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
                m_staticGrid.Remove(idx);
            }
            else
            {
                m_residencyGrid.Remove(idx);
            }

            m_contacts.DropBody(idx);
            m_solver->DropBody(idx); // drop warm-start state for the recycled slot

            // Drop joints referencing the destroyed body (ports PhysicsWorld.lua
            // 281-286: `j.a.idx == idx or j.b.idx == idx`). Match by HANDLE index
            // (stable from construction, independent of Prepare). Iterate in
            // reverse so the swap-free erase keeps the surviving joints' order.
            for (std::size_t i = m_joints.size(); i-- > 0;)
            {
                const Joint* j = m_joints[i].get();
                const BodyHandle ha = j->HandleA();
                const BodyHandle hb = j->HandleB();
                if ((ha.generation != 0u && ha.index == idx) ||
                    (hb.generation != 0u && hb.index == idx))
                {
                    m_joints.erase(m_joints.begin() + static_cast<std::ptrdiff_t>(i));
                }
            }

            m_shape[idx] = Shape{}; // release polygon storage

            // ---- Drop all fixtures belonging to this body (v2 Task 4) -------
            if (idx < m_bodyFixtures.size())
            {
                // Recycle each fixture slot (bump generation; clear shape).
                // We iterate a COPY because DropFixture would modify the list.
                const std::vector<std::uint32_t> fxCopy = m_bodyFixtures[idx];
                for (const std::uint32_t fi : fxCopy)
                {
                    if (fi < m_fxCount && m_fxGen[fi] != 0u)
                    {
                        // Remove the fixture proxy BEFORE bumping gen (Phase 2,
                        // Task 1). Remove only needs the slot id.
                        RemoveFixtureProxy(fi);
                        m_fxGen[fi] += 1u;           // invalidate
                        m_fxShape[fi] = Shape{};     // release storage
                        m_fxFree.push_back(fi);
                    }
                }
                m_bodyFixtures[idx].clear();
                m_bodyMass[idx]     = Real(0);
                m_bodyInertia[idx]  = Real(0);
                m_localCenterX[idx] = Real(0);
                m_localCenterY[idx] = Real(0);
            }

            m_free.push_back(idx);
        }

        Joint* PhysicsWorld::AddJoint(const JointDef& def)
        {
            // Build the concrete joint (the Lua Joints.make factory). Unknown
            // kinds return nullptr (never thrown).
            std::unique_ptr<Joint> j = MakeJoint(*this, def);
            if (!j)
            {
                return nullptr;
            }
            Joint* raw = j.get();
            m_joints.push_back(std::move(j));

            // Wake the jointed bodies so a sleeping captive rejoins the solve
            // (ports addJoint's def.a:wake() / def.b:wake()).
            Wake(def.a);
            Wake(def.b);
            return raw;
        }

        void PhysicsWorld::RemoveJoint(Joint* j)
        {
            if (j == nullptr)
            {
                return;
            }
            for (std::size_t i = 0; i < m_joints.size(); ++i)
            {
                if (m_joints[i].get() == j)
                {
                    m_joints.erase(m_joints.begin() + static_cast<std::ptrdiff_t>(i));
                    return;
                }
            }
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
                UpdateMoverProxies(i);
            }
        }

        void PhysicsWorld::MovePosition(BodyHandle h, Vec2 p)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            // Step-managed-prev move: write pos + the mover-broadphase AABB but
            // leave prev untouched (Step owns prev). Ports the Lua slideMove
            // write-back: w.posX[i] = x; w.posY[i] = y; moverHash:update(...).
            m_posX[i] = p.x;
            m_posY[i] = p.y;
            if (static_cast<BodyType>(m_btype[i]) != BodyType::Static)
            {
                UpdateMoverProxies(i);
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
            // setVelocity). Setting a Dynamic body's velocity WAKES it (line
            // 102) so a sleeping body re-enters integration next Step.
            if (bt == BodyType::Kinematic || bt == BodyType::Dynamic)
            {
                m_velX[i] = v.x;
                m_velY[i] = v.y;
                if (bt == BodyType::Dynamic)
                {
                    m_awake[i]      = 1;
                    m_sleepTimer[i] = Real(0);
                }
            }
        }

        void PhysicsWorld::ApplyImpulse(BodyHandle h, Vec2 impulse)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            // Dynamic only; wakes (ports applyImpulse, the no-point branch).
            if (static_cast<BodyType>(m_btype[i]) != BodyType::Dynamic)
            {
                return;
            }
            m_awake[i]      = 1;
            m_sleepTimer[i] = Real(0);
            m_velX[i] += impulse.x * m_invMass[i];
            m_velY[i] += impulse.y * m_invMass[i];
        }

        void PhysicsWorld::ApplyImpulse(BodyHandle h, Vec2 impulse, Vec2 worldPoint)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            // Dynamic only; wakes (ports applyImpulse, the px,py branch). Linear
            // part as above, plus an angular term from the lever arm about the
            // body's CENTER OF MASS: angVel += cross(p - com, i) * invInertia.
            // The lever MUST be measured from the COM, not the origin: invInertia
            // is the inertia about the COM and the solver integrates rotation
            // about the COM (FinalizePositions). For a single-fixture / COM==origin
            // body WorldCom() == origin, so this is byte-identical there; for an
            // off-COM compound body the origin lever was wrong (it dropped the
            // origin->COM offset, under-rotating or mis-signing the spin).
            if (static_cast<BodyType>(m_btype[i]) != BodyType::Dynamic)
            {
                return;
            }
            m_awake[i]      = 1;
            m_sleepTimer[i] = Real(0);
            m_velX[i] += impulse.x * m_invMass[i];
            m_velY[i] += impulse.y * m_invMass[i];
            const Vec2 com = WorldCom(Vec2(m_posX[i], m_posY[i]), m_angle[i],
                                      Vec2(m_localCenterX[i], m_localCenterY[i]));
            const Real rx = worldPoint.x - com.x;
            const Real ry = worldPoint.y - com.y;
            m_angVel[i] += (rx * impulse.y - ry * impulse.x) * m_invInertia[i];
        }

        void PhysicsWorld::Wake(BodyHandle h)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            // Only Dynamic bodies have a sleep state to clear (ports Body:wake).
            if (static_cast<BodyType>(m_btype[i]) == BodyType::Dynamic)
            {
                m_awake[i]      = 1;
                m_sleepTimer[i] = Real(0);
            }
        }

        bool PhysicsWorld::IsAwake(BodyHandle h) const noexcept
        {
            if (!IsValid(h))
            {
                return false;
            }
            // Static/Kinematic never sleep -> always report awake (their m_awake
            // stays 1). Ports Body:isAwake.
            return m_awake[h.index] != 0;
        }

        Real PhysicsWorld::GetAngle(BodyHandle h) const noexcept
        {
            if (!IsValid(h))
            {
                return Real(0);
            }
            return m_angle[h.index];
        }

        void PhysicsWorld::SetAngle(BodyHandle h, Real angle)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            m_angle[i] = angle;
            // A non-circle body's world AABB is rotation-aware. Statics never
            // re-register via Step, so refresh the static grid; movers refresh
            // their per-fixture proxies + residency immediately (mirrors SetPosition).
            if (static_cast<BodyType>(m_btype[i]) == BodyType::Static)
                m_staticGrid.Move(i, SlotAabb(i));
            else
                UpdateMoverProxies(i);
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
            // STEP ORDER (P2.2 -- Box2D v3 TGS Soft restructure of the P1.8/P2.1
            // pipeline). The P2.1 INLINE dynamic gravity-integrate (old stage 1)
            // and dynamic position-integrate (old stage 4) are SUPERSEDED: the
            // Soft Step solver now OWNS dynamic velocity + position integration,
            // folded INTO its sub-step loop (the proven-stable v3 form). Dynamics
            // are therefore integrated EXACTLY ONCE per Step -- by the solver --
            // with no double-integration.
            //
            //   stage 1: prev snapshot (ALL) + KINEMATIC integrate (once/step) +
            //            kinematic mover-broadphase update.
            //   stage 2: solver contact generation (Part A) -- manifolds for
            //            awake-dynamic pairs (vs tile spans + static bodies via
            //            StaticCandidates; vs mover-mover pairs involving a
            //            dynamic, narrowed). Builds m_contactConstraints.
            //   stage 3: SOFT STEP SOLVE (Part B) -- the solver runs Prepare ->
            //            [per sub-step: integrate vel, warm-start, solve(bias),
            //            integrate pos, relax] -> restitution -> store impulses
            //            -> commit dynamic positions + mover-broadphase update.
            //   stage 4: bullet GJK-TOI clamp (P3.1 CCD) -- sweep each isBullet
            //            body prev->curr vs statics; clamp to time-of-impact so a
            //            thin static wall cannot be tunneled. (The speculative
            //            margin in stage 2 is the inline CCD for fast dynamics;
            //            this is the discrete backup, primarily for kinematics.)
            //            CCD runs after the solver commits positions, before
            //            island/events.
            //   stage 5: island sleep bookkeeping            (P2.4 -- deferred)
            //   stage 6: contacts:step (events + gating + deferred flush)
            //
            // Free-fall parity: with NO contacts the solver's sub-step loop is a
            // pure semi-implicit integrate (gravity per sub-step, position per
            // sub-step). Summed over substepCount sub-steps of length h = dt/N
            // this equals the P2.1 single-step semi-implicit Euler to f32
            // tolerance (the PhysicsDynamics free-fall test's margins absorb the
            // sub-step regrouping). Index-ordered, no wall-clock, no fast-math.

            // ---- stage 1: prev snapshot (all) + kinematic integrate ----------
            for (std::uint32_t i = 0; i < m_count; ++i)
            {
                if (m_alive[i] == 0)
                {
                    continue;
                }
                m_prevX[i] = m_posX[i];
                m_prevY[i] = m_posY[i];

                // Kinematic bodies integrate ONCE per Step (unchanged from P2.1).
                // Dynamic gravity/damping + position now live in the solver's
                // sub-step loop (stage 3); a dynamic body with no contacts still
                // gets the same semi-implicit fall, just regrouped over sub-steps.
                if (static_cast<BodyType>(m_btype[i]) == BodyType::Kinematic)
                {
                    m_posX[i] += m_velX[i] * dt;
                    m_posY[i] += m_velY[i] * dt;
                    // UpdateMoverProxies refreshes all per-fixture mover-broadphase
                    // proxies and the body's residency grid (Phase 2, Task 3).
                    UpdateMoverProxies(i);
                }
            }

            // ---- stage 2: solver contact generation (Part A) -----------------
            GenerateContacts(dt);

            // ---- stage 3: Soft Step solve (Part B) ---------------------------
            // The solver integrates dynamic velocity + position across sub-steps,
            // solves soft contacts, applies restitution, and commits positions +
            // the mover broadphase. Run it whenever there are dynamics to move
            // (constraints OR free-falling bodies); the solver is a no-op for a
            // scene with no awake dynamics.
            // Rebuild the pooled JointConstraint array (Joint* views into the
            // world-owned m_joints) so the SolverContext keeps its P2.1 shape.
            // clear() preserves capacity -> zero steady-state alloc.
            m_jointConstraints.clear();
            for (std::size_t i = 0; i < m_joints.size(); ++i)
            {
                JointConstraint jc;
                jc.joint = m_joints[i].get();
                m_jointConstraints.push_back(jc);
            }

            {
                SolverContext ctx;
                ctx.world        = this;
                ctx.contacts     = m_contactConstraints.empty()
                                       ? nullptr
                                       : m_contactConstraints.data();
                ctx.contactCount = static_cast<std::uint32_t>(m_contactConstraints.size());
                ctx.joints       = m_jointConstraints.empty()
                                       ? nullptr
                                       : m_jointConstraints.data();
                ctx.jointCount   = static_cast<std::uint32_t>(m_jointConstraints.size());
                ctx.dt           = dt;
                ctx.substepCount = m_substepCount;
                ctx.invDt        = dt > Real(0) ? Real(1) / dt : Real(0);
                ctx.subDt        = dt / static_cast<Real>(m_substepCount);
                ctx.invSubDt     = ctx.subDt > Real(0) ? Real(1) / ctx.subDt : Real(0);
                ctx.gravity      = Vec2(m_gravityX, m_gravityY);
                m_solver->Solve(ctx);
            }

            // ---- stage 4: bullet GJK-TOI clamp (P3.1 CCD) --------------------
            // The discrete CCD backup for `isBullet` bodies vs statics. Runs
            // AFTER the solver commits dynamic positions (so a dynamic bullet's
            // post-solve sweep is correct) and BEFORE island sleep + events (so
            // they observe the clamped position). Kinematic bullets -- which the
            // solver never touches -- are clamped from their stage-1-integrated
            // position; the speculative margin (stage 2) handles fast dynamics
            // inline, with this as the safety net. CCD runs after the solver
            // commits positions, before island/events.
            BulletSweep();

            // ---- stage 5: island sleep bookkeeping (P2.4) --------------------
            // Build the per-Step constraint graph (bodies = nodes, THIS step's
            // contacts = edges), advance per-body sleep timers, and sleep any
            // island whose every member is idle past the threshold. Runs AFTER
            // the solve using this step's contacts; sleeping bodies are then
            // skipped by the next Step's GenerateContacts + solver (the awake
            // gate), freezing their positions. Ports PhysicsWorld.lua:403-452.
            Island::UpdateSleep(
                *this,
                m_contactConstraints.empty() ? nullptr : m_contactConstraints.data(),
                static_cast<std::uint32_t>(m_contactConstraints.size()),
                m_jointConstraints.empty() ? nullptr : m_jointConstraints.data(),
                static_cast<std::uint32_t>(m_jointConstraints.size()),
                dt);

            // ---- stage 6: events + gating + deferred flush -------------------
            m_contacts.Step(*this);
        }

        void PhysicsWorld::GenerateContacts(Real dt)
        {
            // Part A: build the dynamics ContactConstraint array (the Lua step()
            // stage 2 "solver contact generation"). For each awake non-sensor
            // DYNAMIC body, generate manifolds vs static candidates (tile spans +
            // static bodies) and vs mover-mover pairs involving a dynamic. A is
            // ALWAYS the dynamic body (Lua lines 377-378); B may be dynamic,
            // kinematic, static, or a tile-span virtual fixture (invMass = 0).
            //
            // T5 (FIXTURE-PAIR ROTATION-AWARE CONTACT GENERATION):
            //   For each candidate body-pair, iterate every fixture of bodyA against
            //   every fixture of bodyB (or the span virtual fixture), and call
            //   Collide(shapeA, worldXfA, shapeB, worldXfB, specMargin) with the
            //   COMPOSED real angle (bodyAngle + fixtureLocalAngle). This is the
            //   sole narrowphase path; the legacy CollideShapes/Dispatch path has
            //   been REMOVED from this function.
            //
            //   Material coefficients come from the two FIXTURES:
            //     friction    = sqrt(fxA.friction * fxB.friction)
            //     restitution = max(fxA.restitution, fxB.restitution)
            //   For tile-span virtual fixtures (no fixture slot) we use the body's
            //   own friction/restitution as both sides (unchanged from M6).
            //
            //   The ContactConstraint still references the two BODY slots (solver
            //   unchanged). Anchor bases are each body's WORLD COM (compound-COM
            //   dynamics: bodies rotate about their COM -- see the `emit` lambda's
            //   WorldCom() anchors below + SoftStep/Baumgarte FinalizePositions).
            //   Byte-identical to origin-relative anchors for localCenter==0.
            //
            //   BROADPHASE: per-fixture (m_fixtureBroadphase; one proxy per live
            //   fixture of a Dynamic/Kinematic body). The mover-mover section below
            //   walks the fixture-pair set directly; no separate body-level cull needed.
            //
            //   fixedRotation bodies keep invInertia=0 (already enforced by AddBody
            //   / RecomputeBodyMass; we do not change integration/solver here).
            //
            // Index-ordered over dynamic bodies, then the broadphase's SORTED
            // mover pairs -> deterministic. The pool only grows (clear preserves
            // capacity) -> zero steady-state allocation.
            m_contactConstraints.clear();

            // ---- per-fixture world-AABB cache (cheap pre-narrowphase reject) --
            //
            // Rebuild m_genFxAabb from the CURRENT (pre-solve) transforms: one
            // tight world AABB per live fixture. The fixture-pair loops below
            // reject provably-disjoint pairs against this cache before paying the
            // full Collide(), so a multi-fixture body (whisk/compound) no longer
            // drives GJK/SAT on every fixture pair. O(total fixtures) once vs.
            // O(pairs x fixtures^2) Collide calls. See the member's doc comment.
            if (m_genFxAabb.size() < m_fxShape.size())
            {
                m_genFxAabb.resize(m_fxShape.size());
            }
            for (std::uint32_t fi = 0; fi < m_fxCount; ++fi)
            {
                if (m_fxGen[fi] == 0u)
                {
                    continue;
                }
                const std::uint32_t bs = m_fxBody[fi];
                if (bs >= m_count || m_alive[bs] == 0)
                {
                    continue;
                }
                const Transform xf = ComposeFixtureXf(
                    Vec2(m_posX[bs], m_posY[bs]), m_angle[bs],
                    Vec2(m_fxLocalPosX[fi], m_fxLocalPosY[fi]), m_fxLocalAngle[fi]);
                m_genFxAabb[fi] = m_fxShape[fi].ComputeAABB(xf);
            }
            // Per-body union AABB (byte-identical to SlotAabb, computed once here
            // instead of re-derived per broadphase pair below).
            if (m_genBodyAabb.size() < m_count)
            {
                m_genBodyAabb.resize(m_count);
            }
            for (std::uint32_t bi = 0; bi < m_count; ++bi)
            {
                if (m_alive[bi] != 0)
                {
                    m_genBodyAabb[bi] = SlotAabb(bi);
                }
            }
            // True iff fixtures fa,fb are separated by more than `margin` on some
            // axis (so Collide(specMargin<=margin) is guaranteed to find no point).
            auto fxDisjoint = [&](std::uint32_t fa, std::uint32_t fb, Real margin)
            {
                const Aabb2& A = m_genFxAabb[fa];
                const Aabb2& B = m_genFxAabb[fb];
                return (A.max.x + margin < B.min.x) || (B.max.x + margin < A.min.x)
                    || (A.max.y + margin < B.min.y) || (B.max.y + margin < A.min.y);
            };

            // ---- P3.1 speculative-contact CCD (the PRIMARY fast-mover CCD) ---
            //
            // MODERNIZE: the speculative margin (the kSkin skin from P1.2) is
            // VELOCITY-SCALED for fast movers so the solver sees an impending wall
            // BEFORE the body reaches it. The margin is the distance the body
            // would travel this Step (|v| * dt), floored at kSkin: a SLOW body's
            // margin is just kSkin (resting/settling behavior UNCHANGED), while a
            // FAST body's margin grows to cover its full sweep so a contact is
            // generated while it is still on the near side of the wall. The Soft
            // Step solver's speculative bias (the s > 0 case: bias = s * invSubDt,
            // which caps the closing velocity to exactly close the gap in ONE
            // sub-step) then prevents the body from advancing more than the gap
            // per sub-step -- stopping tunneling WITHOUT a discrete clamp. This is
            // the modern speculative-contact CCD; the bullet GJK-TOI clamp
            // (Step stage 6) is the discrete backup for flagged bodies vs statics.
            //
            // Per-body margin: max(kSkin, |v| * dt). The query AABB pad and the
            // Collide speculativeMargin both use it (the AABB must be expanded by
            // AT LEAST the margin so the wall candidate is FOUND before geometric
            // overlap, and Collide must report the near-touching contact within
            // that distance).
            const Real moveDt = dt > Real(0) ? dt : Real(0);
            // Threshold for the sqrt skip (Fix 3): if speedSq <= threshSq then
            // speed * moveDt <= kSkin, so margin == kSkin (the floor) regardless.
            // Hoisted once out of both loops; guard moveDt==0 -> threshold 0 so
            // the sqrt is always taken (but moveDt==0 means dt==0 -> no motion).
            const Real threshSq = (moveDt > Real(0))
                                      ? (kSkin / moveDt) * (kSkin / moveDt)
                                      : Real(0);

            // ---- helper: world transform for a body's fixture -----------------
            // Composes (bodyPos, bodyAngle) + (fxLocalPos, fxLocalAngle) into the
            // fixture's world Transform. The body angle is live (m_angle[bodySlot]).
            // Delegates to ComposeFixtureXf (the single copy of the rotate+offset
            // formula) so the math is not duplicated here or in SlotAabb.
            auto FixtureWorldXf = [&](std::uint32_t bodySlot,
                                       std::uint32_t fi) -> Transform
            {
                return ComposeFixtureXf(
                    Vec2(m_posX[bodySlot], m_posY[bodySlot]),
                    m_angle[bodySlot],
                    Vec2(m_fxLocalPosX[fi], m_fxLocalPosY[fi]),
                    m_fxLocalAngle[fi]);
            };

            // ---- helper: append a manifold as a ContactConstraint -----------
            // A is dynamic (aIdx); bIdx is the slot for a real body or
            // kInvalidSlot for a span. invMassB/invInertiaB come from the slot
            // (0 for static/kinematic/span -> push, not pushed).
            // centerB: for a real body this is the body's world position; for a
            // tile-span virtual fixture it is the span's geometric center
            // (span.min+span.max)*0.5. anchorB = mp.point - centerB in both cases.
            // (Passing it explicitly avoids the old Vec2(0,0) fallback that made
            // anchorB wrong-looking for spans -- currently harmless because
            // invInertiaB==0 zeros the lever arm, but would break if spans ever
            // gained DOF.)
            //
            // T5: fricA/fricB and restA/restB now come from the two FIXTURE slots
            // (passed explicitly). For tile-span virtual fixtures the caller passes
            // the body-level friction/restitution as both sides (unchanged from M6).
            auto emit = [&](std::uint32_t aIdx, std::uint32_t bIdx, bool bIsBody,
                            const Vec2& centerB, const Manifold& m,
                            Real fricA, Real fricB, Real restA, Real restB,
                            std::uint32_t fixA, std::uint32_t fixB)
            {
                if (m.pointCount <= 0)
                {
                    return;
                }
                ContactConstraint cc;
                cc.bodyA       = aIdx;
                cc.bodyB       = bIsBody ? bIdx : kInvalidSlot;
                cc.bodyBIsBody = bIsBody;
                cc.invMassA    = m_invMass[aIdx];
                cc.invInertiaA = m_invInertia[aIdx];
                cc.invMassB    = bIsBody ? m_invMass[bIdx] : Real(0);
                cc.invInertiaB = bIsBody ? m_invInertia[bIdx] : Real(0);
                cc.normal      = m.normal;
                // DISPLAY-ONLY narrowphase tag (debug-viz Slice A): carried from
                // the producing manifold. The solver never reads it.
                cc.kind        = m.kind;

                // Combined material coefficients (T5: from the two fixture slots).
                // Friction: geometric mean sqrt(fA*fB). Restitution: max(rA, rB).
                cc.friction    = std::sqrt(fricA * fricB);
                cc.restitution = std::max(restA, restB);

                // Compound-COM dynamics: contact anchors are measured from each
                // body's world CENTER OF MASS, not its origin, so the solver's
                // lever arms (rA x n etc.) rotate the body about its COM. For a
                // body with localCenter == (0,0) -- every single-fixture body
                // and all static/kinematic bodies -- WorldCom() == origin, so
                // this is byte-identical to the old origin-based anchors.
                //   A is always a real dynamic body -> use its world COM.
                //   B real-body  -> use its world COM (recomputed from bIdx;
                //                    the passed centerB was the body origin).
                //   B tile-span  -> no COM/inertia; keep the span's geometric
                //                    center (centerB) UNCHANGED (invInertiaB==0
                //                    makes its lever arm irrelevant).
                const Vec2 cA = WorldCom(Vec2(m_posX[aIdx], m_posY[aIdx]),
                                         m_angle[aIdx],
                                         Vec2(m_localCenterX[aIdx],
                                              m_localCenterY[aIdx]));
                const Vec2 comB = bIsBody
                    ? WorldCom(Vec2(m_posX[bIdx], m_posY[bIdx]),
                               m_angle[bIdx],
                               Vec2(m_localCenterX[bIdx], m_localCenterY[bIdx]))
                    : centerB;

                cc.pointCount = m.pointCount;
                for (int p = 0; p < m.pointCount; ++p)
                {
                    const ManifoldPoint& mp = m.points[p];
                    ContactConstraintPoint& cp = cc.points[p];
                    cp.anchorA = mp.point - cA;
                    cp.anchorB = mp.point - comB;
                    // Manifold separation is POSITIVE for penetration; Box2D's
                    // signed separation is negative for penetration -> negate.
                    cp.baseSeparation = -mp.separation;
                    // Warm-start key: the geometric feature id mixed with the
                    // fixture pair so two fixture pairs on the same body pair do
                    // not alias (see MixContactId).
                    cp.id = MixContactId(mp.id, fixA, fixB);
                }
                m_contactConstraints.push_back(cc);
            };

            // ---- per dynamic body: vs spans + static bodies -----------------
            for (std::uint32_t i = 0; i < m_count; ++i)
            {
                if (m_alive[i] == 0 ||
                    static_cast<BodyType>(m_btype[i]) != BodyType::Dynamic ||
                    m_awake[i] == 0 || m_sensor[i] != 0)
                {
                    continue;
                }

                // Per-body speculative margin (P3.1): max(kSkin, |v| * dt). For a
                // slow body this is kSkin (resting unchanged); for a fast body it
                // grows to cover this Step's sweep so the wall is seen pre-overlap.
                // Exact sqrt skip (Fix 3): if speedSq <= threshSq the margin is
                // just kSkin, so a slow/resting body never pays the sqrt.
                const Real speedSqA = m_velX[i] * m_velX[i] + m_velY[i] * m_velY[i];
                const Real specMargin = (speedSqA > threshSq)
                                            ? std::sqrt(speedSqA) * moveDt
                                            : kSkin;

                const Aabb box = m_genBodyAabb[i]; // cached SlotAabb(i)
                // Query pad: at least the legacy +/-2 broadphase skin, expanded to
                // the speculative margin so a fast mover's wall candidate is FOUND
                // before geometric overlap (otherwise Collide never sees it).
                const Real pad = std::max(Real(2), specMargin);
                Aabb2 query;
                query.min = Vec2(box.min.x - pad, box.min.y - pad);
                query.max = Vec2(box.max.x + pad, box.max.y + pad);
                StaticCandidates(query, m_genSpans, m_genStatics);

                // Collect body A's fixtures (sorted by slot index for determinism).
                const std::vector<std::uint32_t>* fxListA = nullptr;
                if (i < m_bodyFixtures.size() && !m_bodyFixtures[i].empty())
                {
                    fxListA = &m_bodyFixtures[i];
                }

                // tile spans (Aabb2 rects) -> collide each fixture of A vs span-AABB.
                for (std::size_t s = 0; s < m_genSpans.size(); ++s)
                {
                    const Aabb2& span = m_genSpans[s];
                    const Vec2 spanCenter = (span.min + span.max) * Real(0.5);
                    const Vec2 he = (span.max - span.min) * Real(0.5);
                    const Shape spanShape = MakeAabb(he.x, he.y);
                    const Transform xfB{ spanCenter, Real(0) };

                    if (fxListA != nullptr)
                    {
                        // T5: iterate fixtures of body A vs the span (fixture-pair).
                        for (const std::uint32_t fi : *fxListA)
                        {
                            if (fi >= m_fxCount || m_fxGen[fi] == 0u)
                            {
                                continue;
                            }
                            if (m_fxSensor[fi] != 0u)
                            {
                                continue; // sensor fixture: no constraint
                            }
                            const Transform xfA = FixtureWorldXf(i, fi);
                            const Manifold mfld = Collide(m_fxShape[fi], xfA,
                                                           spanShape, xfB, specMargin);
                            // Span has no fixture slot; use body A's fixture material
                            // vs body A itself (tile spans don't carry material).
                            emit(i, kInvalidSlot, /*bIsBody=*/false, spanCenter, mfld,
                                 m_fxFriction[fi], m_fxFriction[fi],
                                 m_fxRestitution[fi], Real(0),
                                 /*fixA=*/fi, /*fixB=*/kInvalidSlot);
                        }
                    }
                    else
                    {
                        // Legacy fallback: single-shape body A vs span. Uses Collide
                        // with the body's real angle (T5 rotation fix even on fallback).
                        const Transform xfA{ Vec2(m_posX[i], m_posY[i]), m_angle[i] };
                        const Manifold mfld = Collide(m_shape[i], xfA,
                                                       spanShape, xfB, specMargin);
                        emit(i, kInvalidSlot, /*bIsBody=*/false, spanCenter, mfld,
                             m_fric[i], m_fric[i], m_rest[i], Real(0),
                             /*fixA=*/kInvalidSlot, /*fixB=*/kInvalidSlot);
                    }
                }

                // static bodies (non-sensor).
                for (std::size_t s = 0; s < m_genStatics.size(); ++s)
                {
                    const std::uint32_t idx = m_genStatics[s];
                    if (m_sensor[idx] != 0)
                    {
                        continue;
                    }
                    const Vec2 centerB(m_posX[idx], m_posY[idx]);

                    // Collect body B's fixtures.
                    const std::vector<std::uint32_t>* fxListB = nullptr;
                    if (idx < m_bodyFixtures.size() && !m_bodyFixtures[idx].empty())
                    {
                        fxListB = &m_bodyFixtures[idx];
                    }

                    if (fxListA != nullptr && fxListB != nullptr)
                    {
                        // T5: iterate (fixtureA x fixtureB) pairs.
                        // Stable order: fxListA is iterated outer, fxListB inner
                        // (both are the m_bodyFixtures[] order -- consistent with
                        // AddFixture insertion order, deterministic).
                        for (const std::uint32_t fiA : *fxListA)
                        {
                            if (fiA >= m_fxCount || m_fxGen[fiA] == 0u)
                            {
                                continue;
                            }
                            if (m_fxSensor[fiA] != 0u)
                            {
                                continue;
                            }
                            const Transform xfA = FixtureWorldXf(i, fiA);

                            for (const std::uint32_t fiB : *fxListB)
                            {
                                if (fiB >= m_fxCount || m_fxGen[fiB] == 0u)
                                {
                                    continue;
                                }
                                if (m_fxSensor[fiB] != 0u)
                                {
                                    continue;
                                }
                                if (fxDisjoint(fiA, fiB, specMargin))
                                {
                                    continue; // fixture AABBs separated -> no contact
                                }
                                const Transform xfB = FixtureWorldXf(idx, fiB);
                                const Manifold mfld = Collide(m_fxShape[fiA], xfA,
                                                               m_fxShape[fiB], xfB,
                                                               specMargin);
                                emit(i, idx, /*bIsBody=*/true, centerB, mfld,
                                     m_fxFriction[fiA], m_fxFriction[fiB],
                                     m_fxRestitution[fiA], m_fxRestitution[fiB],
                                     /*fixA=*/fiA, /*fixB=*/fiB);
                            }
                        }
                    }
                    else if (fxListA != nullptr)
                    {
                        // Body A has fixtures; body B uses legacy single-shape.
                        const Transform xfB{ centerB, m_angle[idx] };
                        for (const std::uint32_t fiA : *fxListA)
                        {
                            if (fiA >= m_fxCount || m_fxGen[fiA] == 0u)
                            {
                                continue;
                            }
                            if (m_fxSensor[fiA] != 0u)
                            {
                                continue;
                            }
                            const Transform xfA = FixtureWorldXf(i, fiA);
                            const Manifold mfld = Collide(m_fxShape[fiA], xfA,
                                                           m_shape[idx], xfB,
                                                           specMargin);
                            emit(i, idx, /*bIsBody=*/true, centerB, mfld,
                                 m_fxFriction[fiA], m_fric[idx],
                                 m_fxRestitution[fiA], m_rest[idx],
                                 /*fixA=*/fiA, /*fixB=*/kInvalidSlot);
                        }
                    }
                    else
                    {
                        // Legacy fallback: single-shape A vs single-shape (or fixture) B.
                        // Use real angle for both (T5 rotation fix).
                        const Transform xfA{ Vec2(m_posX[i], m_posY[i]), m_angle[i] };
                        const Transform xfB{ centerB, m_angle[idx] };
                        const Manifold mfld = Collide(m_shape[i], xfA,
                                                       m_shape[idx], xfB,
                                                       specMargin);
                        emit(i, idx, /*bIsBody=*/true, centerB, mfld,
                             m_fric[i], m_fric[idx], m_rest[i], m_rest[idx],
                             /*fixA=*/kInvalidSlot, /*fixB=*/kInvalidSlot);
                    }
                }
            }

            // ---- mover-mover pairs involving a dynamic (Phase 2 Task 2) ------
            // The per-fixture broadphase emits SORTED fixture-pairs (fa < fb).
            // Each pair carries exactly one fixture from each body; the broadphase
            // already culled provably-disjoint fixture AABBs, so we skip the
            // body-level AABB cull and per-pair nested fixture loop.
            //
            // ORIENTATION RULE (warm-start stability):
            //   A must be dynamic; if both dynamic the LOWER BODY SLOT is A.
            //   Bodies AND fixtures are swapped together so MixContactId receives
            //   (fixA, fixB) in a stable, deterministic order across steps.
            // Incremental pair set (move buffer): drains proxy moves since the
            // last UpdatePairs call (stage-1 kinematic integrate + CREATE pass)
            // and returns the current set. == Pairs() but O(moved log n) when
            // most bodies rest (oracle-proven in the [movebuffer] suite).
            m_fixtureBroadphase->UpdatePairs(m_genPairs);
            for (std::size_t k = 0; k < m_genPairs.size(); ++k)
            {
                // fa,fb are FIXTURE slots (broadphase-sorted fa < fb).
                const std::uint32_t fa0 = m_genPairs[k].a;
                const std::uint32_t fb0 = m_genPairs[k].b;

                // Derive owning body slots.
                std::uint32_t a = m_fxBody[fa0];
                std::uint32_t b = m_fxBody[fb0];

                // Same body -> two fixtures of one body never collide.
                if (a == b)
                {
                    continue;
                }
                if (m_alive[a] == 0 || m_alive[b] == 0)
                {
                    continue;
                }
                // Body-level sensor: skip if either BODY is a sensor body
                // (preserves the old body-pair `m_sensor[a] || m_sensor[b]` gate).
                if (m_sensor[a] != 0 || m_sensor[b] != 0)
                {
                    continue;
                }

                const bool da = static_cast<BodyType>(m_btype[a]) == BodyType::Dynamic;
                const bool db = static_cast<BodyType>(m_btype[b]) == BodyType::Dynamic;
                if (!da && !db)
                {
                    continue; // kinematic-kinematic: no dynamic response
                }

                // Per-pair speculative margin (P3.1): max(kSkin, sqrt(max
                // speedSq) * dt). Same formula as the old body-pair loop.
                const Real speedSqAm = m_velX[a] * m_velX[a] + m_velY[a] * m_velY[a];
                const Real speedSqBm = m_velX[b] * m_velX[b] + m_velY[b] * m_velY[b];
                const Real maxSpeedSq = std::max(speedSqAm, speedSqBm);
                const Real pairMargin = (maxSpeedSq > threshSq)
                                            ? std::sqrt(maxSpeedSq) * moveDt
                                            : kSkin;

                // Wake a sleeping dynamic touched by an awake mover (preserves
                // the exact Lua wake rules, PhysicsWorld.lua:369-382). Uses the
                // pre-orientation a,b so the logic is identical to the old loop.
                if (da && m_awake[a] == 0 && (!db || m_awake[b] != 0))
                {
                    m_awake[a] = 1;
                    m_sleepTimer[a] = Real(0);
                }
                if (db && m_awake[b] == 0 && (!da || m_awake[a] != 0))
                {
                    m_awake[b] = 1;
                    m_sleepTimer[b] = Real(0);
                }

                // ORIENT: A must be dynamic. If both dynamic, lower BODY SLOT is A
                // (deterministic for both warm-start and symmetric contacts).
                // CRITICAL: swap BODY index and its FIXTURE index together so
                // emit(ia, ib, ..., fa, fb) always gets the A-side fixture as fixA.
                std::uint32_t ia = a,   ib = b;
                std::uint32_t fa = fa0, fb = fb0;
                if (!da || (da && db && ib < ia))
                {
                    std::swap(ia, ib);
                    std::swap(fa, fb);
                }
                if (m_awake[ia] == 0)
                {
                    continue; // A (dynamic) asleep -> no constraint
                }

                // Fixture-level sensor: skip constraint (but keep event path if
                // needed). The per-fixture broadphase only tracks non-sensor bodies,
                // but defensive check matches old nested-loop behavior.
                if (m_fxSensor[fa] != 0u || m_fxSensor[fb] != 0u)
                {
                    continue;
                }

                // No body-level AABB cull and no per-fixture fxDisjoint reject here:
                // the per-fixture broadphase already returns only overlapping fixture
                // pairs (tight current-frame shape AABBs). The speculative margin is
                // applied inside Collide (its speculativeMargin arg), NOT as a broadphase
                // expansion -- so a very fast pair not yet in tight-AABB overlap can miss
                // a speculative contact. This is a known limitation shared with the old
                // body-pair path; discrete CCD for flagged bodies is BulletSweep's job.

                // TODO(filter): category/mask collision filtering (m_fxFilterCat/Mask,
                // documented in Fixture.hpp) is NOT enforced here yet -- pre-existing gap
                // (the old body-pair path didn't enforce it either). A pair set with
                // non-default category/mask currently still collides. Enforce per
                // fixture-pair here when filtering is wired up.

                const Vec2    centerB(m_posX[ib], m_posY[ib]);
                const Transform xfA = FixtureWorldXf(ia, fa);
                const Transform xfB = FixtureWorldXf(ib, fb);
                const Manifold mfld = Collide(m_fxShape[fa], xfA,
                                               m_fxShape[fb], xfB,
                                               pairMargin);
                emit(ia, ib, /*bIsBody=*/true, centerB, mfld,
                     m_fxFriction[fa], m_fxFriction[fb],
                     m_fxRestitution[fa], m_fxRestitution[fb],
                     /*fixA=*/fa, /*fixB=*/fb);
            }
        }

        void PhysicsWorld::BulletSweep()
        {
            // P3.1 CCD bullet clamp (port of PhysicsWorld.lua:313-320). For each
            // alive `isBullet` body, cast the swept shape from its start-of-step
            // position (prev, snapshotted in Step stage 1) along this Step's net
            // displacement vs STATICS ONLY, and clamp the position to the time of
            // impact so a thin static wall cannot be tunneled.
            //
            // The Lua epsilon: clamp = max(0, hit.t - 0.001). Pulling the body a
            // hair short of the surface keeps it just OUTSIDE the wall (no
            // depenetration churn next Step).
            constexpr Real kBulletEpsilon = Real(0.001);

            for (std::uint32_t i = 0; i < m_count; ++i)
            {
                if (m_alive[i] == 0 || m_bullet[i] == 0)
                {
                    continue;
                }

                const Vec2 prev(m_prevX[i], m_prevY[i]);
                const Vec2 curr(m_posX[i], m_posY[i]);
                const Vec2 delta = curr - prev;
                // No net travel this Step -> nothing to clamp (ShapeCast also
                // guards a near-zero delta, but skip the candidate gather too).
                if (delta.x == Real(0) && delta.y == Real(0))
                {
                    continue;
                }

                // INVARIANT: bullets are always movers (Kinematic or Dynamic);
                // a Static body is never flagged isBullet so this branch is
                // unreachable for statics.
                assert(static_cast<BodyType>(m_btype[i]) != BodyType::Static &&
                       "bullets are never static");

                // STATICS ONLY: default ShapeCastOpts (movers = false) casts vs
                // tile spans + non-sensor static bodies. The bullet body is a
                // mover, so it is never a self-obstacle here (no exclude needed).
                //
                // T7 Part C (ROTATION + FIXTURE AWARE CCD): sweep EACH of the
                // bullet's fixtures with the body's REAL angle and the fixture's
                // local transform, keeping the EARLIEST TOI across all fixtures.
                // The fixture's start-of-step world pos uses `prev` and the body's
                // current angle (consistent with the existing positional-sweep
                // approximation -- the conservative-advancement holds the angle
                // fixed during the sweep; we use m_angle[i] rather than a per-step
                // angle history, matching the translational CCD model). A bullet
                // with no fixtures falls back to the legacy single m_shape at the
                // body's real angle.
                const Real bodyAngle = m_angle[i];
                bool haveHit = false;
                Real bestT = Real(1);

                const std::vector<std::uint32_t>* fxList = nullptr;
                if (i < m_bodyFixtures.size() && !m_bodyFixtures[i].empty())
                {
                    fxList = &m_bodyFixtures[i];
                }

                if (fxList != nullptr)
                {
                    for (const std::uint32_t fi : *fxList)
                    {
                        if (fi >= m_fxCount || m_fxGen[fi] == 0u)
                        {
                            continue;
                        }
                        // Fixture start-of-step world pos = prev + R(angle)*local;
                        // world angle = bodyAngle + fixtureLocalAngle.
                        const Transform fxStart = ComposeFixtureXf(
                            prev, bodyAngle,
                            Vec2(m_fxLocalPosX[fi], m_fxLocalPosY[fi]),
                            m_fxLocalAngle[fi]);
                        const std::optional<ShapeCastHit> fxHit =
                            ShapeCast(m_fxShape[fi], fxStart.position, delta,
                                      ShapeCastOpts{}, fxStart.rotation);
                        if (fxHit && fxHit->t < bestT)
                        {
                            bestT   = fxHit->t;
                            haveHit = true;
                        }
                    }
                }
                else
                {
                    // Legacy fallback: single m_shape at the body's real angle.
                    const std::optional<ShapeCastHit> hit =
                        ShapeCast(m_shape[i], prev, delta, ShapeCastOpts{}, bodyAngle);
                    if (hit && hit->t < bestT)
                    {
                        bestT   = hit->t;
                        haveHit = true;
                    }
                }

                if (!haveHit || bestT >= Real(1))
                {
                    continue; // clear sweep -> the integrated position stands
                }

                // Clamp to the EARLIEST TOI (a hair short, the Lua's hit.t-0.001).
                // The clamp is applied to the BODY position with the body's delta
                // (the fixture offsets ride along rigidly with the body).
                const Real clamp = std::max(Real(0), bestT - kBulletEpsilon);
                const Vec2 clamped = prev + delta * clamp;
                m_posX[i] = clamped.x;
                m_posY[i] = clamped.y;
                // Unconditional: the assert above guarantees this is always a
                // mover (never Static), so the broadphase update always applies.
                // UpdateMoverProxies keeps all per-fixture mover-broadphase proxies
                // and the body's residency grid in sync (Phase 2, Task 3).
                UpdateMoverProxies(i);
            }
        }

        bool PhysicsWorld::SlotsOverlap(std::uint32_t a, std::uint32_t b) const
        {
            // T7 Part A: rotation + fixture-aware overlap for the ContactManager
            // (events / re-arm). Iterate every fixture of body a against every
            // fixture of body b, compose each fixture's world Transform, and run
            // the unified rotation-aware Collide; true on the FIRST fixture-pair
            // with a contact point. Mirrors GenerateContacts' fixture-pair flow
            // but: (1) does NOT skip sensor fixtures (event gating must detect
            // sensor overlaps; sensor-ness is applied later in Emit), and (2)
            // uses speculativeMargin 0 (exact overlap only -- no speculative gap),
            // matching the old CollideShapes(..., 0) event-overlap semantics.
            //
            // A body with NO fixtures falls back to its legacy single shape at the
            // real body angle (mirrors the GenerateContacts single-shape fallback).
            //
            // The per-iteration `m_fxGen[fi] == 0u` guard below is DEFENSIVE ONLY:
            // DropFixture swap-pops dead slots out of m_bodyFixtures, so a
            // non-empty-but-all-dead fixture list is not normally reachable. The
            // legacy single-shape fallback here applies only to the genuinely
            // fixtureless case (fxA/fxB == nullptr). This intentionally diverges
            // from SlotAabb, which ALSO falls back to the single shape when a
            // body's fixture list is non-empty but every slot is dead -- harmless
            // because that state is unreachable in practice (documented so the
            // divergence reads as intentional, not an oversight).
            const Vec2 posA(m_posX[a], m_posY[a]);
            const Vec2 posB(m_posX[b], m_posY[b]);
            const Real angA = m_angle[a];
            const Real angB = m_angle[b];

            const std::vector<std::uint32_t>* fxA =
                (a < m_bodyFixtures.size() && !m_bodyFixtures[a].empty())
                    ? &m_bodyFixtures[a] : nullptr;
            const std::vector<std::uint32_t>* fxB =
                (b < m_bodyFixtures.size() && !m_bodyFixtures[b].empty())
                    ? &m_bodyFixtures[b] : nullptr;

            // Compose the world transform of a single fixture slot for body whose
            // pos/angle are known.
            auto fxXf = [&](Vec2 bodyPos, Real bodyAngle,
                            std::uint32_t fi) -> Transform
            {
                return ComposeFixtureXf(
                    bodyPos, bodyAngle,
                    Vec2(m_fxLocalPosX[fi], m_fxLocalPosY[fi]),
                    m_fxLocalAngle[fi]);
            };

            // Resolve each body's (shape, xf) test list into a small fixed-size
            // walk. Rather than build temporaries, branch on the four fallback
            // combinations (both-fixtured / a-only / b-only / neither).
            if (fxA != nullptr && fxB != nullptr)
            {
                for (const std::uint32_t fa : *fxA)
                {
                    if (fa >= m_fxCount || m_fxGen[fa] == 0u) { continue; }
                    const Transform xfA = fxXf(posA, angA, fa);
                    for (const std::uint32_t fb : *fxB)
                    {
                        if (fb >= m_fxCount || m_fxGen[fb] == 0u) { continue; }
                        const Transform xfB = fxXf(posB, angB, fb);
                        if (Collide(m_fxShape[fa], xfA,
                                    m_fxShape[fb], xfB, Real(0)).pointCount > 0)
                        {
                            return true;
                        }
                    }
                }
                return false;
            }
            if (fxA != nullptr)
            {
                const Transform xfB{ posB, angB };
                for (const std::uint32_t fa : *fxA)
                {
                    if (fa >= m_fxCount || m_fxGen[fa] == 0u) { continue; }
                    const Transform xfA = fxXf(posA, angA, fa);
                    if (Collide(m_fxShape[fa], xfA,
                                m_shape[b], xfB, Real(0)).pointCount > 0)
                    {
                        return true;
                    }
                }
                return false;
            }
            if (fxB != nullptr)
            {
                const Transform xfA{ posA, angA };
                for (const std::uint32_t fb : *fxB)
                {
                    if (fb >= m_fxCount || m_fxGen[fb] == 0u) { continue; }
                    const Transform xfB = fxXf(posB, angB, fb);
                    if (Collide(m_shape[a], xfA,
                                m_fxShape[fb], xfB, Real(0)).pointCount > 0)
                    {
                        return true;
                    }
                }
                return false;
            }
            // Neither body has fixtures: legacy single-shape vs single-shape at
            // the real angles.
            const Transform xfA{ posA, angA };
            const Transform xfB{ posB, angB };
            return Collide(m_shape[a], xfA, m_shape[b], xfB, Real(0)).pointCount > 0;
        }

        // ---- pull API for debug draw / inspection (P3.6) -------------------

        void PhysicsWorld::ForEachContact(
            const std::function<void(std::uint32_t, std::uint32_t)>& fn) const
        {
            m_contacts.ForEachBegunPair(fn);
        }

        std::uint32_t PhysicsWorld::IslandRootOf(std::uint32_t i) const noexcept
        {
            // m_uf[i] is the union-find parent for slot i from the last Step's
            // island pass (Island::UpdateSleep writes it via UnionFindScratch()).
            // If the island pass has not run or i is beyond the filled range,
            // return i itself (every un-unioned body is its own island root).
            //
            // IMPORTANT: Island::UpdateSleep uses PATH-HALVING when unioning, so
            // m_uf[i] is a halved parent, NOT necessarily the root. Two bodies in
            // the same island at depth > 1 would yield DIFFERENT m_uf[i] values
            // if we return the one-hop parent directly. Walk to the root
            // non-destructively (world is const here -- do NOT mutate m_uf).
            // Termination: the root satisfies m_uf[root] == root (self-pointing);
            // path-halving never reparents a root, so the walk always terminates.
            if (i >= m_uf.size())
                return i;
            std::uint32_t x = i;
            while (m_uf[x] != x)
                x = m_uf[x];
            return x;
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

        // NOTE: the spatial-query surface (Raycast / RayVsBody / LineOfSight /
        // ShapeCast / OverlapShape / StaticCandidates) lives in Queries.cpp
        // (same class, separate TU -- split out in P2.1 to keep this
        // orchestration TU readable once the dynamics stages landed). No API
        // change; PhysicsWorld.hpp still declares them.
        Body PhysicsWorld::GetBody(BodyHandle h) noexcept
        {
            return Body(this, h);
        }

    } // namespace Physics
} // namespace Arcane
