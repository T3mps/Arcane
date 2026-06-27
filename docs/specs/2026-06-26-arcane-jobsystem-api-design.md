# Arcane JobSystem API — `ITaskExecutor` Seam + `FunctionRef` + Category-A Callback Migration (Design)

- **Date:** 2026-06-26
- **Status:** Design approved (brainstorming complete). Implementation plan pending.
- **Scope:** Stand up the **task-parallelism substrate** that the physics multithreading milestone (Phase D) will consume: a Core-owned `Arcane::ITaskExecutor` seam (worker-index-aware `ParallelFor`), a `SerialTaskExecutor` reference impl, an enkiTS-backed impl that extends the existing `JobSystem` (two adapter faces over one scheduler), and the wiring to expose it (Runtime + plugin ABI). Introduce a first-party non-owning `Arcane::FunctionRef<Sig>` callable view and **migrate the existing Category-A `std::function` callback parameters** (transient, synchronous, non-escaping visitors) to it. **Pure substrate — no physics Step is parallelized here** (that is Phase D); the physics solve path stays byte-identical.
- **Relates to:** the physics data-model rearchitecture Phases A–C (the compacted SoA + graph coloring this substrate exists to thread); the Loom host refactor (`Arcane::Cli` — the precedent for a presentation-free, std-only, first-party Core utility, NO `ARCANE_API`); the existing `JobSystem`/`IWorkScheduler` Astra seam; the future event-bus milestone (where the deferred fast `Delegate` belongs).
- **Non-goals:** **no PhysicsWorld solver parallelization** (Phase D — this milestone delivers + validates the substrate, not the consumption); no async `Dispatch -> TaskHandle -> Wait`, no task-graph/DAG, no continuations, no pinned/IO lanes (YAGNI — add when a real consumer needs stage overlap); **no fast single/multicast `Delegate`** (the scan found exactly one stored-callback site, which wants owning semantics — defer to the event-bus milestone); no change to Astra's `IWorkScheduler` ABI (it is vendored; its `ParallelFor` keeps `std::function`); `ContactManager::Listener` stays `std::function` (correctly owning).

---

## 1. Motivation

The physics rearchitecture (Phases A–C) deliberately shrank the per-step work into a cache-local, graph-colored SoA so that the solve could be threaded *additively* ("shrink-then-thread"). The measured 10k-body profile is now collision/scale-bound, and the next lever is multithreading — but the engine has no task-parallelism API a presentation-free Core module can consume. The one primitive that exists, `Astra::IWorkScheduler::ParallelFor(count, minBatch, fn)` (served by `JobSystem`), lives in `Arcane.dll`, is shaped for Astra's ECS scheduler, and **throws away enkiTS's per-worker thread index** (`JobSystem.cpp:32`) — exactly the datum a colored solver needs for per-thread scratch. This milestone builds the missing substrate so Phase D is a focused consumer, not a foundation-and-consumer at once.

A workspace scan for `std::function` (22 hits / 15 files) showed the callback strategy with clarity: **20 of 22 are transient synchronous visitor/callback parameters** (`const std::function<Sig>&`) — textbook `function_ref` — several on hot physics-iteration paths; **1** is a stored listener that wants owning semantics; **2** are Astra-internal reflection-thunk comments. So a non-owning callable view earns its place immediately and broadly; a fast `Delegate` does not (one consumer, wrong semantics).

## 2. Decisions (from brainstorming)

1. **Architecture is settled: Option A — decoupled simulation core + a Core-level task seam.** `Core/Physics/PhysicsWorld` stays Astra-free and owns its own inner-loop threading (consuming `ITaskExecutor`); Astra's `PhysicsSystem` keeps its role of *orchestration + entity↔body boundary sync* (the DESTROY/CREATE/STEP/WRITE-BACK passes), not solver threads. The world is the authoritative SoA; components are authoring + a handle (a mirror model). Rationale: keeps Core liftable + Sandbox/server-standalone, and puts parallelism where the compacted data lives. (Full rationale in §3.)
2. **The seam lives in `Core`** (`Arcane::ITaskExecutor`) — presentation-free, Astra-free, **no mutable global state** (the invariant that lets Core be statically duplicated across modules; see §3). So `ArcaneCore` (server flavor) can thread `PhysicsWorld` too.
3. **Worker index is first-class.** The primitive is `ParallelFor(count, minBatch, fn(begin, end, worker))` — the headline fix over the current adapter.
4. **First-party `Arcane::FunctionRef<Sig>`** (non-owning callable view) is the callback type — not `std::function` (per-step alloc footgun in the alloc-free physics path), not Hazel's `Delegate` (owning-lambda story unfinished, event-binding-shaped, a vendored file). Migrate the Category-A visitors to it in the same milestone so it is load-bearing, not speculative.
5. **Fast `Delegate` deferred** to the future event-bus milestone (input is polling by design; physics visitors want `FunctionRef`; the one stored `Listener` wants owning `std::function`).

## 3. Architecture context (why Core, why a seam, why a mirror)

- **Two layers, already built.** `Core/Physics/PhysicsWorld` (Astra-free, the compacted color/island/awake SoA, `Step(dt)`) is driven *directly* by the Sandbox and *indirectly* by `Arcane.dll`'s `PhysicsSystem` (an Astra fixedUpdate system: DESTROY dead bodies → CREATE/SYNC from `RigidBody2D`/`Collider2D` → `world.Step` → WRITE-BACK poses to `LocalTransform`). The world + `entity↔BodyHandle` map live in a `PhysicsResource`.
- **Why the seam must be in Core, not Astra:** Core is *statically duplicated* into every module (`Arcane.dll`, `ArcaneCore`×services, `Sandbox.dll`, `ArcaneTests`); that is only safe because Core carries **no mutable global singletons** (Server/premake5.lua:317). Astra's `TypeID`/`MetaRegistry` is per-module mutable static state, deliberately centralized into one module via an injected `TypeContext` — so Astra cannot live in Core. The physics simulation *logic* already is in Core (server-reachable today: `ArcaneCore` globs `Physics/`); only the ECS *integration* is excluded. A Core-level `ITaskExecutor` lets the Astra-free world thread itself without taking an Astra or enkiTS dependency.
- **Two parallelism domains, one scheduler:** (1) *inside* `world.Step` → the new `ITaskExecutor` (Phase D's consumer); (2) the `PhysicsSystem` sync passes → Astra `ParallelForEach` over component views via the *existing* `IWorkScheduler`. Both back onto the **one** `enki::TaskScheduler`.

## 4. Unit map

| Unit | Location | Responsibility | Depends on |
|---|---|---|---|
| `Arcane::FunctionRef<Sig>` | `Arcane/Core/src/Arcane/Base/FunctionRef.hpp` (header-only) | Non-owning callable view (`void* + thunk`); implicit-constructs from any callable; zero alloc. | std only |
| `Arcane::ITaskExecutor` | `Arcane/Core/src/Arcane/Jobs/TaskExecutor.hpp` (header-only) | The abstract seam: `ParallelFor(count, minBatch, fn(begin,end,worker))` + `WorkerCount()`. | `FunctionRef`, std |
| `Arcane::SerialTaskExecutor` | `Arcane/Core/src/Arcane/Jobs/TaskExecutor.hpp` | Inline single-thread impl (the deterministic reference; the default when nothing is injected). | `ITaskExecutor` |
| enkiTS task executor | `Arcane/Arcane/src/Arcane/Jobs/JobSystem.{hpp,cpp}` (extend) | `JobSystem` gains a 2nd adapter face — `TaskExecutor()` — over the same `enki::TaskScheduler`; preserves the worker index. | enkiTS, `ITaskExecutor` |
| Wiring | `Runtime`, `PluginABI` (extend) | `Runtime::TaskExecutor()`; new `Arcane::ITaskExecutor* taskExecutor` ABI field (mirrors `workScheduler`). | above |
| Category-A migration | 10 call sites (see §8) | `const std::function<Sig>&` params → `FunctionRef<Sig>`. Behavior-preserving. | `FunctionRef` |

All new files → regen both workspaces (Core + Arcane premake globs are `src/**`; `FunctionRef`/`TaskExecutor` also compile under `ArcaneCore` static-CRT).

## 5. `Arcane::FunctionRef<Sig>` — the callable view

Header-only, `namespace Arcane`, std-only (`<utility>`, `<type_traits>`, `<memory>`). Non-owning; binds to any callable whose lifetime spans the call (always true for synchronous, non-escaping use). Forward-compatible with C++26 `std::function_ref` (drop-in via using-alias later).

```cpp
namespace Arcane {
  template <class Sig> class FunctionRef;            // primary left undefined

  template <class R, class... Args>
  class FunctionRef<R(Args...)> {
    void* m_obj = nullptr;
    R (*m_thunk)(void*, Args...) = nullptr;
  public:
    FunctionRef() = default;                          // empty; operator bool() == false

    template <class F>
      requires (!std::is_same_v<std::remove_cvref_t<F>, FunctionRef>
                && std::is_invocable_r_v<R, F&, Args...>)
    FunctionRef(F&& f) noexcept                        // implicit by design (call-site lambda)
      : m_obj(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
        m_thunk([](void* o, Args... a) -> R {
          return (*static_cast<std::remove_reference_t<F>*>(o))(std::forward<Args>(a)...);
        }) {}

    R operator()(Args... a) const { return m_thunk(m_obj, std::forward<Args>(a)...); }
    explicit operator bool() const noexcept { return m_thunk != nullptr; }
  };
}
```

- **Const-correct:** binds to const callables (the `ForEach(... ) const` visitors) — the thunk instantiates on `remove_reference_t<F>`, which carries constness.
- **Lifetime contract (documented at the type):** the referent must outlive the `FunctionRef`. Safe for every Category-A/`ITaskExecutor` use because the callable is invoked synchronously within the same full-expression / blocking call. **Not** for storage — a stored callback uses owning `std::function` (or a future `Delegate`).
- Passed **by value** (two pointers); trivially copyable.

## 6. `Arcane::ITaskExecutor` — the seam

```cpp
namespace Arcane {
  // Synchronous data-parallel fan-out. The ONLY task primitive this milestone ships.
  struct ITaskExecutor {
    // Partitions [0,count) into disjoint sub-ranges (grain >= minBatch where possible) and
    // invokes fn(begin,end,worker) on each; `worker` in [0,WorkerCount()) names the running
    // thread (per-worker scratch). BLOCKS until every sub-range completes. count==0 is a no-op.
    // Re-entrant: legal to call from within an fn already running on this executor (nested).
    virtual void ParallelFor(std::size_t count, std::size_t minBatch,
                             FunctionRef<void(std::size_t begin, std::size_t end,
                                              std::uint32_t worker)> fn) = 0;

    // Inclusive of the calling thread (the batch-size denominator). Always >= 1.
    virtual std::uint32_t WorkerCount() const noexcept = 0;

    virtual ~ITaskExecutor() = default;
  };
}
```

**Contract guarantees (the spec the impls must honor):**
- Sub-ranges are **disjoint and cover `[0,count)`** exactly once; partition boundaries and the number of partitions are unspecified (and may vary with `WorkerCount`).
- `worker ∈ [0, WorkerCount())`; two sub-ranges running concurrently never share a `worker` value.
- **No FP-order dependence is introduced:** because consumers (the colored solver) only fan out over provably independent work, results are **invariant to partitioning, scheduling, and `WorkerCount`** (see §7).

## 7. Impls + the determinism invariant

**`SerialTaskExecutor` (Core).** `ParallelFor` calls `fn(0, count, 0)` inline; `WorkerCount()==1`. This is the deterministic reference and the default — Sandbox, tests, and the server run with it when no enkiTS executor is injected (mirrors Astra's "null scheduler → sequential"). No null-checks leak into consumers.

**enkiTS face (`JobSystem`, Arcane.dll).** Add an `EnkiTaskExecutor` (internal TU class) implementing `ITaskExecutor` over the existing `enki::TaskScheduler`: an `enki::TaskSet` whose lambda forwards `range.start/range.end` **and `threadnum` (the previously-discarded worker index)** to `fn`, then `AddTaskSetToPipe` + `WaitforTask` (calling thread participates → nested-safe). `JobSystem` exposes both faces over the one scheduler:

```cpp
std::shared_ptr<Astra::IWorkScheduler> WorkScheduler() const;  // existing — Astra ECS, keeps std::function (vendored ABI)
ITaskExecutor*                         TaskExecutor()  const;   // new — physics/general, FunctionRef + worker index
```

**Thread-count invariance (the load-bearing contract).** Physics is deterministic (`/fp:precise`, fixed dt, run-twice gates). Phase C's graph coloring guarantees constraints within a color write **disjoint** bodies → no shared FP accumulation → partition/order/worker-count cannot change the result. This milestone *establishes and validates the invariant on the executor itself* (serial ≡ parallel on a synthetic scatter/reduce at `threads=1` vs `threads=N`, byte-identical); **Phase D makes the physics solver honor it** when it adopts the seam. This milestone parallelizes no Step code, so the existing physics determinism/run-twice gates stay byte-identical by construction.

## 8. Category-A `std::function` → `FunctionRef` migration (in scope)

Mechanical, behavior-preserving swap of transient synchronous visitor *parameters*. These are debug/query/iteration callbacks (not the solve path), so results are unchanged; the win is eliminating per-call `std::function` construction (alloc risk) on hot iteration paths and making `FunctionRef` immediately load-bearing.

| Function | Files |
|---|---|
| `ContactPool::ForEach` (mutable + const overloads) | `Core/Physics/Contact.hpp:156-157`, `Contact.cpp:96,107` |
| `ContactManager::ForEachBegunPair` | `Core/Physics/ContactManager.hpp:195`, `.cpp:30` |
| `DynamicTree::ForEachLeaf` | `Core/Physics/Broadphase/DynamicTree.hpp:87`, `.cpp:468` |
| `SpatialGrid::ForEachCell` | `Core/Physics/Broadphase/SpatialGrid.hpp:49`, `.cpp:61` |
| `PhysicsWorld::ForEachContactConstraint` | `Core/Physics/PhysicsWorld.hpp:693` (inline) |
| `PhysicsWorld::ForEachContact` | `Core/Physics/PhysicsWorld.hpp:884`, `.cpp:3236` |
| `PhysicsWorld::ForEachIsland` | `Core/Physics/PhysicsWorld.hpp:956` (inline) |
| `RunLoop::Advance` (`pluginFixed`, `pluginUpdate` params) | `Arcane/Sim/RunLoop.hpp:59-60` |
| `OffscreenCanvas::Draw` | `Arcane/Render/OffscreenCanvas.hpp:49`, `.cpp:50` |

**Explicitly NOT migrated:**
- `Astra::IWorkScheduler::ParallelFor`'s `std::function` (`JobSystem.cpp:21`) — implements a **vendored** Astra interface; its ABI is fixed. The new `ITaskExecutor` is the `FunctionRef` face; the old one stays.
- `ContactManager::Listener = std::function<void(const ContactEvent&)>` (`ContactManager.hpp:95`) — a **stored** callback; owning is correct (a `FunctionRef` would dangle). Future event-bus milestone may revisit it as a multicast `Delegate`.

## 9. Testing + gates

- **`[jobs]` unit tests** (new `Arcane/Tests/src/TaskExecutorTest.cpp`, no GPU): `ParallelFor` count/minBatch edges (0, 1, < minBatch, exact multiples, ragged), sub-ranges **disjoint + union == [0,count)**, `worker ∈ [0,WorkerCount())`, **serial ≡ parallel** equivalence on a scatter/reduce workload, **thread-count invariance** (`threads=1` vs `threads=N` byte-identical), **nested `ParallelFor` from within a worker**, `WorkerCount() >= 1`.
- **`FunctionRef` unit tests** (`[base]`): binds capturing/mutable/const lambdas, free functions, functors; const-visitor binding; return-value + void; `operator bool`.
- **Physics regression (the behavior-preservation gate):** full `[physics]` + run-twice determinism suites stay **byte-identical** after the Category-A migration (no Step code is parallelized).
- **Build gates:** Arcane Debug+Release both backends; **`ArcaneCore` static-CRT Debug+Release clean** (`FunctionRef`/`TaskExecutor`/`SerialTaskExecutor` compile under the server flavor; migrated Core visitors still build static-CRT); regen both workspaces.
- **Headless smoke:** `Loom --frames N` (dx12/vulkan) exits 0, `RenderErrorCount()==0` (the `RunLoop::Advance` + `OffscreenCanvas::Draw` migrations exercised live).

## 10. Risks

- **Behavior drift in the visitor migration.** Mitigation: it is a 1:1 parameter-type swap on synchronous non-escaping callbacks; the `[physics]` + run-twice byte-identical gate pins equivalence.
- **`FunctionRef` lifetime misuse.** Mitigation: the type documents "non-owning; referent must outlive the ref; synchronous use only," and every introduced use is a blocking call; storage paths keep `std::function`. A `static_assert`/Debug guard is not feasible for dangling, so the contract is enforced by review + the synchronous-only usage rule.
- **enkiTS nested-task safety.** Mitigation: enkiTS supports nested `AddTaskSetToPipe`/`WaitforTask` (the waiting thread participates); the `[jobs]` nested test pins it. This matters because `PhysicsSystem` may run inside Astra's parallel scheduler when it calls `world.Step` (Phase D).
- **Core surface growth.** `FunctionRef` + `ITaskExecutor` are std-only, presentation-free, stateless — consistent with Core's charter (the `Arcane::Cli` precedent) and they earn their place by broad reuse.

## 11. Out of scope / future (explicit)

- **Phase D (next milestone):** `PhysicsWorld::SetExecutor(ITaskExecutor*)` (set-once, `SerialTaskExecutor` default) + parallelizing the colored solve / broadphase refit / narrowphase over the seam; the thread-count-invariance gate becomes load-bearing on the physics suites there.
- **Async lane (add on demand):** an owning `Dispatch(callable) -> TaskHandle` + `Wait(handle)` for stage overlap — needs an *owning* callable (SBO `unique_function`), since the work escapes. Deferred until a consumer needs it.
- **Event-bus milestone:** the fast single/multicast `Delegate` (Hazel-pattern reference) for stored, bound callbacks — first customers would be a contact/gameplay event bus (`ContactManager::Listener` → multicast) and UI behavior-graph events. Input stays polling (snapshot/replay contract), so it is *not* a trigger.
