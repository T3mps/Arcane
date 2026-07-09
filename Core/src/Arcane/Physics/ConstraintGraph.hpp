#pragma once

// ConstraintGraph: owns the CONTACT SUBSYSTEM -- the persistent fixture-pair
// contact pool, the incremental solver contact-coloring (per-body color masks +
// per-color contact-id lists), and the per-Step narrowphase/create/emit driving
// (UpdateContacts / EmitContactConstraints + their MT scratch). Extracted from
// PhysicsWorld as a contained collaborator (PhysicsWorld decomposition step 2,
// 2026-07-09), mirroring Box2D v3's contact subsystem boundary: contact.c +
// constraint_graph.c + the b2Collide driver in world.c form one cohesive unit
// whose only outward seams are island link/unlink, event push, and wake. Distinct
// from:
//   * IslandManager -- island TOPOLOGY + the sleep pass (decomp step 1); the graph
//     reaches it only through PhysicsWorld's existing island forwarders + the
//     public IslandManager API (Attach/DetachContactAdjacency), never directly.
//   * ContactManager -- the begin/stay/end EVENT machine; a downstream consumer of
//     the graph's derived touched-pair list, never sees fixtures/manifolds/colors.
//   * The solver -- consumes the EMITTED ContactConstraint array only (bucketed by
//     the constraint's own `color` field); it never touches the pool or the
//     coloring state and never learns the graph exists (ISolver seam preserved).
//
// STAYS WORLD-OWNED (hot hand-off buffers + shared hot SoA -- resist wrapping):
//   * m_contactConstraints -- the emitted solver feed; SolverContext captures its
//     raw .data() pointer ONCE per Step (the contact-side twin of the
//     AwakeIndexData() handoff). The graph fills a caller-provided vector.
//   * m_touchedEventPairs -- the stage-output buffer handed to ContactManager.
//   * The body/fixture SoA, awake/sleep state + awake-set mechanism, broadphase,
//     and executor -- reached through PhysicsWorld& w under friendship.
//
// DELEGATION: like IslandManager, ConstraintGraph owns its state outright and
// takes PhysicsWorld& per method; PhysicsWorld befriends it (the SAME trust
// boundary this code had when it lived inside the world -- UpdateContacts /
// TryCreateContact / EmitContactConstraints need intimate world SoA). Friendship
// rays stay world -> collaborator only: IslandManager reads the pool through the
// public const Pool() accessor, not friendship.
//
// DETERMINISM (frozen contracts -- see pwdecomp2 surveys 2/3): the create order ->
// EnsurePair LIFO id recycling -> AssignContactColor lowest-free search ->
// ascending-color Gauss-Seidel chain; the MixContactId warm-start keying; the
// wake-before-create order (WakeMoverPair); the merge drain at the END of
// UpdateContacts. All moved verbatim; byte-identity gated.
//
// PRESENTATION-FREE + C++23-clean: std + sibling Physics headers only.
// namespace Arcane::Physics, Core style.

namespace Arcane
{
    namespace Physics
    {
        class PhysicsWorld; // ConstraintGraph reaches the world SoA (befriended).

        class ConstraintGraph
        {
            // T1: scaffold only -- state + methods move in T2 (coloring) and
            // T3 (pool + lifecycle + driver + feed).
        };
    } // namespace Physics
} // namespace Arcane
