#include <Arcane/Physics/ConstraintGraph.hpp>

#include <Arcane/Physics/Contact.hpp>                // Contact + ContactPool + kInvalidColor
#include <Arcane/Physics/PhysicsWorld.hpp>           // the world SoA + island/awake seams (befriended)
#include <Arcane/Physics/Solver/ContactColoring.hpp> // kColorCount

namespace Arcane
{
    namespace Physics
    {
        ConstraintGraph::ConstraintGraph()
        {
            // Phase C, Task 4: one contact-id list per graph color. Sized once here
            // to kColorCount (overflow contacts are not listed -- they carry
            // color == kInvalidColor and never enter m_colorContacts). m_bodyColorMask
            // grows lazily with the body SoA via Grow (default 0).
            m_colorContacts.resize(static_cast<std::size_t>(kColorCount));
        }

        // ----------------------------------------------------------------
        // Persistent incremental contact coloring (Phase C, Stage 2, Tasks 4-5)
        // ----------------------------------------------------------------
        //
        // A solver-relevant body-body contact is colored ONCE at create and frees
        // its color at destroy -- the incremental replacement for the per-step
        // greedy recolor. Task 5 makes the solver CONSUME this coloring
        // (EmitContactConstraints copies Contact::color onto the emitted constraint;
        // SoftStep buckets by it instead of recoloring), so the color is now
        // load-bearing -- the mask GATES this lowest-free search. This persistent
        // coloring is a DIFFERENT but equally-valid color partition than the old
        // per-step greedy one; because the colored solve is Gauss-Seidel (color k's
        // velocity updates feed color k+1), a different valid partition is an
        // INTENTIONAL re-baseline vs pre-Phase-C main (different floats), NOT
        // bit-identical. The contract that holds is run-twice DETERMINISM + the
        // behavioral [physics] suite (no exact goldens) -- per the engine's
        // re-baseline-numerics-on-purpose rule. ValidatePersistentColoring
        // cross-checks the mask against the lists.

        void ConstraintGraph::AssignContactColor(PhysicsWorld& w, std::uint32_t id,
                                                 std::uint32_t a, std::uint32_t b,
                                                 bool aDyn, bool bDyn)
        {
            // Lowest free color: one whose bit is unset in BOTH dynamic endpoints'
            // masks. A static/kinematic endpoint never blocks (it is read-only in
            // the solve, so sharing it across a color is harmless -- mirrors
            // ColorConstraints' aDyn/bDyn rule).
            int chosen = -1;
            for (int k = 0; k < kColorCount; ++k)
            {
                const std::uint32_t bit = 1u << k;
                const bool aFree = !aDyn || !(m_bodyColorMask[a] & bit);
                const bool bFree = !bDyn || !(m_bodyColorMask[b] & bit);
                if (aFree && bFree) { chosen = k; break; }
            }
            Contact& c = w.m_contactPool.Get(id);
            if (chosen < 0)
            {
                // OVERFLOW: no free color. The contact stays uncolored
                // (kInvalidColor) and is NOT listed in m_colorContacts; the solver
                // (Task 5) will solve it in the scalar tail.
                c.color = kInvalidColor;
                return;
            }
            c.color = static_cast<std::uint8_t>(chosen);
            const std::uint32_t bit = 1u << chosen;
            // Occupy the color bit on each DYNAMIC endpoint only.
            if (aDyn) m_bodyColorMask[a] |= bit;
            if (bDyn) m_bodyColorMask[b] |= bit;
            m_colorContacts[chosen].push_back(id);
        }

        void ConstraintGraph::ReleaseContactColor(PhysicsWorld& w, std::uint32_t id)
        {
            Contact& c = w.m_contactPool.Get(id);
            const std::uint8_t col = c.color;
            if (col == kInvalidColor)
            {
                // Sensor / non-solver / span / overflow contact -- never colored,
                // never in m_colorContacts. Nothing to release.
                return;
            }

            // RATIONALE: the coloring invariant (no two same-color contacts share a
            // dynamic body) means each body has AT MOST ONE contact per color, so
            // clearing the body's bit for this color is exact -- no OTHER live
            // contact of this body occupies the same color, so we never strip a bit
            // a sibling contact still needs.
            //
            // Recompute dyn-ness from the cached body slots (a body's type is fixed
            // for its life -- m_btype is set only in AddBody -- so this matches the
            // aDyn/bDyn passed at AssignContactColor). A SPAN (c.bIsBody == false)
            // has no real B body, so only A can be a dynamic endpoint there.
            const std::uint32_t bit = 1u << col;
            const std::uint32_t bA  = c.bodyA;
            const std::uint32_t bB  = c.bodyB;
            const bool aDyn = (bA != kInvalidSlot) &&
                              (static_cast<BodyType>(w.m_btype[bA]) == BodyType::Dynamic);
            const bool bDyn = c.bIsBody && (bB != kInvalidSlot) &&
                              (static_cast<BodyType>(w.m_btype[bB]) == BodyType::Dynamic);
            if (aDyn) m_bodyColorMask[bA] &= ~bit;
            if (bDyn) m_bodyColorMask[bB] &= ~bit;

            // Swap-remove id from this color's contact list (order-independent --
            // the list is a set, never walked in a determinism-sensitive order).
            // TODO(perf): O(1) swap-remove by a stored per-contact index if destroy
            // frequency shows on a profile.
            std::vector<std::uint32_t>& list = m_colorContacts[col];
            for (std::size_t i = 0; i < list.size(); ++i)
            {
                if (list[i] == id)
                {
                    list[i] = list.back();
                    list.pop_back();
                    break;
                }
            }
            c.color = kInvalidColor;
        }

        std::uint8_t ConstraintGraph::ContactColorOf(const PhysicsWorld& w,
                                                     std::uint32_t id) const
        {
            // Read-only probe (not used by the Step path). Get() asserts liveness
            // in Debug; the caller is expected to pass a live id. A dead/recycled
            // slot carries kInvalidColor (the EnsurePair recycle reset), so the
            // value is meaningful even on the recycled path.
            return w.m_contactPool.Get(id).color;
        }

        bool ConstraintGraph::ValidatePersistentColoring(const PhysicsWorld& w) const
        {
            // For each color, no DYNAMIC body slot may appear in two contacts, and
            // every listed contact must be alive AND tagged with this color. Task 5
            // also cross-checks the per-body color MASK against the lists: now that
            // the mask is load-bearing (it GATES AssignContactColor's lowest-free
            // search), a mask/list divergence would silently corrupt the coloring, so
            // reconstruct the mask from the lists and require it to match m_bodyColorMask
            // bit-for-bit -- bit k is set IFF the slot has exactly one contact in
            // m_colorContacts[k] (uniqueness is enforced by the per-color seen check
            // below; "no stray bits" is enforced by the final equality).
            std::vector<std::uint8_t>  seen;      // per-body-slot "claimed this color"
            std::vector<std::uint32_t> rebuilt(m_bodyColorMask.size(), 0u); // mask from the lists
            for (int k = 0; k < kColorCount; ++k)
            {
                seen.assign(m_bodyColorMask.size(), 0u);
                const std::uint32_t bit = 1u << k;
                const std::vector<std::uint32_t>& list = m_colorContacts[static_cast<std::size_t>(k)];
                for (const std::uint32_t id : list)
                {
                    const Contact& c = w.m_contactPool.Get(id); // asserts alive in Debug
                    if (c.color != static_cast<std::uint8_t>(k))
                    {
                        return false; // listed under the wrong color
                    }
                    const std::uint32_t bA = c.bodyA;
                    const std::uint32_t bB = c.bodyB;
                    const bool aDyn = (bA != kInvalidSlot) &&
                                      (static_cast<BodyType>(w.m_btype[bA]) == BodyType::Dynamic);
                    const bool bDyn = c.bIsBody && (bB != kInvalidSlot) &&
                                      (static_cast<BodyType>(w.m_btype[bB]) == BodyType::Dynamic);
                    if (aDyn)
                    {
                        if (seen[bA] != 0u) return false; // dynamic body twice in one color
                        seen[bA] = 1u;
                        rebuilt[bA] |= bit;
                    }
                    if (bDyn)
                    {
                        if (seen[bB] != 0u) return false;
                        seen[bB] = 1u;
                        rebuilt[bB] |= bit;
                    }
                }
            }
            // The mask must equal what the lists imply -- catches a set bit with no
            // backing contact (a missed ReleaseContactColor) or a contact in a list
            // whose mask bit was never set (a missed AssignContactColor mask write).
            for (std::size_t s = 0; s < m_bodyColorMask.size(); ++s)
            {
                if (m_bodyColorMask[s] != rebuilt[s]) { return false; }
            }
            return true;
        }

        std::size_t ConstraintGraph::ColoredContactCount() const noexcept
        {
            // Sum the per-color lists. Read-only probe (not on the Step path): the
            // [phasec] coloring-validity test asserts this > 0 so the oracle cannot
            // trivially pass on an EMPTY coloring.
            std::size_t n = 0;
            for (const std::vector<std::uint32_t>& list : m_colorContacts)
            {
                n += list.size();
            }
            return n;
        }

        // ---- world-lifecycle seams (PhysicsWorld drives these) ---------------

        void ConstraintGraph::Grow(std::uint32_t next)
        {
            // Phase C, Task 4: per-body color-occupancy bitmask. A fresh/recycled
            // slot starts with NO colors occupied (the RemoveBody leak-detector
            // asserts a removed body left mask 0, so a recycled slot is always 0).
            m_bodyColorMask.resize(next, 0u);
        }
    } // namespace Physics
} // namespace Arcane
