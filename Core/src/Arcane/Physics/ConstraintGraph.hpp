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

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Arcane
{
    namespace Physics
    {
        class PhysicsWorld; // ConstraintGraph reaches the world SoA (befriended).

        class ConstraintGraph
        {
        public:
            // Sizes the per-color contact-id lists once to kColorCount (overflow
            // contacts are not listed -- they carry color == kInvalidColor and
            // never enter m_colorContacts). The per-body mask grows via Grow.
            ConstraintGraph();

            // ---- persistent incremental contact coloring (Phase C, Task 4;
            //      moved here in decomp step 2 Task 2) -------------------------
            //
            // A solver-relevant body-body contact is assigned a graph color ONCE
            // at create (AssignContactColor, from TryCreateContact) and releases
            // it at destroy (ReleaseContactColor, from ReleaseAndDestroyContact)
            // -- the assign-at-create / release-at-destroy replacement for the
            // per-step greedy recolor. The invariant: no two same-color contacts
            // share a DYNAMIC body (a static/kinematic endpoint never constrains
            // coloring, mirroring ColorConstraints' aDyn/bDyn rule). The solver
            // consumes the coloring only via the emitted constraint's own `color`
            // field (bucketed into SoftStep's m_colorRefs); it never reads this
            // state -- m_colorContacts is lifecycle/validation bookkeeping.

            // Assign the lowest free color to a NEW solver-relevant body-body
            // contact `id` between body slots `a`/`b`. `aDyn`/`bDyn` mark which
            // endpoints are dynamic (only dynamic endpoints constrain coloring +
            // occupy a per-body color bit). If no color in [0, kColorCount) is
            // free for both dynamic endpoints, the contact spills to overflow
            // (color == kInvalidColor). Caller passes the ORIENTED slots/dyn
            // flags computed in TryCreateContact.
            void AssignContactColor(PhysicsWorld& w, std::uint32_t id,
                                    std::uint32_t a, std::uint32_t b,
                                    bool aDyn, bool bDyn);

            // Release contact `id`'s color back to its dynamic endpoints. No-op
            // for an uncolored (kInvalidColor) contact. Recomputes dyn-ness from
            // the contact's cached body slots (a body's type is fixed for its
            // life), so this is exact: the coloring invariant guarantees a body
            // has at most one contact per color, so clearing the bit on destroy
            // frees it cleanly.
            void ReleaseContactColor(PhysicsWorld& w, std::uint32_t id);

            // The persistent color of contact `id` (kInvalidColor if uncolored).
            // Read-only probe; not used by the Step path.
            [[nodiscard]] std::uint8_t ContactColorOf(const PhysicsWorld& w,
                                                      std::uint32_t id) const;

            // Oracle: walk the live coloring and prove the invariant -- no
            // DYNAMIC body appears twice in one color, every listed contact is
            // alive and tagged with its color, and the per-body mask matches the
            // lists bit-for-bit. Used by the [phasec] coloring-validity test.
            [[nodiscard]] bool ValidatePersistentColoring(const PhysicsWorld& w) const;

            // Read-only probe: the total number of COLORED contacts (the sum of
            // m_colorContacts[k].size() over all colors).
            [[nodiscard]] std::size_t ColoredContactCount() const noexcept;

            // ---- world-lifecycle seams (PhysicsWorld drives these) -----------
            // Grow the per-body color-mask column to `next` (EnsureCapacity seam).
            // A fresh/recycled slot starts with NO colors occupied.
            void Grow(std::uint32_t next);
            // Debug probe for the RemoveBody leak-detector assert: true iff the
            // slot's color mask is 0 (every contact referencing it released its
            // bits). Out-of-range counts as clear (mirrors the original guard).
            [[nodiscard]] bool DebugBodyMaskClear(std::uint32_t slot) const noexcept
            {
                return slot >= m_bodyColorMask.size() || m_bodyColorMask[slot] == 0u;
            }

        private:
            // m_bodyColorMask[slot] is a 12-bit (one bit per color, kColorCount<32)
            // occupancy mask over the colors that body slot currently has a
            // contact in. Keyed by WORLD body slot; a removed/recycled slot resets
            // to 0 (Grow defaults new slots to 0; the RemoveBody leak-detector
            // asserts a removed body left mask 0). m_colorContacts[k] is the list
            // of contact ids assigned to color k (sized kColorCount in the ctor).
            // Maintained at create/destroy; the mask GATES AssignContactColor's
            // lowest-free search. The persistent coloring is a DIFFERENT but
            // equally-valid color partition than the old per-step greedy one, so
            // the colored Gauss-Seidel solve is an INTENTIONAL re-baseline vs
            // pre-Phase-C main (different floats), NOT bit-identical. The contract
            // is run-twice DETERMINISM + the behavioral [physics] suite (no exact
            // goldens) -- per the engine's re-baseline-numerics-on-purpose rule.
            std::vector<std::uint32_t>              m_bodyColorMask;
            std::vector<std::vector<std::uint32_t>> m_colorContacts;
        };
    } // namespace Physics
} // namespace Arcane
