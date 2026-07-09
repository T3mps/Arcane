#pragma once

// IslandManager: owns island TOPOLOGY -- the island registry (roots + free list),
// merge/split linkage, split-linkage contact adjacency, and the sleep-pass
// decision. Extracted from PhysicsWorld as a contained collaborator (PhysicsWorld
// decomposition step 1, 2026-07-09), mirroring Box2D v3's b2Island (connectivity
// topology + the sleep trigger) as distinct from:
//   * b2ConstraintGraph -- the coloring / solve partition, which stays with the
//     contact pool for now (a future Step-2 collaborator), and
//   * b2SolverSet -- the hot awake/kinematic sets, which STAY world-owned as flat
//     SoA: m_awakeIndex[slot] is read per-body every substep (and handed as a raw
//     pointer into the SIMD packer), and the awake-set element ORDER feeds
//     persistent contact coloring -- a determinism tripwire. "Behavior over
//     shared hot SoA -> keep flat" (the data-oriented design constraint).
//
// DELEGATION: like ContactManager, IslandManager owns its state outright and takes
// PhysicsWorld& per method to reach the flat body/fixture/contact SoA + the
// awake-set MECHANISM (AddToAwakeSet/RemoveFromAwakeSet) it does not own. Unlike
// ContactManager's narrow public "port seam", the island logic (SplitIsland) needs
// intimate world state (the contact pool + joint edges), so PhysicsWorld befriends
// IslandManager -- the SAME trust boundary this code had when it lived inside the
// world. This keeps the extraction a verbatim, byte-identity-preserving move
// rather than a new public accessor surface.
//
// PRESENTATION-FREE + C++20-clean: std + sibling Physics headers only.
// namespace Arcane::Physics, Core style.

#include <cstdint>
#include <vector>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Island.hpp> // Island registry struct + constants (Phase A)

namespace Arcane
{
    namespace Physics
    {
        class PhysicsWorld; // IslandManager reaches the world SoA (befriended).

        // Step 1 scaffold: the class exists + is owned by PhysicsWorld; no state
        // or logic has moved yet. Subsequent decomposition tasks relocate the
        // island registry, split-linkage adjacency, and the sleep pass here one
        // byte-identical commit at a time.
        class IslandManager
        {
        };
    } // namespace Physics
} // namespace Arcane
