#pragma once

// ISolver: the velocity/position constraint-solver interface (M6, Task P2.1).
//
// PORT + MODERNIZE: the Lua engine routed all dynamics response through a single
// swappable `solver` seam (PhysicsWorld.lua:181 `solver = SequentialImpulse`,
// called once per Step at line 389 `self.solver.solve(self, contacts, nC,
// joints, dt)`). The C++ engine keeps that seam but splits the monolithic
// `solve(...)` into the Box2D-v3 Soft Step sub-step phases so a TGS soft-
// constraint solver (P2.2) and a retained Baumgarte PGS oracle (the A/B cross-
// check) can BOTH implement one interface without the world knowing which is
// installed. A future XPBD experiment can drop in the same way.
//
// SCAFFOLDING NOTE (P2.1): this header defines the SHAPE of the contract only.
// It is NOT called by PhysicsWorld::Step in P2.1 -- there is no implementation
// yet (a dynamic body free-falls ballistically until P2.2 wires a solver into
// the stage 2-3 seam). The Soft Step algorithm is concrete in P2.2; it is
// EXPECTED that P2.2 refines these signatures once the per-substep data layout
// (warm-start impulse storage, soft (hertz, dampingRatio) tuning, the
// contact/joint constraint SoA) is pinned down. The interface is intentionally
// minimal + adjustable; treat it as a design anchor, not a frozen ABI.
//
// SOFT STEP shape (Box2D v3 -- Solver2D / "Solver 2024"): per Step the dt is
// split into `substepCount` sub-steps. The world integrates velocities (gravity
// + damping) before the solver runs (PhysicsWorld::Step stage 1). The solver
// then, per Step:
//   1. PrepareContacts / PrepareJoints -- build the constraint SoA from this
//      step's manifolds + joint list; seed warm-start impulses from the prior
//      step (TGS warm-starting). dt + invDt(substep) cached here.
//   2. for substep in [0, substepCount):
//        SolveVelocity(substep) -- one TGS pass over SOFT contact + joint
//        velocity constraints (biased toward the soft target), warm-started.
//        (Position integration between substeps is owned by the world's stage 4
//        in P2.1's faithful order; P2.2 may pull the per-substep position
//        integrate inside the loop -- a signature it is free to add.)
//        Relax(ctx, substep)    -- one TGS pass with NO bias (removes the energy
//        the biased SolveVelocity injected for this substep; the v3 "relax" stage).
//   3. ApplyRestitution() -- a final pass applying restitution to contacts whose
//      approach speed exceeded the threshold (separated from the soft solve so
//      bounciness is not damped by the soft target).
//   4. SolvePosition() -- optional position correction / NGS-style cleanup for
//      residual penetration (the Soft Step soft contacts handle most of it; this
//      is the safety pass the Baumgarte oracle leans on more heavily).
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.
// No SDL3/NVRHI/Batcher2D/ImGui. namespace Arcane::Physics, Core style.

#include <cstdint>

#include <Arcane/Physics/PhysicsTypes.hpp>

namespace Arcane
{
    namespace Physics
    {
        class PhysicsWorld; // the SoA owner; the solver mutates velocities through it
        struct Manifold;    // Narrowphase/Manifold.hpp -- per-pair contact points
        struct Joint;       // Joints/Joint.hpp (P2.5) -- forward-declared so JointConstraint
                            // can hold a typed pointer without pulling the full definition.

        // ----------------------------------------------------------------
        // ContactConstraint: one solvable contact pair (scaffolding).
        // ----------------------------------------------------------------
        //
        // P2.2 fleshes this out (per-point normal/tangent impulse accumulators
        // for warm-starting, effective masses, the soft (bias, massScale,
        // impulseScale) coefficients, bodyA/bodyB SoA indices). For P2.1 it is a
        // forward-looking placeholder so SolverContext can name an array of them
        // without pulling in the manifold layout. The world will own the backing
        // storage (pooled -> zero steady-state alloc), exactly like the Lua
        // self._contacts scratch.
        struct ContactConstraint
        {
            std::uint32_t bodyA = 0;  // SoA slot index (dynamic)
            std::uint32_t bodyB = 0;  // SoA slot index (dynamic or static/kinematic)
            const Manifold* manifold = nullptr; // this step's manifold for the pair
            // P2.2: warm-start impulses, effective masses, soft coefficients.
        };

        // ----------------------------------------------------------------
        // JointConstraint: one solvable joint (scaffolding).
        // ----------------------------------------------------------------
        //
        // P2.2 replaces this with the soft-constraint joint formulation
        // (Distance/Revolute/Prismatic/Weld/Mouse/Wheel/Motor). P2.1 keeps an
        // opaque handle so PrepareJoints/SolveVelocity have something to name.
        struct JointConstraint
        {
            // P2.2: joint kind + anchors + soft (hertz, dampingRatio) + state.
            // Typed pointer (forward-declared above) so P2.5's joint module (which
            // will define `struct Joint` in Physics/Joints/) slots in without a
            // struct-layout break. A different name in P2.5 is a trivial rename here.
            Joint* joint = nullptr;
        };

        // ----------------------------------------------------------------
        // SolverContext: everything a Soft Step solver needs for one Step.
        // ----------------------------------------------------------------
        //
        // Bundles the world (the SoA the solver reads/writes through), the
        // constraint arrays built this step, the timestep, and the sub-step
        // schedule. Passed by reference to every ISolver phase so the methods
        // stay parameter-light and the world owns all backing storage (no solver-
        // side allocation -> the zero-steady-state-alloc contract holds).
        //
        // PORT mapping: the Lua passed (world, contacts, nC, joints, dt) to
        // solver.solve. This is the same five things, plus the sub-step schedule
        // the Soft Step modernization adds (the Lua single-shot solve had no
        // sub-steps). counts are explicit (nContacts/nJoints) so the arrays can
        // be over-capacity pooled buffers, mirroring nC over self._contacts.
        struct SolverContext
        {
            PhysicsWorld* world = nullptr;

            // Constraint arrays for this step (world-owned, pooled). The *Count
            // fields are the live prefix length (the arrays may be larger).
            ContactConstraint* contacts = nullptr;
            std::uint32_t       contactCount = 0;
            JointConstraint*    joints = nullptr;
            std::uint32_t       jointCount = 0;

            // Timestep + Soft Step schedule.
            Real          dt = Real(0);            // full Step dt
            std::uint32_t substepCount = 4;        // dt split into this many sub-steps
            Real          invDt = Real(0);         // 1/dt (0 if dt==0)
            Real          subDt = Real(0);         // dt / substepCount
            Real          invSubDt = Real(0);      // 1/subDt (0 if subDt==0)

            // Gravity (read for soft-constraint bias terms that reference it).
            Vec2          gravity{ Real(0), Real(0) };
        };

        // ----------------------------------------------------------------
        // ISolver: the swappable constraint-solver contract.
        // ----------------------------------------------------------------
        //
        // Implemented in P2.2 (Solver/SoftStep.hpp/.cpp -- the modernization
        // centerpiece) and by a retained Baumgarte PGS oracle (the A/B cross-
        // check, validated against the same dynamics invariants). PhysicsWorld
        // holds one ISolver and drives the phase sequence from Step's stage 2-3
        // seam. Phases are separate (not one solve()) so the world owns the
        // sub-step loop ordering and so a profiler/oracle can step phase by phase.
        class ISolver
        {
        public:
            virtual ~ISolver() = default;

            // Build the contact-constraint SoA from this step's manifolds and
            // seed warm-start impulses from the prior step. Caches per-step
            // scalars (invDt, soft coefficients). Ports the prepare half of the
            // Lua solve()'s contact loop.
            virtual void PrepareContacts(SolverContext& ctx) = 0;

            // Build the joint-constraint SoA + warm-start. Ports the joint prep.
            virtual void PrepareJoints(SolverContext& ctx) = 0;

            // One Temporal-Gauss-Seidel velocity pass for sub-step `substep`
            // (contacts + joints), biased toward the SOFT target and warm-
            // started. Called substepCount times per Step.
            virtual void SolveVelocity(SolverContext& ctx, int substep) = 0;

            // One velocity pass with NO bias (the v3 "relax" stage): removes the
            // energy the biased SolveVelocity injected for sub-step `substep` so
            // soft contacts do not gain energy. `substep` matches the index passed
            // to the immediately preceding SolveVelocity call, allowing the relax
            // pass to cancel exactly the bias term that substep introduced.
            // Called once per sub-step after SolveVelocity.
            virtual void Relax(SolverContext& ctx, int substep) = 0;

            // Final pass: apply restitution to contacts whose approach speed
            // exceeded the threshold (kept out of the soft solve so bounce is not
            // damped by the soft target). Called once per Step.
            virtual void ApplyRestitution(SolverContext& ctx) = 0;

            // Optional position-correction / NGS cleanup for residual
            // penetration (soft contacts handle most; this is the safety pass the
            // Baumgarte oracle relies on). Called once per Step.
            virtual void SolvePosition(SolverContext& ctx) = 0;
        };

    } // namespace Physics
} // namespace Arcane
