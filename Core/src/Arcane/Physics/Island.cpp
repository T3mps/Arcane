// Island.cpp -- the constraint-graph + sleep module (M6, Task P2.4).
//
// See Island.hpp for the contract + the PORT mapping (the inline Lua logic at
// PhysicsWorld.lua:28-31 + 403-452 extracted into UpdateSleep). This TU owns the
// union-find graph build, the per-body sleep-timer update, and the all-members-
// idle island-sleep decision. Wake paths live in PhysicsWorld (GenerateContacts
// wake-on-contact + ApplyImpulse/SetVelocity/Wake wake-on-force).
//
// PRESENTATION-FREE + C++20-clean: glm + std + sibling Physics headers only.

#include <Arcane/Physics/Island.hpp>

#include <cmath>
#include <vector>

#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Solver/Solver.hpp> // ContactConstraint + JointConstraint

namespace Arcane
{
    namespace Physics
    {
        namespace Island
        {
            namespace
            {
                // Union-find path-halving (ports PhysicsWorld.lua ufFind, lines
                // 28-31). Order-stable: the find result depends only on the
                // parent links, not on traversal timing -> deterministic.
                std::uint32_t UfFind(std::vector<std::uint32_t>& uf, std::uint32_t x) noexcept
                {
                    while (uf[x] != x)
                    {
                        uf[x] = uf[uf[x]];
                        x = uf[x];
                    }
                    return x;
                }
            } // namespace

            void UpdateSleep(PhysicsWorld& world,
                             const ContactConstraint* contacts,
                             std::uint32_t contactCount,
                             const JointConstraint* joints,
                             std::uint32_t jointCount,
                             Real dt)
            {
                const std::uint32_t count = world.Count();
                if (count == 0)
                {
                    return;
                }

                // ---- build the union-find constraint graph -------------------
                // Bodies = nodes; this step's contacts = edges. Reuse the world's
                // pooled scratch (the Lua w._uf) -> zero steady-state alloc.
                std::vector<std::uint32_t>& uf = world.UnionFindScratch();
                if (uf.size() < count)
                {
                    uf.resize(count);
                }
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    uf[i] = i;
                }

                // Union two DYNAMIC bodies sharing a contact (ports line 411:
                // `if ct.b >= 0 and btype[ct.b] == DYNAMIC then union(ct.a, ct.b)`).
                // GenerateContacts already orients A = dynamic; we union only when
                // B is ALSO a real dynamic body. A dynamic-vs-static / kinematic /
                // tile-span contact does NOT union (statics anchor, they are not
                // island members). Index-ordered over the contact prefix.
                for (std::uint32_t k = 0; k < contactCount; ++k)
                {
                    const ContactConstraint& ct = contacts[k];
                    if (!ct.bodyBIsBody)
                    {
                        continue; // tile-span virtual fixture -> no union
                    }
                    const std::uint32_t a = ct.bodyA;
                    const std::uint32_t b = ct.bodyB;
                    if (b == kInvalidSlot)
                    {
                        continue;
                    }
                    // A is always dynamic (GenerateContacts orientation); union
                    // only when B is dynamic too.
                    if (world.TypeSlot(b) == BodyType::Dynamic)
                    {
                        uf[UfFind(uf, a)] = UfFind(uf, b);
                    }
                }

                // Joint-attached dynamic bodies reset their sleep timer so they
                // never sleep (target joints keep authority over their captives;
                // ports lines 415-423). The joint constraint list is empty until
                // P2.5; this loop is a no-op until JointConstraint carries the two
                // body slots. (When P2.5 lands, reset SleepTimerSlot for each
                // jointed dynamic body here.)
                (void)joints;
                (void)jointCount;

                // ---- per-body sleep-timer update (awake dynamics) -----------
                // Idle: linear speed^2 < kSleepLinVel2 AND |angVel| < kSleepAngVel
                // -> accumulate dt; otherwise reset to 0 (ports lines 424-433).
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    if (!world.Alive(i) ||
                        world.TypeSlot(i) != BodyType::Dynamic ||
                        !world.AwakeSlot(i))
                    {
                        continue;
                    }
                    const Vec2 v = world.VelSlot(i);
                    const Real v2 = v.x * v.x + v.y * v.y;
                    const Real w = world.AngVelSlot(i);
                    if (v2 < kSleepLinVel2 && std::fabs(w) < kSleepAngVel)
                    {
                        world.SetSleepTimerSlot(i, world.SleepTimerSlot(i) + dt);
                    }
                    else
                    {
                        world.SetSleepTimerSlot(i, Real(0));
                    }
                }

                // ---- island sleep (all members idle) ------------------------
                // An island sleeps only when EVERY awake-dynamic member has
                // sleepT > kSleepTime. For each such candidate, find its island
                // root, scan ALL island members; if any member's sleepT is not
                // yet past the threshold the island stays awake. If all are past,
                // sleep this member (awake=0; zero velocities). Ports lines
                // 435-451 -- order-stable: bodies scanned by index, so once one
                // member sleeps the others in the same island fall asleep in the
                // same pass (their per-member all-check still holds).
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    if (!world.Alive(i) ||
                        world.TypeSlot(i) != BodyType::Dynamic ||
                        !world.AwakeSlot(i) ||
                        world.SleepTimerSlot(i) <= kSleepTime)
                    {
                        continue;
                    }
                    const std::uint32_t root = UfFind(uf, i);
                    bool all = true;
                    for (std::uint32_t j = 0; j < count; ++j)
                    {
                        if (world.Alive(j) &&
                            world.TypeSlot(j) == BodyType::Dynamic &&
                            UfFind(uf, j) == root &&
                            world.SleepTimerSlot(j) <= kSleepTime)
                        {
                            all = false;
                            break;
                        }
                    }
                    if (all)
                    {
                        world.SetAwakeSlot(i, false);
                        world.SetVelSlot(i, Vec2(Real(0), Real(0)));
                        world.SetAngVelSlot(i, Real(0));
                    }
                }
            }

        } // namespace Island
    } // namespace Physics
} // namespace Arcane
