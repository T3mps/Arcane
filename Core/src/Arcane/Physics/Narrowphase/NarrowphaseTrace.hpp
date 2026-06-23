#pragma once

// NarrowphaseKind: which narrowphase path produced a contact manifold.
//
// A pure display/inspection tag (debug-visualization Slice A). It is written
// once per Collide() branch at the point that path resolves a manifold, carried
// onto the Manifold, and copied to the ContactConstraint by GenerateContacts'
// emit. NOTHING in the Step path reads it -- it never changes simulation
// behavior (determinism is unaffected).
//
// `Separated` is the default: no contact was produced (an empty manifold leaves
// this value), so a ContactConstraint that still reads `Separated` means the
// kind was never set by a producing branch.
//
// This task adds the ENUM ONLY. The richer per-call trace struct lands in a
// later debug-viz task.
//
// PRESENTATION-FREE + C++20-clean: <cstdint> only. Compiles both /MD
// (Arcane.dll) and static-CRT (ArcaneCore server flavor).

#include <cstdint>

namespace Arcane
{
    namespace Physics
    {
        enum class NarrowphaseKind : std::uint8_t
        {
            Separated = 0,    // no contact (empty manifold; default)
            CircleCircle,     // round path, both cores are points (circles)
            CircleVsPolygon,  // round path, one circle core vs a polygon core
            Capsule,          // round path involving a 2-vert segment (capsule)
            SatPolygon,       // poly-poly SAT reference-face clip
            Epa,              // deep round overlap resolved by EPA
            Mpr,              // deep round overlap resolved by the MPR fallback
        };
    } // namespace Physics
} // namespace Arcane
