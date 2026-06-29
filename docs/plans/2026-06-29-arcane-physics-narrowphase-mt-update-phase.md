# Narrowphase MT (update phase) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Parallelize the persistent-contact manifold recompute (the update phase of `PhysicsWorld::UpdateContacts`) using the Box2D-v3 `b2Collide` technique — gather a stable contact list, recompute manifolds in parallel while only *flagging* state changes into a per-worker bit set, then apply all structural mutations serially — bit-for-bit identical to serial at any worker count.

**Architecture:** Three seams replacing the single update `ForEach`: (0) serial gather of live contact-pool ids; (1) `ParallelFor` (minRange=64) computing each manifold + setting a per-worker `BitSet` bit keyed on pool id, plus a transient `Contact::npState` flag; (2) serial OR-reduce of the bit sets + ascending-id CTZ walk applying destroy / island merge / split, then the existing sorted `MergeIslands`.

**Tech Stack:** C++23, Arcane Core physics (`Arcane/Core/src/Arcane/Physics`), `ITaskExecutor::ParallelFor` (enkiTS), Catch2 (`ArcaneTests`).

**Spec:** `docs/superpowers/specs/2026-06-29-arcane-physics-narrowphase-mt-update-phase-design.md`

## Global Constraints

- **Branch:** already on `feature/arcane-physics-narrowphase-mt` (off `main` @ `fce20ccf`). Do not commit to `main`.
- **Core is presentation-free + C++23-clean:** glm + std + sibling Physics/Util headers only. `namespace Arcane` / `namespace Arcane::Physics`. UTF-8 no BOM, ASCII comments.
- **Determinism:** byte-identical serial-vs-MT is the contract here (a free tripwire). No `/fp:fast`, index-ordered, no wall-clock in sim paths.
- **Box2D-faithful:** gather → parallel-flag → serial-apply; per-worker bit set keyed on stable contact id; deferred destruction; `minRange = 64`. Diverge only where documented (keep the flat `ContactPool`).
- **Build (VS2026):** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m`. Regenerate projects after adding a NEW file: `"D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe" vs2026` from `Arcane/` (the `.bat` hangs on `pause` — run the exe directly).
- **Tests FROM the exe dir:** `cd "D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"; .\ArcaneTests.exe "<filter>"`.
- **clangd diagnostics are FALSE POSITIVES** (expects Clang 20). MSVC + ArcaneTests are truth.
- **Throwaway measurement instrumentation** (STEPPROF=1 in StepProf.hpp, NPPROF block + includes in PhysicsWorld.cpp, ARCANE_PILE_COUNT in Scenes.cpp) is currently in the working tree — keep it for the perf A/B (Task 6), revert it in the final task.
- **Commit trailers:** end every commit body with
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux`.

## File structure

- `Arcane/Core/src/Arcane/Util/BitSet.hpp` — NEW. Header-only `Arcane::BitSet` (b2BitSet equivalent).
- `Arcane/Tests/src/BitSetTest.cpp` — NEW. `[bitset]` unit tests.
- `Arcane/Core/src/Arcane/Physics/Contact.hpp` — add transient `npState` + `kNp*` constants to `Contact`.
- `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` — add `m_npContacts` + `m_npStateBits` scratch; declare the per-contact helper `UpdateOneContact`.
- `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` — refactor `UpdateContacts` update phase (2742–2936) into the three seams + the helper.
- `Arcane/Tests/src/PhysicsNarrowphaseMtTest.cpp` — NEW. `[physics][mt]` MT==serial byte-identity test.

---

## Task 1: `Arcane::BitSet` utility

**Files:**
- Create: `Arcane/Core/src/Arcane/Util/BitSet.hpp`
- Create: `Arcane/Tests/src/BitSetTest.cpp`

**Interfaces:**
- Produces: `class Arcane::BitSet` with `void Resize(std::size_t bitCount)`, `void ClearAll() noexcept`, `void Set(std::size_t i) noexcept`, `void InPlaceUnion(const BitSet& other) noexcept`, and `template<class Fn> void ForEachSetBit(Fn&& fn) const` (calls `fn(std::uint32_t id)` in ascending id order). Backing `std::vector<std::uint64_t> m_blocks`.

- [ ] **Step 1: Write the failing test** — create `Arcane/Tests/src/BitSetTest.cpp`:

```cpp
// Arcane::BitSet (b2BitSet equivalent) unit tests.
#include <cstdint>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Util/BitSet.hpp>
using Arcane::BitSet;

TEST_CASE("BitSet: set + ForEachSetBit walks ascending", "[bitset]")
{
    BitSet b; b.Resize(200);
    b.Set(5); b.Set(63); b.Set(64); b.Set(199);
    std::vector<std::uint32_t> got;
    b.ForEachSetBit([&](std::uint32_t i){ got.push_back(i); });
    REQUIRE(got == std::vector<std::uint32_t>{5u, 63u, 64u, 199u});
}

TEST_CASE("BitSet: ClearAll empties", "[bitset]")
{
    BitSet b; b.Resize(100); b.Set(10); b.Set(70);
    b.ClearAll();
    int n = 0; b.ForEachSetBit([&](std::uint32_t){ ++n; });
    REQUIRE(n == 0);
}

TEST_CASE("BitSet: InPlaceUnion ORs, walks ascending across blocks", "[bitset]")
{
    BitSet a; a.Resize(130); BitSet b; b.Resize(130);
    a.Set(1); a.Set(129); b.Set(1); b.Set(64);
    a.InPlaceUnion(b);
    std::vector<std::uint32_t> got;
    a.ForEachSetBit([&](std::uint32_t i){ got.push_back(i); });
    REQUIRE(got == std::vector<std::uint32_t>{1u, 64u, 129u}); // 1 deduped
}
```

- [ ] **Step 2: Regenerate projects (new files) + build to verify it fails**

Run: `"D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe" vs2026` from `Arcane/`, then build Debug.
Expected: COMPILE FAIL — `Arcane/Util/BitSet.hpp` not found.

- [ ] **Step 3: Implement `BitSet.hpp`** — create `Arcane/Core/src/Arcane/Util/BitSet.hpp`:

```cpp
#pragma once

// Arcane::BitSet -- a minimal fixed-capacity bit set (the b2BitSet equivalent).
// Used by the narrowphase MT serial tail: each worker flags changed contact ids
// into its own BitSet; the tail OR-reduces (InPlaceUnion) and walks set bits in
// ascending order (ForEachSetBit, via std::countr_zero == Box2D b2CTZ64).
// Presentation-free + C++23-clean: std only.

#include <bit>        // std::countr_zero
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Arcane
{
    class BitSet
    {
    public:
        // Size to hold [0, bitCount) bits. Grows the block backing; never shrinks
        // capacity (grow-only -> zero steady-state alloc when reused per step).
        void Resize(std::size_t bitCount)
        {
            const std::size_t blocks = (bitCount + 63u) / 64u;
            if (blocks > m_blocks.size())
            {
                m_blocks.resize(blocks, 0ull);
            }
            m_blockCount = blocks;
        }

        void ClearAll() noexcept
        {
            for (std::size_t i = 0; i < m_blockCount; ++i) { m_blocks[i] = 0ull; }
        }

        void Set(std::size_t i) noexcept
        {
            m_blocks[i >> 6] |= (1ull << (i & 63u));
        }

        // OR `other` into this. Both must have been Resize()d to the same bitCount.
        void InPlaceUnion(const BitSet& other) noexcept
        {
            const std::size_t n = m_blockCount < other.m_blockCount
                                      ? m_blockCount : other.m_blockCount;
            for (std::size_t i = 0; i < n; ++i) { m_blocks[i] |= other.m_blocks[i]; }
        }

        // Call fn(id) for every set bit, ascending id order (block scan + CTZ).
        template <class Fn>
        void ForEachSetBit(Fn&& fn) const
        {
            for (std::size_t k = 0; k < m_blockCount; ++k)
            {
                std::uint64_t bits = m_blocks[k];
                while (bits != 0ull)
                {
                    const std::uint32_t ctz = static_cast<std::uint32_t>(std::countr_zero(bits));
                    fn(static_cast<std::uint32_t>(64u * k) + ctz);
                    bits &= (bits - 1ull); // clear lowest set bit
                }
            }
        }

    private:
        std::vector<std::uint64_t> m_blocks;
        std::size_t                m_blockCount = 0;
    };
} // namespace Arcane
```

- [ ] **Step 4: Build + run the BitSet tests**

Run: `.\ArcaneTests.exe "[bitset]"`
Expected: PASS (3 cases).

- [ ] **Step 5: Commit**

```bash
git add Arcane/Core/src/Arcane/Util/BitSet.hpp Arcane/Tests/src/BitSetTest.cpp
git commit  # feat(arcane/core): Arcane::BitSet utility (b2BitSet equivalent) for narrowphase MT
```

---

## Task 2: `Contact::npState` transient flag

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/Contact.hpp` (the `Contact` struct)

**Interfaces:**
- Produces: `std::uint8_t Contact::npState = 0;` + free constants `Arcane::Physics::kNpDestroy = 1u`, `kNpStarted = 2u`, `kNpStopped = 4u`.

- [ ] **Step 1: Add the field + constants** in `Contact.hpp`. Inside `namespace Arcane { namespace Physics {`, above the `Contact` struct, add:

```cpp
        // Transient per-step narrowphase state-change flags (Box2D simFlags
        // equivalent). Set in the MT collide pass, consumed+cleared in the serial
        // tail. NOT serialized (transient). 0 = no change this step.
        inline constexpr std::uint8_t kNpDestroy = 1u; // stale handle OR fat-box separated
        inline constexpr std::uint8_t kNpStarted = 2u; // false->true touching
        inline constexpr std::uint8_t kNpStopped = 4u; // true->false touching
```

Then inside the `Contact` struct, next to `bool touching`, add:

```cpp
            // Transient narrowphase MT state-change flags (kNp*); reset each step.
            std::uint8_t npState = 0;
```

- [ ] **Step 2: Build to verify it compiles**

Run: build Debug.
Expected: builds clean (additive field; `<cstdint>` is already included via Contact.hpp's existing includes — if not, add `#include <cstdint>`).

- [ ] **Step 3: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/Contact.hpp
git commit  # feat(arcane/physics): transient Contact::npState flag (Box2D simFlags equivalent)
```

---

## Task 3: PhysicsWorld scratch + per-contact `UpdateOneContact` helper (serial-equivalent refactor)

This task extracts the existing per-contact update body into a helper and routes the current serial `ForEach` through it — NO behavior change, NO parallelism yet. This isolates the risky extraction from the parallel wiring (Task 5), so a reviewer can verify byte-identity before MT is introduced.

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` (scratch members + helper decl)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (`UpdateContacts` + new `UpdateOneContact`)

**Interfaces:**
- Consumes: `Contact::npState` + `kNp*` (Task 2).
- Produces: `void PhysicsWorld::UpdateOneContact(std::uint32_t id, Contact& c, Real moveDt, Real threshSq) noexcept;` — runs the cheap filters + manifold recompute for ONE contact, writing only `c.manifold` / `c.touching` / `c.npState` (NO pool/color/island mutation). Returns nothing; sets `c.npState` bits for the serial tail. Scratch `std::vector<std::uint32_t> m_npContacts;` + `std::vector<Arcane::BitSet> m_npStateBits;`.

- [ ] **Step 1: Add scratch + helper decl** in `PhysicsWorld.hpp`. Add `#include <Arcane/Util/BitSet.hpp>` near the other includes. Next to the other per-step scratch vectors (where `m_pendingMerges` is declared), add:

```cpp
            // Narrowphase-MT scratch (gather -> parallel collide+flag -> serial apply).
            // m_npContacts: the gathered stable live-contact id list (Box2D's
            // contactSims gather). m_npStateBits: one BitSet per worker, each Resize'd
            // to the pool id capacity per step (Box2D's per-worker contactStateBitSet).
            std::vector<std::uint32_t> m_npContacts;
            std::vector<Arcane::BitSet> m_npStateBits;
```

In the private methods region (next to other narrowphase helpers), add:

```cpp
            // Recompute one contact's manifold + classify its state change for the
            // MT serial tail. Reads stable body transforms + the contact; writes ONLY
            // c.manifold / c.touching / c.npState (no pool/color/island mutation).
            // moveDt = the step dt; threshSq = the speculative-margin speed^2 gate.
            void UpdateOneContact(std::uint32_t id, Contact& c,
                                  Real moveDt, Real threshSq) noexcept;
```

- [ ] **Step 2: Implement `UpdateOneContact`** in `PhysicsWorld.cpp` (place just above `UpdateContacts`). Move the per-contact logic currently inside the update `ForEach` (the body at lines ~2747–2913) into it, replacing the inline `Destroy`/`ReleaseContactColor`/`MarkSplitCandidate`/`m_pendingMerges.push_back` mutations with `npState` flag-sets:

```cpp
        void PhysicsWorld::UpdateOneContact(std::uint32_t id, Contact& c,
                                            Real moveDt, Real threshSq) noexcept
        {
            (void)id;
            c.npState = 0;

            // Stale-handle: a removed/recycled fixture or dead body -> flag destroy.
            if (!FixtureSlotLive(c.a) || (c.bIsBody && !FixtureSlotLive(c.b)))
            {
                c.npState |= kNpDestroy;
                return;
            }

            // Velocity-scaled speculative margin (CCD) over the two bodies.
            const Real speedSqA = m_velX[c.bodyA] * m_velX[c.bodyA] +
                                  m_velY[c.bodyA] * m_velY[c.bodyA];
            const Real speedSqB = m_velX[c.bodyB] * m_velX[c.bodyB] +
                                  m_velY[c.bodyB] * m_velY[c.bodyB];
            const Real maxSpeedSq = std::max(speedSqA, speedSqB);
            const Real margin = (maxSpeedSq > threshSq)
                                    ? std::sqrt(maxSpeedSq) * moveDt
                                    : kSkin;

            // Fat-box separation (widened by the speculative margin) -> flag destroy.
            const Real extra = std::max(Real(0), margin - DynamicTree::kMargin);
            if (!FatBoxesOverlap(c, extra))
            {
                c.npState |= kNpDestroy;
                return;
            }

            // Both asleep (and not an event-only pair) -> keep cached manifold, no work.
            const bool eventOnly = c.eventRelevant && !c.solverRelevant;
            if (BothAsleep(c) && !eventOnly)
            {
                return;
            }

            // Warm-start carry-forward snapshot, then recompute the manifold.
            const Manifold oldManifold = c.manifold;
            const Transform xfA = ComposeFixtureXf(
                Vec2(m_posX[c.bodyA], m_posY[c.bodyA]), m_angle[c.bodyA],
                Vec2(m_fxLocalPosX[c.a.index], m_fxLocalPosY[c.a.index]),
                m_fxLocalAngle[c.a.index]);
            const Transform xfB = ComposeFixtureXf(
                Vec2(m_posX[c.bodyB], m_posY[c.bodyB]), m_angle[c.bodyB],
                Vec2(m_fxLocalPosX[c.b.index], m_fxLocalPosY[c.b.index]),
                m_fxLocalAngle[c.b.index]);
            c.manifold = Collide(m_fxShape[c.a.index], xfA,
                                 m_fxShape[c.b.index], xfB, margin);

            const bool wasTouching = c.touching;
            c.touching = (c.manifold.pointCount > 0);

            // Classify the dyn-dyn touch transition (island edge) -> flag for the tail.
            if (c.bIsBody && c.bodyB != kInvalidSlot &&
                TypeSlot(c.bodyA) == BodyType::Dynamic &&
                TypeSlot(c.bodyB) == BodyType::Dynamic)
            {
                if (!wasTouching && c.touching)      { c.npState |= kNpStarted; }
                else if (wasTouching && !c.touching) { c.npState |= kNpStopped; }
            }

            // Warm-start: copy impulses forward by feature id (<=2x2 fixed loop).
            for (int np = 0; np < c.manifold.pointCount; ++np)
            {
                ManifoldPoint& nm = c.manifold.points[np];
                for (int op = 0; op < oldManifold.pointCount; ++op)
                {
                    const ManifoldPoint& om = oldManifold.points[op];
                    if (om.id == nm.id)
                    {
                        nm.normalImpulse  = om.normalImpulse;
                        nm.tangentImpulse = om.tangentImpulse;
                        break;
                    }
                }
            }
        }
```

(`moveDt`/`threshSq` are the same locals the current `UpdateContacts` computes before the update pass — find them above line 2742; pass them in.)

- [ ] **Step 3: Rewrite the update pass to call the helper serially + apply via npState.** Replace the update `ForEach` body (2742–2913) so it calls `UpdateOneContact` then applies the flags inline (still serial, ascending-id — byte-identical to before):

```cpp
            // ---- 2. UPDATE + DESTROY: recompute (helper) then apply (ascending id) --
            m_contactPool.ForEach([&](std::uint32_t id, Contact& c)
            {
                UpdateOneContact(id, c, moveDt, threshSq);
                if (c.npState & kNpDestroy)
                {
                    if (c.bIsBody && c.touching &&
                        c.bodyA != kInvalidSlot && c.bodyB != kInvalidSlot &&
                        c.bodyA < m_islandId.size() &&
                        TypeSlot(c.bodyA) == BodyType::Dynamic &&
                        c.bodyB < m_islandId.size() &&
                        TypeSlot(c.bodyB) == BodyType::Dynamic)
                    {
                        MarkSplitCandidate(IslandOf(c.bodyA));
                    }
                    ReleaseContactColor(id);
                    m_contactPool.Destroy(id);
                    return;
                }
                if (c.npState & kNpStarted)
                {
                    const std::uint32_t lo = c.bodyA < c.bodyB ? c.bodyA : c.bodyB;
                    const std::uint32_t hi = c.bodyA < c.bodyB ? c.bodyB : c.bodyA;
                    m_pendingMerges.push_back(BroadphasePair{ lo, hi });
                }
                else if (c.npState & kNpStopped)
                {
                    MarkSplitCandidate(IslandOf(c.bodyA));
                }
            });
```

Keep the existing `std::sort(m_pendingMerges)` + `MergeIslands` loop after it, unchanged. Ensure `moveDt`/`threshSq` are still computed before this pass (they were locals used by the old inline body — keep their definitions).

- [ ] **Step 4: Build + run the full `[physics]` suite (must stay byte-identical green)**

Run: build Debug, then `.\ArcaneTests.exe "[physics]"`
Expected: ALL pass, identical counts to before (pure extraction — no behavior change). If anything fails, the extraction changed behavior — diff against the old inline body.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp
git commit  # refactor(arcane/physics): extract UpdateOneContact + npState-driven apply (serial, byte-identical)
```

---

## Task 4: MT==serial byte-identity test (regression guard, written before the MT wiring)

**Files:**
- Create: `Arcane/Tests/src/PhysicsNarrowphaseMtTest.cpp`

**Interfaces:**
- Consumes: the public `PhysicsWorld` API + `SetExecutor`. Mirrors the existing `SolverMtInvarianceTest` pattern (find it for the exact multi-worker executor injection helper; reuse it).

- [ ] **Step 1: Find the MT-invariance pattern.** Read `Arcane/Tests/src/SolverMtInvarianceTest.cpp` to copy how it builds a churning scene, injects a K-worker executor (`world.SetExecutor(...)` with an `EnkiTaskExecutor` or the test's job-system helper), and captures comparable state. Reuse that executor-injection helper verbatim.

- [ ] **Step 2: Write the test** — create `Arcane/Tests/src/PhysicsNarrowphaseMtTest.cpp`:

```cpp
// Narrowphase MT == serial byte-identity. Run the SAME churning scene with a
// 1-worker and a K-worker executor; the per-contact manifolds + body state +
// island assignments must be bit-for-bit identical (the parallel collide only
// reorders independent work). Mirrors SolverMtInvarianceTest.
#include <cstdint>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
// + the job-system / executor includes SolverMtInvarianceTest uses (copy them).
using namespace Arcane::Physics;
namespace {
    constexpr Real kStep = Real(1) / Real(60);

    // Build a churning mixed pile + a moving kinematic stirrer (forces contact
    // create/destroy/touch-change every step -> exercises all npState paths).
    void BuildChurn(PhysicsWorld& w) {
        BodyDef fl; fl.type=BodyType::Static; fl.position=Vec2(Real(0),Real(300));
        fl.shape=MakeAabb(Real(400),Real(20)); fl.friction=Real(0.6); w.AddBody(fl);
        std::uint32_t seed = 12345u;
        auto rnd=[&](Real a,Real b){ seed=seed*1664525u+1013904223u;
            return a+(b-a)*Real((seed>>8)&0xFFFF)/Real(65535); };
        for (int i=0;i<120;++i) {
            BodyDef d; d.type=BodyType::Dynamic; d.density=Real(1); d.friction=Real(0.4);
            d.position=Vec2(rnd(Real(-300),Real(300)), rnd(Real(-200),Real(260)));
            if (i%3==0) { d.shape=MakeCircle(rnd(Real(6),Real(12))); }
            else if (i%3==1){ d.shape=MakeAabb(Real(8),Real(8)); d.fixedRotation=true; }
            else { d.shape=MakeCapsule(Real(10),Real(5)); }
            w.AddBody(d);
        }
    }

    // Run N steps with `workers`-wide executor; capture final positions+angles.
    std::vector<Real> RunCapture(std::uint32_t workers) {
        WorldDef wd; wd.gravityY=Real(400);
        PhysicsWorld w(wd);
        /* w.SetExecutor( make a `workers`-wide executor -- copy the helper from
           SolverMtInvarianceTest; for workers==1 use the serial default ); */
        BuildChurn(w);
        for (int k=0;k<200;++k){ w.Step(kStep); }
        std::vector<Real> out;
        for (std::uint32_t i=0;i<w.Count();++i){
            if (!w.Alive(i)) { out.push_back(Real(0)); out.push_back(Real(0)); continue; }
            const Vec2 p=w.PosSlot(i); out.push_back(p.x); out.push_back(p.y);
        }
        return out;
    }
}

TEST_CASE("Narrowphase MT == serial: positions bit-identical", "[physics][mt]")
{
    const std::vector<Real> serial = RunCapture(1);
    const std::vector<Real> multi  = RunCapture(8);
    REQUIRE(serial.size() == multi.size());
    for (std::size_t i=0;i<serial.size();++i){ REQUIRE(serial[i] == multi[i]); }
}
```

(Adjust `PosSlot`/`Count`/`Alive` to the real accessors; the executor-injection lines are filled from `SolverMtInvarianceTest`. Until Task 5, both runs are serial under the hood, so this passes as a baseline.)

- [ ] **Step 3: Regenerate projects + build + run**

Run: premake `vs2026`, build Debug, `.\ArcaneTests.exe "[physics][mt]"`
Expected: PASS (both runs serial pre-Task-5 → trivially identical; establishes the guard).

- [ ] **Step 4: Commit**

```bash
git add Arcane/Tests/src/PhysicsNarrowphaseMtTest.cpp
git commit  # test(arcane/physics): narrowphase MT==serial byte-identity guard (churn scene)
```

---

## Task 5: Wire the three seams (the parallelization)

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (`UpdateContacts` update pass)

**Interfaces:**
- Consumes: `UpdateOneContact`, `m_npContacts`, `m_npStateBits` (Task 3); `Executor()->ParallelFor` / `WorkerCount()` (the D2 broadphase-MT pattern — read `UpdateContacts`' Phase-1 mover-mover ParallelFor at ~2438 for the exact call shape).

- [ ] **Step 1: Replace the serial update pass with the three seams.** Swap the Task-3 serial `ForEach` for:

```cpp
            // ---- 2. UPDATE + DESTROY: Box2D b2Collide -- gather, parallel collide
            //         (flag only), serial apply. ------------------------------------
            // Seam 0: gather the stable live-contact id list (Box2D contactSims).
            m_npContacts.clear();
            m_contactPool.ForEach([&](std::uint32_t id, Contact&) {
                m_npContacts.push_back(id);
            });

            // Seam 1: parallel collide. Each worker recomputes its range's manifolds
            // and sets a bit (keyed on pool id) in its OWN BitSet -- no structural
            // mutation. minRange=64 (Box2D's grain); below it, runs serial on worker 0.
            const std::uint32_t workers = Executor()->WorkerCount();
            if (m_npStateBits.size() < workers) { m_npStateBits.resize(workers); }
            const std::size_t idCap = m_contactPool.Capacity();
            for (std::uint32_t w = 0; w < workers; ++w) {
                m_npStateBits[w].Resize(idCap);
                m_npStateBits[w].ClearAll();
            }
            Executor()->ParallelFor(m_npContacts.size(), /*minRange=*/64,
                [&](std::size_t begin, std::size_t end, std::uint32_t worker)
                {
                    Arcane::BitSet& bits = m_npStateBits[worker];
                    for (std::size_t k = begin; k < end; ++k)
                    {
                        const std::uint32_t id = m_npContacts[k];
                        Contact& c = m_contactPool.Get(id);
                        UpdateOneContact(id, c, moveDt, threshSq);
                        if (c.npState != 0) { bits.Set(id); }
                    }
                });

            // Seam 2: serial apply. OR-reduce into bits[0], walk ascending (CTZ),
            // apply destroy / merge-edge / split per npState (ascending id == serial).
            if (workers > 0)
            {
                for (std::uint32_t w = 1; w < workers; ++w) {
                    m_npStateBits[0].InPlaceUnion(m_npStateBits[w]);
                }
                m_npStateBits[0].ForEachSetBit([&](std::uint32_t id)
                {
                    Contact& c = m_contactPool.Get(id);
                    if (c.npState & kNpDestroy)
                    {
                        if (c.bIsBody && c.touching &&
                            c.bodyA != kInvalidSlot && c.bodyB != kInvalidSlot &&
                            c.bodyA < m_islandId.size() &&
                            TypeSlot(c.bodyA) == BodyType::Dynamic &&
                            c.bodyB < m_islandId.size() &&
                            TypeSlot(c.bodyB) == BodyType::Dynamic)
                        {
                            MarkSplitCandidate(IslandOf(c.bodyA));
                        }
                        ReleaseContactColor(id);
                        m_contactPool.Destroy(id);
                    }
                    else if (c.npState & kNpStarted)
                    {
                        const std::uint32_t lo = c.bodyA < c.bodyB ? c.bodyA : c.bodyB;
                        const std::uint32_t hi = c.bodyA < c.bodyB ? c.bodyB : c.bodyA;
                        m_pendingMerges.push_back(BroadphasePair{ lo, hi });
                    }
                    else if (c.npState & kNpStopped)
                    {
                        MarkSplitCandidate(IslandOf(c.bodyA));
                    }
                });
            }
```

Keep the existing `std::sort(m_pendingMerges)` + `MergeIslands` loop after, unchanged.

NOTE: confirm `m_contactPool.Get(id)` returns the live `Contact&` and `m_contactPool.Capacity()` exists (the agent map cites `ContactPool` Contact.hpp:114-175 — if the accessor is named differently, e.g. `Pool()[id]` / `Size()`, use the real one). The `ForEach` in seam 0 stays the gather; pool mutation happens ONLY in seam 2.

- [ ] **Step 2: Build + run the MT byte-identity test (now actually parallel)**

Run: build Debug, `.\ArcaneTests.exe "[physics][mt]"`
Expected: PASS — the 8-worker run now genuinely parallelizes seam 1 yet matches the 1-worker run bit-for-bit. If it FAILS: a non-disjoint write or non-ascending apply slipped in — check that seam 1 touches only `c.manifold`/`c.touching`/`c.npState` and that seam 2 is the only mutation site.

- [ ] **Step 3: Run the full `[physics]` suite + determinism**

Run: `.\ArcaneTests.exe "[physics]"`
Expected: ALL pass, identical counts (byte-identical; no re-baseline). Determinism/run-twice tests green.

- [ ] **Step 4: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp
git commit  # feat(arcane/physics): parallel narrowphase update (Box2D b2Collide -- gather/parallel-flag/serial-apply, byte-identical)
```

---

## Task 6: Perf A/B + threshold confirm, then revert throwaway instrumentation

**Files:**
- Modify (revert): `Arcane/Core/src/Arcane/Physics/StepProf.hpp`, `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`, `Arcane/Sandbox/src/Scenes.cpp`

- [ ] **Step 1: Build Dist + measure the win.** Build Dist. From `Arcane/bin/Dist-windows-x86_64-md/Loom/`:
```
set ARCANE_SANDBOX_SCENE=8 & set ARCANE_PILE_COUNT=2000 & set ARCANE_NPPROF=1
.\Loom.exe --frames 4000
```
Confirm the `[NPPROF] ... update=` ms dropped vs the pre-MT baseline (~2.5 ms → toward 2.5/cores). Record the number. Confirm a settled/low-churn case (or scene 1 box-stack) shows no regression (small `m_npContacts` stays serial under minRange=64).

- [ ] **Step 2: Revert the throwaway instrumentation.**
  - `StepProf.hpp`: restore `#define ARCANE_STEPPROF 0`.
  - `PhysicsWorld.cpp`: remove the `#include <chrono>/<cstdio>/<cstdlib>` THROWAWAY lines, the `[PROF]` `#if ARCANE_STEPPROF` block in `Step`, and the `ARCANE_NPPROF` timers (npT0/npT1/npT2 + the dump block) in `UpdateContacts`.
  - `Scenes.cpp`: restore `BuildStressTest` to `BuildStressTestImpl(reg, kStressBodyCount);` and remove the `<cstdlib>` THROWAWAY include + the `ARCANE_PILE_COUNT` block.

- [ ] **Step 3: Build + full suite green after revert**

Run: build Debug, `.\ArcaneTests.exe "[physics]"` + `.\ArcaneTests.exe "[bitset]"`
Expected: ALL pass.

- [ ] **Step 4: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/StepProf.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Sandbox/src/Scenes.cpp
git commit  # chore(arcane/physics): revert throwaway narrowphase-MT perf instrumentation
```

---

## Final verification

- [ ] Build Debug + Dist clean: `msbuild Arcane.slnx /p:Configuration=Debug /m` and `/p:Configuration=Dist /m`.
- [ ] `.\ArcaneTests.exe ~[gpu]` green (full non-GPU suite); `[physics][mt]` + `[bitset]` green.
- [ ] Headless Loom GPU-verify clean: `Loom.exe --backend vulkan --frames 180` (scene 8) exits 0.
- [ ] Push branch; CI green; hand to T3mps for review/merge.

## Self-review notes (addressed)

- **Spec coverage:** BitSet util (T1), npState/simFlags (T2), gather+helper (T3), minRange=64 + per-worker bitset + OR-reduce + CTZ walk + deferred destruction (T5), MT==serial test (T4), perf A/B (T6). All spec sections mapped.
- **Type consistency:** `UpdateOneContact(id, c, moveDt, threshSq)`, `Contact::npState` + `kNpDestroy/kNpStarted/kNpStopped`, `BitSet::{Resize,ClearAll,Set,InPlaceUnion,ForEachSetBit}`, `m_npContacts`/`m_npStateBits` — used identically across T2/T3/T5.
- **Risky-extraction isolation:** T3 extracts the helper + routes serial through it (byte-identical, reviewable) BEFORE T5 adds parallelism — so an MT failure in T5 is isolated to the seam wiring, not the body extraction.
- **Real-API confirmations flagged in-task:** `m_contactPool.Get(id)`/`Capacity()`, `Executor()->ParallelFor/WorkerCount` shape (read the Phase-1 ParallelFor at ~2438), `PosSlot`/`Count`/`Alive` accessors, and the `SolverMtInvarianceTest` executor-injection helper — each task says to confirm against the real names before coding.
