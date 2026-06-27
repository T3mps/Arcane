# Arcane JobSystem API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the task-parallelism substrate (`Arcane::FunctionRef`, `Arcane::ITaskExecutor` + serial/enkiTS impls, Runtime + plugin-ABI wiring) that physics Phase D will consume, and migrate the 10 Category-A `std::function` visitor params to `FunctionRef`.

**Architecture:** A Core-owned, presentation-free, Astra-free seam. `ITaskExecutor::ParallelFor(count, minBatch, fn(begin,end,worker))` is the one primitive; `SerialTaskExecutor` (Core) is the deterministic default; an `EnkiTaskExecutor` face is added to the existing `Arcane.dll` `JobSystem` over the *same* `enki::TaskScheduler` (so Astra's `IWorkScheduler` and the new `ITaskExecutor` are two adapters on one pool). `FunctionRef<Sig>` is a non-owning, zero-alloc callable view for the synchronous, non-escaping callbacks. **No physics Step code is parallelized here** (that is Phase D); the solve path stays byte-identical.

**Tech Stack:** C++23, MSVC (VS 2026), enkiTS, Catch2 (+ rapidcheck), premake5, NVRHI/SDL3 (only for the headless smoke gate).

**Spec:** `docs/superpowers/specs/2026-06-26-arcane-jobsystem-api-design.md`

> **Path note (correction to spec §4):** Core uses `Util/`, not `Base/`. Final paths: `FunctionRef` → `Arcane/Core/src/Arcane/Util/FunctionRef.hpp` (`#include <Arcane/Util/FunctionRef.hpp>`); `ITaskExecutor`/`SerialTaskExecutor` → `Arcane/Core/src/Arcane/Jobs/TaskExecutor.hpp` (`#include <Arcane/Jobs/TaskExecutor.hpp>`, a new `Jobs/` dir in Core whose include-root merges with `Arcane.dll`'s `Arcane/Jobs/`).

---

## Conventions (referenced by every task)

All commands are PowerShell, run from the repo root `D:\dev\starworks\Gacha` unless noted.

- **REGEN** (after adding any new `.cpp`/`.hpp` file — the projects glob `src/**`, and a new `.cpp` must be added to the vcxproj):
  ```powershell
  Push-Location Arcane; & "..\ThirdParty\premake5\premake5.exe" vs2026; Pop-Location
  ```
- **BUILD-DEBUG:**
  ```powershell
  msbuild Arcane\Arcane.slnx -p:Configuration=Debug -m
  ```
  (Use the VS 2026 `msbuild`; full path if not on PATH. `GenerateProjects.bat` is NOT used — it hangs on a `pause`.)
- **TEST** (Catch2 exe must run from its own directory — it loads `Arcane.dll` + plugins from there):
  ```powershell
  Push-Location Arcane\bin\Debug-windows-x86_64-md\ArcaneTests; .\ArcaneTests.exe "<tagexpr>"; Pop-Location
  ```
- Catch2 idiom: `TEST_CASE("name", "[tag]") { REQUIRE(expr); }`. Tags used: `[functionref]`, `[jobs]`.
- Commit messages use conventional-commit style and end with the trailer:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`

---

## Task 1: `Arcane::FunctionRef<Sig>` — non-owning callable view

**Files:**
- Create: `Arcane/Core/src/Arcane/Util/FunctionRef.hpp`
- Create: `Arcane/Tests/src/FunctionRefTest.cpp`

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/FunctionRefTest.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Util/FunctionRef.hpp>

#include <string>

using Arcane::FunctionRef;

namespace
{
    int FreeAdd(int a, int b) { return a + b; }

    struct Functor
    {
        int factor;
        int operator()(int x) const { return x * factor; }
    };
}

TEST_CASE("FunctionRef binds a capturing lambda", "[functionref]")
{
    int captured = 10;
    auto lam = [&](int x) { return x + captured; };
    FunctionRef<int(int)> ref = lam;
    REQUIRE(static_cast<bool>(ref));
    REQUIRE(ref(5) == 15);
}

TEST_CASE("FunctionRef binds a free function", "[functionref]")
{
    FunctionRef<int(int, int)> ref = FreeAdd;
    REQUIRE(ref(2, 3) == 5);
}

TEST_CASE("FunctionRef binds a const functor", "[functionref]")
{
    const Functor f{3};
    FunctionRef<int(int)> ref = f;
    REQUIRE(ref(4) == 12);
}

TEST_CASE("FunctionRef binds a const-qualified call (const visitor case)", "[functionref]")
{
    int sum = 0;
    const auto visit = [&](int x) { sum += x; };   // const lambda
    FunctionRef<void(int)> ref = visit;
    ref(1); ref(2); ref(7);
    REQUIRE(sum == 10);
}

TEST_CASE("FunctionRef default-constructs empty", "[functionref]")
{
    FunctionRef<void()> ref;
    REQUIRE_FALSE(static_cast<bool>(ref));
}

TEST_CASE("FunctionRef supports void return + multiple args", "[functionref]")
{
    std::string out;
    auto append = [&](const char* s, int n) { for (int i = 0; i < n; ++i) out += s; };
    FunctionRef<void(const char*, int)> ref = append;
    ref("ab", 2);
    REQUIRE(out == "abab");
}
```

- [ ] **Step 2: Regen + build to verify it fails**

Run REGEN, then BUILD-DEBUG.
Expected: FAIL — `cannot open include file 'Arcane/Util/FunctionRef.hpp'`.

- [ ] **Step 3: Implement `FunctionRef.hpp`**

Create `Arcane/Core/src/Arcane/Util/FunctionRef.hpp`:
```cpp
#pragma once

// FunctionRef<Sig>: a non-owning, zero-allocation callable VIEW (void* + thunk).
// For SYNCHRONOUS, non-escaping callbacks only -- the referent MUST outlive the
// FunctionRef. Do NOT store one (it would dangle); a stored callback uses an
// owning std::function (or a future Delegate). C++26 std::function_ref-aligned:
// this can be replaced by a using-alias when MSVC ships it.

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace Arcane
{
    template <class Sig> class FunctionRef;

    template <class R, class... Args>
    class FunctionRef<R(Args...)>
    {
        void* m_obj = nullptr;
        R (*m_thunk)(void*, Args...) = nullptr;

    public:
        FunctionRef() = default;

        // Implicit by design: call sites pass a lambda directly.
        template <class F>
            requires (!std::is_same_v<std::remove_cvref_t<F>, FunctionRef>
                      && std::is_invocable_r_v<R, F&, Args...>)
        FunctionRef(F&& f) noexcept
            : m_obj(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
              m_thunk(+[](void* o, Args... a) -> R {
                  return (*static_cast<std::remove_reference_t<F>*>(o))(static_cast<Args&&>(a)...);
              })
        {
        }

        R operator()(Args... a) const { return m_thunk(m_obj, static_cast<Args&&>(a)...); }

        explicit operator bool() const noexcept { return m_thunk != nullptr; }
    };
}
```

- [ ] **Step 4: Build + run the test to verify it passes**

Run BUILD-DEBUG, then TEST with `"[functionref]"`.
Expected: PASS (6 test cases).

- [ ] **Step 5: Commit** (only the tracked source files — the REGEN'd `.slnx`/`.vcxproj` are gitignored)
```powershell
git add Arcane/Core/src/Arcane/Util/FunctionRef.hpp Arcane/Tests/src/FunctionRefTest.cpp
git commit -m @'
feat(arcane/core): Arcane::FunctionRef<Sig> -- non-owning zero-alloc callable view

Core/Util header-only function_ref for synchronous non-escaping callbacks
(the seam for ITaskExecutor + the Category-A visitor migration). C++26
std::function_ref-aligned.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 2: `ITaskExecutor` + `SerialTaskExecutor` (Core seam + reference impl)

**Files:**
- Create: `Arcane/Core/src/Arcane/Jobs/TaskExecutor.hpp`
- Create: `Arcane/Tests/src/TaskExecutorTest.cpp`

- [ ] **Step 1: Write the failing test (SerialTaskExecutor contract)**

Create `Arcane/Tests/src/TaskExecutorTest.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>

#include <cstdint>
#include <vector>

using Arcane::ITaskExecutor;
using Arcane::SerialTaskExecutor;

// Fill out[i] = i*i over [0,count); verify disjoint full cover + worker range.
static void RunCoverWorkload(ITaskExecutor& exec, std::size_t count, std::size_t minBatch,
                             std::vector<int>& visited, std::vector<std::uint64_t>& out,
                             std::uint32_t& maxWorker)
{
    visited.assign(count, 0);
    out.assign(count, 0);
    maxWorker = 0;
    exec.ParallelFor(count, minBatch,
        [&](std::size_t b, std::size_t e, std::uint32_t worker)
        {
            if (worker > maxWorker) maxWorker = worker;   // serial: single worker
            for (std::size_t i = b; i < e; ++i)
            {
                visited[i] += 1;                           // disjoint => exactly 1
                out[i] = static_cast<std::uint64_t>(i) * i;
            }
        });
}

TEST_CASE("SerialTaskExecutor: disjoint full cover + per-element result", "[jobs]")
{
    SerialTaskExecutor exec;
    REQUIRE(exec.WorkerCount() == 1);

    std::vector<int> visited;
    std::vector<std::uint64_t> out;
    std::uint32_t maxWorker = 999;
    RunCoverWorkload(exec, 1000, 64, visited, out, maxWorker);

    REQUIRE(maxWorker < exec.WorkerCount());               // worker in [0,WorkerCount())
    for (std::size_t i = 0; i < 1000; ++i)
    {
        REQUIRE(visited[i] == 1);                          // covered exactly once
        REQUIRE(out[i] == static_cast<std::uint64_t>(i) * i);
    }
}

TEST_CASE("SerialTaskExecutor: edge counts", "[jobs]")
{
    SerialTaskExecutor exec;
    std::vector<int> visited; std::vector<std::uint64_t> out; std::uint32_t mw;

    RunCoverWorkload(exec, 0, 64, visited, out, mw);        // no-op
    REQUIRE(visited.empty());

    RunCoverWorkload(exec, 1, 64, visited, out, mw);        // count < minBatch
    REQUIRE(visited.size() == 1);
    REQUIRE(visited[0] == 1);

    RunCoverWorkload(exec, 7, 0, visited, out, mw);         // minBatch 0 (=>1)
    for (int v : visited) REQUIRE(v == 1);
}
```

- [ ] **Step 2: Regen + build to verify it fails**

Run REGEN, then BUILD-DEBUG.
Expected: FAIL — `cannot open include file 'Arcane/Jobs/TaskExecutor.hpp'`.

- [ ] **Step 3: Implement `TaskExecutor.hpp`**

Create `Arcane/Core/src/Arcane/Jobs/TaskExecutor.hpp`:
```cpp
#pragma once

// ITaskExecutor: the engine's synchronous data-parallel seam (presentation-free,
// Astra-free, no global state -> safe in Core). ParallelFor is the ONLY primitive
// this milestone ships. The enkiTS-backed impl lives in Arcane.dll (JobSystem);
// SerialTaskExecutor below is the deterministic reference + the default when no
// executor is injected. The colored physics solver (Phase D) is the consumer.

#include <Arcane/Util/FunctionRef.hpp>

#include <cstddef>
#include <cstdint>

namespace Arcane
{
    struct ITaskExecutor
    {
        // Partition [0,count) into DISJOINT sub-ranges (grain >= minBatch where
        // possible) covering it exactly once; invoke fn(begin,end,worker) on each.
        // worker in [0,WorkerCount()) names the running thread (per-worker scratch).
        // BLOCKS until all sub-ranges complete. count==0 is a no-op. Re-entrant:
        // legal to call from within an fn already running on this executor.
        virtual void ParallelFor(std::size_t count, std::size_t minBatch,
                                 FunctionRef<void(std::size_t begin, std::size_t end,
                                                  std::uint32_t worker)> fn) = 0;

        // Inclusive of the calling thread; always >= 1. Batch-size denominator.
        virtual std::uint32_t WorkerCount() const noexcept = 0;

        virtual ~ITaskExecutor() = default;
    };

    // Single-threaded reference: runs the whole range inline as worker 0.
    class SerialTaskExecutor final : public ITaskExecutor
    {
    public:
        void ParallelFor(std::size_t count, std::size_t /*minBatch*/,
                         FunctionRef<void(std::size_t, std::size_t, std::uint32_t)> fn) override
        {
            if (count == 0) return;
            fn(0, count, 0);
        }

        std::uint32_t WorkerCount() const noexcept override { return 1; }
    };
}
```

- [ ] **Step 4: Build + run the test to verify it passes**

Run BUILD-DEBUG, then TEST with `"[jobs]"`.
Expected: PASS (the 2 SerialTaskExecutor cases).

- [ ] **Step 5: Commit** (source files only; REGEN'd project files are gitignored)
```powershell
git add Arcane/Core/src/Arcane/Jobs/TaskExecutor.hpp Arcane/Tests/src/TaskExecutorTest.cpp
git commit -m @'
feat(arcane/core): ITaskExecutor seam + SerialTaskExecutor (Core)

The synchronous ParallelFor(count,minBatch,fn(begin,end,worker)) primitive +
the deterministic single-thread reference/default. enkiTS face follows.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 3: enkiTS `ITaskExecutor` face on `JobSystem` (worker index restored)

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Jobs/JobSystem.hpp`
- Modify: `Arcane/Arcane/src/Arcane/Jobs/JobSystem.cpp`
- Modify: `Arcane/Tests/src/TaskExecutorTest.cpp`

- [ ] **Step 1: Write the failing tests (enkiTS face: serial≡parallel, thread-count invariance, nested)**

Append to `Arcane/Tests/src/TaskExecutorTest.cpp`:
```cpp
#include <Arcane/Jobs/JobSystem.hpp>

using Arcane::JobSystem;

TEST_CASE("JobSystem exposes an ITaskExecutor over the enki pool", "[jobs]")
{
    JobSystem jobs;                          // hardware default threads
    ITaskExecutor* exec = jobs.TaskExecutor();
    REQUIRE(exec != nullptr);
    REQUIRE(exec->WorkerCount() >= 1);
}

TEST_CASE("enki executor: disjoint full cover, worker index in range", "[jobs]")
{
    JobSystem jobs;
    ITaskExecutor* exec = jobs.TaskExecutor();

    const std::size_t count = 10000;
    std::vector<int> visited(count, 0);                  // disjoint ranges => no race per index
    std::vector<std::uint32_t> seenWorker(count, 0xFFFFFFFFu);
    exec->ParallelFor(count, 256,
        [&](std::size_t b, std::size_t e, std::uint32_t worker)
        {
            for (std::size_t i = b; i < e; ++i) { visited[i] += 1; seenWorker[i] = worker; }
        });

    for (std::size_t i = 0; i < count; ++i)
    {
        REQUIRE(visited[i] == 1);
        REQUIRE(seenWorker[i] < exec->WorkerCount());
    }
}

// Per-element independent fill -> output is invariant to thread count + to serial vs parallel.
static std::vector<std::uint64_t> FillSquares(ITaskExecutor& exec, std::size_t count)
{
    std::vector<std::uint64_t> out(count, 0);
    exec.ParallelFor(count, 128, [&](std::size_t b, std::size_t e, std::uint32_t)
    {
        for (std::size_t i = b; i < e; ++i) out[i] = static_cast<std::uint64_t>(i) * i + 7u;
    });
    return out;
}

TEST_CASE("thread-count invariance: serial == enki(1) == enki(N), byte-identical", "[jobs]")
{
    const std::size_t count = 50000;
    SerialTaskExecutor serial;
    JobSystem oneThread(1);
    JobSystem manyThreads(0);                            // hardware default

    auto s = FillSquares(serial,            count);
    auto a = FillSquares(*oneThread.TaskExecutor(),   count);
    auto b = FillSquares(*manyThreads.TaskExecutor(),  count);

    REQUIRE(s == a);
    REQUIRE(a == b);                                     // independent work => identical regardless of N
}

TEST_CASE("nested ParallelFor from within a worker completes correctly", "[jobs]")
{
    JobSystem jobs;
    ITaskExecutor* exec = jobs.TaskExecutor();

    const std::size_t outer = 64, inner = 64;
    std::vector<int> grid(outer * inner, 0);
    exec->ParallelFor(outer, 1, [&](std::size_t ob, std::size_t oe, std::uint32_t)
    {
        for (std::size_t o = ob; o < oe; ++o)
            exec->ParallelFor(inner, 1, [&](std::size_t ib, std::size_t ie, std::uint32_t)
            {
                for (std::size_t i = ib; i < ie; ++i) grid[o * inner + i] += 1;
            });
    });
    for (int v : grid) REQUIRE(v == 1);
}
```

- [ ] **Step 2: Build to verify it fails**

Run BUILD-DEBUG.
Expected: FAIL — `'TaskExecutor': is not a member of 'Arcane::JobSystem'`.

- [ ] **Step 3: Add the declaration to `JobSystem.hpp`**

In `Arcane/Arcane/src/Arcane/Jobs/JobSystem.hpp`, add a forward declaration near the top of `namespace Arcane` (before the class) and the accessor inside the class after `WorkScheduler()`:

Add forward decl (just inside `namespace Arcane {`):
```cpp
    struct ITaskExecutor;   // <Arcane/Jobs/TaskExecutor.hpp>; full def used in the .cpp
```
Add accessor (after the `WorkScheduler()` declaration, ~line 34):
```cpp
        // The SAME enki pool as WorkScheduler(), exposed through the engine's
        // ITaskExecutor seam (worker-index-aware ParallelFor; FunctionRef callback).
        // Borrowed pointer; lifetime == this JobSystem. Consume from PhysicsWorld etc.
        ITaskExecutor* TaskExecutor() const noexcept;
```

- [ ] **Step 4: Implement the enki face in `JobSystem.cpp`**

In `Arcane/Arcane/src/Arcane/Jobs/JobSystem.cpp`:

Add the include near the existing includes:
```cpp
#include <Arcane/Jobs/TaskExecutor.hpp>
```

Add an `EnkiTaskExecutor` inside the anonymous namespace (next to `EnkiWorkScheduler`):
```cpp
        // Adapts enkiTS to Arcane::ITaskExecutor -- the worker-index-aware face.
        // Internal to this TU: consumers only see ITaskExecutor via JobSystem::TaskExecutor().
        class EnkiTaskExecutor final : public ITaskExecutor
        {
        public:
            explicit EnkiTaskExecutor(enki::TaskScheduler& ts) : m_ts(ts) {}

            void ParallelFor(std::size_t count, std::size_t minBatch,
                             FunctionRef<void(std::size_t, std::size_t, std::uint32_t)> fn) override
            {
                if (count == 0)
                    return;
                assert(count <= static_cast<std::size_t>((std::numeric_limits<uint32_t>::max)()));

                // fn outlives the task: WaitforTask blocks until all partitions complete.
                // The enki `threadnum` is the worker index (the value the old
                // IWorkScheduler adapter discarded).
                enki::TaskSet task(
                    static_cast<uint32_t>(count),
                    [&fn](enki::TaskSetPartition range, uint32_t threadnum)
                    {
                        fn(range.start, range.end, threadnum);
                    });
                task.m_MinRange = static_cast<uint32_t>(minBatch == 0 ? 1 : minBatch);

                m_ts.AddTaskSetToPipe(&task);
                m_ts.WaitforTask(&task);   // calling thread participates -> nested-safe
            }

            std::uint32_t WorkerCount() const noexcept override
            {
                return static_cast<std::uint32_t>(m_ts.GetNumTaskThreads());
            }

        private:
            enki::TaskScheduler& m_ts;
        };
```

Add the member to `JobSystem::Impl` (after `adapter`):
```cpp
        std::unique_ptr<EnkiTaskExecutor> taskExec;
```

Construct it in the `JobSystem` ctor (after `adapter` is built):
```cpp
        m_impl->taskExec = std::make_unique<EnkiTaskExecutor>(m_impl->ts);
```

Reset it in `~JobSystem()` BEFORE the scheduler shuts down (next to `adapter.reset()`):
```cpp
        m_impl->taskExec.reset();
```

Add the accessor definition (next to `WorkScheduler()`):
```cpp
    ITaskExecutor* JobSystem::TaskExecutor() const noexcept
    {
        return m_impl->taskExec.get();
    }
```

(`<limits>` is already included; `<cassert>` is already included.)

- [ ] **Step 5: Build + run the tests to verify they pass**

Run BUILD-DEBUG, then TEST with `"[jobs]"`.
Expected: PASS (all SerialTaskExecutor + enki cases, incl. thread-count invariance + nested).

- [ ] **Step 6: Commit**
```powershell
git add Arcane/Arcane/src/Arcane/Jobs/JobSystem.hpp Arcane/Arcane/src/Arcane/Jobs/JobSystem.cpp Arcane/Tests/src/TaskExecutorTest.cpp
git commit -m @'
feat(arcane/jobs): EnkiTaskExecutor -- ITaskExecutor face over the one enki pool

JobSystem::TaskExecutor() adds the worker-index-aware ParallelFor face beside
WorkScheduler() (Astra). Restores the enki threadnum the old adapter discarded.
[jobs] proves disjoint cover, thread-count invariance (serial==enki(1)==enki(N)),
and nested ParallelFor safety.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 4: Wiring — `Runtime::TaskExecutor()` + plugin ABI v3 + `PluginHost`

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Base/Runtime.hpp`
- Modify: `Arcane/Arcane/src/Arcane/Base/Runtime.cpp`
- Modify: `Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp`
- Modify: `Arcane/Arcane/src/Arcane/Plugin/PluginHost.cpp`
- Modify: `Arcane/Tests/src/RuntimeTest.cpp`

- [ ] **Step 1: Write the failing test**

In `Arcane/Tests/src/RuntimeTest.cpp`, add the include at the top:
```cpp
#include <Arcane/Jobs/TaskExecutor.hpp>
```
And add, inside the existing test case that already checks `rt.WorkScheduler()` (right after the `WorkScheduler` REQUIREs near line 27-28):
```cpp
    REQUIRE(rt.TaskExecutor() != nullptr);
    REQUIRE(rt.TaskExecutor()->WorkerCount() >= 1);
```

- [ ] **Step 2: Build to verify it fails**

Run BUILD-DEBUG.
Expected: FAIL — `'TaskExecutor': is not a member of 'Arcane::Runtime'`.

- [ ] **Step 3a: Declare `Runtime::TaskExecutor()` in `Runtime.hpp`**

In `Arcane/Arcane/src/Arcane/Base/Runtime.hpp`: add `struct ITaskExecutor;` to the `namespace Arcane { ... }` forward-declarations, and add the accessor next to `WorkScheduler()` (~line 49):
```cpp
        ITaskExecutor* TaskExecutor() noexcept;   // enki pool, worker-index ParallelFor face
```

- [ ] **Step 3b: Define it in `Runtime.cpp`**

In `Arcane/Arcane/src/Arcane/Base/Runtime.cpp`: add `#include <Arcane/Jobs/TaskExecutor.hpp>` near the JobSystem include, and add next to the `WorkScheduler()` definition (~line 72):
```cpp
    ITaskExecutor* Runtime::TaskExecutor() noexcept { return m_impl->jobs.TaskExecutor(); }
```

- [ ] **Step 3c: Add the ABI field + bump version in `PluginABI.hpp`**

In `Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp`:

Add `struct ITaskExecutor;` to the `namespace Arcane { ... }` forward block (the line that already declares `class Runtime;`):
```cpp
    class Runtime;  // defined in Arcane.dll; the plugin holds it opaquely via EngineContext
    struct ITaskExecutor;  // <Arcane/Jobs/TaskExecutor.hpp>; same enki pool, worker-index face
```

Bump the version constant + comment (replace lines 17-19):
```cpp
    // Bump on ANY change to EngineContext layout or the entry-point set/signatures.
    // v2 (2026-06-20): added the ImGui cross-DLL handoff fields below + GamePlugin_DrawUI.
    // v3 (2026-06-26): added taskExecutor (the ITaskExecutor face of the engine scheduler).
    inline constexpr uint32_t kGamePluginABIVersion = 3;
```

Add the field immediately after `workScheduler` (line 25) in `struct EngineContext`:
```cpp
        Astra::IWorkScheduler* workScheduler;  // the one engine enkiTS adapter (shared instance)
        Arcane::ITaskExecutor* taskExecutor;   // SAME enki pool, worker-index ParallelFor (physics/general)
```

- [ ] **Step 3d: Populate the field in `PluginHost.cpp`**

In `Arcane/Arcane/src/Arcane/Plugin/PluginHost.cpp`, right after `ctx.workScheduler = runtime.WorkScheduler();` (~line 75):
```cpp
            ctx.taskExecutor = runtime.TaskExecutor();
```

- [ ] **Step 4: Build + run tests to verify they pass**

Run BUILD-DEBUG (this rebuilds ALL plugins against ABI v3 — they report 3 via the shared header, so the host's ABI check still accepts them), then:
```powershell
Push-Location Arcane\bin\Debug-windows-x86_64-md\ArcaneTests; .\ArcaneTests.exe "[runtime],[plugin]"; Pop-Location
```
Expected: PASS — Runtime exposes a non-null `TaskExecutor()`; the plugin-load/ABI tests still accept the (rebuilt, v3) fixtures.

- [ ] **Step 5: Commit**
```powershell
git add Arcane/Arcane/src/Arcane/Base/Runtime.hpp Arcane/Arcane/src/Arcane/Base/Runtime.cpp Arcane/Arcane/src/Arcane/Plugin/PluginABI.hpp Arcane/Arcane/src/Arcane/Plugin/PluginHost.cpp Arcane/Tests/src/RuntimeTest.cpp
git commit -m @'
feat(arcane/plugin): expose ITaskExecutor via Runtime + EngineContext (ABI v3)

Runtime::TaskExecutor() forwards the JobSystem face; EngineContext gains a
taskExecutor field beside workScheduler (kGamePluginABIVersion 2->3, all
in-tree plugins rebuild + report v3). Phase D's physics consumer reaches the
executor through here.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 5: Category-A migration — Core/Physics visitors → `FunctionRef`

Mechanical 1:1 swap: each `const std::function<SIG>&` visitor parameter becomes `FunctionRef<SIG>` (SIG preserved verbatim), and each touched header gains `#include <Arcane/Util/FunctionRef.hpp>`. Bodies are unchanged. These are debug/query/iteration visitors — NOT the solve path — so behavior is preserved. **Do NOT touch `ContactManager::Listener` (line 95, stays `std::function`) or the Astra `IWorkScheduler::ParallelFor` impl.** Keep `#include <functional>` in any file that still references `std::function` (e.g. `ContactManager.hpp` for `Listener`).

**Files (declaration in `.hpp`, definition in `.cpp` where present):**
- `Arcane/Core/src/Arcane/Physics/Contact.hpp:156-157` + `Contact.cpp:96,107`
- `Arcane/Core/src/Arcane/Physics/ContactManager.hpp:195` + `ContactManager.cpp:30`
- `Arcane/Core/src/Arcane/Physics/Broadphase/DynamicTree.hpp:87` + `DynamicTree.cpp:468`
- `Arcane/Core/src/Arcane/Physics/Broadphase/SpatialGrid.hpp:49` + `SpatialGrid.cpp:61`
- `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp:693,884,956` + `PhysicsWorld.cpp:3236`

- [ ] **Step 1: Apply the signature swaps**

Add `#include <Arcane/Util/FunctionRef.hpp>` to each of `Contact.hpp`, `ContactManager.hpp`, `DynamicTree.hpp`, `SpatialGrid.hpp`, `PhysicsWorld.hpp`. Then change each declaration AND its matching definition (`Arcane::Physics` namespace, so unqualified `FunctionRef` resolves):

`Contact.hpp:156-157` (and same in `Contact.cpp:96,107`):
```cpp
            void ForEach(FunctionRef<void(std::uint32_t id, Contact&)> fn);
            void ForEach(FunctionRef<void(std::uint32_t id, const Contact&)> fn) const;
```

`ContactManager.hpp:195` (and `ContactManager.cpp:30`):
```cpp
            void ForEachBegunPair(FunctionRef<void(std::uint32_t a, std::uint32_t b)> fn) const;
```

`DynamicTree.hpp:87` (and `DynamicTree.cpp:468`):
```cpp
            void ForEachLeaf(
                FunctionRef<void(std::uint32_t id,
                                 const Aabb2& tight,
                                 const Aabb2& fat)> fn) const;
```

`SpatialGrid.hpp:49` (and `SpatialGrid.cpp:61`) — note this one is `namespace Arcane::Physics` too; use unqualified `FunctionRef`:
```cpp
    void ForEachCell(
        FunctionRef<void(int cx, int cy,
                         const std::vector<std::uint32_t>& ids)> fn) const;
```

`PhysicsWorld.hpp:693` (inline, no .cpp):
```cpp
            void ForEachContactConstraint(
                FunctionRef<void(const ContactConstraint&)> fn) const
```

`PhysicsWorld.hpp:884` (and `PhysicsWorld.cpp:3236`):
```cpp
            void ForEachContact(
                FunctionRef<void(std::uint32_t a,
                                 std::uint32_t b)> fn) const;
```

`PhysicsWorld.hpp:956` (inline, no .cpp):
```cpp
            void ForEachIsland(
                FunctionRef<void(const std::vector<std::uint32_t>&)> fn) const
```

- [ ] **Step 2: Build to verify it compiles (call sites bind transparently)**

Run BUILD-DEBUG.
Expected: PASS to compile — every call site passes a lambda, which binds to `FunctionRef` exactly as it did to `const std::function&`. If any call site fails, it is passing a stored `std::function` that escapes; treat that as a real finding and stop (none expected — all are call-site lambdas).

- [ ] **Step 3: Run the physics + determinism suites (behavior-preservation gate)**

```powershell
Push-Location Arcane\bin\Debug-windows-x86_64-md\ArcaneTests; .\ArcaneTests.exe "[physics],[broadphase],[determinism]"; Pop-Location
```
Expected: PASS, identical pass counts to pre-migration (no Step code changed; visitors are caller-transparent).

- [ ] **Step 4: Commit**
```powershell
git add Arcane/Core/src/Arcane/Physics
git commit -m @'
refactor(arcane/physics): Cat-A visitors std::function -> Arcane::FunctionRef

ContactPool/ContactManager/DynamicTree/SpatialGrid/PhysicsWorld ForEach*
visitor params become non-owning FunctionRef (zero per-call construction on
the hot iteration paths). Caller-transparent; ContactManager::Listener stays
owning std::function. Behavior byte-identical ([physics]/[determinism] green).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 6: Category-A migration — Arcane.dll visitors (`RunLoop`, `OffscreenCanvas`)

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Sim/RunLoop.hpp:59-60`
- Modify: `Arcane/Arcane/src/Arcane/Render/OffscreenCanvas.hpp:49` + `OffscreenCanvas.cpp:50`

- [ ] **Step 1: Apply the swaps**

Add `#include <Arcane/Util/FunctionRef.hpp>` to `RunLoop.hpp` and `OffscreenCanvas.hpp`. These are `namespace Arcane`, so unqualified `FunctionRef` resolves.

`RunLoop.hpp:58-60`:
```cpp
        double Advance(double realDt,
                       FunctionRef<void(double)> pluginFixed,
                       FunctionRef<void(double, double)> pluginUpdate)
```
(The callbacks are invoked within `Advance` across substeps and never stored — non-escaping, so `FunctionRef` is safe.)

`OffscreenCanvas.hpp:49` (the pure-virtual) AND `OffscreenCanvas.cpp:50` (the override) — both must match:
```cpp
        virtual void Draw(FunctionRef<void(Batcher2D&)> fn,
                          glm::vec4 clear) = 0;
```
```cpp
            void Draw(FunctionRef<void(Batcher2D&)> fn,
                      glm::vec4 clear) override
```

If `RunLoop.hpp`/`OffscreenCanvas.hpp` no longer reference `std::function`, drop their now-unused `#include <functional>`.

- [ ] **Step 2: Build to verify it compiles**

Run BUILD-DEBUG.
Expected: PASS — `Loom`'s `Advance` call site (passes `FixedUpdate`/`Update` lambdas) and the `OffscreenCanvas::Draw` call sites (the narrowphase inspector) bind transparently.

- [ ] **Step 3: Run the render/sim suites**

```powershell
Push-Location Arcane\bin\Debug-windows-x86_64-md\ArcaneTests; .\ArcaneTests.exe "[runloop],[render],[offscreen]"; Pop-Location
```
Expected: PASS (use whichever of these tags exist; if a tag matches nothing Catch2 reports 0 — fall back to the full suite in Task 7).

- [ ] **Step 4: Commit**
```powershell
git add Arcane/Arcane/src/Arcane/Sim/RunLoop.hpp Arcane/Arcane/src/Arcane/Render/OffscreenCanvas.hpp Arcane/Arcane/src/Arcane/Render/OffscreenCanvas.cpp
git commit -m @'
refactor(arcane): RunLoop::Advance + OffscreenCanvas::Draw -> FunctionRef

The last two Category-A synchronous callback params move off std::function.
Caller-transparent (Loom Advance callbacks + the narrowphase inspector).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 7: Full gate (no new code) — both configs, both backends, server-CRT, smoke

- [ ] **Step 1: Full ArcaneTests Debug (both backends)**
```powershell
msbuild Arcane\Arcane.slnx -p:Configuration=Debug -m
Push-Location Arcane\bin\Debug-windows-x86_64-md\ArcaneTests; .\ArcaneTests.exe; Pop-Location
```
Expected: all green (incl. `[jobs]`, `[functionref]`, `[physics]`, `[determinism]`, `[gpu]` on D3D12 + Vulkan).

- [ ] **Step 2: Full ArcaneTests Release (both backends)**
```powershell
msbuild Arcane\Arcane.slnx -p:Configuration=Release -m
Push-Location Arcane\bin\Release-windows-x86_64-md\ArcaneTests; .\ArcaneTests.exe; Pop-Location
```
Expected: all green.

- [ ] **Step 3: ArcaneCore static-CRT (server flavor compiles the migrated Core/Physics + FunctionRef/TaskExecutor)**
```powershell
Push-Location Server; & "..\ThirdParty\premake5\premake5.exe" vs2026; Pop-Location
msbuild Server\Aphelyon.slnx -p:Configuration=Debug -m -t:ArcaneCore
msbuild Server\Aphelyon.slnx -p:Configuration=Release -m -t:ArcaneCore
```
Expected: `ArcaneCore` builds clean under static CRT (the header-only Core additions + migrated visitors compile in the server flavor).

- [ ] **Step 4: Headless Loom smoke (the live RunLoop::Advance + plugin ABI v3 path)**
```powershell
Push-Location Arcane\bin\Debug-windows-x86_64-md\Loom
.\Loom.exe --backend dx12 --frames 30
.\Loom.exe --backend vulkan --frames 30
Pop-Location
```
Expected: both exit 0, `RenderErrorCount()==0` (the v3 EngineContext loads Sandbox.dll; Advance runs through `FunctionRef`).

- [ ] **Step 5: No commit (pure verification).** Record the green results in the task notes / for the holistic review.

---

## Notes for the executor

- **Determinism scope:** this milestone validates thread-count invariance on the *executor itself* ([jobs]); it parallelizes no Step code, so `[physics]`/`[determinism]` stay byte-identical by construction. Phase D makes the solver honor the invariant.
- **ABI bump:** Task 4 takes the plugin ABI 2→3. All in-tree plugins (Sandbox, PlaygroundGame, HotReload fixtures) rebuild with the shared header and report v3, so the host's version check still accepts them. There are no out-of-tree plugins.
- **No `premake5.lua` edits, no project-file commits:** Core and Tests glob `src/**`, so every new file is auto-included on the next REGEN. The REGEN'd `.slnx`/`.vcxproj` are **gitignored** — regenerate them locally so MSVC sees new files, but commit only the tracked `.hpp`/`.cpp` sources.
