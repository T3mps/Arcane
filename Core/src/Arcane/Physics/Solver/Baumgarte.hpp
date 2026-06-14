#pragma once

// Baumgarte: the retained sequential-impulse PGS oracle (M6, Task P2.3).
//
// PORT -- NOT a modernization. This is a faithful port of the Lua engine's
// default solver, Client/src/physics/SequentialImpulse.lua: velocity iterations
// with accumulated, warm-started normal + friction impulses and the Baumgarte
// positional bias FOLDED INTO the velocity solve. It is the A/B cross-check
// behind the ISolver seam (the modernization centerpiece, SoftStep, is the
// other impl). Running BOTH solvers over the same scenes guards against
// solver-specific bugs: a bug in one solver's contact handling shows up as that
// solver failing a stability invariant the other passes.
//
// PORT MAP (SequentialImpulse.lua -> this file):
//   BETA       = 0.2   positional-correction factor (bias = BETA/dt * pen).
//   SLOP       = 0.5   penetration tolerance before the bias kicks in.
//   REST_VEL   = 20    restitution only above this approach speed.
//   CACHE_LIFE = 2     steps a warm-start entry survives unused.
//   velIters           WorldDef::velIters (Lua w.velIters, default 8).
//
// INTEGRATION OWNERSHIP (since P2.2 the world's Step no longer integrates
// dynamics inline -- see Solver.hpp): Solve() is SINGLE-STEP and owns dynamic
// integration over the FULL dt:
//   1. integrate dynamic velocities (gravity + linear damping), awake only.
//   2. prepare constraints (effective normal/tangent masses, friction, the
//      restitution target, the Baumgarte bias) + warm-start from the cache.
//   3. velIters Gauss-Seidel passes (joints [P2.5 stub] then contacts):
//      normal impulse with accumulated >=0 clamp, friction in the Coulomb cone.
//   4. integrate dynamic positions (pos += dt*v, angle += dt*w), awake only,
//      committing the mover broadphase AABB.
//   5. persist + evict the warm-start cache (CACHE_LIFE).
//
// CONSUMES the SAME world-generated raw ContactConstraint array as SoftStep
// (GenerateContacts is solver-agnostic). Baumgarte does its OWN prepare:
// effective masses (normalMass/tangentMass) + the Baumgarte bias are recomputed
// here from the raw geometry; SoftStep's soft (biasRate/massScale/impulseScale)
// fields are IGNORED. The shared accumulated-impulse fields (normalImpulse /
// tangentImpulse) carry warm-start state both solvers use.
//
// DETERMINISM: fixed contact + point iteration order over the world-built
// (already sorted) ContactConstraint array; the warm-start cache is keyed by the
// stable manifold-point id and seeded by find(id) (never by map-iteration order),
// so iteration over the cache for eviction is unobservable. No wall-clock; no
// fast-math (the workspace builds /fp:precise). The per-solver cache is bounded
// (stamp-based eviction at CACHE_LIFE).
//
// PRESENTATION-FREE + C++20-clean: glm::vec2 + std + sibling Physics headers
// only. No SDL3/NVRHI/ImGui. namespace Arcane::Physics, Core style.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Solver/Solver.hpp>

namespace Arcane
{
    namespace Physics
    {
        // ----------------------------------------------------------------
        // Baumgarte: ISolver implementation (Lua SequentialImpulse PGS port).
        // ----------------------------------------------------------------
        class Baumgarte final : public ISolver
        {
        public:
            Baumgarte() = default;
            ~Baumgarte() override = default;

            // ---- ISolver entry (the single per-Step driver) ----------------
            //
            // Single-step sequential-impulse solve that OWNS dynamic integration
            // (integrate velocities full-dt -> prepare + warm-start -> velIters
            // contact passes with the Baumgarte bias -> integrate positions
            // full-dt -> persist/evict the warm-start cache).
            void Solve(SolverContext& ctx) override;

            // Drop a body's warm-start state on slot recycle (RemoveBody). Stale
            // entries also self-evict by stamp (CACHE_LIFE); this keeps the
            // contract explicit for a recycled slot.
            void DropBody(std::uint32_t slot) override;

            // Current warm-start cache entry count (ISolver inspection hook).
            [[nodiscard]] std::size_t WarmStartCacheSize() const noexcept override
            {
                return m_cache.size();
            }

        private:
            // Integrate awake-dynamic velocities (gravity + linear damping) over
            // the full dt. Ports the Lua world stage-1 dynamic branch (now owned
            // by the solver). Pinned dynamics (invMass == 0) never integrate.
            void IntegrateVelocities(SolverContext& ctx);

            // Integrate awake-dynamic positions (pos += dt*v, angle += dt*w) over
            // the full dt + refresh the mover broadphase AABB.
            void IntegratePositions(SolverContext& ctx);

            // ---- per-Step prepared scratch (reused; zero steady-state alloc) -
            //
            // Mirrors the fields the Lua stashed on each contact ROW (ct.massN/
            // massT/restVel/bias/jn/jt) -- kept here, NOT on the shared
            // ContactConstraint, so the world pool stays solver-agnostic. Sized
            // to ctx.contactCount each Step (clear()+resize preserves capacity).
            struct PointPrep
            {
                Real massN   = Real(0);
                Real massT   = Real(0);
                Real restVel = Real(0);
                Real bias    = Real(0);
                Real jn      = Real(0); // accumulated normal impulse (warm-started)
                Real jt      = Real(0); // accumulated tangent impulse (warm-started)
            };
            struct ConstraintPrep
            {
                PointPrep pt[2]{};
                int       pointCount = 0;
            };
            std::vector<ConstraintPrep> m_prep;

            // ---- warm-start cache (per-solver; bounded) --------------------
            //
            // Keyed by the stable manifold-point id (the same key SoftStep uses;
            // the Lua keyOf packed pair+id, but cp.id is already a stable per-
            // point identity). Holds the last step's accumulated (normal,
            // tangent) impulse + a stamp; entries unused for more than
            // kCacheLife stamps are evicted in Solve so the map stays bounded.
            struct CacheEntry
            {
                Real          normalImpulse  = Real(0);
                Real          tangentImpulse = Real(0);
                std::uint32_t stamp          = 0;
            };
            std::unordered_map<std::uint32_t, CacheEntry> m_cache;
            std::uint32_t m_stamp = 0;
        };

    } // namespace Physics
} // namespace Arcane
