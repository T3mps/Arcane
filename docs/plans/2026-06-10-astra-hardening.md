# Astra Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Harden the Astra ECS (the engine's adopted ECS pillar) per the 5-item workstream in `docs/superpowers/specs/2026-06-10-engine-thirdparty-stack-design.md`: pure `IWorkScheduler` seam replacing ad-hoc `std::async` (Astra creates ZERO threads — user decision 2026-06-10: core is single-threaded, CommandBuffer defers modifications, job system stays open-ended; enkiTS adapter lives in the engine, never in Astra), cross-DLL TypeContext, container differential-fuzz tests, MSVC+gcc+clang CI, and doc reconciliation.

**Architecture:** All work happens in **`D:\dev\starworks\Astra`** (the user's working copy — NOT a git repo). Code verifies locally via MSVC build + GoogleTest suite. Each task group ends with a sync to the git repo `D:\dev\github\Astra` (branch `hardening/v3.1`) where commits happen; GitHub Actions CI (Task 11) is the gcc/clang verifier. Library is header-only C++20, exception-free (Result types), GoogleTest + GoogleBenchmark vendored, premake5.

**Tech Stack:** C++20, premake5 (vs2026 locally / vs2022+gmake2 in CI), GoogleTest, GoogleBenchmark, GitHub Actions.

**Key existing code (from exploration):**
- Ad-hoc `std::async` sites: `include/Astra/Registry/View.hpp:78-153` (ParallelForEach), `include/Astra/Registry/Relations.hpp:196-268` (ParallelForEachDescendant), `include/Astra/System/SystemExecutor.hpp:31-64` (ParallelExecutor — uncapped, never benchmarked).
- TypeID: `include/Astra/Core/TypeID.hpp` — `TypeIDGenerator::s_nextId` inline-static atomic (per-module = the DLL bug); stable `TypeID<T>::Hash()` (XXHash64 of name) already exists. **ComponentMask bit index == ComponentID** (`Archetype.hpp:105-110` MakeComponentMask), so the context MUST assign dense sequential IDs. NOTE: systems (SystemScheduler.hpp:57,120,390) draw from the same counter as components today — preserve single-counter semantics.
- `MetaRegistry::Instance()` magic static: `include/Astra/Reflection/MetaRegistry.hpp` (second cross-DLL hazard; holds hash→TypeMeta, hash↔ComponentID maps, shared_mutex).
- `ComponentRegistry::RegisterComponent<T>()`: `include/Astra/Component/ComponentRegistry.hpp:22-99` — early-outs on `Contains(id)`; descriptors hold raw function pointers into the defining module (the hot-reload dangling hazard).
- Containers: `include/Astra/Container/FlatMap.hpp` / `FlatSet.hpp` (Swiss tables: H1 bottom bits, H2 top-7, 16-slot groups, TOMBSTONE=0xFE, load factor 0.875), `SmallVector.hpp` (N=4 inline), `Bitmap.hpp`. Existing tests cover basics; NOT covered: deletion-heavy tombstone churn, rehash-during-iteration, adversarial hashes, repeated inline↔heap spill.
- Tests: `tests/<Subsystem>/XxxTest.cpp`, custom `TestMain.cpp` (InitGoogleTest). Benchmarks: `benchmark/Benchmark.cpp`; `BM_SystemScheduler_Parallel` currently does NOT use ParallelExecutor (bug). Measured baseline: ParallelForEach **wall-time loses** to sequential (2.43ms vs 1.30ms @1M single-component).
- Build: `premake5.lua` (workspace Astra; AstraTest links GoogleTest, rtti on, exceptions off, `GTEST_HAS_EXCEPTIONS=0`; `/Zc:__cplusplus /arch:AVX /bigobj /fp:fast` + manual `__SSE2__`/`__SSE4_2__` defines on Windows; `-fopenmp` everywhere; `-march=native` benchmark-only). Scripts: `scripts/generate_vs2022.bat|generate_linux.sh|generate_macos.sh`; premake assumed on PATH (locally use `D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe`). Release notes: `release_notes/v1.0.0.md`..`v3.0.0.md`; `include/Astra/Core/Version.hpp` exists.

**Local build/test loop (used by every task):**
```powershell
cd D:\dev\starworks\Astra
& D:\dev\starworks\Gacha\ThirdParty\premake5\premake5.exe vs2026   # only when premake5.lua or file lists change
$vsroot = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
& (Join-Path $vsroot "MSBuild\Current\Bin\MSBuild.exe") Astra.slnx /p:Configuration=Release /m /v:minimal /nologo
& bin\Release-windows-x86_64\AstraTest\AstraTest.exe --gtest_brief=1
```
Expected: build succeeds, `[  PASSED  ]` with 0 failures (baseline: 455 tests).

**Sync-and-commit loop (end of each task group):** run `scripts\sync_to_github.ps1` (created in Task 0), then commit in `D:\dev\github\Astra` on branch `hardening/v3.1`.

---

### Task 0: Sync script + git branch

**Files:**
- Create: `D:\dev\starworks\Astra\scripts\sync_to_github.ps1`

- [ ] **Step 1: Create the branch in the git repo**

```powershell
git -C D:\dev\github\Astra checkout -b hardening/v3.1
```
Expected: `Switched to a new branch 'hardening/v3.1'`.

- [ ] **Step 2: Write the sync script**

```powershell
# scripts/sync_to_github.ps1 — mirror working copy into the git repo.
# Excludes build artifacts and the destination's .git (robocopy /XD also
# protects excluded dirs from /MIR deletion).
$src = "D:\dev\starworks\Astra"
$dst = "D:\dev\github\Astra"
robocopy $src $dst /MIR /NFL /NDL /NJH `
    /XD .git bin bin-int ide .vs `
    /XF *.user error_list.txt Astra.slnx Astra.sln
if ($LASTEXITCODE -ge 8) { Write-Error "robocopy failed ($LASTEXITCODE)"; exit 1 }
Write-Host "Synced. Review with: git -C $dst status"
exit 0
```

- [ ] **Step 3: Dry-run verify**

Run: `powershell -File D:\dev\starworks\Astra\scripts\sync_to_github.ps1` then `git -C D:\dev\github\Astra status --short`.
Expected: only intentional differences between the two trees appear (the script itself + any pre-existing divergence; if large unexplained divergence appears, STOP and show the user before committing anything).

- [ ] **Step 4: Commit (in the git repo)**

```powershell
git -C D:\dev\github\Astra add -A; git -C D:\dev\github\Astra commit -m "chore: add sync script from working copy"
```

---

### Task 1: IWorkScheduler seam (interface-only) + reference test pool

**Design (user decision, 2026-06-10):** Astra ships NO threads. `include/Astra/Core/WorkScheduler.hpp` contains only the `IWorkScheduler` interface; every Parallel* API runs sequentially inline when no scheduler is injected. The multithreaded fork-join pool below exists ONLY as test/benchmark support (`tests/Support/TestWorkerPool.hpp`) so the parallel code paths get exercised — the real scheduler is the engine's enkiTS adapter, which stays in the engine repo.

**Files:**
- Create: `include/Astra/Core/WorkScheduler.hpp` (interface only)
- Create: `tests/Support/TestWorkerPool.hpp` (reference implementation, NOT shipped in include/)
- Create: `tests/Core/WorkSchedulerTest.cpp`
- Modify: `premake5.lua` — add `"tests"` to AstraBenchmark's `includedirs` (benchmarks reuse the reference pool in Task 4); AstraTest needs nothing (it globs `tests/**`). Regenerate the solution.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/Core/WorkSchedulerTest.cpp
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <Astra/Core/WorkScheduler.hpp>
#include "../Support/TestWorkerPool.hpp"

using Astra::Testing::TestWorkerPool;

namespace
{
    TEST(WorkerPool, ProcessesEveryIndexExactlyOnce)
    {
        TestWorkerPool pool;
        constexpr size_t kCount = 100'000;
        std::vector<std::atomic<int>> hits(kCount);
        pool.ParallelFor(kCount, 64, [&](size_t begin, size_t end)
        {
            for (size_t i = begin; i < end; ++i)
                hits[i].fetch_add(1, std::memory_order_relaxed);
        });
        for (size_t i = 0; i < kCount; ++i)
            ASSERT_EQ(hits[i].load(), 1) << "index " << i;
    }

    TEST(WorkerPool, ReusableAcrossManyCalls)
    {
        TestWorkerPool pool;
        for (int iter = 0; iter < 200; ++iter)
        {
            std::atomic<size_t> sum{0};
            pool.ParallelFor(1000, 16, [&](size_t b, size_t e)
            {
                sum.fetch_add(e - b, std::memory_order_relaxed);
            });
            ASSERT_EQ(sum.load(), 1000u);
        }
    }

    TEST(WorkerPool, SmallCountRunsInline)
    {
        TestWorkerPool pool;
        const auto caller = std::this_thread::get_id();
        std::atomic<bool> sameThread{true};
        pool.ParallelFor(8, 64, [&](size_t, size_t)  // count <= minBatch
        {
            if (std::this_thread::get_id() != caller) sameThread = false;
        });
        EXPECT_TRUE(sameThread.load());
    }

    TEST(WorkerPool, ZeroCountIsNoop)
    {
        TestWorkerPool pool;
        bool called = false;
        pool.ParallelFor(0, 16, [&](size_t, size_t) { called = true; });
        EXPECT_FALSE(called);
    }

    TEST(WorkerPool, NestedCallRunsInline)
    {
        TestWorkerPool pool;
        std::atomic<size_t> inner{0};
        pool.ParallelFor(4 * pool.WorkerCount() + 4, 1, [&](size_t b, size_t e)
        {
            for (size_t i = b; i < e; ++i)
                pool.ParallelFor(10, 1, [&](size_t b2, size_t e2)  // must not deadlock
                {
                    inner.fetch_add(e2 - b2, std::memory_order_relaxed);
                });
        });
        EXPECT_EQ(inner.load(), (4 * pool.WorkerCount() + 4) * 10);
    }

    TEST(WorkerPool, ConcurrentExternalCallersAreSafe)
    {
        TestWorkerPool pool;
        std::vector<std::thread> callers;
        std::atomic<size_t> total{0};
        for (int t = 0; t < 4; ++t)
            callers.emplace_back([&]
            {
                for (int i = 0; i < 50; ++i)
                    pool.ParallelFor(500, 8, [&](size_t b, size_t e)
                    {
                        total.fetch_add(e - b, std::memory_order_relaxed);
                    });
            });
        for (auto& th : callers) th.join();
        EXPECT_EQ(total.load(), 4u * 50u * 500u);
    }

    TEST(WorkerPool, ExplicitThreadCount)
    {
        TestWorkerPool pool(2);
        EXPECT_EQ(pool.WorkerCount(), 2u);
        std::atomic<size_t> sum{0};
        pool.ParallelFor(10'000, 64, [&](size_t b, size_t e) { sum += e - b; });
        EXPECT_EQ(sum.load(), 10'000u);
    }
}
```

- [ ] **Step 2: Regenerate + build to verify failure**

Run the local build loop. Expected: **compile error** — `Astra/Core/WorkScheduler.hpp` not found.

- [ ] **Step 3a: Implement `include/Astra/Core/WorkScheduler.hpp` (interface ONLY)**

```cpp
#pragma once

#include <cstddef>
#include <functional>

#include "Base.hpp"

namespace Astra
{
    // Astra deliberately creates NO threads. Every Parallel* API accepts an
    // implementation of this seam (see Registry::Config::workScheduler);
    // when none is provided, the API executes sequentially inline. Hook up
    // the job system of your choice in the host application (e.g. an
    // enkiTS-backed adapter) — Astra itself stays scheduler-agnostic.
    class IWorkScheduler
    {
    public:
        virtual ~IWorkScheduler() = default;

        // Partition [0, count) into batches of at least minBatch and invoke
        // fn(begin, end) for each batch, possibly concurrently. Blocks until
        // all batches complete. Must be safe to call from multiple threads
        // and re-entrantly from inside fn (implementations may degrade to
        // inline execution in either case).
        virtual void ParallelFor(size_t count, size_t minBatch,
                                 const std::function<void(size_t, size_t)>& fn) = 0;

        ASTRA_NODISCARD virtual size_t WorkerCount() const noexcept = 0;
    };
}
```

- [ ] **Step 3b: Implement `tests/Support/TestWorkerPool.hpp` (reference pool, test-side only)**

Design: fork-join pool, one in-flight job at a time (external `ParallelFor` calls serialized by `m_submitMutex`); the calling thread participates; job state is a `std::shared_ptr` copied by workers under lock (lifetime-safe against late wakers); nested calls from worker threads detected via `thread_local` and run inline (no deadlock). Exception-free (matches library policy). Lives in `Astra::Testing` so it can never be mistaken for shipped API.

```cpp
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <Astra/Core/WorkScheduler.hpp>

namespace Astra::Testing
{
    class TestWorkerPool final : public IWorkScheduler
    {
    public:
        explicit TestWorkerPool(size_t threadCount = 0)
        {
            if (threadCount == 0)
            {
                const size_t hw = std::thread::hardware_concurrency();
                threadCount = hw > 1 ? hw - 1 : 1;  // caller participates too
            }
            m_threads.reserve(threadCount);
            for (size_t i = 0; i < threadCount; ++i)
                m_threads.emplace_back([this] { WorkerLoop(); });
        }

        ~TestWorkerPool() override
        {
            {
                std::lock_guard lock(m_mutex);
                m_stop = true;
            }
            m_wakeCv.notify_all();
            for (auto& t : m_threads) t.join();
        }

        TestWorkerPool(const TestWorkerPool&) = delete;
        TestWorkerPool& operator=(const TestWorkerPool&) = delete;

        void ParallelFor(size_t count, size_t minBatch,
                         const std::function<void(size_t, size_t)>& fn) override
        {
            if (count == 0)
                return;
            if (minBatch == 0)
                minBatch = 1;
            // Inline when: trivial size, no workers, or nested call from a
            // worker thread (taking m_submitMutex there would deadlock).
            if (count <= minBatch || m_threads.empty() || t_insideWorker)
            {
                fn(0, count);
                return;
            }

            std::lock_guard submitLock(m_submitMutex);

            auto job = std::make_shared<Job>();
            job->fn = &fn;
            job->count = count;
            job->batch = minBatch;

            {
                std::lock_guard lock(m_mutex);
                m_job = job;
                ++m_generation;
            }
            m_wakeCv.notify_all();

            RunJob(*job);  // caller participates

            // Wait until every participant has drained out of the job.
            {
                std::unique_lock lock(m_mutex);
                m_doneCv.wait(lock, [&]
                {
                    return job->next.load(std::memory_order_acquire) >= job->count &&
                           job->active.load(std::memory_order_acquire) == 0;
                });
                m_job.reset();  // late wakers see a null job and skip
            }
        }

        ASTRA_NODISCARD size_t WorkerCount() const noexcept override
        {
            return m_threads.size();
        }

    private:
        struct Job
        {
            const std::function<void(size_t, size_t)>* fn = nullptr;
            size_t count = 0;
            size_t batch = 1;
            std::atomic<size_t> next{0};
            std::atomic<size_t> active{0};
        };

        void WorkerLoop()
        {
            t_insideWorker = true;
            uint64_t seen = 0;
            for (;;)
            {
                std::shared_ptr<Job> job;
                {
                    std::unique_lock lock(m_mutex);
                    m_wakeCv.wait(lock, [&] { return m_stop || m_generation != seen; });
                    if (m_stop)
                        return;
                    seen = m_generation;
                    job = m_job;  // may be null if we woke late
                }
                if (job)
                    RunJob(*job);
            }
        }

        void RunJob(Job& job)
        {
            job.active.fetch_add(1, std::memory_order_acq_rel);
            size_t i;
            while ((i = job.next.fetch_add(job.batch, std::memory_order_relaxed)) < job.count)
            {
                const size_t end = i + job.batch < job.count ? i + job.batch : job.count;
                (*job.fn)(i, end);
            }
            if (job.active.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                std::lock_guard lock(m_mutex);  // pairs with the waiter's predicate read
                m_doneCv.notify_all();
            }
        }

        inline static thread_local bool t_insideWorker = false;

        std::vector<std::thread> m_threads;
        std::mutex m_mutex;
        std::condition_variable m_wakeCv;
        std::condition_variable m_doneCv;
        std::shared_ptr<Job> m_job;
        uint64_t m_generation = 0;
        bool m_stop = false;
        std::mutex m_submitMutex;
    };
}
```

- [ ] **Step 4: Build + run the new tests**

Run: local build loop, then `AstraTest.exe --gtest_filter=WorkerPool.* --gtest_brief=1`.
Expected: 7 tests PASS. Then full suite: 455 + 7 pass. Also verify the library stays thread-free: `rg "std::thread|std::async" D:\dev\starworks\Astra\include` must show NO matches in `Core/WorkScheduler.hpp` (the View/Relations/SystemExecutor matches disappear in Tasks 2-3; CommandBuffer's `hardware_concurrency` sizing hint may remain — it creates no threads).

- [ ] **Step 5: Sync + commit**

```powershell
powershell -File D:\dev\starworks\Astra\scripts\sync_to_github.ps1
git -C D:\dev\github\Astra add -A; git -C D:\dev\github\Astra commit -m "feat(core): IWorkScheduler seam (interface-only, zero threads in library) + reference test pool"
```

---

### Task 2: Route View::ParallelForEach through the scheduler

**Files:**
- Modify: `include/Astra/Registry/Registry.hpp` (Config + member + CreateView, ~lines 36-49, 971)
- Modify: `include/Astra/Registry/View.hpp` (ctor + ParallelForEach, lines 41-49, 78-153)
- Create: `tests/Registry/ParallelIterationTest.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/Registry/ParallelIterationTest.cpp
#include <gtest/gtest.h>
#include <atomic>
#include <Astra/Astra.hpp>
#include "../Support/TestWorkerPool.hpp"
#include "../TestComponents.hpp"

namespace
{
    // Uses Position/Velocity from TestComponents.hpp (adjust names to that
    // header's actual component types if they differ).

    // No scheduler injected => ParallelForEach degrades to sequential inline
    // execution (Astra creates no threads). Correctness must be identical.
    TEST(ParallelIteration, NoSchedulerFallsBackSequentially)
    {
        Astra::Registry registry;
        constexpr size_t kCount = 50'000;
        std::vector<Astra::Entity> entities(kCount);
        registry.CreateEntities<Position>(kCount, entities);

        auto view = registry.CreateView<Position>();
        std::atomic<size_t> visits{0};
        view.ParallelForEach([&](Astra::Entity, Position& p)
        {
            p.x += 1.0f;
            visits.fetch_add(1, std::memory_order_relaxed);
        });
        EXPECT_EQ(visits.load(), kCount);

        // Every entity was mutated exactly once (sequential read-back).
        size_t mutated = 0;
        view.ForEach([&](Astra::Entity, Position& p)
        {
            if (p.x == 1.0f) ++mutated;
        });
        EXPECT_EQ(mutated, kCount);
    }

    TEST(ParallelIteration, InjectedSchedulerIsUsed)
    {
        struct CountingScheduler final : Astra::IWorkScheduler
        {
            std::shared_ptr<Astra::Testing::TestWorkerPool> inner =
                std::make_shared<Astra::Testing::TestWorkerPool>();
            std::atomic<int> calls{0};
            void ParallelFor(size_t count, size_t minBatch,
                             const std::function<void(size_t, size_t)>& fn) override
            {
                calls.fetch_add(1);
                inner->ParallelFor(count, minBatch, fn);
            }
            size_t WorkerCount() const noexcept override { return inner->WorkerCount(); }
        };

        auto sched = std::make_shared<CountingScheduler>();
        Astra::Registry::Config config;
        config.workScheduler = sched;
        Astra::Registry registry(config);

        constexpr size_t kCount = 50'000;  // big enough to clear parallel thresholds
        std::vector<Astra::Entity> entities(kCount);
        registry.CreateEntities<Position>(kCount, entities);

        auto view = registry.CreateView<Position>();
        view.ParallelForEach([](Astra::Entity, Position&) {});
        EXPECT_GE(sched->calls.load(), 1);
    }
}
```

- [ ] **Step 2: Build to verify failure**

Expected: compile error — `Config` has no member `workScheduler`.

- [ ] **Step 3: Implement**

In `Registry.hpp`: include `"../Core/WorkScheduler.hpp"`; add to `Registry::Config`:
```cpp
// Scheduler used by parallel iteration/execution. Astra creates no threads:
// null (the default) means every Parallel* API runs sequentially inline.
// Hosts inject one shared instance (e.g. an enkiTS adapter) — and in
// multi-module (DLL) setups, the SAME instance into every module.
std::shared_ptr<IWorkScheduler> workScheduler;
```
In the constructors that take a `Config`, copy it straight through (`m_workScheduler = config.workScheduler;` — null stays null); the ctors without a `Config` leave it null. Add `std::shared_ptr<IWorkScheduler> m_workScheduler;` to members. In `CreateView` (line ~971) pass `m_workScheduler` as second ctor arg.

In `View.hpp`: add member `std::shared_ptr<IWorkScheduler> m_scheduler;`, extend the ctor:
```cpp
explicit View(std::shared_ptr<ArchetypeManager> manager,
              std::shared_ptr<IWorkScheduler> scheduler = nullptr) :
    m_archetypeManager(manager),
    m_scheduler(std::move(scheduler)),   // null => sequential fallback
    ...
```
In `ParallelForEach`, right after the existing manager/empty early-outs, add the no-scheduler fallback:
```cpp
if (!m_scheduler)
    return ForEach(std::forward<Func>(func));   // Astra spawns no threads
```
then replace the body from the thread-count selection through the join (current lines ~127-152) with:
```cpp
m_scheduler->ParallelFor(chunkWork.size(), MIN_CHUNKS_PER_THREAD,
    [&](size_t begin, size_t end)
    {
        for (size_t w = begin; w < end; ++w)
        {
            auto [archetype, chunkIndex] = chunkWork[w];
            ParallelForEachChunkImpl(archetype, chunkIndex, func, RequiredTypes{}, OptionalTypes{});
        }
    });
```
Keep the existing sequential-fallback thresholds unchanged. Remove the now-unused `<future>`/`<thread>` includes from View.hpp if nothing else uses them.

- [ ] **Step 4: Build + run**

Run full suite. Expected: all pass (462 + 2 new).

- [ ] **Step 5: Sync + commit**

```powershell
powershell -File D:\dev\starworks\Astra\scripts\sync_to_github.ps1
git -C D:\dev\github\Astra add -A; git -C D:\dev\github\Astra commit -m "feat(view): ParallelForEach runs on injected IWorkScheduler"
```

---

### Task 3: Route Relations + ParallelExecutor through the scheduler

**Files:**
- Modify: `include/Astra/Registry/Relations.hpp:196-268`
- Modify: `include/Astra/Registry/Registry.hpp` (`GetRelations`, line ~1194 — pass scheduler)
- Modify: `include/Astra/System/SystemExecutor.hpp:31-64`
- Test: extend `tests/Registry/ParallelIterationTest.cpp`

- [ ] **Step 1: Write failing tests** (append to ParallelIterationTest.cpp)

```cpp
    TEST(ParallelIteration, ParallelForEachDescendantVisitsAll)
    {
        Astra::Registry::Config config;
        config.workScheduler = std::make_shared<Astra::Testing::TestWorkerPool>();
        Astra::Registry registry(config);
        auto root = registry.CreateEntity<Position>();
        constexpr size_t kChildren = 5'000;
        std::vector<Astra::Entity> kids(kChildren);
        registry.CreateEntities<Position>(kChildren, kids);
        for (auto e : kids) registry.SetParent(e, root);

        auto relations = registry.GetRelations(root);
        std::atomic<size_t> visits{0};
        relations.ParallelForEachDescendant([&](Astra::Entity, size_t)
        {
            visits.fetch_add(1, std::memory_order_relaxed);
        });
        EXPECT_EQ(visits.load(), kChildren);
    }

    TEST(ParallelIteration, ParallelExecutorRunsAllSystems)
    {
        Astra::Registry registry;
        Astra::SystemScheduler scheduler;
        std::atomic<int> ran{0};
        // Two trait-less lambdas => conservative sequential groups; still must all run.
        scheduler.AddSystem([&](Astra::Registry&) { ran.fetch_add(1); });
        scheduler.AddSystem([&](Astra::Registry&) { ran.fetch_add(1); });
        Astra::ParallelExecutor executor(std::make_shared<Astra::Testing::TestWorkerPool>());
        scheduler.Execute(registry, &executor);
        EXPECT_EQ(ran.load(), 2);

        // No scheduler => degrades to sequential, still runs everything.
        ran = 0;
        Astra::ParallelExecutor sequentialFallback;
        scheduler.Execute(registry, &sequentialFallback);
        EXPECT_EQ(ran.load(), 2);
    }
```
(Adjust the lambda-system signature to the `LambdaLike` shape SystemScheduler.hpp:106-111 accepts — lambdas taking `Registry&`.)

- [ ] **Step 2: Build to verify failure**

Expected: compile error — `ParallelExecutor` has no scheduler ctor (and/or Relations test fails to find the new path; the descendant test may pass pre-change — that's fine, it's a regression guard).

- [ ] **Step 3: Implement**

`Relations.hpp`: add `std::shared_ptr<IWorkScheduler> m_scheduler;` member (null by default; `Registry::GetRelations` passes `m_workScheduler` — mirror exactly how Task 2 threaded it into View). In `ParallelForEachDescendant`, extend the existing sequential fallback so null-scheduler also takes it, then replace the worker-spawn block (lines ~212-267) with:
```cpp
if (!m_scheduler || count < MIN_ENTITIES_FOR_PARALLEL)
{
    // Sequential path (Astra spawns no threads) — reuse the existing
    // sequential descendant loop above.
    ...existing sequential traversal...
    return;
}
m_scheduler->ParallelFor(count, 64, [&](size_t begin, size_t end)
{
    for (size_t i = begin; i < end; ++i)
    {
        const auto& entry = cache.entries[i];
        if (PassesFilter(entry.entity))
            InvokeParallelWithDepth(entry.entity, entry.depth, func, RequiredTuple{});
    }
});
```

`SystemExecutor.hpp`: rewrite `ParallelExecutor` (null scheduler = sequential):
```cpp
struct ParallelExecutor : public ISystemExecutor
{
    ParallelExecutor() = default;  // no scheduler => sequential execution
    explicit ParallelExecutor(std::shared_ptr<IWorkScheduler> scheduler) :
        m_scheduler(std::move(scheduler))
    {}

    void Execute(const SystemExecutionContext& context) override
    {
        for (const auto& group : context.parallelGroups)
        {
            if (group.size() == 1 || !m_scheduler)
            {
                for (size_t systemIdx : group)
                    context.systems[systemIdx](*context.registry);
            }
            else
            {
                m_scheduler->ParallelFor(group.size(), 1, [&](size_t begin, size_t end)
                {
                    for (size_t i = begin; i < end; ++i)
                        context.systems[group[i]](*context.registry);
                });
            }
        }
    }

private:
    std::shared_ptr<IWorkScheduler> m_scheduler;
};
```
Add `#include "../Core/WorkScheduler.hpp"` to SystemExecutor.hpp; remove `<future>` if now unused.

- [ ] **Step 4: Build + run full suite** — all pass.

- [ ] **Step 5: Sync + commit**

```powershell
powershell -File D:\dev\starworks\Astra\scripts\sync_to_github.ps1
git -C D:\dev\github\Astra add -A; git -C D:\dev\github\Astra commit -m "feat(parallel): Relations + ParallelExecutor on IWorkScheduler; ad-hoc std::async eliminated"
```

---

### Task 4: Benchmark fixes + wall-time acceptance

**Files:**
- Modify: `benchmark/Benchmark.cpp` (lines ~1093-1282: `BM_SystemScheduler_Parallel` currently passes no executor — make it construct and pass `Astra::ParallelExecutor`)

- [ ] **Step 1: Inject the reference pool into the parallel benchmarks.** With the pure seam, a plain `Registry` runs Parallel* sequentially — so every `BM_ParallelIterate*` / `BM_ParallelForEachDescendant` benchmark must construct its registry with the pool injected, and `BM_SystemScheduler_Parallel` must actually pass a `ParallelExecutor` (today it passes nothing — pre-existing bug):
```cpp
// top of benchmark/Benchmark.cpp (after includes):
#include <Support/TestWorkerPool.hpp>   // via the new "tests" includedir from Task 1

static std::shared_ptr<Astra::IWorkScheduler> BenchPool()
{
    static auto s_pool = std::make_shared<Astra::Testing::TestWorkerPool>();
    return s_pool;
}

// in each BM_Parallel* benchmark, replace `Astra::Registry registry;` with:
Astra::Registry::Config config;
config.workScheduler = BenchPool();
Astra::Registry registry(config);

// in BM_SystemScheduler_Parallel, construct once outside the timing loop:
Astra::ParallelExecutor executor(BenchPool());
// and inside the loop:
scheduler.Execute(registry, &executor);
```

- [ ] **Step 2: Verify no `std::async` remains in the library**

Run: `rg "std::async" D:\dev\starworks\Astra\include`
Expected: no matches.

- [ ] **Step 3: Build Release benchmarks + run the comparison set**

```powershell
& bin\Release-windows-x86_64\AstraBenchmark\AstraBenchmark.exe --benchmark_filter="(ParallelIterate|IterateSingleComponent|IterateTwoComponents|SystemScheduler)" --benchmark_out=benchmark\results_hardening.json --benchmark_out_format=json
```
**Acceptance (wall-time `Time` column, not CPU):** `BM_ParallelIterateSingleComponent/1000000` ≤ `BM_IterateSingleComponent/1000000` (baseline was 2.43ms vs 1.30ms — must at least reach parity; expect materially better). At 100K, parallel within 1.2× of sequential or better. If the trivial kernel stays memory-bound near parity, record the numbers honestly in the commit message — the requirement is "no longer loses badly," with real wins expected on heavier kernels.

- [ ] **Step 4: Sync + commit** (include before/after numbers in the message)

```powershell
powershell -File D:\dev\starworks\Astra\scripts\sync_to_github.ps1
git -C D:\dev\github\Astra add -A; git -C D:\dev\github\Astra commit -m "perf(parallel): pooled execution; wall-time before/after: <numbers>"
```

---

### Task 5: TypeContext — failing tests first

**Files:**
- Create: `tests/Core/TypeContextTest.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/Core/TypeContextTest.cpp
#include <gtest/gtest.h>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/TypeID.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>  // completes TypeContext::Meta()

namespace
{
    struct CtxA { int v; };
    struct CtxB { float v; };

    TEST(TypeContext, AssignsDenseSequentialIds)
    {
        Astra::TypeContext ctx;
        EXPECT_EQ(ctx.GetOrAssignComponentID(111, "T1"), 0u);
        EXPECT_EQ(ctx.GetOrAssignComponentID(222, "T2"), 1u);
        EXPECT_EQ(ctx.GetOrAssignComponentID(333, "T3"), 2u);
    }

    TEST(TypeContext, SameHashSameId)
    {
        Astra::TypeContext ctx;
        const auto id = ctx.GetOrAssignComponentID(42, "T");
        EXPECT_EQ(ctx.GetOrAssignComponentID(42, "T"), id);
    }

    TEST(TypeContext, IndependentContextsAssignIndependently)
    {
        Astra::TypeContext a, b;
        EXPECT_EQ(a.GetOrAssignComponentID(7, "X"), 0u);
        a.GetOrAssignComponentID(8, "Y");
        EXPECT_EQ(b.GetOrAssignComponentID(7, "X"), 0u);  // b never saw Y
    }

    TEST(TypeContext, TypeIdRoutesThroughCurrentContext)
    {
        // The IDs for fresh types come from the active context and are stable.
        const auto a1 = Astra::TypeID<CtxA>::Value();
        const auto a2 = Astra::TypeID<CtxA>::Value();
        const auto b1 = Astra::TypeID<CtxB>::Value();
        EXPECT_EQ(a1, a2);
        EXPECT_NE(a1, b1);
        // And the context can resolve the stable hash back to the same id.
        EXPECT_EQ(Astra::GetTypeContext()->GetOrAssignComponentID(
                      Astra::TypeID<CtxA>::Hash(), Astra::TypeID<CtxA>::Name()),
                  a1);
    }

    TEST(TypeContext, MetaRegistryLivesInContext)
    {
        Astra::TypeContext ctx;
        ctx.Meta().Register(999u, "Fake");
        EXPECT_NE(ctx.Meta().Get(999u), nullptr);
        Astra::TypeContext other;
        EXPECT_EQ(other.Meta().Get(999u), nullptr);
    }
}
```
(Adapt `MetaRegistry::Register`'s exact signature to MetaRegistry.hpp — exploration shows `Register(uint64_t hash, std::string_view name)` and `Get(uint64_t)`; use whichever overload exists.)

- [ ] **Step 2: Build to verify failure** — `Astra/Core/TypeContext.hpp` not found.

- [ ] **Step 3: Implement `include/Astra/Core/TypeContext.hpp`**

```cpp
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "../Component/Component.hpp"
#include "../Container/FlatMap.hpp"
#include "Base.hpp"

// NOTE: deliberately does NOT include MetaRegistry.hpp — MetaRegistry's
// templated API uses TypeID, and TypeID.hpp includes this header. The meta
// registry is held through a forward declaration; the accessor is defined
// in MetaRegistry.hpp (which includes this header). Include order is:
// TypeID.hpp -> TypeContext.hpp <- MetaRegistry.hpp  (no cycle).

namespace Astra
{
    class MetaRegistry;  // see layering note above

    // Process-wide type identity service. Component/type IDs are assigned
    // densely (ComponentMask bit index == ComponentID) keyed by the STABLE
    // XXHash64 type-name hash, so every module (EXE/DLL) that shares one
    // TypeContext agrees on IDs. Hosts create one context and hand it to
    // each plugin module via SetTypeContext() BEFORE that module touches
    // any Registry/TypeID API.
    class TypeContext
    {
    public:
        ASTRA_NODISCARD ComponentID GetOrAssignComponentID(uint64_t hash, std::string_view name)
        {
            std::lock_guard lock(m_mutex);
            if (auto it = m_hashToId.Find(hash); it != m_hashToId.end())
            {
#ifdef ASTRA_BUILD_DEBUG
                ASTRA_ASSERT(m_names[it->second] == name,
                             "TypeContext hash collision: two distinct type names share a hash");
#endif
                return it->second;
            }
            ASTRA_ASSERT(m_next != INVALID_COMPONENT, "TypeContext ID space exhausted");
            const ComponentID id = m_next++;
            m_hashToId[hash] = id;
            m_names.emplace_back(name);
            return id;
        }

        // Defined inline in MetaRegistry.hpp (lazy-constructs the registry);
        // declared here against the forward declaration.
        ASTRA_NODISCARD MetaRegistry& Meta();

    private:
        std::mutex m_mutex;
        FlatMap<uint64_t, ComponentID> m_hashToId;
        std::deque<std::string> m_names;  // index == id; collision diagnostics
        ComponentID m_next = 0;
        std::shared_ptr<MetaRegistry> m_meta;  // shared_ptr: deleter captured where type is complete
    };

    namespace Detail
    {
        inline TypeContext*& CurrentTypeContextSlot() noexcept
        {
            static TypeContext* s_ctx = nullptr;  // per-module slot, by design
            return s_ctx;
        }
    }

    // The module-default context (created lazily for standalone use).
    inline TypeContext& DefaultTypeContext()
    {
        static TypeContext s_ctx;
        return s_ctx;
    }

    // Install the process-shared context for THIS module. Must run before
    // the module's first TypeID<T>::Value() / Registry use — per-type IDs
    // are cached in per-module statics and will not re-resolve afterwards.
    inline void SetTypeContext(TypeContext* ctx) noexcept
    {
        Detail::CurrentTypeContextSlot() = ctx;
    }

    ASTRA_NODISCARD inline TypeContext* GetTypeContext() noexcept
    {
        TypeContext* ctx = Detail::CurrentTypeContextSlot();
        return ctx ? ctx : &DefaultTypeContext();
    }
}
```

**MetaRegistry integration (in `MetaRegistry.hpp`):** make MetaRegistry's constructor public (keep it non-copyable), add `#include "../Core/TypeContext.hpp"` to MetaRegistry.hpp, and define the two pieces that close the forward declaration:
```cpp
// MetaRegistry.hpp, AFTER the MetaRegistry class definition:

// Lazy per-context meta registry (guarded by the context's mutex).
inline MetaRegistry& TypeContext::Meta()
{
    std::lock_guard lock(m_mutex);
    if (!m_meta)
        m_meta = std::make_shared<MetaRegistry>();
    return *m_meta;
}

// MetaRegistry::Instance() body (inside the class) becomes a delegation —
// the singleton API is preserved for all existing callers:
static MetaRegistry& Instance() { return GetTypeContext()->Meta(); }
```
Include order ends up: `TypeID.hpp → TypeContext.hpp` and `MetaRegistry.hpp → TypeContext.hpp` — no cycle, regardless of MetaRegistry's own TypeID usage.

**Static-init ordering (DLL-critical):** `StaticTypeRegistrar` (Macros.hpp) runs during module static initialization — in a plugin DLL that is DURING `LoadLibrary`, before the host can call `SetTypeContext`. Registrations must therefore NOT hit a context eagerly. Change `StaticTypeRegistrar` to push its builder lambda into a **module-local pending queue** instead of calling `MetaRegistry::Instance()` directly:
```cpp
// TypeContext.hpp (module-local, deliberately):
namespace Detail
{
    using PendingMetaRegistration = std::function<void(TypeContext&)>;
    inline std::vector<PendingMetaRegistration>& PendingMetaQueue()
    {
        static std::vector<PendingMetaRegistration> s_queue;
        return s_queue;
    }
    inline void DrainPendingMeta(TypeContext& ctx)
    {
        for (auto& reg : PendingMetaQueue()) reg(ctx);
        PendingMetaQueue().clear();
    }
}
```
`SetTypeContext(ctx)` calls `Detail::DrainPendingMeta(*ctx)` after installing; `GetTypeContext()`'s lazy default-creation path drains into the default context (standalone behavior unchanged — registrations just become lazy). `StaticTypeRegistrar`'s constructor becomes `PendingMetaQueue().emplace_back([fn](TypeContext& c){ /* existing registration body against c.Meta() */ });`. Add a test: queue a registration before `SetTypeContext`, install a fresh context, verify the type is registered THERE and not in the default. **Documented contract** (Task 11 DLL section): plugin code must not call `TypeID<T>::Value()` from its own static initializers — IDs cache per-module on first call and would bind to the wrong context.

In `TypeID.hpp`: delete `TypeIDGenerator`; rewrite `TypeIDStorage`:
```cpp
template<typename T>
class TypeIDStorage
{
public:
    ASTRA_NODISCARD static ComponentID Value() noexcept
    {
        // Cached per-module; resolves through the context installed at
        // module init. Identical hash => identical ID in every module
        // sharing one TypeContext.
        static const ComponentID s_id =
            GetTypeContext()->GetOrAssignComponentID(TypeHash<T>(), TypeNameInternal<T>());
        return s_id;
    }
};
```
Add `#include "TypeContext.hpp"` to TypeID.hpp — **check for cycles**: TypeContext.hpp must NOT include TypeID.hpp (it doesn't — it takes raw hash+name). Component.hpp must not include TypeContext (it doesn't).

- [ ] **Step 4: Build + run full suite.** All existing 455 tests must still pass — the single-process behavior (sequential dense IDs in first-use order) is unchanged by construction.

- [ ] **Step 5: Sync + commit**

```powershell
powershell -File D:\dev\starworks\Astra\scripts\sync_to_github.ps1
git -C D:\dev\github\Astra add -A; git -C D:\dev\github\Astra commit -m "feat(core): TypeContext — injectable cross-module type identity keyed by stable name hashes"
```

---

### Task 6: Re-entrant component registration (hot-reload support)

**Files:**
- Modify: `include/Astra/Component/ComponentRegistry.hpp:22-99`
- Test: `tests/Component/ComponentRegistryTest.cpp` (append)

- [ ] **Step 1: Write the failing test** (append to existing ComponentRegistryTest.cpp; reuse its existing test components/fixtures)

```cpp
TEST(ComponentRegistry, ReRegisterOverwritesDescriptor)
{
    struct ReRegProbe { int v; };
    Astra::ComponentRegistry registry;
    registry.RegisterComponent<ReRegProbe>();
    const auto* before = registry.GetComponentDescriptor(Astra::TypeID<ReRegProbe>::Value());
    ASSERT_NE(before, nullptr);
    const auto id = before->id;

    // Re-registration must keep the same id (hash-stable) and refresh the
    // descriptor (in a real reload the function pointers move to the new DLL).
    registry.ReRegisterComponent<ReRegProbe>();
    const auto* after = registry.GetComponentDescriptor(id);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->id, id);
    EXPECT_EQ(after->hash, before->hash);
    EXPECT_NE(after->defaultConstruct, nullptr);
}

TEST(ComponentRegistry, RegisterRemainsIdempotent)
{
    struct IdemProbe { int v; };
    Astra::ComponentRegistry registry;
    registry.RegisterComponent<IdemProbe>();
    const size_t count = registry.Size();
    registry.RegisterComponent<IdemProbe>();
    EXPECT_EQ(registry.Size(), count);
}
```

- [ ] **Step 2: Build to verify failure** — no member `ReRegisterComponent`.

- [ ] **Step 3: Implement** — refactor, don't duplicate: extract the descriptor-building body of `RegisterComponent<T>` (everything after the `Contains` early-out, ComponentRegistry.hpp:29-98) into a private `template<Component T> void RegisterComponentImpl(ComponentID id)`. Then:

```cpp
template<Component T>
void RegisterComponent()
{
    ComponentID id = TypeID<T>::Value();
    if (m_components.Contains(id))
        return;
    RegisterComponentImpl<T>(id);
}

// Hot-reload path: rebuilds the descriptor unconditionally so function
// pointers point into the currently loaded module. ID is stable because
// TypeID resolves through the shared TypeContext by hash.
template<Component T>
void ReRegisterComponent()
{
    RegisterComponentImpl<T>(TypeID<T>::Value());
}
```
Inside `RegisterComponentImpl`, the final stores (`m_components[id] = desc; m_hashToID[desc.hash] = id;`) already overwrite-on-assign via `FlatMap::operator[]` — verify that and the `m_componentNames` deque only grows (acceptable: tiny strings; note it in a comment).

- [ ] **Step 4: Build + run full suite.** All pass.

- [ ] **Step 5: Sync + commit**

```powershell
powershell -File D:\dev\starworks\Astra\scripts\sync_to_github.ps1
git -C D:\dev\github\Astra add -A; git -C D:\dev\github\Astra commit -m "feat(component): ReRegisterComponent for DLL hot-reload descriptor refresh"
```

---

### Task 7: Container differential fuzz — FlatMap & FlatSet

**Files:**
- Create: `tests/Container/FlatMapFuzzTest.cpp`
- Create: `tests/Container/FlatSetFuzzTest.cpp`

- [ ] **Step 1: Write the FlatMap differential test** (this is a real test expected to pass — but it targets the untested churn paths, so a genuine failure is a FINDING: stop, capture the seed, report)

```cpp
// tests/Container/FlatMapFuzzTest.cpp
#include <gtest/gtest.h>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <Astra/Container/FlatMap.hpp>

namespace
{
    // Differential test vs std::unordered_map. Small key space forces
    // collisions, tombstone churn, and rehash cycles.
    void RunDifferential(uint64_t seed, int ops, int keySpace)
    {
        std::mt19937_64 rng(seed);
        Astra::FlatMap<int, int> dut;
        std::unordered_map<int, int> ref;

        for (int op = 0; op < ops; ++op)
        {
            const int key = static_cast<int>(rng() % keySpace);
            switch (rng() % 100)
            {
                case 0:  // rare: Clear
                    dut.Clear(); ref.clear(); break;
                case 1: case 2:  // Reserve at awkward sizes
                    dut.Reserve(static_cast<size_t>(rng() % 512)); break;
                default:
                    if (rng() % 3 == 0)
                    {
                        dut.Erase(key); ref.erase(key);
                    }
                    else
                    {
                        const int val = static_cast<int>(rng());
                        dut[key] = val; ref[key] = val;
                    }
                    break;
            }

            ASSERT_EQ(dut.Size(), ref.size()) << "seed=" << seed << " op=" << op;
            if (op % 257 == 0)  // periodic full-state check
            {
                for (const auto& [k, v] : ref)
                {
                    auto it = dut.Find(k);
                    ASSERT_NE(it, dut.end()) << "missing key " << k << " seed=" << seed;
                    ASSERT_EQ(it->second, v) << "wrong value for " << k << " seed=" << seed;
                }
                size_t iterCount = 0;
                for (const auto& [k, v] : dut)
                {
                    auto rit = ref.find(k);
                    ASSERT_NE(rit, ref.end()) << "phantom key " << k << " seed=" << seed;
                    ASSERT_EQ(rit->second, v);
                    ++iterCount;
                }
                ASSERT_EQ(iterCount, ref.size());
            }
```
And the test cases:
```cpp
    TEST(FlatMapFuzz, ChurnSmallKeySpace)   { RunDifferential(0xA57A1, 60'000, 64); }
    TEST(FlatMapFuzz, ChurnMediumKeySpace)  { RunDifferential(0xA57A2, 60'000, 1'024); }
    TEST(FlatMapFuzz, ChurnLargeKeySpace)   { RunDifferential(0xA57A3, 60'000, 100'000); }
    TEST(FlatMapFuzz, ManySeeds)
    {
        for (uint64_t s = 1; s <= 16; ++s) RunDifferential(s * 7919, 8'000, 128);
    }

    // Adversarial hashing: collapse H1 so probing chains span groups, and
    // exercise the H2-derivation path with degenerate top bits.
    struct AwfulHash
    {
        size_t operator()(int k) const noexcept { return static_cast<size_t>(k) & 0xF; }
    };

    TEST(FlatMapFuzz, AdversarialHashStillCorrect)
    {
        Astra::FlatMap<int, int, AwfulHash> dut;
        std::unordered_map<int, int> ref;
        std::mt19937_64 rng(0xBADBAD);
        for (int op = 0; op < 20'000; ++op)
        {
            const int key = static_cast<int>(rng() % 512);
            if (rng() % 3 == 0) { dut.Erase(key); ref.erase(key); }
            else { dut[key] = key * 3; ref[key] = key * 3; }
            ASSERT_EQ(dut.Size(), ref.size()) << "op=" << op;
        }
        for (const auto& [k, v] : ref)
        {
            auto it = dut.Find(k);
            ASSERT_NE(it, dut.end()) << k;
            ASSERT_EQ(it->second, v);
        }
    }
```
**Before writing:** check `FlatMap`'s template parameter list (it may be `FlatMap<K, V, Hasher>` or use a fixed internal hasher — exploration showed heterogeneous lookup via template, so a Hasher param likely exists; if there is NO hasher parameter, drop `AdversarialHashStillCorrect` and instead add a churn case with keys crafted to collide under the real hasher's H1 for capacity 16: keys `k, k+16, k+32, ...`).

- [ ] **Step 2: Write the FlatSet differential test** — same harness shape against `std::unordered_set<int>`: ops Insert/Erase/Contains/Clear/Reserve, plus an Emplace-duplicate-rejection case:

```cpp
    TEST(FlatSetFuzz, EmplaceDuplicateRejected)
    {
        Astra::FlatSet<int> set;
        EXPECT_TRUE(set.Emplace(42).second);
        EXPECT_FALSE(set.Emplace(42).second);
        EXPECT_EQ(set.Size(), 1u);
    }
```
(Verify `Emplace`'s return type in FlatSet.hpp first — if it returns iterator-only or bool-only, adjust the assertions to match the real signature.)

- [ ] **Step 3: Build + run** `AstraTest.exe --gtest_filter=*Fuzz* --gtest_brief=1`.
Expected: PASS in a few seconds. **If any differential check fails: STOP — that is a real container bug. Capture seed + op count, minimize by bisecting `ops`, and report the finding before fixing.**

- [ ] **Step 4: Sync + commit**

```powershell
powershell -File D:\dev\starworks\Astra\scripts\sync_to_github.ps1
git -C D:\dev\github\Astra add -A; git -C D:\dev\github\Astra commit -m "test(container): differential fuzz for FlatMap/FlatSet (churn, rehash, adversarial hash)"
```

---

### Task 8: Container differential fuzz — SmallVector & Bitmap

**Files:**
- Create: `tests/Container/SmallVectorFuzzTest.cpp`
- Create: `tests/Container/BitmapFuzzTest.cpp`

- [ ] **Step 1: SmallVector vs std::vector** (targets the inline↔heap spill boundary: N=4 default, so sizes 3↔5 cross it constantly)

```cpp
// tests/Container/SmallVectorFuzzTest.cpp
#include <gtest/gtest.h>
#include <random>
#include <vector>
#include <Astra/Container/SmallVector.hpp>

namespace
{
    void RunDifferential(uint64_t seed, int ops)
    {
        std::mt19937_64 rng(seed);
        Astra::SmallVector<int, 4> dut;
        std::vector<int> ref;

        auto check = [&]
        {
            ASSERT_EQ(dut.size(), ref.size());
            for (size_t i = 0; i < ref.size(); ++i)
                ASSERT_EQ(dut[i], ref[i]) << "i=" << i << " seed=" << seed;
        };

        for (int op = 0; op < ops; ++op)
        {
            switch (rng() % 10)
            {
                case 0: case 1: case 2: case 3:
                {
                    const int v = static_cast<int>(rng());
                    dut.push_back(v); ref.push_back(v); break;
                }
                case 4:
                    if (!ref.empty()) { dut.pop_back(); ref.pop_back(); }
                    break;
                case 5:
                {
                    const size_t pos = ref.empty() ? 0 : rng() % ref.size();
                    const int v = static_cast<int>(rng());
                    dut.insert(dut.begin() + pos, v); ref.insert(ref.begin() + pos, v);
                    break;
                }
                case 6:
                    if (!ref.empty())
                    {
                        const size_t pos = rng() % ref.size();
                        dut.erase(dut.begin() + pos); ref.erase(ref.begin() + pos);
                    }
                    break;
                case 7:
                {
                    const size_t n = rng() % 12;  // hovers around the inline boundary
                    dut.resize(n); ref.resize(n); break;
                }
                case 8:  // copy round-trip
                {
                    Astra::SmallVector<int, 4> copy(dut);
                    dut = copy; break;
                }
                case 9:  // move round-trip (steals heap when large, per impl)
                {
                    Astra::SmallVector<int, 4> moved(std::move(dut));
                    dut = std::move(moved); break;
                }
            }
            check();
        }
        dut.shrink_to_fit();
        check();
    }

    TEST(SmallVectorFuzz, SpillBoundaryChurn) { RunDifferential(0x5111A11, 40'000); }
    TEST(SmallVectorFuzz, ManySeeds)
    {
        for (uint64_t s = 1; s <= 16; ++s) RunDifferential(s * 104'729, 5'000);
    }
}
```

- [ ] **Step 2: Bitmap vs std::bitset<128>** — random Set/Reset/Test plus the bulk ops:

```cpp
// tests/Container/BitmapFuzzTest.cpp
#include <gtest/gtest.h>
#include <bitset>
#include <random>
#include <Astra/Container/Bitmap.hpp>

namespace
{
    TEST(BitmapFuzz, DifferentialVsBitset)
    {
        std::mt19937_64 rng(0xB17B17);
        Astra::Bitmap<128> dut;
        std::bitset<128> ref;
        for (int op = 0; op < 50'000; ++op)
        {
            const size_t idx = rng() % 128;
            switch (rng() % 3)
            {
                case 0: dut.Set(idx);   ref.set(idx);   break;
                case 1: dut.Reset(idx); ref.reset(idx); break;
                case 2: ASSERT_EQ(dut.Test(idx), ref.test(idx)) << idx; break;
            }
            ASSERT_EQ(dut.Count(), ref.count());
            ASSERT_EQ(dut.Any(),   ref.any());
            ASSERT_EQ(dut.None(),  ref.none());
        }
    }

    TEST(BitmapFuzz, HasAllMatchesManualCheck)
    {
        std::mt19937_64 rng(0xCAFE);
        for (int trial = 0; trial < 2'000; ++trial)
        {
            Astra::Bitmap<128> a, mask;
            std::bitset<128> ra, rmask;
            for (int i = 0; i < 24; ++i)
            {
                size_t x = rng() % 128; a.Set(x);    ra.set(x);
                size_t y = rng() % 128; mask.Set(y); rmask.set(y);
            }
            const bool expected = (ra & rmask) == rmask;
            ASSERT_EQ(a.HasAll(mask), expected) << "trial=" << trial;
        }
    }

    TEST(BitmapFuzz, OutOfBoundsIsNoop)
    {
        Astra::Bitmap<128> b;
        b.Set(128);   // documented gate: index < Bits
        b.Set(9999);
        EXPECT_TRUE(b.None());
        EXPECT_FALSE(b.Test(128));
    }
}
```

- [ ] **Step 3: Build + run full suite.** All pass (same finding-protocol as Task 7 if not).

- [ ] **Step 4: Sync + commit**

```powershell
powershell -File D:\dev\starworks\Astra\scripts\sync_to_github.ps1
git -C D:\dev\github\Astra add -A; git -C D:\dev\github\Astra commit -m "test(container): differential fuzz for SmallVector spill boundary + Bitmap bulk ops"
```

---

### Task 9: Cross-compiler portability fixes (local pass)

**Files:**
- Modify: `premake5.lua`
- Modify: whatever `include/Astra/**.hpp` g++/clang reject (discovery task — categories below)

- [ ] **Step 1: premake portability fixes**

In `premake5.lua`: (a) benchmark-only `-march=native` stays (benchmarks aren't built in CI cross jobs, see Task 10) but ADD a comment that it's non-portable by intent; (b) confirm `cppdialect "C++20"` is set on all projects (it is) — premake emits `-std=c++20` for gmake2, nothing to add; (c) Linux test filter adds `-mavx` to AstraTest (test code includes Simd.hpp which compiles AVX paths under `__AVX__`; MSVC side already uses `/arch:AVX`) — keep parity.

- [ ] **Step 2: Local g++ smoke if available, else lean on CI**

Check: `wsl --status` / `wsl which g++`. If WSL with g++ ≥ 11 exists:
```bash
cd /mnt/d/dev/starworks/Astra
# premake5 linux binary: download once to scripts/premake5 if not present
./scripts/premake5 gmake2 && make -C . config=release -j$(nproc) AstraTest
bin/Release-linux-x86_64/AstraTest/AstraTest --gtest_brief=1
```
(Verify the generated config name with `make help` first — premake gmake2 typically emits `config=release`.) If no WSL/g++: skip — Task 10's CI is the verifier; expect to iterate on CI logs.

- [ ] **Step 3: Fix what the compiler surfaces.** Expected categories (fix ONLY what errors/warnings demand, no drive-by refactors): missing `typename`/`template` disambiguators in dependent contexts; missing `#include <cstring>`/`<cstdint>` that MSVC headers transitively provided; `ASTRA_FORCEINLINE`/attribute placement differences; signed/unsigned `-Wall -Wextra -Wpedantic` warnings (the premake config enables them — fix warnings in Astra headers, ignore vendor warnings); the `Entity.hpp:78` `requires` clause `!std::same_as<T, bool>` needs parentheses on some compilers (`requires std::convertible_to<StorageType, T> && (!std::same_as<T, bool>)`).

- [ ] **Step 4: Re-run MSVC full suite** (regression guard) — all pass.

- [ ] **Step 5: Sync + commit**

```powershell
powershell -File D:\dev\starworks\Astra\scripts\sync_to_github.ps1
git -C D:\dev\github\Astra add -A; git -C D:\dev\github\Astra commit -m "fix(portability): gcc/clang build fixes"
```

---

### Task 10: GitHub Actions CI matrix

**Files:**
- Create: `.github/workflows/ci.yml` (in the working copy; it syncs over)

- [ ] **Step 1: Write the workflow**

```yaml
name: CI
on:
  push:
    branches: ["**"]
  pull_request:

env:
  PREMAKE_VERSION: 5.0.0-beta2

jobs:
  windows-msvc:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - name: Download premake
        shell: pwsh
        run: |
          Invoke-WebRequest "https://github.com/premake/premake-core/releases/download/v${env:PREMAKE_VERSION}/premake-${env:PREMAKE_VERSION}-windows.zip" -OutFile premake.zip
          Expand-Archive premake.zip -DestinationPath .
      - name: Generate
        run: .\premake5.exe vs2022
      - name: Setup MSBuild
        uses: microsoft/setup-msbuild@v2
      - name: Build (Release)
        run: msbuild Astra.sln /p:Configuration=Release /m /v:minimal
      - name: Test
        run: .\bin\Release-windows-x86_64\AstraTest\AstraTest.exe --gtest_brief=1

  linux:
    strategy:
      fail-fast: false
      matrix:
        compiler: [g++, clang++]
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install toolchain
        run: |
          sudo apt-get update
          sudo apt-get install -y ${{ matrix.compiler == 'clang++' && 'clang libomp-dev' || 'g++' }}
      - name: Download premake
        run: |
          curl -L "https://github.com/premake/premake-core/releases/download/v${PREMAKE_VERSION}/premake-${PREMAKE_VERSION}-linux.tar.gz" | tar xz
      - name: Generate
        run: ./premake5 gmake2
      - name: Build (Release, tests only)
        run: |
          make config=release AstraTest -j"$(nproc)" \
            CC=${{ matrix.compiler == 'clang++' && 'clang' || 'gcc' }} \
            CXX=${{ matrix.compiler }}
      - name: Test
        run: ./bin/Release-linux-x86_64/AstraTest/AstraTest --gtest_brief=1
```
Notes for the executor: (a) the `vs2022` action is correct for CI (GitHub runners have VS2022; locally we use vs2026 — the .sln/.slnx are gitignored either way per the sync script's /XF, so CI always generates fresh); (b) if `make config=release` names differ, run `make help` in a debug step and fix; (c) building only the `AstraTest` target skips GoogleBenchmark's `-march=native` on CI — if premake's generated Makefile requires building deps explicitly, use `make config=release GoogleTest AstraTest`.

- [ ] **Step 2: Sync, commit, push, watch**

```powershell
powershell -File D:\dev\starworks\Astra\scripts\sync_to_github.ps1
git -C D:\dev\github\Astra add -A
git -C D:\dev\github\Astra commit -m "ci: MSVC + gcc + clang matrix (premake vs2022/gmake2, full test suite)"
git -C D:\dev\github\Astra push -u origin hardening/v3.1
gh run watch --repo T3mps/Astra; gh run list --repo T3mps/Astra --branch hardening/v3.1 --limit 3
```
Expected: all three jobs green. If linux jobs fail: pull the log (`gh run view --log-failed`), apply Task 9 Step 3 categories in the WORKING COPY, re-sync, re-push. Iterate until green.

---

### Task 11: Documentation reconciliation + threading model

**Files:**
- Modify: `README.md`, `CLAUDE.md`

Apply the verified drift list (every item below was confirmed against code during exploration):

- [ ] **Step 1: README fixes**
  1. **Config example (README ~lines 282-299):** replace `config.threadSafe = true`, `config.initialArchetypeCapacity = 256`, and `config.initialChunkCount` with the real shape:
     ```cpp
     Astra::Registry::Config config;
     config.chunkPoolConfig.chunkSize = 16384;     // 16KB chunks (default)
     config.chunkPoolConfig.useHugePages = true;   // 2MB pages if available
     config.chunkPoolConfig.initialBlocks = 4;     // verify field name/default in ArchetypeChunkPool::Config and use the real ones
     config.workScheduler = myScheduler;           // optional: inject shared IWorkScheduler (new)
     Astra::Registry registry(config);
     ```
  2. **"Components must be trivially copyable" (lines ~19, ~149):** replace with the truth: *"Components must be nothrow-move-constructible and nothrow-destructible (see the `Component` concept). Trivially-copyable components get memcpy fast paths automatically; non-trivial components are fully supported via type-erased descriptors."*
  3. **`CreateEntity` with values (lines ~33-36, ~174-176):** change examples to `CreateEntityWith(Position{...}, Velocity{...})`; keep `CreateEntity<Position, Velocity>()` for default-construction.
  4. **Range-for example (lines ~213-217):** open `include/Astra/Registry/ViewIterator.hpp`, check what `operator*` yields (pointers vs references) and make the example match reality (`pos->x` vs `pos.x`).
  5. **Batch-create example (lines ~263-270):** the generator overload is `CreateEntitiesWith(count, outSpan, generator)` — rename in the example.
  6. **Add a "Threading model" section** (replaces the deleted threadSafe lie):
     > Astra creates no threads. Registries are single-threaded by design: structural changes (create/destroy/add/remove) must not race. The job system is an open seam: inject an `IWorkScheduler` (e.g. an enkiTS adapter) via `Registry::Config::workScheduler` and `ParallelForEach` / `ParallelForEachDescendant` / `ParallelExecutor` will use it — with no scheduler injected they run sequentially inline. Structural changes from worker threads are deferred via `CommandBuffer` (thread-safe). `RelationshipGraph` traversal caches and `MetaRegistry` are internally synchronized so concurrent *reads* through an injected scheduler stay safe.
  7. **Add a "Multi-module (DLL) usage" section** documenting `TypeContext`: host calls `CreateTypeContext`-equivalent (`auto ctx = std::make_unique<Astra::TypeContext>()`), passes `ctx.get()` to each plugin which calls `Astra::SetTypeContext(ctx)` before any ECS use; hot reload sequence: serialize world → unload → load → `SetTypeContext` → `ReRegisterComponent` per type → deserialize.

- [ ] **Step 2: CLAUDE.md fixes**
  1. `Platform/Simd.hpp` → `Core/Simd.hpp` (line ~111).
  2. Entity config wording (line ~69): "Entities default to 32 bits total: 24-bit ID + 8-bit version."
  3. Remove "premake5 is not available in Claude Code environment" (it is; document the bundled-premake path used locally and `scripts/` generators).
  4. Document the new pieces: `Core/WorkScheduler.hpp` (the IWorkScheduler seam — Astra ships no threads; `tests/Support/TestWorkerPool.hpp` is the test-only reference), `Core/TypeContext.hpp`, `ReRegisterComponent`, the fuzz-test convention (`tests/Container/*FuzzTest.cpp`), and CI (`.github/workflows/ci.yml`).

- [ ] **Step 3: Verify every claim you wrote** — for each API name/field mentioned in the edited docs, `rg` it in `include/` and confirm exact spelling.

- [ ] **Step 4: Sync + commit**

```powershell
powershell -File D:\dev\starworks\Astra\scripts\sync_to_github.ps1
git -C D:\dev\github\Astra add -A; git -C D:\dev\github\Astra commit -m "docs: reconcile README/CLAUDE.md with code; threading model + DLL usage sections"
```

---

### Task 12: Version bump + release notes + final verification

**Files:**
- Modify: `include/Astra/Core/Version.hpp` (read it first; bump minor → 3.1.0 pattern matching whatever macros exist)
- Create: `release_notes/v3.1.0.md`

- [ ] **Step 1: Bump Version.hpp** to 3.1.0 following its existing macro shape.

- [ ] **Step 2: Write release notes** following the v3.0.0.md heading style:

```markdown
# Astra v3.1.0 — Hardening Release

## Parallel Execution
- Astra now creates **zero threads**. New `IWorkScheduler` seam: inject your job system (e.g. an enkiTS adapter) via `Registry::Config::workScheduler`; without one, all Parallel* APIs run sequentially inline.
- `View::ParallelForEach`, `Relations::ParallelForEachDescendant`, and `ParallelExecutor` route through the seam — the ad-hoc `std::async` thread spawning (which lost to sequential in wall-time) is removed.
- Reference fork-join pool lives in test support (`tests/Support/TestWorkerPool.hpp`); benchmark wall-time at 1M entities through it: <before> → <after> (fill from Task 4 measurements).

## Multi-Module / DLL Support
- New `TypeContext`: process-shared type identity keyed by stable XXHash64 type-name hashes; dense sequential ComponentIDs preserved (mask-compatible). `SetTypeContext()` per module.
- `MetaRegistry` now lives in the context (singleton API preserved).
- `ComponentRegistry::ReRegisterComponent<T>()` refreshes descriptors after DLL hot reload. Blessed reload sequence: serialize → swap DLL → re-register → deserialize.

## Testing & CI
- Differential fuzz suites for FlatMap/FlatSet (churn, rehash, adversarial hashing), SmallVector (spill boundary), Bitmap (bulk ops).
- GitHub Actions matrix: MSVC, gcc, clang — full test suite on every push.

## Documentation
- README/CLAUDE.md reconciled with code (removed nonexistent `threadSafe` config, corrected component requirements, CreateEntityWith examples, threading-model and DLL sections added).
```

- [ ] **Step 3: Full local verification**

Run: regenerate, full Release build, full `AstraTest.exe --gtest_brief=1` (expect ~480+ tests, 0 failures), benchmark filter run from Task 4 for the final numbers, `rg "std::async" include/` (no matches).

- [ ] **Step 4: Final sync + commit + push; confirm CI green**

```powershell
powershell -File D:\dev\starworks\Astra\scripts\sync_to_github.ps1
git -C D:\dev\github\Astra add -A
git -C D:\dev\github\Astra commit -m "release: v3.1.0 hardening (scheduler, TypeContext, fuzz, CI, docs)"
git -C D:\dev\github\Astra push
gh run watch --repo T3mps/Astra
```

- [ ] **Step 5: Re-vendor into the game repo** — after the user merges `hardening/v3.1`, refresh `D:\dev\starworks\Gacha\ThirdParty\Astra` from the working copy (same exclusions as the sync script: no `bin`/`bin-int`/`ide`/`.vs`), rerun the Gacha editor/server builds that include Astra headers (currently none consume it — verify with `rg -l "Astra/" D:\dev\starworks\Gacha\Server D:\dev\starworks\Gacha\Tools` and skip if empty), and note completion in the engine spec's Astra section.

---

## Deferred (explicitly NOT in this plan)

- enkiTS-backed `IWorkScheduler` implementation — lives in the ENGINE repo (enkiTS is not and will never be an Astra dependency; Astra ships the seam ONLY — no threads, no pool, in the library).
- libFuzzer/sanitizer harnesses — YAGNI for now; the differential suites run on all three compilers in CI. Revisit if a fuzz finding suggests deeper state-space issues.
- True two-DLL integration test for TypeContext — requires a multi-module test rig; lands with the engine's plugin-ABI milestone (the context API is unit-tested here).
- Separating system TypeIDs from the component ID space — preserved single-counter semantics deliberately (pre-existing behavior; masks only ever index component IDs that components actually use). Revisit if MAX_COMPONENTS pressure appears.
```
