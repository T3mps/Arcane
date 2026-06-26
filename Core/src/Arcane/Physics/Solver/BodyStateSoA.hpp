#pragma once

// BodyStateSoA: solver-local packed body state for the SIMD constraint solver.
//
// The lane-wide (graph-colored, SoA) solve gathers/scatters body velocities by
// the constraint's body-index lanes. The world stores velocity as a per-
// component Vec2 SoA (m_velX/m_velY/m_angVel) that is NOT gather-friendly, so
// per Step we mirror a PACKED copy of the body state into the parallel float
// arrays below, indexed BY SOLVERINDEX -- a DENSE per-step index space (Phase C,
// Task 2), NOT the sparse world slot. The solver sizes them to solverCount+1
// where solverCount = AwakeCount() + KinematicCount():
//   * awake dynamics occupy [0, AwakeCount())              at AwakeIndexOf(slot)
//   * kinematics    occupy [AwakeCount(), solverCount)     at AwakeCount()+KinematicIndexOf(slot)
//   * statics / spans / padding map to the shared zero DUMMY tail at solverCount.
// The "+1" tail (index solverCount) is the SCATTER-SAFE DUMMY that padding +
// static/span B lanes gather/scatter through; see ContactConstraintSimd Build's
// SCATTER-SAFE DUMMY note. The packed scratch is dense (no per-world-slot holes),
// so the gathers stay cache-local (the WIN -- not gather elimination).
//
// SCALAR CHOICE: these arrays are `float` (NOT Real) deliberately -- they feed
// the SIMD f32w path directly. Vec2/Real/std::uint32_t come from PhysicsTypes.
//
// SCOPE: the struct + its SyncIn/SyncOut bridge. SoftStep::Solve consumes it (the
// lane-wide colored SoA contact solve gathers/scatters through it). The sync
// helpers are DECLARED here (lightweight: forward-declare PhysicsWorld, include
// only PhysicsTypes) and DEFINED in SoftStep.cpp where the PhysicsWorld accessors
// are in scope.
//
// PRESENTATION-FREE + C++20-clean.

#include <cstdint>
#include <vector>

#include <Arcane/Physics/PhysicsTypes.hpp>

namespace Arcane
{
    namespace Physics
    {
        class PhysicsWorld;

        // Solver-local packed body state, indexed BY SOLVERINDEX -- a DENSE per-step
        // index space (Phase C, Task 2), NOT the sparse world slot. The solver sizes
        // it to solverCount+1 (= AwakeCount()+KinematicCount()+1); the extra tail slot
        // is the SCATTER-SAFE DUMMY padding + static/span B lanes target so an unmasked
        // scatter never clobbers a real body. The lane-wide solve gathers/scatters
        // these by the constraint's (solverIndex) body-index lanes; the world Vec2 SoA
        // is not gather-friendly, so we mirror a DENSE packed copy per Step.
        struct BodyStateSoA
        {
            std::vector<float> vx, vy, w;     // velocity (lin x/y, ang)
            std::vector<float> dpx, dpy, dq;  // accumulated position/angle delta (TGS)

            void Resize(std::uint32_t n)
            {
                vx.assign(n, 0.f); vy.assign(n, 0.f); w.assign(n, 0.f);
                dpx.assign(n, 0.f); dpy.assign(n, 0.f); dq.assign(n, 0.f);
            }

            // Declared here, defined in SoftStep.cpp (needs the PhysicsWorld
            // accessors).
            //
            // SyncInCompacted (Phase C, Task 2): fill the DENSE scratch -- awake
            // dynamics at AwakeIndexOf(slot) (vel mirrored, dp/dq zeroed), kinematics
            // at AwakeCount()+KinematicIndexOf(slot) (vel mirrored; dp/dq stay zero,
            // they never integrate). Statics have no row (they map to the dummy tail,
            // which Resize already zeroed). The caller Resize()s to solverCount+1.
            //
            // SyncOut writes awake dynamics' velocity back to the world (kinematics are
            // read-only -> never written). The caller passes the same dense index map
            // by re-deriving AwakeIndexOf(slot) per awake slot.
            //
            // SyncIn (legacy, world-slot-indexed): the original sparse fill -- world
            // vel -> vx/vy/w by WORLD SLOT; dp/dq zeroed for awake dynamics. NO LONGER
            // on the solver hot path (SyncInCompacted replaced it in Solve), but kept
            // for the standalone SyncIn/SyncOut round-trip contract test which pins the
            // world-slot bridge in isolation. Caller Resize()s to world.Count() first.
            void SyncIn(const PhysicsWorld& world);          // legacy world-slot fill
            void SyncInCompacted(const PhysicsWorld& world); // dense fill (solverIndex)
            void SyncOut(PhysicsWorld& world) const;         // legacy world-slot vel write-back
            // Dense write-back (Phase C, Task 2): awake dynamics' vel at
            // AwakeIndexOf(slot) -> world (kinematics read-only -> never written).
            void SyncOutCompacted(PhysicsWorld& world) const;
        };
    } // namespace Physics
} // namespace Arcane
