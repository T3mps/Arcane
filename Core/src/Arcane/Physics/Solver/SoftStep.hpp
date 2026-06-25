#pragma once

// SoftStep: the Box2D-v3-class TGS Soft constraint solver (M6, Task P2.2).
//
// MODERNIZATION CENTERPIECE -- NOT a port. The Lua engine's solver
// (SequentialImpulse.lua) is a Baumgarte sequential-impulse PGS solver; it is
// retained SEPARATELY as the A/B cross-check oracle in P2.3. SoftStep is a
// from-scratch implementation of the Box2D v3 "Solver2D" / "TGS Soft"
// algorithm (https://box2d.org/posts/2024/02/solver2d/,
// https://box2d.org/posts/2024/08/releasing-box2d-3.0/). It is validated by
// BEHAVIORAL INVARIANTS (a stack settles, a ball rests, restitution rebounds,
// friction stops a slide, kinematic pushes dynamic, energy stays bounded,
// determinism holds), NOT bit-matched to the Lua.
//
// ALGORITHM (per full Step(dt), substepCount sub-steps, h = dt/substepCount):
//   1. PrepareContacts (once): per contact point compute anchors, normal +
//      tangent effective masses, the rest-relative normal velocity (for
//      restitution), and the SOFT (biasRate, massScale, impulseScale) from
//      b2MakeSoft(contactHertz, dampingRatio, h). Warm-start impulses are NOT
//      seeded here -- they arrive already set on each ContactConstraintPoint
//      (EmitContactConstraints copies them off the persistent Contact's
//      manifold point). PrepareContacts must NOT reset them.
//   2. for substep in [0, substepCount):
//        a. integrate velocities (gravity + linear damping) for awake dynamics
//        b. warm start: apply accumulated normal + tangent impulses
//        c. SolveVelocity(useBias=true): TGS soft normal solve (re-evaluate
//           separation from the per-body position deltas accumulated this step)
//           + friction
//        d. integrate positions: accumulate deltaPos += h*v, deltaRot += h*w
//        e. Relax(useBias=false): same solve with bias=0, massScale=1,
//           impulseScale=0 -- removes the bias-injected energy
//   3. ApplyRestitution (once): add restitution impulse for points whose
//      approach speed exceeded the threshold.
// The converged per-point impulses are NOT stored by the solver -- after Solve()
// returns, PhysicsWorld::Step writes each ContactConstraintPoint's accumulated
// (normal, tangent) impulse back onto its source persistent Contact's manifold
// point (via ContactConstraint::sourceContactId), where they persist to next
// step. This replaced the old solver-owned m_cache (a behavior-preserving
// refactor that de-risks the SIMD rewrite).
// At the end the solver commits each awake dynamic body's accumulated deltaPos
// /deltaRot to the world position/angle (the world's stage-4 inline dynamic
// position integrate is SUPERSEDED -- the solver owns dynamic integration).
//
// The world drives the phase sequence (PhysicsWorld::Step). The world owns the
// ContactConstraint pool AND the warm-start impulses (on the persistent Contact);
// the solver owns only the per-body sub-step scratch (deltaPos/deltaRot), reused
// across steps -> zero steady-state allocation. Determinism: fixed iteration order by stable slot
// index over the world-built (already sorted) contact array; no wall-clock; no
// fast-math (the workspace builds /fp:precise).
//
// PRESENTATION-FREE + C++20-clean: glm::vec2 + std + sibling Physics headers
// only. No SDL3/NVRHI/ImGui. namespace Arcane::Physics, Core style.

#include <cstdint>
#include <vector>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Solver/Solver.hpp>

namespace Arcane
{
    namespace Physics
    {
        // ----------------------------------------------------------------
        // SoftStep: ISolver implementation (Box2D v3 TGS Soft).
        // ----------------------------------------------------------------
        class SoftStep final : public ISolver
        {
        public:
            SoftStep() = default;
            ~SoftStep() override = default;

            // ---- ISolver entry (the single per-Step driver) ----------------
            //
            // Runs the full TGS Soft pipeline (Prepare -> sub-step loop[integrate-
            // vel, warm-start, solve, integrate-pos, relax] -> restitution -> store
            // impulses -> commit positions). The solver owns the dynamic velocity
            // + position integration the v3 algorithm folds into the sub-steps; the
            // world just calls Solve once per Step. Behavior is byte-identical to
            // the P2.2 implementation (the P2.3 ISolver slimming was a pure
            // interface refactor -- this method's body did not change).
            void Solve(SolverContext& ctx) override;

            // Drop a body's warm-start state on slot recycle (RemoveBody). SoftStep
            // keeps NO solver-side cache anymore -- warm-start impulses live on the
            // persistent Contact, which the broadphase destroys when a fixture/body
            // is removed (DestroyContactsForBody/Fixture), so a recycled slot
            // inherits nothing. This override stays a no-op purely to honor the
            // ISolver contract. (WarmStartCacheSize() is intentionally NOT
            // overridden: it falls back to ISolver's default 0 -- there is no cache
            // to report. The hook + PhysicsWorld::SolverWarmStartCacheSize remain
            // for Baumgarte, which still keeps its own m_cache.)
            void DropBody(std::uint32_t slot) override;

        private:
            // ---- per-Step phase helpers (private since P2.3) ---------------
            //
            // These WERE the P2.1 ISolver phase pure-virtuals; Solve() above
            // sequences them (it interleaves SolveContacts with the velocity +
            // position integration the v3 algorithm folds into the sub-step loop).
            // For the P2.3 A/B seam ISolver was slimmed to Solve()+DropBody(), so
            // they are now SoftStep-private non-virtual helpers (no `override`):
            // SoftStep owns its own phase ordering internally.
            void PrepareContacts(SolverContext& ctx);
            void PrepareJoints(SolverContext& ctx);            // joint Prepare (subDt bias)
            void SolveJoints(SolverContext& ctx);              // one joint velocity pass (per sub-step)
            void SolveVelocity(SolverContext& ctx, int substep);
            void Relax(SolverContext& ctx, int substep);
            void ApplyRestitution(SolverContext& ctx);
            void SolvePosition(SolverContext& ctx);            // no-op (soft handles it)

            // Ensure the per-body sub-step scratch is sized for `n` slots.
            void EnsureScratch(std::uint32_t n);

            // Integrate awake-dynamic velocities (gravity + linear damping) for
            // one sub-step of length h. Ports the Lua stage-1 dynamic branch,
            // moved INTO the sub-step loop (Box2D v3 form).
            void IntegrateVelocities(SolverContext& ctx, Real h);

            // Warm start: apply the accumulated impulses to body velocities.
            void WarmStart(SolverContext& ctx);

            // One velocity pass. useBias selects the biased soft solve (true,
            // the SolveVelocity stage) vs the relax pass (false: bias=0,
            // massScale=1, impulseScale=0). h is the sub-step length (for the
            // speculative-contact bias rate). Always runs friction.
            void SolveContacts(SolverContext& ctx, Real h, bool useBias);

            // Integrate awake-dynamic positions for one sub-step: accumulate the
            // per-body deltaPos/deltaRot the soft solve re-reads next sub-step.
            void IntegratePositions(SolverContext& ctx, Real h);

            // Commit the accumulated deltaPos/deltaRot onto the world's
            // position/angle + refresh the mover broadphase (end of Step).
            void FinalizePositions(SolverContext& ctx);

            // ---- per-body sub-step scratch (SoA-indexed; reused) -----------
            //
            // deltaPos/deltaRot accumulate this Step's motion across sub-steps
            // (TGS: positions are tracked as deltas so the soft solve can
            // re-evaluate the current separation each sub-step, then committed
            // once at the end). Sized to the world's high-water slot count.
            std::vector<Vec2> m_deltaPos;
            std::vector<Real> m_deltaRot;
        };

    } // namespace Physics
} // namespace Arcane
