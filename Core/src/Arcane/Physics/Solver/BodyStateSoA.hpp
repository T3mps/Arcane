#pragma once

// BodyStateSoA: solver-local packed body state for the SIMD constraint solver.
//
// The lane-wide (graph-colored, SoA) solve gathers/scatters body velocities by
// the constraint's body-index lanes. The world stores velocity as a per-
// component Vec2 SoA (m_velX/m_velY/m_angVel) that is NOT gather-friendly, so
// per Step we mirror a PACKED copy of the awake-dynamic body state into the
// parallel float arrays below, indexed BY WORLD SLOT (the solver sizes them to
// world.Count()+1 -- the extra "+1" tail slot is the SCATTER-SAFE DUMMY that
// padding + read-only-B lanes gather/scatter through; see ContactConstraintSimd
// Build's SCATTER-SAFE DUMMY note).
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

        // Solver-local packed body state, indexed BY WORLD SLOT (the solver sizes
        // it to world.Count()+1 -- the extra tail slot is the SCATTER-SAFE DUMMY
        // padding + read-only-B lanes target so an unmasked scatter never clobbers a
        // real body). The lane-wide solve gathers/scatters these by the constraint's
        // body-index lanes; the world Vec2 SoA is not gather-friendly, so we mirror a
        // packed copy per Step.
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
            // accessors). The caller Resize()s to world.Count() first.
            void SyncIn(const PhysicsWorld& world);   // world vel -> vx/vy/w; dp/dq = 0
            void SyncOut(PhysicsWorld& world) const;  // vx/vy/w -> world vel (awake dynamics)
        };
    } // namespace Physics
} // namespace Arcane
