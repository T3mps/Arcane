// Island.cpp -- the constraint-graph + sleep module (Phase A).
//
// See Island.hpp for the contract. This TU owns the per-island sleep pass
// (UpdateSleep). The old per-step union-find graph build + O(n^2) scan have
// been replaced by a per-island O(island) pass over the PERSISTENT registry
// (members already known -- no UF rebuild, no global scan).
//
// Wake paths live in PhysicsWorld (UpdateContacts wake-on-contact +
// ApplyImpulse/SetVelocity/Wake wake-on-force).
//
// PRESENTATION-FREE + C++23-clean: glm + std + sibling Physics headers only.

#include <Arcane/Physics/Island.hpp>

#include <cmath>
#include <vector>

#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Solver/Solver.hpp> // JointConstraint
#include <Arcane/Physics/Joints/Joint.hpp>  // Joint (BodyA/BodyB slots)

namespace Arcane
{
    namespace Physics
    {
        namespace Island
        {
            void UpdateSleep(PhysicsWorld& world,
                             const JointConstraint* joints,
                             std::uint32_t jointCount,
                             Real dt)
            {
                const std::uint32_t count = world.Count();
                if (count == 0)
                {
                    return;
                }

                // ---- joint-attached dynamics reset their sleep timer ------------
                // Jointed dynamic bodies never sleep so target joints keep authority
                // (ports the original behavior verbatim). The solver Prepared each
                // joint earlier this Step, so BodyA()/BodyB() are resolved slots.
                for (std::uint32_t k = 0; k < jointCount; ++k)
                {
                    const Joint* j = joints[k].joint;
                    if (j == nullptr)
                    {
                        continue;
                    }
                    const std::uint32_t a = j->BodyA();
                    const std::uint32_t b = j->BodyB();
                    if (a != kInvalidSlot && a < count &&
                        world.Alive(a) && world.TypeSlot(a) == BodyType::Dynamic)
                    {
                        world.SetSleepTimerSlot(a, Real(0));
                    }
                    if (b != kInvalidSlot && b < count &&
                        world.Alive(b) && world.TypeSlot(b) == BodyType::Dynamic)
                    {
                        world.SetSleepTimerSlot(b, Real(0));
                    }
                }

                // ---- per-body idle-timer update (awake dynamics) ----------------
                // Idle: linear speed^2 < kSleepLinVel2 AND |angVel| < kSleepAngVel
                // -> accumulate dt; otherwise reset to 0 (UNCHANGED thresholds).
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    if (!world.Alive(i) ||
                        world.TypeSlot(i) != BodyType::Dynamic ||
                        !world.AwakeSlot(i))
                    {
                        continue;
                    }
                    const Vec2 v  = world.VelSlot(i);
                    const Real v2 = v.x * v.x + v.y * v.y;
                    const Real wv = world.AngVelSlot(i);
                    if (v2 < kSleepLinVel2 && std::fabs(wv) < kSleepAngVel)
                    {
                        world.SetSleepTimerSlot(i, world.SleepTimerSlot(i) + dt);
                    }
                    else
                    {
                        world.SetSleepTimerSlot(i, Real(0));
                    }
                }

                // ---- per-island sleep decision (O(island), no global scan) ------
                // For each island: if EVERY awake-dynamic member is past kSleepTime,
                // sleep the WHOLE island as a unit (clear awake + zero linear AND
                // angular velocity for each member). A member already asleep is
                // skipped (it does not veto). The per-body idle timer (reset above
                // for movers) gates each member; a transiently over-grouped island
                // only DELAYS sleep, never sleeps a mover -- mirrors the old
                // global-UF behavior (which also never woke bodies on a split).
                world.ForEachIsland([&](const std::vector<std::uint32_t>& bodies)
                {
                    bool anyAwake             = false;
                    bool allIdlePastThreshold = true;
                    for (const std::uint32_t b : bodies)
                    {
                        if (!world.Alive(b) || world.TypeSlot(b) != BodyType::Dynamic)
                        {
                            continue; // defensive: a stale member is ignored
                        }
                        if (!world.AwakeSlot(b))
                        {
                            continue; // already asleep -> does not veto
                        }
                        anyAwake = true;
                        if (world.SleepTimerSlot(b) <= kSleepTime)
                        {
                            allIdlePastThreshold = false;
                            break;
                        }
                    }
                    if (anyAwake && allIdlePastThreshold)
                    {
                        for (const std::uint32_t b : bodies)
                        {
                            if (world.Alive(b) &&
                                world.TypeSlot(b) == BodyType::Dynamic &&
                                world.AwakeSlot(b))
                            {
                                world.SetAwakeSlot(b, false);
                                world.SetVelSlot(b, Vec2(Real(0), Real(0)));
                                world.SetAngVelSlot(b, Real(0));
                            }
                        }
                    }
                });
            }

        } // namespace Island
    } // namespace Physics
} // namespace Arcane
