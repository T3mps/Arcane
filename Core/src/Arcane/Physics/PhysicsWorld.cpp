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

#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/Broadphase/DynamicTree.hpp>
#include <Arcane/Physics/Broadphase/SpatialHash.hpp>
#include <Arcane/Physics/Broadphase/SweepAndPrune.hpp>
#include <Arcane/Physics/Narrowphase/GeometryKernel.hpp> // AabbOverlap (QueryAABB)
#include <Arcane/Physics/Narrowphase/Collide.hpp>        // Collide (rotation-aware fixture-pair narrowphase: contacts T5, events/overlap T7)
#include <Arcane/Physics/Solver/SoftStep.hpp>            // SoftStep solver impl
#include <Arcane/Physics/Solver/Baumgarte.hpp>           // Baumgarte oracle impl (A/B)
#include <Arcane/Physics/Solver/ContactColoring.hpp>     // kColorCount (persistent incremental coloring, Phase C Task 4)
#include <Arcane/Physics/Island.hpp>                     // island sleep pass (stage 4)
#include <Arcane/Physics/Joints/Joints.hpp>              // joint set + MakeJoint factory (P2.5)
#include <Arcane/Physics/StepProf.hpp>                   // opt-in per-Step-phase timing (zero-cost when ARCANE_STEPPROF==0)

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
            , m_maxLinearVelocity(def.maxLinearVelocity)
            , m_sleepThresholdDefault(def.sleepThreshold)
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

            // Phase C, Task 4: one contact-id list per graph color. Sized once here
            // to kColorCount (overflow contacts are not listed -- they carry
            // color == kInvalidColor and never enter m_colorContacts). m_bodyColorMask
            // grows lazily with the body SoA in EnsureCapacity (default 0).
            m_colorContacts.resize(static_cast<std::size_t>(kColorCount));
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
            m_maxExtent.resize(next, Real(0));
            m_sleepThreshold.resize(next, m_sleepThresholdDefault);
            m_awake.resize(next, std::uint8_t(1));
            m_bullet.resize(next, std::uint8_t(0));
            // Phase A: new per-body island id column. A never-touched tail slot
            // has no island yet; it will be assigned (or left kInvalidIsland for
            // non-Dynamic) in AddBody.
            m_islandId.resize(next, Island::kInvalidIsland);
            // Phase B: awake-set back-index column. kNotAwake = not in the set.
            // A tail slot is never in the set until AddBody writes it.
            m_awakeIndex.resize(next, kNotAwake);
            // Phase C: kinematic-set back-index column. kNotKinematic = not in the
            // set. A tail slot is never in the set until AddBody writes it.
            m_kinematicIndex.resize(next, kNotKinematic);
            // Phase C, Task 4: per-body color-occupancy bitmask. A fresh/recycled
            // slot starts with NO colors occupied (the RemoveBody leak-detector
            // asserts a removed body left mask 0, so a recycled slot is always 0).
            m_bodyColorMask.resize(next, 0u);
            // SplitIsland scratch column. All-sentinel; SplitIsland writes only
            // its members then resets them, so the tail stays sentinel.
            m_splitLocalIndex.resize(next, kSplitLocalNone);
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
            m_bodyContacts.resize(next);
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
                m_maxExtent[bodySlot]    = Real(0);
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
                m_maxExtent[bodySlot]    = Real(0);
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

            // COM is now final -> refresh the cached sleep-test extent.
            RecomputeMaxExtent(bodySlot);
        }

        void PhysicsWorld::RecomputeMaxExtent(std::uint32_t bodySlot)
        {
            // maxExtent = max over fixtures of ( max over core verts of
            // |vert_bodyLocal - COM| + shape.radius ). Body-local frame; COM is
            // m_localCenter. This is Box2D's sim->maxExtent, used by the sleep
            // velocity test ( |v| + |w|*maxExtent ). 0 for a body with no fixtures.
            Real maxExt = Real(0);
            const Real comX = m_localCenterX[bodySlot];
            const Real comY = m_localCenterY[bodySlot];
            for (const std::uint32_t fi : m_bodyFixtures[bodySlot])
            {
                if (fi >= m_fxCount || m_fxGen[fi] == 0u)
                {
                    continue; // dead slot (defensive)
                }
                const Shape& s = m_fxShape[fi];
                const Real la  = m_fxLocalAngle[fi];
                const Real lc  = std::cos(la), ls = std::sin(la);
                const Real lpx = m_fxLocalPosX[fi], lpy = m_fxLocalPosY[fi];
                for (const Vec2& v : s.verts)
                {
                    // Core vert in body-local frame: localPos + R(localAngle)*v.
                    const Real px = lpx + lc * v.x - ls * v.y;
                    const Real py = lpy + ls * v.x + lc * v.y;
                    const Real dx = px - comX, dy = py - comY;
                    const Real d  = std::sqrt(dx * dx + dy * dy) + s.radius;
                    if (d > maxExt) { maxExt = d; }
                }
            }
            m_maxExtent[bodySlot] = maxExt;
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

            // Immediately destroy any persistent contacts on this fixture (Task 5)
            // so a recycled slot never leaves a stale contact for the single Step
            // before UpdateContacts' guard would reap it. The walk matches by slot
            // index, so it runs correctly after the generation bump above.
            DestroyContactsForFixture(fi);

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

        // --------------------------------------------------------------------
        // DebugCollide -- re-run the REAL narrowphase on two fixtures + record a
        // NarrowphaseTrace (debug-viz Slice B inspector seam, Task 3).
        //
        // PURE: reads the SoA + fixture arrays, writes only `out`. Composes each
        // fixture's world transform with the SAME ComposeFixtureXf the Step
        // path's GenerateContacts/FixtureWorldXf uses (so the reproduced
        // manifold matches the Step's manifold exactly), Clears the trace, then
        // calls the unified Collide with a non-null recorder. Hard-contact (no
        // speculative margin), matching how the inspector re-runs a settled
        // contact. Stale/invalid handles -> empty manifold + Cleared trace.
        // --------------------------------------------------------------------
        Manifold PhysicsWorld::DebugCollide(FixtureHandle a, FixtureHandle b,
                                            NarrowphaseTrace& out) const
        {
            out.Clear();

            if (!IsValid(a) || !IsValid(b))
            {
                return Manifold{};
            }

            const std::uint32_t fa = a.index;
            const std::uint32_t fb = b.index;
            const std::uint32_t ba = m_fxBody[fa];
            const std::uint32_t bb = m_fxBody[fb];

            // Compose each fixture's world transform exactly as the Step path
            // does (ComposeFixtureXf == the one copy of the rotate+offset form).
            const Transform xfA = ComposeFixtureXf(
                Vec2(m_posX[ba], m_posY[ba]), m_angle[ba],
                Vec2(m_fxLocalPosX[fa], m_fxLocalPosY[fa]), m_fxLocalAngle[fa]);
            const Transform xfB = ComposeFixtureXf(
                Vec2(m_posX[bb], m_posY[bb]), m_angle[bb],
                Vec2(m_fxLocalPosX[fb], m_fxLocalPosY[fb]), m_fxLocalAngle[fb]);

            // Re-run the real narrowphase with the recorder attached. The Step
            // path passes NO trace to this same Collide (so it stays
            // byte-identical); here the &out recorder captures the intermediate
            // geometry while the returned manifold reproduces the Step's.
            return Collide(m_fxShape[fa], xfA, m_fxShape[fb], xfB,
                           /*speculativeMargin*/ Real(0), &out);
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
            // Per-body sleep gate: explicit BodyDef override (>= 0) or inherit world.
            m_sleepThreshold[idx] = (def.sleepThreshold >= Real(0))
                                        ? def.sleepThreshold
                                        : m_sleepThresholdDefault;
            m_awake[idx]      = 1;
            m_bullet[idx]     = def.bullet ? std::uint8_t(1) : std::uint8_t(0);

            // ---- persistent island assignment (Phase A) ---------------------
            // A new DYNAMIC body is its own 1-body island; static/kinematic are
            // not island members (they anchor). The slot may be recycled, so
            // overwrite unconditionally.
            if (def.type == BodyType::Dynamic)
            {
                const std::uint32_t isl = AllocIsland();
                m_islands[isl].bodies.push_back(idx);
                m_islandId[idx] = isl;
                // Phase B: a recycled slot may carry a stale awakeIndex; clear it
                // BEFORE AddToAwakeSet so the idempotency guard does not wrongly
                // skip re-adding a slot that belonged to a prior body.
                m_awakeIndex[idx] = kNotAwake;
                AddToAwakeSet(idx); // new dynamic body starts awake (m_awake[idx]=1 above)
            }
            else
            {
                m_islandId[idx] = Island::kInvalidIsland;
                // Phase B: static/kinematic never enter the awake-set; clear any
                // stale index left by a recycled slot's prior dynamic lifetime.
                m_awakeIndex[idx] = kNotAwake;
            }

            // Phase C: maintain the kinematic solver-set. A KINEMATIC body enters
            // the set; static/dynamic do not. Reset the back-index BEFORE the add
            // so a recycled slot carrying a stale m_kinematicIndex cannot make the
            // idempotency guard wrongly skip the re-add (mirrors the awake-set's
            // reset-before-add). For a non-kinematic recycled slot, clear any stale
            // index left by a prior kinematic lifetime so RemoveFromKinematicSet
            // stays a no-op and the invariant holds.
            m_kinematicIndex[idx] = kNotKinematic;
            if (def.type == BodyType::Kinematic)
            {
                AddToKinematicSet(idx); // m_btype[idx] is already set to Kinematic above
            }

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
            m_bodyContacts[idx].clear(); // fresh slot (may be recycled)
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

            // COM + the auto-fixture are now in place -> cache the sleep-test extent
            // (Box2D sim->maxExtent). A later AddFixture re-runs this via RecomputeBodyMass.
            RecomputeMaxExtent(idx);

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

            // Immediately destroy every persistent-pool contact referencing this
            // body (Task 5). RemoveBody drops ALL the body's fixtures, so the
            // by-body walk catches every one of them in a single pass -- a removed
            // body never leaves a stale contact even for the one Step before
            // UpdateContacts' stale-handle guard would reap it.
            DestroyContactsForBody(idx);

            // Phase C, Task 4 leak-detector: every contact referencing this body
            // released its color bits above (DestroyContactsForBody -> ReleaseContactColor
            // per contact), so the removed slot's color mask MUST be 0. If this
            // fires, a destroy path is missing its ReleaseContactColor call (a leaked
            // bit would mis-color a future body recycled into this slot). Debug-only;
            // the recycle path also defaults the mask to 0 in EnsureCapacity, so a
            // leak here is a real bug, not a benign stale value.
            assert((idx >= m_bodyColorMask.size() || m_bodyColorMask[idx] == 0u) &&
                   "RemoveBody: body left a non-zero color mask -- a Destroy site is missing ReleaseContactColor");

            // Phase B: remove from the awake-set while the slot is still typed
            // Dynamic (btype has not been touched yet; RemoveFromAwakeSet checks
            // the btype to guard non-dynamics -- though a sleeping body is already
            // absent, being idempotent makes it unconditional and safe).
            RemoveFromAwakeSet(idx);

            // Phase C: remove from the kinematic-set while the slot is still typed
            // Kinematic (btype is untouched here; RemoveFromKinematicSet is
            // idempotent, so a non-kinematic slot -- already absent -- is a no-op).
            RemoveFromKinematicSet(idx);

            // Release the removed body's island membership (Phase A). Erase the
            // slot from its island's member list; if the island is now empty, free
            // it; otherwise mark it a split candidate (losing a member can fracture
            // the remaining pile). A recycled slot is reassigned a fresh island in
            // AddBody. Order-stable enough: SplitIsland re-derives membership from
            // contacts, not member order, so the swap-erase is determinism-safe.
            {
                const std::uint32_t isl = IslandOf(idx);
                if (isl != Island::kInvalidIsland)
                {
                    auto& bodies = m_islands[isl].bodies;
                    for (std::size_t i = 0; i < bodies.size(); ++i)
                    {
                        if (bodies[i] == idx)
                        {
                            bodies[i] = bodies.back();
                            bodies.pop_back();
                            break;
                        }
                    }
                    if (bodies.empty())
                    {
                        FreeIsland(isl);
                    }
                    else
                    {
                        MarkSplitCandidate(isl);
                    }
                    m_islandId[idx] = Island::kInvalidIsland;
                }
            }

            // Drop joints referencing the destroyed body (ports PhysicsWorld.lua
            // 281-286: `j.a.idx == idx or j.b.idx == idx`). Match by HANDLE index
            // (stable from construction, independent of Prepare). Iterate in
            // reverse so the swap-free erase keeps the surviving joints' order.
            for (std::size_t i = m_joints.size(); i-- > 0;)
            {
                const Joint* j = m_joints[i].get();
                const BodyHandle ha = j->HandleA();
                const BodyHandle hb = j->HandleB();
                const bool aIsIdx = (ha.generation != 0u && ha.index == idx);
                const bool bIsIdx = (hb.generation != 0u && hb.index == idx);
                if (aIsIdx || bIsIdx)
                {
                    // If this joint bridged two Dynamic bodies, dropping idx removes
                    // the joint's island edge: wake + mark the SURVIVING endpoint's
                    // island a split candidate so it re-derives its components
                    // (mirrors the contact-removal pattern). idx already left its own
                    // island above (m_islandId[idx] == kInvalidIsland here).
                    const BodyHandle other = aIsIdx ? hb : ha;
                    if (other.generation != 0u && other.index < m_count &&
                        TypeSlot(idx) == BodyType::Dynamic &&
                        TypeSlot(other.index) == BodyType::Dynamic)
                    {
                        WakeIsland(other.index);
                        MarkSplitCandidate(IslandOf(other.index));
                    }
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
                m_bodyContacts[idx].clear(); // DestroyContactsForBody already drained it; defensive
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

            // Union the two bodies' islands so a jointed dynamic construct sleeps
            // as ONE unit (Box2D treats a joint as an island edge). Only DYNAMIC
            // bodies are island members, so a joint to a static/kinematic anchor
            // unions nothing. Resolve the def's body HANDLES to slots directly:
            // BodyA()/BodyB() are not resolved until the joint's first Prepare, but
            // the handle indices are stable from construction. Both bodies were just
            // woken above, so the merged island is uniformly awake.
            const BodyHandle ha = def.a;
            const BodyHandle hb = def.b;
            if (ha.generation != 0u && hb.generation != 0u &&
                ha.index < m_count && hb.index < m_count &&
                TypeSlot(ha.index) == BodyType::Dynamic &&
                TypeSlot(hb.index) == BodyType::Dynamic)
            {
                const std::uint32_t ia = IslandOf(ha.index);
                const std::uint32_t ib = IslandOf(hb.index);
                if (ia != ib &&
                    ia != Island::kInvalidIsland &&
                    ib != Island::kInvalidIsland)
                {
                    MergeIslands(ia, ib);
                }
            }
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
                    // If this joint bridged two Dynamic bodies, its island edge is
                    // gone: wake the island + mark it a split candidate so the
                    // deferred pass re-derives its connected components (mirrors the
                    // contact-removal pattern in DestroyContactsForBody). Both
                    // endpoints share the island, so marking/waking one covers both.
                    const BodyHandle ha = j->HandleA();
                    const BodyHandle hb = j->HandleB();
                    if (ha.generation != 0u && hb.generation != 0u &&
                        ha.index < m_count && hb.index < m_count &&
                        TypeSlot(ha.index) == BodyType::Dynamic &&
                        TypeSlot(hb.index) == BodyType::Dynamic)
                    {
                        WakeIsland(ha.index);
                        MarkSplitCandidate(IslandOf(ha.index));
                    }
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

        Real PhysicsWorld::AngularVelocity(BodyHandle h) const noexcept
        {
            if (!IsValid(h))
            {
                return Real(0);
            }
            return m_angVel[h.index];
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
                    AddToAwakeSet(i); // Phase B: kInvalidIsland safety net (WakeIsland also adds, but may not run for isolated body)
                    WakeIsland(i); // wake the whole island, not just this body
                }
            }
        }

        void PhysicsWorld::SetAngularVelocity(BodyHandle h, Real w)
        {
            if (!IsValid(h))
            {
                return;
            }
            const std::uint32_t i = h.index;
            const BodyType bt = static_cast<BodyType>(m_btype[i]);
            // Mirror SetVelocity: Kinematic + Dynamic accept it; Static ignores.
            // Setting a Dynamic body's angular velocity WAKES it (and its island).
            if (bt == BodyType::Kinematic || bt == BodyType::Dynamic)
            {
                m_angVel[i] = w;
                if (bt == BodyType::Dynamic)
                {
                    m_awake[i]      = 1;
                    m_sleepTimer[i] = Real(0);
                    AddToAwakeSet(i);
                    WakeIsland(i);
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
            AddToAwakeSet(i); // Phase B: kInvalidIsland safety net
            WakeIsland(i); // wake the whole island, not just this body
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
            AddToAwakeSet(i); // Phase B: kInvalidIsland safety net
            WakeIsland(i); // wake the whole island, not just this body
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
                AddToAwakeSet(i); // Phase B: kInvalidIsland safety net
                WakeIsland(i); // wake the whole island, not just this body
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

            // ---- stage 1: prev snapshot + kinematic integrate ----------------
            //
            // WHY two passes instead of one (Phase B, Task 4):
            //
            // Pass A (0..m_count, non-dynamics): statics + kinematics.
            //   * Statics: snapping prev is harmless (AddBody already sets
            //     prev==pos and statics never move), but it keeps the old
            //     invariant for any teleport/manual-edit that touched pos.
            //   * Kinematics: snap prev then integrate pos; update broadphase.
            //   Kinematics are NOT in the awake-set (YAGNI a kinematic list), so
            //   they stay on the 0..m_count path.
            //
            // Pass B (ForEachAwake): AWAKE dynamic slots only.
            //   Sleeping dynamics are snapped by NEITHER pass -- their prev was
            //   set to pos at the moment they fell asleep (SnapPrevToPos in
            //   Island::UpdateSleep), and their pos is frozen thereafter, so
            //   prev==pos persists without any per-step work.
            //
            // WIN: sleeping dynamics (potentially the MAJORITY in a settled
            // scene) are entirely skipped by Stage 1 after warmup.
            {
                ARCANE_STEPPROF_SCOPE(Stage1Snapshot);

                // Pass A: statics + kinematics (0..m_count; no dynamic slots).
                for (std::uint32_t i = 0; i < m_count; ++i)
                {
                    if (m_alive[i] == 0)
                    {
                        continue;
                    }
                    const BodyType t = static_cast<BodyType>(m_btype[i]);
                    if (t == BodyType::Dynamic)
                    {
                        continue; // handled in Pass B below
                    }
                    // Static prev==pos always (AddBody seeds it; statics never move).
                    // Snap harmlessly keeps the invariant for manual teleports.
                    m_prevX[i] = m_posX[i];
                    m_prevY[i] = m_posY[i];

                    // Kinematic bodies integrate ONCE per Step (unchanged from P2.1).
                    // Dynamic gravity/damping + position now live in the solver's
                    // sub-step loop (stage 3); a dynamic body with no contacts still
                    // gets the same semi-implicit fall, just regrouped over sub-steps.
                    if (t == BodyType::Kinematic)
                    {
                        // Only a MOVING kinematic integrates + refreshes its broadphase
                        // proxy.  A stationary kinematic's proxy is already current (its
                        // last mutation -- SetPosition/SetAngle/SetVelocity -- refreshed
                        // it at the call site), so the per-step UpdateMoverProxies +
                        // residency-grid Move would be pure waste.  Stage-1 integration
                        // is LINEAR only (angle is not integrated here; SetAngle refreshes
                        // at its call site), so "moved this step" == nonzero linear vel.
                        if (m_velX[i] != Real(0) || m_velY[i] != Real(0))
                        {
                            m_posX[i] += m_velX[i] * dt;
                            m_posY[i] += m_velY[i] * dt;
                            // UpdateMoverProxies refreshes all per-fixture mover-broadphase
                            // proxies and the body's residency grid (Phase 2, Task 3).
                            UpdateMoverProxies(i);
                        }
                    }
                }

                // Pass B: snap prev for every AWAKE dynamic slot.
                // Sleeping dynamics are covered by SnapPrevToPos at sleep-time.
                ForEachAwake([&](std::uint32_t i)
                {
                    m_prevX[i] = m_posX[i];
                    m_prevY[i] = m_posY[i];
                });
            }

            // ---- stage 2: solver contact generation (Part A) -----------------
            // Collision-rebuild Phase 3, Task 4 -- THE SWAP. The persistent-contact
            // path is now the SOLE solver feed; GenerateContacts' per-step rebuild
            // is RETIRED. UpdateContacts does ALL the narrowphase this Step:
            //   * fixture<->fixture (mover<->mover + mover<->static-body) into the
            //     PERSISTENT m_contactPool (created/updated/destroyed across steps),
            //   * tile spans into the TRANSIENT m_spanContacts scratch (virtual
            //     fixtures, refilled each Step).
            // EmitContactConstraints then walks BOTH into m_contactConstraints in
            // the canonical (bodyA, bodyB, fixtureA, fixtureB) order; the ids are
            // stable (MixContactId) so warm-start impulses (now carried on each
            // persistent Contact's manifold point, seeded into the emitted
            // constraint here and written back after Solve -- see stage 3b) line up
            // with the same physical contact across steps.
            { ARCANE_STEPPROF_SCOPE(Narrowphase); UpdateContacts(dt); }
            m_contactConstraints.clear();
            { ARCANE_STEPPROF_SCOPE(EmitConstraints); EmitContactConstraints(m_contactConstraints); }

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
                Joint* jt = m_joints[i].get();
                if (jt == nullptr) { continue; }
                // Skip a joint whose Dynamic endpoints are ALL asleep: the solver's
                // global joint passes must not integrate a sleeping construct (the
                // perf win + the sleeping-frozen contract, mirroring the contact
                // awake-gate). Since a jointed dynamic pair shares one island (Step 1)
                // and islands sleep/wake atomically, this never mixes awake+asleep
                // endpoints. A joint anchored to a static/kinematic body is driven by
                // its one Dynamic endpoint. A woken island re-enters its joints next
                // Step (this array is rebuilt every Step).
                const BodyHandle ha = jt->HandleA();
                const BodyHandle hb = jt->HandleB();
                bool anyAwakeDynamic = false;
                if (ha.generation != 0u && ha.index < m_count &&
                    TypeSlot(ha.index) == BodyType::Dynamic && AwakeSlot(ha.index))
                {
                    anyAwakeDynamic = true;
                }
                if (hb.generation != 0u && hb.index < m_count &&
                    TypeSlot(hb.index) == BodyType::Dynamic && AwakeSlot(hb.index))
                {
                    anyAwakeDynamic = true;
                }
                if (!anyAwakeDynamic) { continue; }
                JointConstraint jc;
                jc.joint = jt;
                m_jointConstraints.push_back(jc);
            }

            {
                ARCANE_STEPPROF_SCOPE(Solve);
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
                ctx.executor     = Executor();   // Phase D1: always non-null (serial default)
                m_solver->Solve(ctx);
            }

            // ---- stage 3b: warm-start write-back (warm-start-on-Contact) -----
            // The solver no longer owns a warm-start cache: the converged per-point
            // impulses live on the persistent Contact. After Solve() (POST-
            // restitution, so we capture the FINAL accumulated impulses -- exactly
            // what the retired SoftStep Store loop stored), walk the emitted
            // constraints and write each point's (normal, tangent) impulse back onto
            // its source Contact's manifold point. Pool contacts carry their pool id
            // in sourceContactId; transient tile spans carry kNoContact and are
            // skipped (they have no persistent home -> cold-start next step). The
            // Contact then carries these into next step's emit (UpdateContacts
            // copies them across the manifold recompute by feature id). Decoupled:
            // the solver never touches the pool; only the world does the round-trip.
            // Baumgarte keeps its own cache and ignores sourceContactId, so this is
            // a no-op for it (its constraints still carry the field; Get() is only
            // reached for SoftStep-emitted pool contacts -- the same constraints
            // either way -- and writing the manifold impulses is harmless for a
            // solver that does not read them back).
            {
                ARCANE_STEPPROF_SCOPE(WarmStartWriteback);
                for (const ContactConstraint& cc : m_contactConstraints)
                {
                    if (cc.sourceContactId == ContactConstraint::kNoContact)
                    {
                        continue; // transient span: no persistent Contact to update
                    }
                    Contact& src = m_contactPool.Get(cc.sourceContactId);
                    // Defensive bound: within one Step the source Contact's manifold
                    // cannot change after emit, so cc.pointCount == manifold.pointCount;
                    // clamp anyway so a future reordering can never index out of range.
                    const int n = std::min(cc.pointCount, src.manifold.pointCount);
                    for (int p = 0; p < n; ++p)
                    {
                        src.manifold.points[p].normalImpulse  = cc.points[p].normalImpulse;
                        src.manifold.points[p].tangentImpulse = cc.points[p].tangentImpulse;
                    }
                }
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
            { ARCANE_STEPPROF_SCOPE(Bullet); BulletSweep(); }

            // ---- stage 5: island sleep bookkeeping (P2.4) --------------------
            // Build the per-Step constraint graph (bodies = nodes, THIS step's
            // contacts = edges), advance per-body sleep timers, and sleep any
            // island whose every member is idle past the threshold. Runs AFTER
            // the solve using this step's contacts; sleeping bodies are then
            // skipped by the next Step's contact feed + solver (the awake gate in
            // EmitContactConstraints), freezing their positions. Ports PhysicsWorld.lua:403-452.
            {
                ARCANE_STEPPROF_SCOPE(IslandSleep);
                // ---- deferred island split (quota-limited, Phase A) -----------------
                // Process AT MOST kMaxSplitsPerStep split-candidate islands per Step
                // (Box2D's amortization). Collect candidates ascending-id (determinism),
                // then split the first quota; the rest carry their flag to the next Step.
                // A single big pile that fractures resolves over a few steps.
                m_splitCandidates.clear();
                for (std::uint32_t id = 0; id < static_cast<std::uint32_t>(m_islands.size()); ++id)
                {
                    if (m_islands[id].splitCandidate && !m_islands[id].bodies.empty())
                    {
                        m_splitCandidates.push_back(id);
                    }
                }
                {
                    std::uint32_t processed = 0;
                    for (const std::uint32_t id : m_splitCandidates)
                    {
                        if (processed >= Island::kMaxSplitsPerStep) { break; }
                        SplitIsland(id);
                        ++processed;
                    }
                }

                Island::UpdateSleep(*this, dt);
            }

            // ---- stage 6: events + gating + deferred flush -------------------
            // Events-as-byproduct (collision-rebuild Phase 4): derive the touched
            // EVENT body-pairs from the persistent pool (UpdateContacts already
            // computed each pair's manifold ONCE this Step), then hand them to the
            // ContactManager -- it no longer runs its own second broadphase +
            // kinematic-static overlap pass. Walk the pool ascending-id
            // (deterministic), collect {min,max} body-pairs for every event-relevant
            // EXACTLY-OVERLAPPING contact, then sort + unique so a compound body's
            // N^2 fixture-pairs collapse to ONE body-pair and the Begin/Stay order
            // matches the old sorted-body-pair emission order. clear() keeps capacity.
            //
            // EXACT-OVERLAP, NOT speculative `touching`: the old ContactManager
            // tested overlap via SlotsOverlap with margin 0, which reports a contact
            // ONLY on STRICT penetration (depth > 0); a speculative gap (the manifold
            // point a velocity-scaled margin emits at NEGATIVE separation) is NOT an
            // event overlap. The pool's c.touching is pointCount>0 INCLUDING those
            // speculative gaps (correct for the SOLVER feed), so event derivation
            // must instead require a manifold point with separation > 0 -- byte-
            // identical to the old margin-0 SlotsOverlap (a genuinely penetrating
            // point reports the SAME positive separation regardless of the margin
            // used to compute the manifold, and an exact edge-touch at separation==0
            // is excluded by both, matching the old semantics).
            {
                ARCANE_STEPPROF_SCOPE(Events);
                auto exactlyOverlapping = [](const Contact& c) noexcept -> bool
                {
                    for (int p = 0; p < c.manifold.pointCount; ++p)
                    {
                        if (c.manifold.points[p].separation > Real(0))
                        {
                            return true;
                        }
                    }
                    return false;
                };
                m_touchedEventPairs.clear();
                m_contactPool.ForEach(
                    [&](std::uint32_t /*id*/, const Contact& c)
                    {
                        if (!c.eventRelevant || !exactlyOverlapping(c))
                        {
                            return;
                        }
                        const std::uint32_t a = c.bodyA < c.bodyB ? c.bodyA : c.bodyB;
                        const std::uint32_t b = c.bodyA < c.bodyB ? c.bodyB : c.bodyA;
                        m_touchedEventPairs.push_back(BroadphasePair{ a, b });
                    });
                std::sort(m_touchedEventPairs.begin(), m_touchedEventPairs.end());
                m_touchedEventPairs.erase(
                    std::unique(m_touchedEventPairs.begin(),
                                m_touchedEventPairs.end()),
                    m_touchedEventPairs.end());
                m_contacts.Step(*this, m_touchedEventPairs);
            }
        }

        // ----------------------------------------------------------------
        // Persistent-contact helpers (collision-rebuild Phase 3, Task 2).
        // ----------------------------------------------------------------

        bool PhysicsWorld::FixtureSlotLive(FixtureHandle h) const noexcept
        {
            return h.index < m_fxGen.size() &&
                   m_fxGen[h.index] == h.generation &&
                   m_fxGen[h.index] != 0u;
        }

        // ---- immediate lifecycle-seam contact destruction (Task 5) -------------
        //
        // Walk the persistent pool by STORED slot (ascending id, deterministic) and
        // destroy any contact that touches the removed fixture / body. The match is
        // by slot INDEX (not the live handle) because the caller runs these AFTER
        // the slot has already been recycled (generation bumped). Destroying the
        // CURRENT id mid-ForEach is safe: ForEach iterates ascending ids checking
        // m_alive[id], and Destroy only flips the alive flag + frees the id without
        // resizing the pool. Additive over the update-pass stale-handle guard.
        // NOTE: each helper is a full ContactPool::ForEach scan (O(contacts)), so
        // mass world teardown should prefer ContactPool::Clear() over a RemoveBody
        // loop (which would be O(bodies x contacts)).
        // CAVEAT (Phase C, Task 5): ContactPool::Clear() bypasses ReleaseContactColor,
        // so a future Clear()-based teardown path MUST also reset m_bodyColorMask (to
        // 0) and m_colorContacts (clear each list) -- otherwise the persistent coloring
        // bookkeeping leaks stale bits/ids against recycled slots.
        void PhysicsWorld::DestroyContactsForFixture(std::uint32_t fixtureSlot)
        {
            m_contactPool.ForEach([&](std::uint32_t id, Contact& c)
            {
                if (c.a.index == fixtureSlot ||
                    (c.bIsBody && c.b.index == fixtureSlot))
                {
                    // A removed fixture's touching dynamic-dynamic contact may
                    // fracture its island. Mark both bodies' islands and wake them
                    // so the removed body's pile re-settles.
                    if (c.bIsBody && c.touching &&
                        c.bodyA != kInvalidSlot && c.bodyB != kInvalidSlot &&
                        c.bodyA < m_islandId.size() && c.bodyB < m_islandId.size() &&
                        TypeSlot(c.bodyA) == BodyType::Dynamic &&
                        TypeSlot(c.bodyB) == BodyType::Dynamic)
                    {
                        MarkSplitCandidate(IslandOf(c.bodyA));
                        MarkSplitCandidate(IslandOf(c.bodyB));
                        WakeIsland(c.bodyA);
                        WakeIsland(c.bodyB);
                    }
                    ReleaseAndDestroyContact(id, c);
                }
            });
        }

        void PhysicsWorld::DestroyContactsForBody(std::uint32_t bodySlot)
        {
            m_contactPool.ForEach([&](std::uint32_t id, Contact& c)
            {
                if (c.bodyA == bodySlot ||
                    (c.bIsBody && c.bodyB == bodySlot))
                {
                    // A removed body's touching dynamic-dynamic contact may fracture
                    // its island. Mark both bodies' islands and wake them so the
                    // remaining pile re-settles. The removed body's slot still has a
                    // valid m_islandId here -- RemoveBody clears it AFTER this call.
                    if (c.bIsBody && c.touching &&
                        c.bodyA != kInvalidSlot && c.bodyB != kInvalidSlot &&
                        c.bodyA < m_islandId.size() && c.bodyB < m_islandId.size() &&
                        TypeSlot(c.bodyA) == BodyType::Dynamic &&
                        TypeSlot(c.bodyB) == BodyType::Dynamic)
                    {
                        MarkSplitCandidate(IslandOf(c.bodyA));
                        MarkSplitCandidate(IslandOf(c.bodyB));
                        WakeIsland(c.bodyA);
                        WakeIsland(c.bodyB);
                    }
                    ReleaseAndDestroyContact(id, c);
                }
            });
        }

        // ----------------------------------------------------------------
        // Persistent incremental contact coloring (Phase C, Stage 2, Tasks 4-5)
        // ----------------------------------------------------------------
        //
        // A solver-relevant body-body contact is colored ONCE at create and frees
        // its color at destroy -- the incremental replacement for the per-step
        // greedy recolor. Task 5 makes the solver CONSUME this coloring
        // (EmitContactConstraints copies Contact::color onto the emitted constraint;
        // SoftStep buckets by it instead of recoloring), so the color is now
        // load-bearing -- the mask GATES this lowest-free search. This persistent
        // coloring is a DIFFERENT but equally-valid color partition than the old
        // per-step greedy one; because the colored solve is Gauss-Seidel (color k's
        // velocity updates feed color k+1), a different valid partition is an
        // INTENTIONAL re-baseline vs pre-Phase-C main (different floats), NOT
        // bit-identical. The contract that holds is run-twice DETERMINISM + the
        // behavioral [physics] suite (no exact goldens) -- per the engine's
        // re-baseline-numerics-on-purpose rule. ValidatePersistentColoring
        // cross-checks the mask against the lists.

        void PhysicsWorld::AssignContactColor(std::uint32_t id, std::uint32_t a, std::uint32_t b,
                                              bool aDyn, bool bDyn)
        {
            // Lowest free color: one whose bit is unset in BOTH dynamic endpoints'
            // masks. A static/kinematic endpoint never blocks (it is read-only in
            // the solve, so sharing it across a color is harmless -- mirrors
            // ColorConstraints' aDyn/bDyn rule).
            int chosen = -1;
            for (int k = 0; k < kColorCount; ++k)
            {
                const std::uint32_t bit = 1u << k;
                const bool aFree = !aDyn || !(m_bodyColorMask[a] & bit);
                const bool bFree = !bDyn || !(m_bodyColorMask[b] & bit);
                if (aFree && bFree) { chosen = k; break; }
            }
            Contact& c = m_contactPool.Get(id);
            if (chosen < 0)
            {
                // OVERFLOW: no free color. The contact stays uncolored
                // (kInvalidColor) and is NOT listed in m_colorContacts; the solver
                // (Task 5) will solve it in the scalar tail.
                c.color = kInvalidColor;
                return;
            }
            c.color = static_cast<std::uint8_t>(chosen);
            const std::uint32_t bit = 1u << chosen;
            // Occupy the color bit on each DYNAMIC endpoint only.
            if (aDyn) m_bodyColorMask[a] |= bit;
            if (bDyn) m_bodyColorMask[b] |= bit;
            m_colorContacts[chosen].push_back(id);
        }

        void PhysicsWorld::ReleaseContactColor(std::uint32_t id)
        {
            Contact& c = m_contactPool.Get(id);
            const std::uint8_t col = c.color;
            if (col == kInvalidColor)
            {
                // Sensor / non-solver / span / overflow contact -- never colored,
                // never in m_colorContacts. Nothing to release.
                return;
            }

            // RATIONALE: the coloring invariant (no two same-color contacts share a
            // dynamic body) means each body has AT MOST ONE contact per color, so
            // clearing the body's bit for this color is exact -- no OTHER live
            // contact of this body occupies the same color, so we never strip a bit
            // a sibling contact still needs.
            //
            // Recompute dyn-ness from the cached body slots (a body's type is fixed
            // for its life -- m_btype is set only in AddBody -- so this matches the
            // aDyn/bDyn passed at AssignContactColor). A SPAN (c.bIsBody == false)
            // has no real B body, so only A can be a dynamic endpoint there.
            const std::uint32_t bit = 1u << col;
            const std::uint32_t bA  = c.bodyA;
            const std::uint32_t bB  = c.bodyB;
            const bool aDyn = (bA != kInvalidSlot) &&
                              (static_cast<BodyType>(m_btype[bA]) == BodyType::Dynamic);
            const bool bDyn = c.bIsBody && (bB != kInvalidSlot) &&
                              (static_cast<BodyType>(m_btype[bB]) == BodyType::Dynamic);
            if (aDyn) m_bodyColorMask[bA] &= ~bit;
            if (bDyn) m_bodyColorMask[bB] &= ~bit;

            // Swap-remove id from this color's contact list (order-independent --
            // the list is a set, never walked in a determinism-sensitive order).
            // TODO(perf): O(1) swap-remove by a stored per-contact index if destroy
            // frequency shows on a profile.
            std::vector<std::uint32_t>& list = m_colorContacts[col];
            for (std::size_t i = 0; i < list.size(); ++i)
            {
                if (list[i] == id)
                {
                    list[i] = list.back();
                    list.pop_back();
                    break;
                }
            }
            c.color = kInvalidColor;
        }

        std::uint8_t PhysicsWorld::ContactColorOf(std::uint32_t id) const
        {
            // Read-only probe (not used by the Step path). Get() asserts liveness
            // in Debug; the caller is expected to pass a live id. A dead/recycled
            // slot carries kInvalidColor (the EnsurePair recycle reset), so the
            // value is meaningful even on the recycled path.
            return m_contactPool.Get(id).color;
        }

        bool PhysicsWorld::ValidatePersistentColoring() const
        {
            // For each color, no DYNAMIC body slot may appear in two contacts, and
            // every listed contact must be alive AND tagged with this color. Task 5
            // also cross-checks the per-body color MASK against the lists: now that
            // the mask is load-bearing (it GATES AssignContactColor's lowest-free
            // search), a mask/list divergence would silently corrupt the coloring, so
            // reconstruct the mask from the lists and require it to match m_bodyColorMask
            // bit-for-bit -- bit k is set IFF the slot has exactly one contact in
            // m_colorContacts[k] (uniqueness is enforced by the per-color seen check
            // below; "no stray bits" is enforced by the final equality).
            std::vector<std::uint8_t>  seen;      // per-body-slot "claimed this color"
            std::vector<std::uint32_t> rebuilt(m_bodyColorMask.size(), 0u); // mask from the lists
            for (int k = 0; k < kColorCount; ++k)
            {
                seen.assign(m_bodyColorMask.size(), 0u);
                const std::uint32_t bit = 1u << k;
                const std::vector<std::uint32_t>& list = m_colorContacts[static_cast<std::size_t>(k)];
                for (const std::uint32_t id : list)
                {
                    const Contact& c = m_contactPool.Get(id); // asserts alive in Debug
                    if (c.color != static_cast<std::uint8_t>(k))
                    {
                        return false; // listed under the wrong color
                    }
                    const std::uint32_t bA = c.bodyA;
                    const std::uint32_t bB = c.bodyB;
                    const bool aDyn = (bA != kInvalidSlot) &&
                                      (static_cast<BodyType>(m_btype[bA]) == BodyType::Dynamic);
                    const bool bDyn = c.bIsBody && (bB != kInvalidSlot) &&
                                      (static_cast<BodyType>(m_btype[bB]) == BodyType::Dynamic);
                    if (aDyn)
                    {
                        if (seen[bA] != 0u) return false; // dynamic body twice in one color
                        seen[bA] = 1u;
                        rebuilt[bA] |= bit;
                    }
                    if (bDyn)
                    {
                        if (seen[bB] != 0u) return false;
                        seen[bB] = 1u;
                        rebuilt[bB] |= bit;
                    }
                }
            }
            // The mask must equal what the lists imply -- catches a set bit with no
            // backing contact (a missed ReleaseContactColor) or a contact in a list
            // whose mask bit was never set (a missed AssignContactColor mask write).
            for (std::size_t s = 0; s < m_bodyColorMask.size(); ++s)
            {
                if (m_bodyColorMask[s] != rebuilt[s]) { return false; }
            }
            return true;
        }

        std::size_t PhysicsWorld::ColoredContactCount() const noexcept
        {
            // Sum the per-color lists. Read-only probe (not on the Step path): the
            // [phasec] coloring-validity test asserts this > 0 so the oracle cannot
            // trivially pass on an EMPTY coloring.
            std::size_t n = 0;
            for (const std::vector<std::uint32_t>& list : m_colorContacts)
            {
                n += list.size();
            }
            return n;
        }

        bool PhysicsWorld::BothAsleep(const Contact& c) const noexcept
        {
            // A static/kinematic body never wakes the recompute on its own, so it
            // counts as "asleep" here. m_awake is 1 for static/kinematic (they are
            // never integrated/slept), so guard on body TYPE: only a DYNAMIC body
            // being awake forces a recompute. Mirrors GenerateContacts' awake gate
            // (it only ever generates for an awake DYNAMIC A; a static/kinematic B
            // does not drive the narrowphase by itself).
            auto bodyAwake = [&](std::uint32_t b) -> bool
            {
                if (b >= m_count || m_alive[b] == 0)
                {
                    return false;
                }
                return static_cast<BodyType>(m_btype[b]) == BodyType::Dynamic &&
                       m_awake[b] != 0;
            };
            const bool aAwake = bodyAwake(c.bodyA);
            const bool bAwake = c.bIsBody && bodyAwake(c.bodyB);
            return !aAwake && !bAwake;
        }

        bool PhysicsWorld::FatBoxesOverlap(const Contact& c,
                                           Real extraMargin) const noexcept
        {
            // Fat box of a fixture slot: a MOVER fixture's fat box lives in the
            // DynamicTree (margin-grown, the broadphase invariant); a STATIC
            // fixture is not in the mover tree, so reconstruct its fat box as the
            // tight fixture AABB grown by the SAME tree margin (DynamicTree::kMargin)
            // -- the consistent fat-box definition the broadphase would use.
            // FixtureBroadphaseTree() is non-null ONLY for a DynamicTree mover
            // broadphase; it returns nullptr for SpatialHash / SweepAndPrune. The
            // tight + DynamicTree::kMargin reconstruction below assumes the tree's
            // margin invariant, so for a non-DynamicTree WorldDef::broadphase even
            // mover fixtures fall to that conservative fallback (a fixture's contact
            // may persist slightly longer than its true fat box). The default /
            // production broadphase is DynamicTree, so this is latent.
            const DynamicTree* tree = FixtureBroadphaseTree();
            auto fatOf = [&](std::uint32_t fxSlot, std::uint32_t bodySlot) -> Aabb2
            {
                // Mover fixture (Dynamic/Kinematic): its proxy is in the tree.
                // O(1) id->fat lookup (vs. the old O(leaves) ForEachLeaf scan).
                if (tree != nullptr && bodySlot < m_count && m_alive[bodySlot] != 0 &&
                    static_cast<BodyType>(m_btype[bodySlot]) != BodyType::Static)
                {
                    Aabb2 fat{};
                    if (tree->TryGetFatBox(fxSlot, fat))
                    {
                        return fat;
                    }
                }
                // Static fixture (or no tree): tight AABB grown by the tree margin.
                const Aabb2 tight = FixtureAabb(fxSlot);
                const Real  m     = DynamicTree::kMargin;
                return Aabb2{ Vec2(tight.min.x - m, tight.min.y - m),
                              Vec2(tight.max.x + m, tight.max.y + m) };
            };
            Aabb2 fatA = fatOf(c.a.index, c.bodyA);
            Aabb2 fatB = fatOf(c.b.index, c.bodyB);
            // Speculative widening: grow BOTH boxes by half the extra margin so the
            // overlap test admits a pair whose CLOSING distance this Step is up to
            // `extraMargin` (a fast mover approaching a wall). Zero by default keeps
            // the plain fat-box gate (the resting / settled persistence behavior).
            if (extraMargin > Real(0))
            {
                const Real h = extraMargin * Real(0.5);
                fatA.min.x -= h; fatA.min.y -= h; fatA.max.x += h; fatA.max.y += h;
                fatB.min.x -= h; fatB.min.y -= h; fatB.max.x += h; fatB.max.y += h;
            }
            return AabbOverlap(fatA, fatB);
        }

        void PhysicsWorld::WakeMoverPair(std::uint32_t fa, std::uint32_t fb)
        {
            // Wake-on-contact (Task 4): ports the rule the retired GenerateContacts
            // ran in its mover-mover loop (PhysicsWorld.lua:369-382). A sleeping
            // dynamic touched by an awake mover wakes so the island re-forms; the
            // [physics][island] "new contact wakes a sleeping body" test gates this.
            //
            // Apply the gates the old GenerateContacts mover-mover loop ran BEFORE
            // its wake block (same body, alive, BODY sensor, da/db) on the
            // PRE-ORIENTATION (a, b). The fixture-sensor gate is INTENTIONALLY NOT
            // applied here: in the old path the wake ran first and the fixture-sensor
            // `continue` came AFTER it (skipping only the CONTACT/constraint, not the
            // wake). TryCreateContact still applies the fixture-sensor gate, so a
            // pair overlapping only via a sensor fixture wakes but creates no contact
            // -- byte-identical to the old ordering.
            const std::uint32_t a = m_fxBody[fa];
            const std::uint32_t b = m_fxBody[fb];
            if (a == b)
            {
                return;
            }
            if (a >= m_count || b >= m_count || m_alive[a] == 0 || m_alive[b] == 0)
            {
                return;
            }
            if (m_sensor[a] != 0 || m_sensor[b] != 0)
            {
                return;
            }
            const bool da = static_cast<BodyType>(m_btype[a]) == BodyType::Dynamic;
            const bool db = static_cast<BodyType>(m_btype[b]) == BodyType::Dynamic;
            if (!da && !db)
            {
                return; // kinematic-kinematic: no dynamic response, nothing to wake
            }
            // Only a NON-IDLE (moving) mover wakes a sleeping neighbour. A body idle
            // enough to be a sleep candidate itself (same predicate as
            // Island::UpdateSleep) must NOT wake its sleeping neighbours -- otherwise
            // two near-resting bodies in different islands (a sub-pixel gap; NOT
            // touching) ping-pong each other awake forever (each wakes the other the
            // step it sleeps). A real mover (thrown body, moving/spinning kinematic)
            // is non-idle and still wakes. (Static wakers never reach here -- this
            // loop is the mover-mover broadphase.)
            auto moverIsMoving = [&](std::uint32_t s) -> bool {
                // Same combined test as Island::UpdateSleep: a mover "is moving"
                // (and thus wakes a sleeping neighbour) iff it is NOT idle, i.e.
                // |v| + |w|*maxExtent >= its sleepThreshold. Keeps the wake + sleep
                // predicates consistent at the threshold margin.
                const Real lin = std::sqrt(m_velX[s] * m_velX[s] + m_velY[s] * m_velY[s]);
                return (lin + std::fabs(m_angVel[s]) * m_maxExtent[s]) >= m_sleepThreshold[s];
            };
            // Wake a sleeping dynamic touched by an awake mover (a static/kinematic
            // counterpart reports awake; a dynamic counterpart must itself be awake).
            // The wake GATING here matches the old GenerateContacts pre-orientation
            // wake byte-for-byte -- including fixture-sensor pairs, which the old
            // path woke before reaching its fixture-sensor `continue` (that skip
            // gated the constraint, not the wake).
            if (da && m_awake[a] == 0 && (!db || m_awake[b] != 0) && moverIsMoving(b))
            {
                m_awake[a]      = 1;
                m_sleepTimer[a] = Real(0);
                AddToAwakeSet(a); // Phase B: kInvalidIsland safety net
                WakeIsland(a); // wake the sleeper's whole island (Box2D contact wake)
            }
            if (db && m_awake[b] == 0 && (!da || m_awake[a] != 0) && moverIsMoving(a))
            {
                m_awake[b]      = 1;
                m_sleepTimer[b] = Real(0);
                AddToAwakeSet(b); // Phase B: kInvalidIsland safety net
                WakeIsland(b); // wake the sleeper's whole island (Box2D contact wake)
            }
        }

        void PhysicsWorld::TryCreateContact(std::uint32_t fa, std::uint32_t fb)
        {
            // Resolve owning body slots (mover-mover orientation rule).
            std::uint32_t a = m_fxBody[fa];
            std::uint32_t b = m_fxBody[fb];

            // Same body -> two fixtures of one body never collide.
            if (a == b)
            {
                return;
            }
            if (a >= m_count || b >= m_count || m_alive[a] == 0 || m_alive[b] == 0)
            {
                return;
            }
            // Collision filter (Box2D rule): collide iff each side's category is in the
            // other's mask. A filtered-out pair never enters the pool -> no solve, no event.
            if (((m_fxFilterCat[fa] & m_fxFilterMask[fb]) == 0u) ||
                ((m_fxFilterCat[fb] & m_fxFilterMask[fa]) == 0u))
            {
                return;
            }
            // PHASE 4, Task 1: the sensor skip + the `!da && !db` skip are REMOVED
            // from creation -- a mover<->mover overlapping fixture-pair now ALWAYS
            // creates a contact (sensors + kinematic-kinematic INCLUDED), so the
            // pool covers the EVENT union, not just the solver-relevant pairs. The
            // SOLVER feed stays byte-identical because each contact is tagged
            // solverRelevant below and EmitContactConstraints emits only those.

            const bool da = static_cast<BodyType>(m_btype[a]) == BodyType::Dynamic;
            const bool db = static_cast<BodyType>(m_btype[b]) == BodyType::Dynamic;

            // ORIENT: prefer A dynamic (so the solver-side A is the dynamic body,
            // matching GenerateContacts' (fixA, fixB) order -- Task 3's oracle +
            // MixContactId rely on this). If neither is dynamic (kinematic-kinematic,
            // now poolable), fall back to the LOWER BODY SLOT as A so the orientation
            // is deterministic. Swap the BODY index + its FIXTURE index together.
            std::uint32_t ia = a,  ib = b;
            std::uint32_t fia = fa, fib = fb;
            const bool swap = da ? (db && ib < ia)   // both dynamic: lower slot is A
                                 : (db || ib < ia);  // B dynamic, or neither -> lower slot is A
            if (swap)
            {
                std::swap(ia, ib);
                std::swap(fia, fib);
            }

            // SOLVER RELEVANCE (the OLD create filter): true iff a dynamic body is
            // present AND neither side is a sensor (body-level OR per-fixture). The
            // sensor check uses the ORIENTED fixtures so each side's body+fixture is
            // tested consistently. dynamic-dynamic / dynamic-kinematic / dynamic-static
            // non-sensor -> true; sensors + kinematic-kinematic -> false.
            const bool sensorA = (m_sensor[ia] != 0) || (m_fxSensor[fia] != 0u);
            const bool sensorB = (m_sensor[ib] != 0) || (m_fxSensor[fib] != 0u);
            const bool solverRelevant = (da || db) && !sensorA && !sensorB;

            // EVENT RELEVANCE (Phase 4, Task 2): events fire for every pooled
            // body-pair EXCEPT dynamic-vs-static-body (the design's explicit
            // exclusion -- the solver owns dynamic-vs-static response; events are
            // gameplay triggers). A static body is `TypeSlot == Static`; the only
            // created pairs are mover-mover, dynamic-static, kinematic-static (tiles
            // never reach the pool), so this filter leaves mover-mover (sensors +
            // kinematic-kinematic included) + kinematic-static event-relevant and
            // excludes ONLY dynamic-static. Uses the ORIENTED (ia, ib) types so it
            // reads symmetrically; a/b vs ia/ib is identical (orientation only swaps
            // the two slots, not their type set).
            const bool aStatic =
                static_cast<BodyType>(m_btype[ia]) == BodyType::Static;
            const bool bStatic =
                static_cast<BodyType>(m_btype[ib]) == BodyType::Static;
            const bool aDyn =
                static_cast<BodyType>(m_btype[ia]) == BodyType::Dynamic;
            const bool bDyn =
                static_cast<BodyType>(m_btype[ib]) == BodyType::Dynamic;
            const bool eventRelevant = !((aDyn && bStatic) || (aStatic && bDyn));

            const FixtureHandle hA{ fia, m_fxGen[fia] };
            const FixtureHandle hB{ fib, m_fxGen[fib] };
            const ContactPool::EnsureResult r = m_contactPool.EnsurePair(hA, hB);
            if (r.created)
            {
                // EnsurePair already stored c.a = hA / c.b = hB on the fresh slot
                // (and Destroy re-keys from them, so we must NOT overwrite them).
                // We only fill the body slots + bIsBody + solver/event relevance,
                // which the pool defaults until the caller sets them (created == true).
                Contact& c = m_contactPool.Get(r.id);
                c.bodyA          = ia;
                c.bodyB          = ib;
                c.bIsBody        = true;
                c.solverRelevant = solverRelevant;
                c.eventRelevant  = eventRelevant;

                // Phase C, Task 4: assign a persistent graph color to a NEW
                // solver-relevant body-body contact (assign-at-create). Sensors,
                // non-solver, and span contacts stay uncolored (kInvalidColor).
                // Every created contact here is body-body (bIsBody == true), but
                // keep the explicit gate for intent.
                //
                // Pass the ORIENTED slots ia/ib together with the ORIENTED dyn
                // flags aDyn/bDyn (computed above from m_btype[ia]/m_btype[ib]),
                // NOT the pre-orientation da/db: the swap moves the slots but not
                // the da/db labels, so under a swap da/db would describe the wrong
                // endpoint. ReleaseContactColor + ValidatePersistentColoring both
                // recompute dyn-ness from m_btype[bodyA]/m_btype[bodyB] (== ia/ib),
                // so assign MUST key off the same oriented flags to stay consistent.
                if (solverRelevant && c.bIsBody)
                {
                    AssignContactColor(r.id, ia, ib, aDyn, bDyn);
                }
                // Per-body contact adjacency (G1 island-split linkage): a dyn-dyn
                // body contact is an island edge -> record it on BOTH endpoints so
                // SplitIsland can walk only this island's contacts. aDyn/bDyn are
                // the ORIENTED dyn flags (m_btype[ia]/m_btype[ib]); bodyA is
                // canonical-dynamic, so this fires exactly for dyn-dyn pairs.
                // Gate on solverRelevant: a sensor dyn-dyn pair must NOT become
                // an island edge (sensors fire events but must not merge islands).
                if (aDyn && bDyn && solverRelevant)
                {
                    m_bodyContacts[ia].push_back(r.id);
                    m_bodyContacts[ib].push_back(r.id);
                }
            }
            // On a non-created HIT we leave the existing contact untouched (its
            // body slots + manifold persist) -- mirrors EnsurePair's contract.
        }

        void PhysicsWorld::UpdateOneContact(std::uint32_t id, Contact& c,
                                            Real moveDt, Real threshSq) noexcept
        {
            (void)id;
            c.npState = 0;

            // Stale-handle: a removed/recycled fixture or dead body -> flag destroy.
            if (!FixtureSlotLive(c.a) || (c.bIsBody && !FixtureSlotLive(c.b)))
            {
                c.npState |= kNpDestroy;
                return;
            }

            // Velocity-scaled speculative margin (CCD) over the two bodies.
            const Real speedSqA = m_velX[c.bodyA] * m_velX[c.bodyA] +
                                  m_velY[c.bodyA] * m_velY[c.bodyA];
            const Real speedSqB = m_velX[c.bodyB] * m_velX[c.bodyB] +
                                  m_velY[c.bodyB] * m_velY[c.bodyB];
            const Real maxSpeedSq = std::max(speedSqA, speedSqB);
            const Real margin = (maxSpeedSq > threshSq)
                                    ? std::sqrt(maxSpeedSq) * moveDt
                                    : kSkin;

            // Fat-box separation (widened by the speculative margin) -> flag destroy.
            const Real extra = std::max(Real(0), margin - DynamicTree::kMargin);
            if (!FatBoxesOverlap(c, extra))
            {
                c.npState |= kNpDestroy;
                return;
            }

            // Both asleep (and not an event-only pair) -> keep cached manifold, no work.
            const bool eventOnly = c.eventRelevant && !c.solverRelevant;
            if (BothAsleep(c) && !eventOnly)
            {
                return;
            }

            // Warm-start carry-forward snapshot, then recompute the manifold.
            const Manifold oldManifold = c.manifold;
            const Transform xfA = ComposeFixtureXf(
                Vec2(m_posX[c.bodyA], m_posY[c.bodyA]), m_angle[c.bodyA],
                Vec2(m_fxLocalPosX[c.a.index], m_fxLocalPosY[c.a.index]),
                m_fxLocalAngle[c.a.index]);
            const Transform xfB = ComposeFixtureXf(
                Vec2(m_posX[c.bodyB], m_posY[c.bodyB]), m_angle[c.bodyB],
                Vec2(m_fxLocalPosX[c.b.index], m_fxLocalPosY[c.b.index]),
                m_fxLocalAngle[c.b.index]);
            c.manifold = Collide(m_fxShape[c.a.index], xfA,
                                 m_fxShape[c.b.index], xfB, margin);

            const bool wasTouching = c.touching;
            c.touching = (c.manifold.pointCount > 0);

            // Classify the dyn-dyn touch transition (island edge) -> flag for the tail.
            // Gate on c.solverRelevant: sensor dyn-dyn pairs must never trigger a
            // merge (kNpStarted) or split (kNpStopped) -- they fire events but must
            // not couple rigid islands.
            if (c.solverRelevant && c.bIsBody && c.bodyB != kInvalidSlot &&
                TypeSlot(c.bodyA) == BodyType::Dynamic &&
                TypeSlot(c.bodyB) == BodyType::Dynamic)
            {
                if (!wasTouching && c.touching)      { c.npState |= kNpStarted; }
                else if (wasTouching && !c.touching) { c.npState |= kNpStopped; }
            }

            // Warm-start: copy impulses forward by feature id (<=2x2 fixed loop).
            for (int np = 0; np < c.manifold.pointCount; ++np)
            {
                ManifoldPoint& nm = c.manifold.points[np];
                for (int op = 0; op < oldManifold.pointCount; ++op)
                {
                    const ManifoldPoint& om = oldManifold.points[op];
                    if (om.id == nm.id)
                    {
                        nm.normalImpulse  = om.normalImpulse;
                        nm.tangentImpulse = om.tangentImpulse;
                        break;
                    }
                }
            }
        }

        void PhysicsWorld::UpdateContacts(Real dt)
        {
            // Phase A per-step island merge scratch: reset here so the apply pass
            // below only sees pairs collected THIS step. clear() keeps capacity
            // -> zero steady-state allocation after the first few steps.
            m_pendingMerges.clear();

            // ---- 1. CREATE: a contact for every fixture-pair in the EVENT UNION --
            // (solver-relevant pairs + the event-only tail: sensors,
            //  kinematic-kinematic, kinematic-vs-static-body; tiles stay out).
            //
            // (a) mover<->mover: the Phase-2 incremental fixture-pair set (sorted
            //     fa < fb). Phase D2 Task 3: parallel broadphase pair-finding.
            //     Serial seams: EvictTouchedAndCollectMoved (snapshot moved ids,
            //     evict stale pairs) and MergeAndEmit (union per-worker key sets
            //     into the persistent pair set, emit sorted pairs) bracket a
            //     per-proxy QueryProxyPairs ParallelFor. Each worker uses its OWN
            //     stack and key buffer (disjoint write) so the tree descent is
            //     read-only under parallelism. MergeAndEmit + the sorted output
            //     are order-independent -> byte-identical at any worker count.
            //
            //     The serial UpdatePairs wrapper (tests/oracle) is unchanged.
            {
                auto* bp   = m_fixtureBroadphase.get();
                auto* exec = Executor();   // always non-null (serial fallback)

                bp->EvictTouchedAndCollectMoved(m_bpMovedScratch);

                const auto W = static_cast<std::size_t>(exec->WorkerCount());
                if (m_bpFindScratch.size()  < W) m_bpFindScratch.resize(W);
                if (m_bpStackScratch.size() < W) m_bpStackScratch.resize(W);
                for (auto& s : m_bpFindScratch) s.clear(); // per-step clear; capacity retained

                exec->ParallelFor(m_bpMovedScratch.size(), kBroadphaseGrain,
                    [&](std::size_t b, std::size_t e, std::uint32_t w) {
                        for (std::size_t k = b; k < e; ++k)
                            bp->QueryProxyPairs(m_bpMovedScratch[k],
                                                m_bpStackScratch[w],
                                                m_bpFindScratch[w]);
                    });

                bp->MergeAndEmit(
                    std::span<const std::vector<std::uint64_t>>(m_bpFindScratch.data(), W),
                    m_cpPairs);
            }
            for (const BroadphasePair& p : m_cpPairs)
            {
                WakeMoverPair(p.a, p.b);
                TryCreateContact(p.a, p.b);
            }

            // (b) mover<->static-BODY + tile SPANS: per awake non-sensor DYNAMIC
            //     body, the StaticCandidates lists. MIRRORS the legacy
            //     GenerateContacts static path (the query pad + the genStatics loop)
            //     AND its tile-SPAN path (Task 4). A static body's fixture is a real
            //     fixture slot, so we pair each (dynamic fixture, static fixture)
            //     into the PERSISTENT pool the same way TryCreateContact does. Tile
            //     spans are virtual fixtures (no slot), so they go into the TRANSIENT
            //     m_spanContacts scratch (cleared here, refilled each Step).
            //
            // Rerouted to ForEachAwake (Phase B, Task 4): the awake-set is the
            // compact list of awake dynamic slots; sleeping dynamics are skipped
            // entirely (they cannot move, so their static candidates are stable).
            // The !Dynamic / !awake guards inside the old loop body are DROPPED
            // (the set guarantees awake-dynamic); the sensor skip is KEPT.
            // The kinematic<->static-body sub-loop (c) stays on 0..m_count
            // (kinematics are not in the awake-set -- YAGNI a kinematic list).
            //
            // Clear the transient span scratch -- it is rebuilt from scratch this
            // Step (spans are not pooled; they are virtual/transient fixtures).
            m_spanContacts.clear();
            m_spanCenters.clear();
            // (m_newPairs is rebuilt by the serial merge below, which clears it
            //  right before the per-worker concat -- no top-of-stage clear needed.)
            const Real moveDt = dt > Real(0) ? dt : Real(0);
            const Real threshSq = (moveDt > Real(0))
                                      ? (kSkin / moveDt) * (kSkin / moveDt)
                                      : Real(0);
            // ---- DETECT (parallel; writes ONLY per-worker scratch) ----------------
            // Each worker handles a disjoint range of m_awakeBodies[begin..end).
            // The parallel section is STRUCTURALLY MUTATION-FREE: no TryCreateContact,
            // no m_spanContacts/m_spanCenters writes, no m_newPairs writes, no pool or
            // color mutation.  Every write targets only m_spanEntriesW[worker] or
            // m_newPairsW[worker] -- the caller's own [worker] entry.
            //
            // The serial tail (after ParallelFor) concatenates the per-worker buffers,
            // stable_sorts by awakeIndex (reproducing ForEachAwake / k-ascending order;
            // within-k push order is preserved because each k is processed by exactly
            // one worker and ranges are disjoint), then appends spans and calls
            // TryCreateContact per record.  AssignContactColor runs here, serially,
            // so the persistent coloring sequence is byte-identical to the serial path.
            const std::uint32_t awakeCount = AwakeCount();
            const std::uint32_t cWorkers   = Executor()->WorkerCount();
            if (m_genSpansW.size()    < cWorkers) { m_genSpansW.resize(cWorkers); }
            if (m_genStaticsW.size()  < cWorkers) { m_genStaticsW.resize(cWorkers); }
            if (m_gridScratchW.size() < cWorkers) { m_gridScratchW.resize(cWorkers); }
            if (m_spanEntriesW.size() < cWorkers) { m_spanEntriesW.resize(cWorkers); }
            if (m_newPairsW.size()    < cWorkers) { m_newPairsW.resize(cWorkers); }
            for (std::uint32_t w = 0; w < cWorkers; ++w)
            {
                m_spanEntriesW[w].clear();
                m_newPairsW[w].clear();
            }
            Executor()->ParallelFor(awakeCount, /*minRange=*/kCreateGrain,
                [&](std::size_t begin, std::size_t end, std::uint32_t worker)
                {
                    std::vector<Aabb2>&         spans   = m_genSpansW[worker];
                    std::vector<std::uint32_t>& statics = m_genStaticsW[worker];
                    std::vector<std::uint32_t>& grid    = m_gridScratchW[worker];
                    std::vector<SpanEntry>&     spanOut = m_spanEntriesW[worker];
                    std::vector<NewPairRecord>& pairOut = m_newPairsW[worker];
                    for (std::size_t kk = begin; kk < end; ++kk)
                    {
                        const std::uint32_t k = static_cast<std::uint32_t>(kk);
                        const std::uint32_t i = AwakeBodies()[k];
                        if (m_sensor[i] != 0) { continue; }
                        // Same query pad as the legacy GenerateContacts: max(2, specMargin),
                        // where specMargin = max(kSkin, |v|*dt) so the candidate set is
                        // identical (the velocity-scaled speculative margin, P3.1).
                        const Real speedSqA   = m_velX[i] * m_velX[i] + m_velY[i] * m_velY[i];
                        const Real specMargin = (speedSqA > threshSq)
                                                    ? std::sqrt(speedSqA) * moveDt : kSkin;
                        const Aabb box = SlotAabb(i);
                        const Real pad = std::max(Real(2), specMargin);
                        Aabb2 query;
                        query.min = Vec2(box.min.x - pad, box.min.y - pad);
                        query.max = Vec2(box.max.x + pad, box.max.y + pad);
                        // Fills BOTH spans (tile spans, processed transiently below) and
                        // statics (static bodies, pooled). Uses per-worker grid scratch.
                        StaticCandidates(query, spans, statics, grid);

                        const std::vector<std::uint32_t>* fxListA = nullptr;
                        if (i < m_bodyFixtures.size() && !m_bodyFixtures[i].empty())
                        {
                            fxListA = &m_bodyFixtures[i];
                        }

                        // ---- tile spans: push SpanEntry{k, c, spanCenter} into spanOut
                        // (no m_spanContacts/m_spanCenters writes here -- per-worker only)
                        for (std::size_t s = 0; s < spans.size(); ++s)
                        {
                            const Aabb2& span = spans[s];
                            const Vec2 spanCenter = (span.min + span.max) * Real(0.5);
                            const Vec2 he = (span.max - span.min) * Real(0.5);
                            const Shape spanShape = MakeAabb(he.x, he.y);
                            const Transform xfB{ spanCenter, Real(0) };

                            if (fxListA != nullptr)
                            {
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
                                    const Transform xfA = ComposeFixtureXf(
                                        Vec2(m_posX[i], m_posY[i]), m_angle[i],
                                        Vec2(m_fxLocalPosX[fi], m_fxLocalPosY[fi]),
                                        m_fxLocalAngle[fi]);
                                    const Manifold mfld = Collide(m_fxShape[fi], xfA,
                                                                  spanShape, xfB, specMargin);
                                    if (mfld.pointCount <= 0)
                                    {
                                        continue; // not touching -> no transient contact
                                    }
                                    Contact c;
                                    c.a        = FixtureHandle{ fi, m_fxGen[fi] };
                                    c.b        = FixtureHandle{}; // span has no fixture slot
                                    c.bodyA    = i;
                                    c.bodyB    = kInvalidSlot;
                                    c.bIsBody  = false;
                                    c.manifold = mfld;
                                    c.touching = true;
                                    spanOut.push_back(SpanEntry{ k, c, spanCenter });
                                }
                            }
                            else
                            {
                                // Legacy single-shape fallback (a dynamic body with no live
                                // fixtures -- AddBody always makes one, so this is defensive).
                                const Transform xfA{ Vec2(m_posX[i], m_posY[i]), m_angle[i] };
                                const Manifold mfld = Collide(m_shape[i], xfA,
                                                              spanShape, xfB, specMargin);
                                if (mfld.pointCount <= 0)
                                {
                                    continue;
                                }
                                Contact c;
                                c.a        = FixtureHandle{}; // no fixture slot
                                c.b        = FixtureHandle{};
                                c.bodyA    = i;
                                c.bodyB    = kInvalidSlot;
                                c.bIsBody  = false;
                                c.manifold = mfld;
                                c.touching = true;
                                spanOut.push_back(SpanEntry{ k, c, spanCenter });
                            }
                        }

                        if (fxListA == nullptr) { continue; }

                        // ---- static fixture pairs: EMIT records into pairOut --------
                        // (no m_newPairs.push_back / TryCreateContact here -- per-worker only)
                        for (std::size_t s = 0; s < statics.size(); ++s)
                        {
                            const std::uint32_t idx = statics[s];
                            if (idx >= m_count || m_alive[idx] == 0 || m_sensor[idx] != 0) { continue; }
                            const std::vector<std::uint32_t>* fxListB = nullptr;
                            if (idx < m_bodyFixtures.size() && !m_bodyFixtures[idx].empty())
                            {
                                fxListB = &m_bodyFixtures[idx];
                            }
                            if (fxListB == nullptr) { continue; }
                            for (const std::uint32_t fiA : *fxListA)
                            {
                                if (fiA >= m_fxCount || m_fxGen[fiA] == 0u || m_fxSensor[fiA] != 0u) { continue; }
                                for (const std::uint32_t fiB : *fxListB)
                                {
                                    if (fiB >= m_fxCount || m_fxGen[fiB] == 0u || m_fxSensor[fiB] != 0u) { continue; }
                                    pairOut.push_back(NewPairRecord{ k, fiA, fiB });
                                }
                            }
                        }
                    }
                });
            // ---- SERIAL APPLY: order by awakeIndex, reproduce ForEachAwake order ---
            // Spans: concatenate all per-worker entries + stable_sort by awakeIndex ->
            // append to m_spanContacts/m_spanCenters in the same order the serial loop
            // produced them (k ascending, within-k span-s ascending).
            {
                m_allSpans.clear(); // reuse the member buffer (zero steady-state alloc)
                for (std::uint32_t w = 0; w < cWorkers; ++w)
                    m_allSpans.insert(m_allSpans.end(), m_spanEntriesW[w].begin(), m_spanEntriesW[w].end());
                std::stable_sort(m_allSpans.begin(), m_allSpans.end(),
                    [](const SpanEntry& a, const SpanEntry& b) { return a.awakeIndex < b.awakeIndex; });
                for (const SpanEntry& e : m_allSpans)
                {
                    m_spanContacts.push_back(e.c);
                    m_spanCenters.push_back(e.center);
                }
            }
            // New pairs: same merge + sort -> TryCreateContact each (AssignContactColor
            // runs here, serially, preserving the order-dependent color assignment).
            m_newPairs.clear();
            for (std::uint32_t w = 0; w < cWorkers; ++w)
                m_newPairs.insert(m_newPairs.end(), m_newPairsW[w].begin(), m_newPairsW[w].end());
            std::stable_sort(m_newPairs.begin(), m_newPairs.end(),
                [](const NewPairRecord& a, const NewPairRecord& b) { return a.awakeIndex < b.awakeIndex; });
            for (const NewPairRecord& rec : m_newPairs)
            {
                TryCreateContact(rec.fiA, rec.fiB);
            }

            // (c) KINEMATIC<->static-BODY (Phase 4, Task 1): event-relevant but NOT
            //     solver-relevant. Static bodies are NOT in the mover broadphase and
            //     the dynamic-driven static-candidate loop above only covers DYNAMIC
            //     bodies, so kinematic-vs-static pairs are created here by iterating
            //     StaticList() per alive Kinematic body -- MIRRORING the old
            //     ContactManager::Step kinematic-static loop (StaticList, AABB-reject)
            //     so the create order is index-deterministic. TryCreateContact tags
            //     these solverRelevant == false (no dynamic body), so the solver feed
            //     is unchanged; the touch-state still drives the contact's manifold +
            //     `touching` in the update pass below for the event derivation (Task 2).
            //     dynamic-vs-static is ALREADY created in (b) -- this path is additive.
            {
                const std::vector<std::uint32_t>& statics = m_staticList;
                for (std::uint32_t i = 0; i < m_count; ++i)
                {
                    if (m_alive[i] == 0 ||
                        static_cast<BodyType>(m_btype[i]) != BodyType::Kinematic)
                    {
                        continue;
                    }
                    const std::vector<std::uint32_t>* fxListK = nullptr;
                    if (i < m_bodyFixtures.size() && !m_bodyFixtures[i].empty())
                    {
                        fxListK = &m_bodyFixtures[i];
                    }
                    if (fxListK == nullptr)
                    {
                        continue; // kinematic body with no live fixtures
                    }
                    const Aabb2 kinBox = SlotAabb(i);
                    for (std::size_t s = 0; s < statics.size(); ++s)
                    {
                        const std::uint32_t idx = statics[s];
                        if (idx >= m_count || m_alive[idx] == 0)
                        {
                            continue;
                        }
                        // Cheap body-union AABB reject before the per-fixture pairing
                        // (mirrors the old ContactManager AABB pre-filter).
                        if (!AabbOverlap(kinBox, SlotAabb(idx)))
                        {
                            continue;
                        }
                        const std::vector<std::uint32_t>* fxListB = nullptr;
                        if (idx < m_bodyFixtures.size() && !m_bodyFixtures[idx].empty())
                        {
                            fxListB = &m_bodyFixtures[idx];
                        }
                        if (fxListB == nullptr)
                        {
                            continue; // static body with no real fixture slot
                        }
                        for (const std::uint32_t fiK : *fxListK)
                        {
                            if (fiK >= m_fxCount || m_fxGen[fiK] == 0u)
                            {
                                continue; // dead slot (defensive); sensors INCLUDED
                            }
                            for (const std::uint32_t fiB : *fxListB)
                            {
                                if (fiB >= m_fxCount || m_fxGen[fiB] == 0u)
                                {
                                    continue; // dead slot; sensors INCLUDED (events)
                                }
                                // Orientation: A = the kinematic mover (neither is
                                // dynamic, so TryCreateContact's lower-slot tiebreak
                                // orders deterministically); for events the body-pair
                                // is canonicalized later anyway.
                                TryCreateContact(fiK, fiB);
                            }
                        }
                    }
                }
            }

            // ---- 2. UPDATE + DESTROY: Box2D b2Collide -- gather, parallel collide
            //         (flag only), serial apply. ------------------------------------
            // Seam 0: gather the stable live-contact id list (Box2D contactSims).
            m_npContacts.clear();
            m_contactPool.ForEach([&](std::uint32_t id, Contact&) {
                m_npContacts.push_back(id);
            });

            // Seam 1: parallel collide. Each worker recomputes its range's manifolds
            // and sets a bit (keyed on pool id) in its OWN BitSet -- no structural
            // mutation. minRange=64 (Box2D's grain); below it, runs serial on worker 0.
            const std::uint32_t workers = Executor()->WorkerCount();
            if (m_npStateBits.size() < workers) { m_npStateBits.resize(workers); }
            const std::size_t idCap = m_contactPool.Capacity();
            for (std::uint32_t w = 0; w < workers; ++w) {
                m_npStateBits[w].Resize(idCap);
                m_npStateBits[w].ClearAll();
            }
            Executor()->ParallelFor(m_npContacts.size(), /*minRange=*/64,
                [&](std::size_t begin, std::size_t end, std::uint32_t worker)
                {
                    Arcane::BitSet& bits = m_npStateBits[worker];
                    for (std::size_t k = begin; k < end; ++k)
                    {
                        const std::uint32_t id = m_npContacts[k];
                        Contact& c = m_contactPool.Get(id);
                        UpdateOneContact(id, c, moveDt, threshSq);
                        if (c.npState != 0) { bits.Set(id); }
                    }
                });

            // Seam 2: serial apply. OR-reduce into bits[0], walk ascending (CTZ),
            // apply destroy / merge-edge / split per npState (ascending id == serial).
            if (workers > 0)
            {
                for (std::uint32_t w = 1; w < workers; ++w) {
                    m_npStateBits[0].InPlaceUnion(m_npStateBits[w]);
                }
                m_npStateBits[0].ForEachSetBit([&](std::uint32_t id)
                {
                    Contact& c = m_contactPool.Get(id);
                    if (c.npState & kNpDestroy)
                    {
                        // Defensive: only schedule a split for solver-relevant
                        // contacts (island edges). A sensor dyn-dyn contact was
                        // never an island edge, so destroying it cannot fracture
                        // an island -- skip the MarkSplitCandidate call.
                        if (c.solverRelevant && c.bIsBody && c.touching &&
                            c.bodyA != kInvalidSlot && c.bodyB != kInvalidSlot &&
                            c.bodyA < m_islandId.size() &&
                            TypeSlot(c.bodyA) == BodyType::Dynamic &&
                            c.bodyB < m_islandId.size() &&
                            TypeSlot(c.bodyB) == BodyType::Dynamic)
                        {
                            MarkSplitCandidate(IslandOf(c.bodyA));
                        }
                        ReleaseAndDestroyContact(id, c);
                    }
                    else if (c.npState & kNpStarted)
                    {
                        const std::uint32_t lo = c.bodyA < c.bodyB ? c.bodyA : c.bodyB;
                        const std::uint32_t hi = c.bodyA < c.bodyB ? c.bodyB : c.bodyA;
                        m_pendingMerges.push_back(BroadphasePair{ lo, hi });
                    }
                    else if (c.npState & kNpStopped)
                    {
                        MarkSplitCandidate(IslandOf(c.bodyA));
                    }
                });
            }

            // ---- apply queued island merges in a canonical order ----------------
            // Sort by (min,max) body slot (mirrors the m_touchedEventPairs sort) so
            // the merge sequence is run-twice-identical regardless of pool emission
            // order. Each pair re-resolves its bodies' CURRENT islands (an earlier
            // merge this step may have already united them -> MergeIslands is a
            // no-op when both resolve to the same id). No dedup needed: duplicate
            // pairs become same-island no-ops after the first merge.
            std::sort(m_pendingMerges.begin(), m_pendingMerges.end());
            for (const BroadphasePair& pr : m_pendingMerges)
            {
                const std::uint32_t ia = IslandOf(pr.a);
                const std::uint32_t ib = IslandOf(pr.b);
                if (ia != Island::kInvalidIsland &&
                    ib != Island::kInvalidIsland &&
                    ia != ib)
                {
                    // Uniform-awake invariant (Box2D "island is uniformly awake"):
                    // a begin-touch always involves at least one moving/awake body,
                    // so the merged island MUST end awake. If the two sides differ in
                    // awake state, wake the SLEEPING side's island BEFORE the merge.
                    // Otherwise MergeIslands would graft an already-sleeping singleton
                    // (a body that slept early resting purely on tile spans, which the
                    // WakeMoverPair moverIsMoving gate declined to wake) into the awake
                    // island, leaving a mixed awake/asleep island whose touching
                    // contact trips the no-sleeping-dynamic assert in
                    // EmitContactConstraints. Per-body awake flags mean this is robust
                    // to earlier merges this step (WakeIsland resolves the CURRENT
                    // island of the sleeping slot).
                    if (m_awake[pr.a] != m_awake[pr.b])
                    {
                        WakeIsland(m_awake[pr.a] == 0 ? pr.a : pr.b);
                    }
                    MergeIslands(ia, ib);
                }
            }
        }

        void PhysicsWorld::EmitContactConstraints(
            std::vector<ContactConstraint>& out) const
        {
            // The persistent solver feed (Phase 3, Task 4). Walk BOTH the persistent
            // m_contactPool (fixture<->fixture, ascending id) AND the transient
            // m_spanContacts (dynamic-fixture<->tile-span), emit a ContactConstraint
            // per touching+awake contact (mirroring the retired GenerateContacts
            // `emit` lambda field-for-field), then sort into the canonical
            // (bodyA, bodyB, fixtureA, fixtureB) order so the live feed is
            // run-twice-identical regardless of pool/broadphase emission order.
            // READ-ONLY w.r.t. SIM STATE: writes ONLY `out` + the persistent emit
            // scratch members (m_emitKeys/m_emitOrder/m_emitSorted -- `mutable`, so
            // this method stays `const`). The span scratch was filled by
            // UpdateContacts; this method mutates no body/contact sim state.
            out.clear();

            // Parallel sort key per emitted constraint: (bodyA, bodyB, fixA, fixB).
            // Carried from each source Contact during emit so the canonical sort
            // has the fixture slots available (ContactConstraint does not store
            // fixture slots -- only body slots). Sorted together with `out`.
            // Persistent scratch: clear() keeps capacity (no realloc after warmup).
            std::vector<EmitSortKey>& keys = m_emitKeys;
            keys.clear();

            // Emit one ContactConstraint for a single Contact. Returns true if a
            // constraint was emitted (touching + awake A). `spanCenter` is the
            // span's geometric center (used as comB for a tile span); unused when
            // c.bIsBody. Mirrors the retired GenerateContacts `emit` lambda.
            auto emitContact = [&](const Contact& c, const Vec2& spanCenter) -> bool
            {
                // Not touching -> the legacy emit early-returns on pointCount <= 0.
                if (!c.touching || c.manifold.pointCount <= 0)
                {
                    return false;
                }
                // Awake-gate: GenerateContacts only ever emitted for an AWAKE dynamic
                // A. The pool keeps contacts for sleeping mover-pairs too, so without
                // this gate an asleep pair would (wrongly) feed the solver.
                const std::uint32_t aIdx = c.bodyA;
                if (aIdx >= m_count || m_alive[aIdx] == 0 || m_awake[aIdx] == 0)
                {
                    return false;
                }

                const bool          bIsBody = c.bIsBody;
                const std::uint32_t bIdx    = c.bodyB;
                // fixA: the dynamic fixture slot for a fixture-path contact; for the
                // legacy single-shape span fallback c.a is an invalid handle (gen 0)
                // and fixA is kInvalidSlot (matching GenerateContacts' fallback id).
                const bool          aHasFix = (c.a.generation != 0u);
                const std::uint32_t fixA    = aHasFix ? c.a.index : kInvalidSlot;
                const std::uint32_t fixB    = bIsBody ? c.b.index : kInvalidSlot;

                const Manifold& m = c.manifold;

                ContactConstraint cc;
                cc.bodyA       = aIdx;
                cc.bodyB       = bIsBody ? bIdx : kInvalidSlot;
                cc.bodyBIsBody = bIsBody;
                cc.invMassA    = m_invMass[aIdx];
                cc.invInertiaA = m_invInertia[aIdx];
                cc.invMassB    = bIsBody ? m_invMass[bIdx] : Real(0);
                cc.invInertiaB = bIsBody ? m_invInertia[bIdx] : Real(0);
                cc.normal      = m.normal;
                cc.kind        = m.kind;

                // Combined material: friction = sqrt(fA*fB), restitution = max(rA,rB).
                //   * fixture-path (aHasFix): per-fixture material (both sides for a
                //     fixture<->fixture; for a span, body A's fixture material as both
                //     sides + restB = 0 -- exactly GenerateContacts' span emit args).
                //   * single-shape fallback (!aHasFix): body-level m_fric/m_rest as
                //     both sides + restB = 0 (the legacy fallback span emit).
                Real fricA, fricB, restA, restB;
                if (aHasFix)
                {
                    fricA = m_fxFriction[fixA];
                    fricB = bIsBody ? m_fxFriction[fixB] : m_fxFriction[fixA];
                    restA = m_fxRestitution[fixA];
                    restB = bIsBody ? m_fxRestitution[fixB] : Real(0);
                }
                else
                {
                    // Single-shape body A (no fixture) vs span: body-level material.
                    fricA = m_fric[aIdx];
                    fricB = m_fric[aIdx];
                    restA = m_rest[aIdx];
                    restB = Real(0);
                }
                cc.friction    = std::sqrt(fricA * fricB);
                cc.restitution = std::max(restA, restB);

                // Compound-COM anchors: from each body's world CENTER OF MASS
                // (WorldCom == origin for localCenter==0). A is always dynamic; B is
                // either a real body's world COM or the span's geometric center.
                const Vec2 cA = WorldCom(Vec2(m_posX[aIdx], m_posY[aIdx]),
                                         m_angle[aIdx],
                                         Vec2(m_localCenterX[aIdx],
                                              m_localCenterY[aIdx]));
                const Vec2 comB = bIsBody
                    ? WorldCom(Vec2(m_posX[bIdx], m_posY[bIdx]),
                               m_angle[bIdx],
                               Vec2(m_localCenterX[bIdx], m_localCenterY[bIdx]))
                    : spanCenter; // tile span: invInertiaB==0 zeros the lever arm

                cc.pointCount = m.pointCount;
                for (int p = 0; p < m.pointCount; ++p)
                {
                    const ManifoldPoint&    mp = m.points[p];
                    ContactConstraintPoint& cp = cc.points[p];
                    cp.anchorA        = mp.point - cA;
                    cp.anchorB        = mp.point - comB;
                    cp.baseSeparation = -mp.separation;
                    cp.id             = MixContactId(mp.id, fixA, fixB);
                    // WARM-START SEED (read path): carry the persistent Contact's
                    // accumulated impulses INTO the emitted constraint point. For a
                    // pool contact these were written back after last step's Solve
                    // (+ carried across the manifold recompute by UpdateContacts);
                    // for a transient tile span mp.normalImpulse is 0, so spans
                    // cold-start (acceptable -- they have no persistent home). The
                    // solver's Prepare leaves these untouched.
                    cp.normalImpulse  = mp.normalImpulse;
                    cp.tangentImpulse = mp.tangentImpulse;
                }
                // Phase B invariant: no emitted constraint references a SLEEPING
                // dynamic. Awake-A gate above + island-as-a-unit sleep (a touching
                // dynamic-dynamic pair shares one island, so awake-A => awake-B)
                // guarantees this. This assertion proves SyncIn can safely skip
                // sleeping dynamics (they are NEVER gathered by a live constraint).
                assert(!(static_cast<BodyType>(m_btype[aIdx]) == BodyType::Dynamic &&
                         m_awake[aIdx] == 0));
                assert(!(bIsBody &&
                         static_cast<BodyType>(m_btype[bIdx]) == BodyType::Dynamic &&
                         m_awake[bIdx] == 0));

                out.push_back(cc);
                keys.push_back(EmitSortKey{ aIdx, cc.bodyB, fixA, fixB });
                return true;
            };

            // (a) fixture<->fixture: the persistent pool (ascending id).
            // SOLVER-RELEVANCE FILTER (Phase 4, Task 1): the pool now holds the
            // EVENT union (sensors + kinematic-kinematic + kinematic-vs-static-body)
            // in addition to the solver pairs. Emit a ContactConstraint ONLY for a
            // solverRelevant contact, so the solver feed stays byte-identical to
            // Phase 3 even though the pool is a superset. (Spans below are always
            // solver-relevant -- a dynamic fixture vs a tile span -- and are NOT
            // gated here; they carry the default solverRelevant==false, so the gate
            // must NOT be inside the shared emitContact lambda.)
            m_contactPool.ForEach([&](std::uint32_t id, const Contact& c)
            {
                if (!c.solverRelevant)
                {
                    return; // event-only contact (sensor / kinematic): no constraint
                }
                if (emitContact(c, Vec2(Real(0), Real(0))))
                {
                    // Tag the just-emitted constraint with its persistent pool id so
                    // the post-Solve write-back lands the converged impulses on THIS
                    // Contact. Spans (below) leave the default kNoContact. The field
                    // travels with the constraint through the canonical sort.
                    out.back().sourceContactId = id;
                    // Phase C, Task 5: also carry the persistent contact COLOR so the
                    // solver buckets by it (deleting the per-step greedy recolor). An
                    // overflow contact carries kInvalidColor here -> the scalar tail;
                    // spans (below) keep the default kInvalidColor (no pool home).
                    out.back().color = c.color;
                }
            });

            // (b) tile spans: the transient scratch UpdateContacts filled this Step.
            for (std::size_t s = 0; s < m_spanContacts.size(); ++s)
            {
                emitContact(m_spanContacts[s], m_spanCenters[s]);
            }

            // ---- canonical sort (design Sec 7): (bodyA, bodyB, fixtureA, fixtureB).
            // Sort `out` and `keys` together via an index permutation so the live
            // solver feed is deterministic / run-twice-identical. The key is unique
            // per emitted constraint (a fixture-pair contributes exactly one
            // constraint per body-pair; two fixture-pairs differ in fixA/fixB).
            const std::size_t n = out.size();
            std::vector<std::size_t>& order = m_emitOrder; // persistent scratch
            order.clear();
            order.resize(n);
            for (std::size_t k = 0; k < n; ++k)
            {
                order[k] = k;
            }
            std::sort(order.begin(), order.end(),
                      [&](std::size_t lhs, std::size_t rhs)
            {
                const EmitSortKey& x = keys[lhs];
                const EmitSortKey& y = keys[rhs];
                if (x.bodyA != y.bodyA) { return x.bodyA < y.bodyA; }
                if (x.bodyB != y.bodyB) { return x.bodyB < y.bodyB; }
                if (x.fixA  != y.fixA)  { return x.fixA  < y.fixA;  }
                return x.fixB < y.fixB;
            });
            // Apply the permutation into the persistent staging vector (n is small
            // per Step). clear() keeps capacity; swap hands the sorted buffer to
            // `out` and parks `out`'s old buffer in m_emitSorted for next Step.
            std::vector<ContactConstraint>& sorted = m_emitSorted;
            sorted.clear();
            sorted.reserve(n);
            for (std::size_t k = 0; k < n; ++k)
            {
                sorted.push_back(out[order[k]]);
            }
            out.swap(sorted);
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
            FunctionRef<void(std::uint32_t, std::uint32_t)> fn) const
        {
            m_contacts.ForEachBegunPair(fn);
        }

        // ---- island registry management (Phase A) --------------------------

        std::uint32_t PhysicsWorld::AllocIsland()
        {
            if (!m_islandFree.empty())
            {
                const std::uint32_t id = m_islandFree.back();
                m_islandFree.pop_back();
                m_islands[id].bodies.clear();      // capacity kept (zero re-alloc)
                m_islands[id].splitCandidate = false;
                return id;
            }
            const std::uint32_t id = static_cast<std::uint32_t>(m_islands.size());
            m_islands.emplace_back();
            return id;
        }

        void PhysicsWorld::FreeIsland(std::uint32_t id) noexcept
        {
            // Defensive: never free an invalid/out-of-range id.
            if (id == Island::kInvalidIsland || id >= m_islands.size())
            {
                return;
            }
            m_islands[id].bodies.clear();
            m_islands[id].splitCandidate = false;
            m_islandFree.push_back(id);
        }

        std::uint32_t PhysicsWorld::MergeIslands(std::uint32_t idA, std::uint32_t idB)
        {
            // Weighted union: keep the LARGER island; relabel the smaller's
            // members + append, then free the smaller id. Stable membership ->
            // fewer relabels -> the island id of a big pile is sticky.
            if (idA == idB)
            {
                return idA;
            }
            std::uint32_t big   = idA;
            std::uint32_t small = idB;
            if (m_islands[big].bodies.size() < m_islands[small].bodies.size())
            {
                std::swap(big, small);
            }
            for (const std::uint32_t s : m_islands[small].bodies)
            {
                m_islandId[s] = big;
                m_islands[big].bodies.push_back(s);
            }
            // The survivor inherits the UNION of both islands' pending split-candidate
            // state. If the absorbed island was flagged for a deferred split (a member
            // separated/was destroyed before this merge), that fracture need transfers
            // to the survivor -- dropping it could leave a genuinely disconnected pile
            // over-grouped until another edge re-marks it. big keeps its own flag.
            const bool absorbedSplit = m_islands[small].splitCandidate;
            FreeIsland(small);
            m_islands[big].splitCandidate = m_islands[big].splitCandidate || absorbedSplit;
            return big;
        }

        void PhysicsWorld::MarkSplitCandidate(std::uint32_t islandId) noexcept
        {
            if (islandId == Island::kInvalidIsland || islandId >= m_islands.size())
            {
                return;
            }
            m_islands[islandId].splitCandidate = true;
        }

        void PhysicsWorld::SplitIsland(std::uint32_t islandId)
        {
            // Re-derive the connected components of one candidate island. Bodies
            // joined by a touching dyn-dyn contact share a component; the FIRST
            // component reuses islandId, others get fresh ids. The contact walk is
            // scoped to the island's OWN contacts via per-body adjacency
            // (m_bodyContacts) -> O(islandBodies + islandEdges), replacing the old
            // O(poolSize x islandSize) whole-pool scan. Byte-identical components.
            if (islandId == Island::kInvalidIsland || islandId >= m_islands.size())
            {
                return;
            }
            m_islands[islandId].splitCandidate = false;

            // Snapshot members (the rebuild reassigns m_islandId + may reuse this id).
            std::vector<std::uint32_t> members = m_islands[islandId].bodies;
            const std::uint32_t n = static_cast<std::uint32_t>(members.size());
            if (n <= 1)
            {
                return; // 0 or 1 member: nothing to fracture
            }

            // O(1) member-slot -> local index. Scratch is all-sentinel on entry.
            // members are live island body slots (< m_count) and EnsureCapacity
            // sizes m_splitLocalIndex >= m_count, so this write is always in-bounds.
            for (std::uint32_t i = 0; i < n; ++i)
            {
                m_splitLocalIndex[members[i]] = i;
            }

            std::vector<std::uint32_t> parent(n);
            for (std::uint32_t i = 0; i < n; ++i) { parent[i] = i; }
            auto find = [&](std::uint32_t x) -> std::uint32_t
            {
                while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
                return x;
            };

            // Union members joined by a TOUCHING dyn-dyn contact, walking only the
            // island's own contacts. Each edge is visited from both endpoints; the
            // union is idempotent and connected components are union-order-invariant,
            // so the resulting partition is identical to the old whole-pool walk.
            for (std::uint32_t i = 0; i < n; ++i)
            {
                const std::uint32_t slot = members[i];
                for (const std::uint32_t cid : m_bodyContacts[slot])
                {
                    const Contact& c = m_contactPool.Get(cid);
                    if (!c.touching) { continue; }
                    const std::uint32_t other = (c.bodyA == slot) ? c.bodyB : c.bodyA;
                    if (other >= m_splitLocalIndex.size()) { continue; }
                    const std::uint32_t j = m_splitLocalIndex[other];
                    if (j == kSplitLocalNone) { continue; } // other body not in island
                    parent[find(i)] = find(j);
                }
            }

            // Union members joined by a JOINT edge too (Box2D treats a joint as an
            // island edge). A jointed dynamic pair stays in ONE component even when
            // it shares no touching contact -- so removing a contact never wrongly
            // splits a still-jointed pair. Iterate m_joints in index order
            // (deterministic); only a dyn-dyn joint whose BOTH endpoints are members
            // of THIS island (m_splitLocalIndex != kSplitLocalNone) unions. Resolve
            // via the stable HANDLE slots (Prepare-independent -- a joint whose
            // bodies are all asleep is not Prepared this Step, so BodyA()/BodyB()
            // may be stale).
            for (const std::unique_ptr<Joint>& jp : m_joints)
            {
                const Joint* jt = jp.get();
                if (jt == nullptr) { continue; }
                const BodyHandle ha = jt->HandleA();
                const BodyHandle hb = jt->HandleB();
                if (ha.generation == 0u || hb.generation == 0u) { continue; } // static-anchor / missing-body joint
                const std::uint32_t sa = ha.index;
                const std::uint32_t sb = hb.index;
                if (sa >= m_splitLocalIndex.size() || sb >= m_splitLocalIndex.size()) { continue; }
                if (TypeSlot(sa) != BodyType::Dynamic || TypeSlot(sb) != BodyType::Dynamic) { continue; }
                const std::uint32_t la = m_splitLocalIndex[sa];
                const std::uint32_t lb = m_splitLocalIndex[sb];
                if (la == kSplitLocalNone || lb == kSplitLocalNone) { continue; } // not both in this island
                parent[find(la)] = find(lb);
            }

            // Group by local root; FIRST component reuses islandId, others alloc a
            // fresh id. UNCHANGED from the original (byte-identical id assignment).
            // SAFETY: AllocIsland() may emplace_back + REALLOCATE m_islands -- never
            // hold an Island& across it; index m_islands[isl] freshly by id.
            m_islands[islandId].bodies.clear();
            std::vector<std::uint32_t> rootLocal;   // distinct local roots, first-seen order
            std::vector<std::uint32_t> rootIsland;  // parallel island id per root
            for (std::uint32_t i = 0; i < n; ++i)
            {
                const std::uint32_t r = find(i);
                std::uint32_t ri = 0xFFFFFFFFu;
                for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(rootLocal.size()); ++k)
                {
                    if (rootLocal[k] == r) { ri = k; break; }
                }
                std::uint32_t isl;
                if (ri == 0xFFFFFFFFu)
                {
                    isl = rootLocal.empty() ? islandId : AllocIsland();
                    rootLocal.push_back(r);
                    rootIsland.push_back(isl);
                }
                else
                {
                    isl = rootIsland[ri];
                }
                const std::uint32_t slot = members[i];
                m_islandId[slot] = isl;
                m_islands[isl].bodies.push_back(slot);
            }

            // Reset only the touched scratch entries -> keep O(island), all-sentinel
            // between calls.
            for (std::uint32_t i = 0; i < n; ++i)
            {
                m_splitLocalIndex[members[i]] = kSplitLocalNone;
            }
        }

        // ---- per-body contact adjacency helpers (G1 island-split linkage) -------
        //
        // SwapRemoveId: remove the first occurrence of `id` from `v` by
        // swap-with-back + pop. No-op if absent. Order within m_bodyContacts is
        // irrelevant to SplitIsland (connected components are union-order-invariant),
        // so swap-remove is safe.
        static void SwapRemoveId(std::vector<std::uint32_t>& v, std::uint32_t id) noexcept
        {
            for (std::size_t k = 0; k < v.size(); ++k)
            {
                if (v[k] == id) { v[k] = v.back(); v.pop_back(); return; }
            }
        }

        void PhysicsWorld::DetachContactAdjacency(std::uint32_t id, const Contact& c) noexcept
        {
            // Only dyn-dyn body contacts were ever attached (see TryCreateContact).
            if (!c.bIsBody || c.bodyA == kInvalidSlot || c.bodyB == kInvalidSlot) { return; }
            if (TypeSlot(c.bodyA) != BodyType::Dynamic ||
                TypeSlot(c.bodyB) != BodyType::Dynamic) { return; }
            if (c.bodyA < m_bodyContacts.size()) { SwapRemoveId(m_bodyContacts[c.bodyA], id); }
            if (c.bodyB < m_bodyContacts.size()) { SwapRemoveId(m_bodyContacts[c.bodyB], id); }
        }

        void PhysicsWorld::ReleaseAndDestroyContact(std::uint32_t id, const Contact& c) noexcept
        {
            DetachContactAdjacency(id, c); // reads c before the pool frees the slot
            ReleaseContactColor(id);       // free the color while c still holds it
            m_contactPool.Destroy(id);
        }

        bool PhysicsWorld::DebugValidateBodyContacts() const
        {
            // 1) every id in every list is a live dyn-dyn body contact incident to
            //    that slot, with no duplicates within the list.
            for (std::uint32_t slot = 0; slot < m_bodyContacts.size(); ++slot)
            {
                const std::vector<std::uint32_t>& list = m_bodyContacts[slot];
                for (std::size_t k = 0; k < list.size(); ++k)
                {
                    const std::uint32_t id = list[k];
                    for (std::size_t j = k + 1; j < list.size(); ++j)
                    {
                        if (list[j] == id) { return false; } // duplicate
                    }
                    if (!m_contactPool.Alive(id)) { return false; }
                    const Contact& c = m_contactPool.Get(id);
                    if (!c.bIsBody) { return false; }
                    if (c.bodyA != slot && c.bodyB != slot) { return false; }
                    if (TypeSlot(c.bodyA) != BodyType::Dynamic ||
                        TypeSlot(c.bodyB) != BodyType::Dynamic) { return false; }
                }
            }
            // 2) every live dyn-dyn body contact appears in BOTH endpoints' lists.
            //    (const ForEach overload binds here; it already skips dead ids.)
            bool ok = true;
            m_contactPool.ForEach([&](std::uint32_t id, const Contact& c)
            {
                if (!c.bIsBody || c.bodyA == kInvalidSlot || c.bodyB == kInvalidSlot) { return; }
                if (TypeSlot(c.bodyA) != BodyType::Dynamic ||
                    TypeSlot(c.bodyB) != BodyType::Dynamic) { return; }
                auto has = [&](std::uint32_t s) -> bool {
                    if (s >= m_bodyContacts.size()) { return false; }
                    const std::vector<std::uint32_t>& l = m_bodyContacts[s];
                    for (std::uint32_t x : l) { if (x == id) { return true; } }
                    return false;
                };
                if (!has(c.bodyA) || !has(c.bodyB)) { ok = false; }
            });
            return ok;
        }

        // ---- awake-set maintainers (Phase B, Task 2) ----------------------------
        //
        // AddToAwakeSet / RemoveFromAwakeSet keep m_awakeBodies + m_awakeIndex
        // in sync with the awake-dynamic population. Both are idempotent: a
        // double-add or double-remove is a no-op. The Step loops are NOT rerouted
        // here (Tasks 3/4 do that); these are PURE bookkeeping appended to every
        // existing wake/sleep seam so the invariant holds at all times.

        void PhysicsWorld::AddToAwakeSet(std::uint32_t slot) noexcept
        {
            // Only awake dynamics belong in the set; idempotent (already-present
            // or non-dynamic is a no-op). The size guard handles a slot index that
            // has not been grown into yet (should not occur in normal flow, but
            // guards against out-of-bound reads on a half-initialized world).
            if (slot >= m_awakeIndex.size()) { return; }
            if (static_cast<BodyType>(m_btype[slot]) != BodyType::Dynamic) { return; }
            if (m_awakeIndex[slot] != kNotAwake) { return; }  // already in the set
            m_awakeIndex[slot] = static_cast<std::uint32_t>(m_awakeBodies.size());
            m_awakeBodies.push_back(slot);
        }

        void PhysicsWorld::RemoveFromAwakeSet(std::uint32_t slot) noexcept
        {
            // Swap-remove: move the last element into the vacated position + patch
            // the moved element's back-index.  Idempotent: a slot that is already
            // absent (kNotAwake) or out of range is a no-op.
            if (slot >= m_awakeIndex.size()) { return; }
            const std::uint32_t pos = m_awakeIndex[slot];
            if (pos == kNotAwake) { return; }                  // not in the set
            const std::uint32_t last  = static_cast<std::uint32_t>(m_awakeBodies.size() - 1u);
            const std::uint32_t moved = m_awakeBodies[last];
            m_awakeBodies[pos]  = moved;   // fill the hole with the last element
            m_awakeIndex[moved] = pos;     // patch the moved element's back-index
            m_awakeBodies.pop_back();
            m_awakeIndex[slot]  = kNotAwake;
        }

        // ---- kinematic solver-set maintainers (Phase C, Task 1) -----------------
        //
        // AddToKinematicSet / RemoveFromKinematicSet keep m_kinematicBodies +
        // m_kinematicIndex in sync with the live-kinematic population. They mirror
        // AddToAwakeSet/RemoveFromAwakeSet exactly (idempotent swap-remove with a
        // back-index patch) but gate on BodyType::Kinematic instead of Dynamic, and
        // -- because kinematics never sleep -- they are wired ONLY at the AddBody /
        // RemoveBody seams (there is no sleep/wake path for this set). The solver is
        // NOT rerouted onto this list here (Task 2 does that); this is PURE
        // bookkeeping so behavior stays byte-identical.

        void PhysicsWorld::AddToKinematicSet(std::uint32_t slot) noexcept
        {
            // Only kinematics belong in the set; idempotent (already-present or
            // non-kinematic is a no-op). The size guard handles a slot index not
            // grown into yet (should not occur in normal flow, but guards against
            // out-of-bound reads on a half-initialized world).
            if (slot >= m_kinematicIndex.size()) { return; }
            if (static_cast<BodyType>(m_btype[slot]) != BodyType::Kinematic) { return; }
            if (m_kinematicIndex[slot] != kNotKinematic) { return; }  // already in the set
            m_kinematicIndex[slot] = static_cast<std::uint32_t>(m_kinematicBodies.size());
            m_kinematicBodies.push_back(slot);
        }

        void PhysicsWorld::RemoveFromKinematicSet(std::uint32_t slot) noexcept
        {
            // Swap-remove: move the last element into the vacated position + patch
            // the moved element's back-index. Idempotent: a slot that is already
            // absent (kNotKinematic) or out of range is a no-op.
            if (slot >= m_kinematicIndex.size()) { return; }
            const std::uint32_t pos = m_kinematicIndex[slot];
            if (pos == kNotKinematic) { return; }                  // not in the set
            const std::uint32_t last  = static_cast<std::uint32_t>(m_kinematicBodies.size() - 1u);
            const std::uint32_t moved = m_kinematicBodies[last];
            m_kinematicBodies[pos]  = moved;   // fill the hole with the last element
            m_kinematicIndex[moved] = pos;     // patch the moved element's back-index
            m_kinematicBodies.pop_back();
            m_kinematicIndex[slot]  = kNotKinematic;
        }

        void PhysicsWorld::WakeIsland(std::uint32_t slot) noexcept
        {
            const std::uint32_t isl = IslandOf(slot);
            if (isl == Island::kInvalidIsland)
            {
                return; // static/kinematic -> nothing to wake on itself
            }
            for (const std::uint32_t b : m_islands[isl].bodies)
            {
                m_awake[b]      = 1;
                m_sleepTimer[b] = Real(0);
                AddToAwakeSet(b); // Phase B: migrate every island member back into the awake-set
            }
        }

        std::uint32_t PhysicsWorld::IslandRootOf(std::uint32_t i) const noexcept
        {
            // Phase A: the persistent island id IS the root (equal for all
            // co-island members after merge, distinct across islands). A non-member
            // (static/kinematic, or an un-assigned slot) has no island; return
            // a high-bit-tagged slot so it can never collide with a real island
            // id (ids are dense + small, < 2^31). Consumed by PhysicsDebugDraw
            // (color-by-island, Dynamic only) + the island tests.
            if (i >= m_islandId.size() || m_islandId[i] == Island::kInvalidIsland)
            {
                return i | 0x80000000u;
            }
            return m_islandId[i];
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
