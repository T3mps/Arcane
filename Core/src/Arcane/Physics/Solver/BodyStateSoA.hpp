#pragma once

// BodyStateSoA: solver-local packed body state for the SIMD constraint solver.
//
// The lane-wide (graph-colored, SoA) solve gathers/scatters body velocities by
// the constraint's body-index lanes. The world stores velocity as a per-
// component Vec2 SoA (m_velX/m_velY/m_angVel) that is NOT gather-friendly, so
// per Step we mirror a PACKED copy of the awake-dynamic body state into the
// parallel float arrays below, indexed BY WORLD SLOT (sized to world.Count()).
//
// SCALAR CHOICE: these arrays are `float` (NOT Real) deliberately -- they feed
// the SIMD f32w path directly. Vec2/Real/std::uint32_t come from PhysicsTypes.
//
// SCOPE (Task 1): the struct + its SyncIn/SyncOut bridge. NOTHING consumes it
// yet; the solver wiring is a later task. The sync helpers are DECLARED here
// (lightweight: forward-declare PhysicsWorld, include only PhysicsTypes) and
// DEFINED in SoftStep.cpp where the PhysicsWorld accessors are in scope.
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

        // Solver-local packed body state, indexed BY WORLD SLOT (sized to
        // world.Count()). The lane-wide solve gathers/scatters these by the
        // constraint's body-index lanes; the world Vec2 SoA is not gather-
        // friendly, so we mirror a packed copy per Step.
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
