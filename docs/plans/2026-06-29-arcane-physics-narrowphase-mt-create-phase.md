# Narrowphase MT (create phase) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Parallelize the serial contact-create phase of `PhysicsWorld::UpdateContacts` (the dynamic-vs-static candidate find, ~3.0 ms) using Box2D v3's parallel-find / serial-create split — bit-for-bit identical to serial at any worker count.

**Architecture:** Split the stage-2b `ForEachAwake` create loop into (1) a serial prerequisite refactor that threads per-worker scratch into `StaticCandidates`, (2) a serial detect-into-records / create-from-records restructure (byte-identical), then (3) a `ParallelFor` over awake-body ranges that runs the detect into per-worker scratch with NO structural mutation, followed by a serial create tail that replays `TryCreateContact` ordered by `(awakeIndex, candidateSeq)` so it reproduces today's `ForEachAwake` create order exactly (the order-dependent `AssignContactColor` stays in the serial tail).

**Tech Stack:** C++23, Arcane Core physics (`Arcane/Core/src/Arcane/Physics`), `ITaskExecutor::ParallelFor` (enkiTS), Catch2 (`ArcaneTests`).

**Spec:** `docs/superpowers/specs/2026-06-29-arcane-physics-narrowphase-mt-create-phase-design.md`

## Global Constraints

- **Branch:** `feature/arcane-physics-narrowphase-mt-create-phase` (stacked on the unmerged narrowphase-MT update-phase branch `2e2697fb`). Do NOT commit to `main`.
- **Core is presentation-free + C++23-clean:** glm + std + sibling Physics/Util headers only. `namespace Arcane::Physics`. UTF-8 no BOM, ASCII comments only.
- **Determinism:** byte-identical serial-vs-MT is the contract (a free tripwire). No `/fp:fast`, index-ordered, no wall-clock in sim paths.
- **Box2D-faithful:** parallel detect (no structural mutation) -> serial create. The serial create tail orders by the stable `(awakeIndex, candidateSeq)` key (NOT worker-id order; `ParallelFor` does not guarantee worker id == range order). `AssignContactColor` is order-dependent and MUST stay in the serial tail.
- **Build (VS2026):** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m`. No NEW files are added by this plan, so NO premake regen is needed.
- **Tests FROM the exe dir:** `cd "D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"; .\ArcaneTests.exe "<filter>"`.
- **clangd diagnostics are FALSE POSITIVES** (expects Clang 20). MSVC + ArcaneTests are truth.
- **Stage ONLY the files named per task** by explicit path. The working tree carries UNRELATED uncommitted Loom-refactor changes (Client `data/ui_screens/*.json`, `AGENTS.md`, `Arcane/.screenshots/`, `Server/cpp_coding_style.txt`, two `docs/.../2026-06-24-*.md`) — do NOT touch/stage/commit them. NEVER `git add -A`/`git add .`.
- **Commit trailers:** end every commit body with
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux`.

## File structure

- `Arcane/Core/src/Arcane/Physics/Queries.cpp` — `StaticCandidates` gains a caller-supplied grid-scratch param (Task 1).
- `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` — `StaticCandidates` decl update; new `NewPairRecord`/`SpanEntry` structs; serial scratch (`m_newPairs`) then per-worker scratch vectors (Task 2/4).
- `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` — refactor the stage-2b create loop (~2555-2707) into detect/serial-create (Task 2) then parallel detect (Task 4).
- `Arcane/Tests/src/PhysicsNarrowphaseMtTest.cpp` — add a create-heavy `[physics][mt]` byte-identity case (Task 3).

---

## Task 1: Thread per-worker scratch into `StaticCandidates`

This removes the only shared-mutable internal in the candidate query (`m_staticGridScratch`) so the query can later run on multiple workers. Pure refactor, serial behavior unchanged, byte-identical.

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` (the `StaticCandidates` decl, ~line 1139)
- Modify: `Arcane/Core/src/Arcane/Physics/Queries.cpp` (`StaticCandidates` body, ~line 537)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (the one call site, ~line 2581)

**Interfaces:**
- Produces: `void PhysicsWorld::StaticCandidates(const Aabb2& box, std::vector<Aabb2>& spansOut, std::vector<std::uint32_t>& staticsOut, std::vector<std::uint32_t>& gridScratch) const;`

- [ ] **Step 1: Update the declaration** in `PhysicsWorld.hpp` (~1139). Change:

```cpp
            void StaticCandidates(const Aabb2& box, std::vector<Aabb2>& spansOut,
                                  std::vector<std::uint32_t>& staticsOut) const;
```
to:
```cpp
            // gridScratch: caller-supplied scratch for SpatialGrid::QueryAABB (was the
            // shared mutable m_staticGridScratch; now caller-owned so the query is
            // re-entrant for the parallel create-phase detect).
            void StaticCandidates(const Aabb2& box, std::vector<Aabb2>& spansOut,
                                  std::vector<std::uint32_t>& staticsOut,
                                  std::vector<std::uint32_t>& gridScratch) const;
```

- [ ] **Step 2: Update the body** in `Queries.cpp` (~537). Replace the signature to match, and replace the two uses of `m_staticGridScratch` with `gridScratch`:

```cpp
        void PhysicsWorld::StaticCandidates(const Aabb2& box,
                                            std::vector<Aabb2>& spansOut,
                                            std::vector<std::uint32_t>& staticsOut,
                                            std::vector<std::uint32_t>& gridScratch) const
        {
            spansOut.clear();
            if (m_tileGrid)
            {
                m_tileGrid->Query(box, spansOut);
            }
            staticsOut.clear();
            m_staticGrid.QueryAABB(box, gridScratch);
            for (std::uint32_t idx : gridScratch)
                if (m_alive[idx] && AabbOverlap(box, SlotAabb(idx)))
                    staticsOut.push_back(idx);
        }
```

- [ ] **Step 3: Update the call site** in `PhysicsWorld.cpp` (~2581). Change `StaticCandidates(query, m_genSpans, m_genStatics);` to pass the existing member scratch (still serial here, identical behavior):

```cpp
                StaticCandidates(query, m_genSpans, m_genStatics, m_staticGridScratch);
```

(`m_staticGridScratch` stays declared in `PhysicsWorld.hpp:1376` for now; Task 4 swaps the call to a per-worker scratch. Do NOT delete it in this task.)

- [ ] **Step 4: Build + run the full `[physics]` suite (byte-identical)**

Run: build Debug, then `.\ArcaneTests.exe "[physics]"`
Expected: ALL pass, identical counts to before (pure signature plumbing; `m_staticGridScratch` is now passed in instead of read as a member, same object, same behavior).

- [ ] **Step 5: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/Queries.cpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp
git commit  # refactor(arcane/physics): caller-supplied grid scratch for StaticCandidates (create-MT prereq)
```

---

## Task 2: Detect-into-records / serial-create restructure (byte-identical, serial)

Split the stage-2b `ForEachAwake` body so the per-body work EMITS new fixture-pair records instead of calling `TryCreateContact` inline, then a second serial pass creates them in `(awakeIndex, candidateSeq)` order. Still single-threaded; byte-identical. This isolates the detect/create split + the ordering before any parallelism (Task 4), so a reviewer can verify byte-identity first.

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` (new `NewPairRecord` struct + `m_newPairs` scratch)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (the stage-2b loop ~2555-2707)

**Interfaces:**
- Produces: `struct PhysicsWorld::NewPairRecord { std::uint32_t awakeIndex; std::uint32_t fiA; std::uint32_t fiB; };` and `std::vector<NewPairRecord> m_newPairs;` (serial scratch). Consumes: `TryCreateContact` (unchanged), `AwakeBodies()`/`AwakeCount()`.

- [ ] **Step 1: Add the record struct + scratch** in `PhysicsWorld.hpp`, next to the other narrowphase create scratch (near `m_genSpans`/`m_genStatics`, ~1599):

```cpp
            // Create-phase MT: a deferred new-contact record. The detect pass emits
            // these (no mutation); the serial create tail replays TryCreateContact in
            // (awakeIndex, push-order) order -- reproducing the serial ForEachAwake
            // create order exactly, so the order-dependent AssignContactColor + the
            // EnsurePair id allocation are byte-identical. awakeIndex = the index into
            // m_awakeBodies (NOT the body slot), which is the ForEachAwake visit order.
            struct NewPairRecord { std::uint32_t awakeIndex; std::uint32_t fiA; std::uint32_t fiB; };
            std::vector<NewPairRecord> m_newPairs;
```

- [ ] **Step 2: Rewrite the stage-2b loop** in `PhysicsWorld.cpp` (~2555-2707). Read the current `ForEachAwake([&](std::uint32_t i){ ... })` body first; preserve every filter/branch/formula EXACTLY. Convert it to an indexed detect loop that records pairs, then a serial create pass. The span-push logic is UNCHANGED (spans already push in awake-visit order). Replace the `TryCreateContact(fiA, fiB)` call (~2703) with a record push, and add the create pass after the loop:

```cpp
            m_spanContacts.clear();
            m_spanCenters.clear();
            m_newPairs.clear();
            const Real moveDt = dt > Real(0) ? dt : Real(0);
            const Real threshSq = (moveDt > Real(0))
                                      ? (kSkin / moveDt) * (kSkin / moveDt)
                                      : Real(0);
            // ---- DETECT (serial, awake-visit order) -------------------------------
            const std::uint32_t awakeCount = AwakeCount();
            for (std::uint32_t k = 0; k < awakeCount; ++k)
            {
                const std::uint32_t i = AwakeBodies()[k];
                if (m_sensor[i] != 0) { continue; }
                const Real speedSqA   = m_velX[i] * m_velX[i] + m_velY[i] * m_velY[i];
                const Real specMargin = (speedSqA > threshSq)
                                            ? std::sqrt(speedSqA) * moveDt
                                            : kSkin;
                const Aabb box = SlotAabb(i);
                const Real pad = std::max(Real(2), specMargin);
                Aabb2 query;
                query.min = Vec2(box.min.x - pad, box.min.y - pad);
                query.max = Vec2(box.max.x + pad, box.max.y + pad);
                StaticCandidates(query, m_genSpans, m_genStatics, m_staticGridScratch);

                const std::vector<std::uint32_t>* fxListA = nullptr;
                if (i < m_bodyFixtures.size() && !m_bodyFixtures[i].empty())
                {
                    fxListA = &m_bodyFixtures[i];
                }

                // ---- tile spans (transient virtual fixtures) -- UNCHANGED ----------
                // [KEEP the existing span loop body verbatim: it pushes to
                //  m_spanContacts/m_spanCenters in awake-visit order. Do not change it.]
                // ... existing `for (std::size_t s = 0; s < m_genSpans.size(); ++s) { ... }` ...

                if (fxListA == nullptr) { continue; }

                // ---- static fixture pairs: EMIT records (do NOT create here) -------
                for (std::size_t s = 0; s < m_genStatics.size(); ++s)
                {
                    const std::uint32_t idx = m_genStatics[s];
                    if (idx >= m_count || m_alive[idx] == 0 || m_sensor[idx] != 0) { continue; }
                    const std::vector<std::uint32_t>* fxListB = nullptr;
                    if (idx < m_bodyFixtures.size() && !m_bodyFixtures[idx].empty())
                    {
                        fxListB = &m_bodyFixtures[idx];
                    }
                    if (fxListB == nullptr) { continue; }
                    for (const std::uint32_t fiA : *fxListA)
                    {
                        if (fiA >= m_fxCount || m_fxGen[fiA] == 0u || m_fxSensor[fiA] != 0u) { continue; }
                        for (const std::uint32_t fiB : *fxListB)
                        {
                            if (fiB >= m_fxCount || m_fxGen[fiB] == 0u || m_fxSensor[fiB] != 0u) { continue; }
                            m_newPairs.push_back(NewPairRecord{ k, fiA, fiB });
                        }
                    }
                }
            }
            // ---- CREATE (serial, (awakeIndex, push-order)) ------------------------
            // m_newPairs is already in (k ascending, nested s/fiA/fiB) order because the
            // detect loop is serial and k-ascending -- identical to the old inline order.
            for (const NewPairRecord& rec : m_newPairs)
            {
                TryCreateContact(rec.fiA, rec.fiB);
            }
```

(Keep the `m_genSpans`/`m_genStatics` span-loop body exactly as it is today between the marked points. The ONLY behavioral change is: `TryCreateContact(fiA,fiB)` inline -> `m_newPairs.push_back({k,fiA,fiB})`, then the create pass. Stage 2c kinematic-static (~2709+) is UNCHANGED.)

- [ ] **Step 3: Build + run the full `[physics]` suite (byte-identical)**

Run: build Debug, then `.\ArcaneTests.exe "[physics]"`
Expected: ALL pass, identical counts. Records are pushed in the same k-ascending nested order the old inline loop created in, so the create order (and thus pool ids + colors) is identical. If anything diverges, the record/create order does not match the old inline order — diff against the original loop.

- [ ] **Step 4: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp
git commit  # refactor(arcane/physics): detect-into-records + serial create (byte-identical, create-MT seam)
```

---

## Task 3: MT==serial create-heavy byte-identity test (guard, before MT wiring)

**Files:**
- Modify: `Arcane/Tests/src/PhysicsNarrowphaseMtTest.cpp` (add a create-heavy case; reuse the file's existing `RunCapture`/executor helpers)

**Interfaces:**
- Consumes: the existing test's executor-injection + capture helpers (`RunCapture`, `SerialTaskExecutor`/`JobSystem`, `Position(h)`/`GetAngle(h)`). Read the file first and reuse them verbatim.

- [ ] **Step 1: Read `PhysicsNarrowphaseMtTest.cpp`** and reuse its `RunCapture(executor)` + scene-builder pattern. Add a create-heavy scene builder + a new `TEST_CASE` that captures serial vs multi-worker and asserts exact equality. A create-heavy scene = many dynamic bodies continuously raining onto a STATIC structure (lots of new dynamic-vs-static pairs forming every step):

```cpp
// Create-heavy churn: dynamic bodies rain onto a multi-fixture static field, so
// new dynamic-vs-static contacts form every step -> exercises the parallel CREATE
// detect (Task 4). Mirrors the existing update-phase MT case's capture+executors.
TEST_CASE("Narrowphase create MT == serial: state bit-identical", "[physics][mt]")
{
    auto build = [](PhysicsWorld& w) {
        // a row of static blocks (the "static field") + raining dynamics
        std::uint32_t seed = 99991u;
        auto rnd = [&](Real a, Real b){ seed = seed*1664525u + 1013904223u;
            return a + (b-a) * Real((seed>>8)&0xFFFF)/Real(65535); };
        for (int s = 0; s < 12; ++s) {
            BodyDef st; st.type=BodyType::Static;
            st.position=Vec2(Real(-330 + s*60), Real(280));
            st.shape=MakeAabb(Real(26), Real(12)); w.AddBody(st);
        }
        for (int j = 0; j < 140; ++j) {
            BodyDef d; d.type=BodyType::Dynamic; d.density=Real(1); d.friction=Real(0.4);
            d.position=Vec2(rnd(Real(-330),Real(330)), rnd(Real(-260),Real(220)));
            if (j%3==0) d.shape=MakeCircle(rnd(Real(6),Real(11)));
            else if (j%3==1){ d.shape=MakeAabb(Real(8),Real(8)); d.fixedRotation=true; }
            else d.shape=MakeCapsule(Real(10),Real(5));
            w.AddBody(d);
        }
    };
    const std::vector<Real> serial = RunCapture(/*executor*/ nullptr, build); // adapt to the file's RunCapture signature
    const std::vector<Real> multi  = RunCapture(/*all-cores*/ nullptr, build);
    REQUIRE(serial.size() == multi.size());
    for (std::size_t i = 0; i < serial.size(); ++i) { REQUIRE(serial[i] == multi[i]); }
}
```

(Adapt the `RunCapture` call shape + executor construction to EXACTLY match the existing case in this file — the existing case already constructs `SerialTaskExecutor`/`JobSystem(0)` and captures `Position(h)`/`GetAngle(h)`; pass this `build` lambda the same way. Keep the exact `==` comparison.)

- [ ] **Step 2: Build + run**

Run: build Debug, `.\ArcaneTests.exe "[physics][mt]"`
Expected: PASS (pre-Task-4 the create path is still serial, so both runs are trivially identical -> establishes the guard).

- [ ] **Step 3: Commit**

```bash
git add Arcane/Tests/src/PhysicsNarrowphaseMtTest.cpp
git commit  # test(arcane/physics): create-phase MT==serial byte-identity guard (create-heavy churn)
```

---

## Task 4: Parallelize the detect (the win)

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` (per-worker scratch + a `SpanEntry`)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (the detect loop -> `ParallelFor`; create tail -> ordered merge)

**Interfaces:**
- Consumes: `Executor()->ParallelFor`/`WorkerCount()` (copy the call shape from the update-phase ParallelFor at ~2802 and the Phase-1 mover-mover ParallelFor at ~2473); `StaticCandidates(...,gridScratch)` (Task 1); `m_newPairs`/`NewPairRecord` (Task 2).
- Produces: per-worker scratch vectors sized to `WorkerCount()`.

- [ ] **Step 1: Add per-worker scratch + a span entry** in `PhysicsWorld.hpp`, next to `m_newPairs`:

```cpp
            // Create-phase MT per-worker scratch (sized to WorkerCount() each step,
            // grow-only). Each worker uses ONLY its own [w] entry -> contention-free.
            struct SpanEntry { std::uint32_t awakeIndex; Contact c; Vec2 center; };
            std::vector<std::vector<Aabb2>>         m_genSpansW;       // per-worker StaticCandidates spans out
            std::vector<std::vector<std::uint32_t>> m_genStaticsW;     // per-worker statics out
            std::vector<std::vector<std::uint32_t>> m_gridScratchW;    // per-worker QueryAABB scratch
            std::vector<std::vector<SpanEntry>>     m_spanEntriesW;    // per-worker span contacts (awakeIndex-tagged)
            std::vector<std::vector<NewPairRecord>> m_newPairsW;       // per-worker new-pair records
```

- [ ] **Step 2: Replace the serial detect loop with a `ParallelFor`** in `PhysicsWorld.cpp`. Size the per-worker scratch, dispatch the detect, then do the serial ordered create + span merge. The per-body body is the Task-2 detect body, but writing to `m_*W[worker]` and pushing span entries (tagged with `k`) instead of to `m_spanContacts` directly:

```cpp
            m_spanContacts.clear();
            m_spanCenters.clear();
            m_newPairs.clear();
            const Real moveDt = dt > Real(0) ? dt : Real(0);
            const Real threshSq = (moveDt > Real(0))
                                      ? (kSkin / moveDt) * (kSkin / moveDt)
                                      : Real(0);
            const std::uint32_t awakeCount = AwakeCount();
            const std::uint32_t workers = Executor()->WorkerCount();
            if (m_genSpansW.size()    < workers) { m_genSpansW.resize(workers); }
            if (m_genStaticsW.size()  < workers) { m_genStaticsW.resize(workers); }
            if (m_gridScratchW.size() < workers) { m_gridScratchW.resize(workers); }
            if (m_spanEntriesW.size() < workers) { m_spanEntriesW.resize(workers); }
            if (m_newPairsW.size()    < workers) { m_newPairsW.resize(workers); }
            for (std::uint32_t w = 0; w < workers; ++w)
            {
                m_spanEntriesW[w].clear();
                m_newPairsW[w].clear();
            }
            // ---- DETECT (parallel; writes ONLY per-worker scratch) ----------------
            Executor()->ParallelFor(awakeCount, /*minRange=*/kCreateGrain,
                [&](std::size_t begin, std::size_t end, std::uint32_t worker)
                {
                    std::vector<Aabb2>&         spans   = m_genSpansW[worker];
                    std::vector<std::uint32_t>& statics = m_genStaticsW[worker];
                    std::vector<std::uint32_t>& grid    = m_gridScratchW[worker];
                    std::vector<SpanEntry>&     spanOut = m_spanEntriesW[worker];
                    std::vector<NewPairRecord>& pairOut = m_newPairsW[worker];
                    for (std::size_t kk = begin; kk < end; ++kk)
                    {
                        const std::uint32_t k = static_cast<std::uint32_t>(kk);
                        const std::uint32_t i = AwakeBodies()[k];
                        if (m_sensor[i] != 0) { continue; }
                        const Real speedSqA   = m_velX[i] * m_velX[i] + m_velY[i] * m_velY[i];
                        const Real specMargin = (speedSqA > threshSq)
                                                    ? std::sqrt(speedSqA) * moveDt : kSkin;
                        const Aabb box = SlotAabb(i);
                        const Real pad = std::max(Real(2), specMargin);
                        Aabb2 query;
                        query.min = Vec2(box.min.x - pad, box.min.y - pad);
                        query.max = Vec2(box.max.x + pad, box.max.y + pad);
                        StaticCandidates(query, spans, statics, grid);

                        const std::vector<std::uint32_t>* fxListA = nullptr;
                        if (i < m_bodyFixtures.size() && !m_bodyFixtures[i].empty())
                        {
                            fxListA = &m_bodyFixtures[i];
                        }
                        // span loop: build SpanEntry{k,c,center} into spanOut
                        // [transcribe the existing span loop body, but instead of
                        //  m_spanContacts.push_back(c)/m_spanCenters.push_back(center),
                        //  do spanOut.push_back(SpanEntry{ k, c, spanCenter });]
                        // ... (same Collide + filters as Task 2/today) ...

                        if (fxListA == nullptr) { continue; }
                        for (std::size_t s = 0; s < statics.size(); ++s)
                        {
                            const std::uint32_t idx = statics[s];
                            if (idx >= m_count || m_alive[idx] == 0 || m_sensor[idx] != 0) { continue; }
                            const std::vector<std::uint32_t>* fxListB = nullptr;
                            if (idx < m_bodyFixtures.size() && !m_bodyFixtures[idx].empty())
                            {
                                fxListB = &m_bodyFixtures[idx];
                            }
                            if (fxListB == nullptr) { continue; }
                            for (const std::uint32_t fiA : *fxListA)
                            {
                                if (fiA >= m_fxCount || m_fxGen[fiA] == 0u || m_fxSensor[fiA] != 0u) { continue; }
                                for (const std::uint32_t fiB : *fxListB)
                                {
                                    if (fiB >= m_fxCount || m_fxGen[fiB] == 0u || m_fxSensor[fiB] != 0u) { continue; }
                                    pairOut.push_back(NewPairRecord{ k, fiA, fiB });
                                }
                            }
                        }
                    }
                });
            // ---- SERIAL APPLY: order by awakeIndex, reproduce ForEachAwake order ---
            // Spans: concatenate all per-worker entries + stable_sort by awakeIndex,
            // then append. Each k is handled by exactly one worker (disjoint ranges),
            // so within-k push order is preserved -> byte-identical to the serial
            // span-push order.
            {
                std::vector<SpanEntry> allSpans;
                for (std::uint32_t w = 0; w < workers; ++w)
                    allSpans.insert(allSpans.end(), m_spanEntriesW[w].begin(), m_spanEntriesW[w].end());
                std::stable_sort(allSpans.begin(), allSpans.end(),
                    [](const SpanEntry& a, const SpanEntry& b){ return a.awakeIndex < b.awakeIndex; });
                for (const SpanEntry& e : allSpans)
                {
                    m_spanContacts.push_back(e.c);
                    m_spanCenters.push_back(e.center);
                }
            }
            // New pairs: same merge, then create serially (AssignContactColor here).
            m_newPairs.clear();
            for (std::uint32_t w = 0; w < workers; ++w)
                m_newPairs.insert(m_newPairs.end(), m_newPairsW[w].begin(), m_newPairsW[w].end());
            std::stable_sort(m_newPairs.begin(), m_newPairs.end(),
                [](const NewPairRecord& a, const NewPairRecord& b){ return a.awakeIndex < b.awakeIndex; });
            for (const NewPairRecord& rec : m_newPairs)
            {
                TryCreateContact(rec.fiA, rec.fiB);
            }
```

Add `static constexpr std::size_t kCreateGrain = 16;` near the other grain constants in `PhysicsWorld.hpp` (e.g. by `kBroadphaseGrain`, ~1432). `std::stable_sort` needs `<algorithm>`, already included in the TU (`std::sort(m_pendingMerges)` is used in this same `UpdateContacts`), so no new include. The span-loop body inside the `ParallelFor` is the same Collide + filters as Task 2/today, with the ONLY change being: where today's code does `m_spanContacts.push_back(c); m_spanCenters.push_back(spanCenter);` (in BOTH the `fxListA` branch and the no-fixture fallback branch), instead do `spanOut.push_back(SpanEntry{ k, c, spanCenter });`.

- [ ] **Step 3: Build + run the create-heavy MT test (now actually parallel)**

Run: build Debug, `.\ArcaneTests.exe "[physics][mt]"`
Expected: PASS — the multi-worker run genuinely parallelizes the detect yet matches serial bit-for-bit. If it FAILS: a non-disjoint write slipped into the detect (check each worker touches only `m_*W[worker]`), or the apply order is wrong (the stable_sort key must be `awakeIndex`).

- [ ] **Step 4: Run the full `[physics]` suite (byte-identical, no re-baseline)**

Run: `.\ArcaneTests.exe "[physics]"`
Expected: ALL pass, identical counts. Determinism/run-twice tests green.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp
git commit  # feat(arcane/physics): parallel create-phase detect (Box2D parallel-find/serial-create, byte-identical)
```

---

## Task 5: Perf A/B + revert throwaway instrumentation

**Files:**
- Modify (temporarily, then revert): `Arcane/Core/src/Arcane/Physics/StepProf.hpp`, `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp`, `Arcane/Sandbox/src/Scenes.cpp`

- [ ] **Step 1: Add throwaway create-phase timing.** Set `ARCANE_STEPPROF 1` in `StepProf.hpp`. In `PhysicsWorld.cpp` `UpdateContacts`, bracket the create phase (the detect `ParallelFor` + the serial apply) with chrono timers behind `#if ARCANE_STEPPROF` and a `std::printf` dump (mirror the prior NPPROF block reverted in the update-phase Task 6 — `git show 162374d1` shows the exact shape to reuse). Add `ARCANE_PILE_COUNT` to `Scenes.cpp` `BuildStressTest` for a 2000-body scene (again per `162374d1`). Add the `#include <chrono>/<cstdio>/<cstdlib>` THROWAWAY lines as needed.

- [ ] **Step 2: Build Dist + measure.** Build Dist. From `Arcane/bin/Dist-windows-x86_64-md/Loom/`:
```
set ARCANE_SANDBOX_SCENE=8 & set ARCANE_PILE_COUNT=2000 & set ARCANE_NPPROF=1
.\Loom.exe --frames 4000
```
Record the create-phase ms at the default (all-core) worker count and confirm it dropped vs the ~3.0 ms serial baseline (toward ~3.0/cores + barrier). Confirm a settled/low-churn scene (e.g. scene 1) shows no regression (small `awakeCount` < `kCreateGrain` stays serial). If the win is weak, tune `kCreateGrain` (try 8 / 32) and re-measure; record the chosen value.

- [ ] **Step 3: Revert the throwaway instrumentation.**
  - `StepProf.hpp`: restore `#define ARCANE_STEPPROF 0`.
  - `PhysicsWorld.cpp`: remove the THROWAWAY `#include <chrono>/<cstdio>/<cstdlib>` lines and the create-phase `#if ARCANE_STEPPROF` timer/dump block.
  - `Scenes.cpp`: restore `BuildStressTest` and remove the `ARCANE_PILE_COUNT` block + `<cstdlib>` include.
  Keep `kCreateGrain` at its tuned value (that is real, not throwaway).

- [ ] **Step 4: Build + full suite green after revert**

Run: build Debug, `.\ArcaneTests.exe "[physics]"` + `.\ArcaneTests.exe "[physics][mt]"`
Expected: ALL pass (the StepProf-off meta-test passes again).

- [ ] **Step 5: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/StepProf.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Sandbox/src/Scenes.cpp
git commit  # chore(arcane/physics): revert throwaway create-phase-MT perf instrumentation
```

---

## Final verification

- [ ] Build Debug + Dist clean.
- [ ] `.\ArcaneTests.exe ~[gpu]` green; `[physics][mt]` (both update + create cases) + `[physics]` green, no re-baseline.
- [ ] Headless Loom GPU-verify clean: `Loom.exe --backend vulkan --frames 180` (scene 8) exits 0.
- [ ] Push branch; CI green; hand to T3mps for review/merge.

## Confirm-against-real-code (verify names before coding)

- `StaticCandidates(const Aabb2&, std::vector<Aabb2>&, std::vector<std::uint32_t>&) const` @ Queries.cpp:537 / decl PhysicsWorld.hpp:1139; internal `m_staticGrid.QueryAABB(box, m_staticGridScratch)`; `m_staticGridScratch` @ PhysicsWorld.hpp:1376.
- Stage-2b create loop @ PhysicsWorld.cpp ~2555-2707 (`ForEachAwake` body; span loop ~2598-2662; static loop + `TryCreateContact(fiA,fiB)` ~2669-2705). Stage-2c kinematic-static ~2709+ (leave serial).
- `AwakeBodies()` -> `const std::vector<std::uint32_t>&` @ PhysicsWorld.hpp:782; `AwakeCount()` @ 801; `ForEachAwake` @ 784; `m_awakeBodies` @ 1327.
- `TryCreateContact(std::uint32_t fa, std::uint32_t fb)` @ PhysicsWorld.hpp:1480.
- `m_genSpans`@1599 / `m_genStatics`@1600 / `m_spanContacts`@1667 / `m_spanCenters`@1668.
- `Executor()->ParallelFor(count, minRange, fn(begin,end,worker))` + `WorkerCount()` — exact shape at the update-phase create-MT ParallelFor (PhysicsWorld.cpp ~2802) and the Phase-1 mover-mover ParallelFor (~2473).
- The `[physics][mt]` test helpers (`RunCapture`, executor construction, `Position(h)`/`GetAngle(h)`) — read `PhysicsNarrowphaseMtTest.cpp` and reuse verbatim.
- **Verify-during-impl (from the spec):** awake-set stability during the parallel detect (wakes/sleeps happen in stage 5, after create — confirm); where D2 stage-2a mover-mover pairs get CREATED (confirm they are NOT double-created by this static path and are handled in their own create site); span-entry merge order matches today's span-push order.
