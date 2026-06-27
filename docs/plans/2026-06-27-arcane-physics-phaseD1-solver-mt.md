# Arcane Physics Phase D1 — Within-Color Solver Multithreading Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Multithread the colored `SoftStep` solver (the measured 36% `solve` stage) over `Arcane::ITaskExecutor` using within-color parallelism, byte-identical at any thread count.

**Architecture:** `PhysicsWorld` holds an `ITaskExecutor*` (serial default); `SolverContext` carries it into `SoftStep::Solve`. The **outer color loop stays serial** (Gauss-Seidel); within each color, the per-color `SimdSolve` passes (warm-start / solve / relax / restitution) are distributed over a color's disjoint-body batches via `ParallelFor`, and the per-awake-body integrate loops are `ParallelFor` over `AwakeBodies()`. Scalar overflow stays serial. The coloring invariant (no dynamic body twice per color) makes parallel == serial by construction.

**Tech Stack:** C++23, MSVC (VS 2026), enkiTS via the engine `JobSystem`/`ITaskExecutor`, Catch2.

**Spec:** `docs/superpowers/specs/2026-06-27-arcane-physics-phaseD1-solver-mt-design.md`

---

## Conventions (referenced by every task)

PowerShell from repo root `D:\dev\starworks\Gacha`. `msbuild` is NOT on PATH — use the explicit VS2026 one.

- **BUILD-DEBUG:** `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane\Arcane.slnx -p:Configuration=Debug -m`
- **BUILD-RELEASE:** same with `-p:Configuration=Release`
- **TEST** (run from the exe dir): `Push-Location Arcane\bin\Debug-windows-x86_64-md\ArcaneTests; .\ArcaneTests.exe "<tagexpr>"; Pop-Location`
- **REGEN** (only when adding a new .cpp file; Core/Tests glob `src/**`): `Push-Location Arcane; & "..\ThirdParty\premake5\premake5.exe" vs2026; Pop-Location`
- Generated `.slnx`/`.vcxproj` are gitignored — commit only `.hpp`/`.cpp`. clangd diagnostics are FALSE POSITIVES (MSVC is truth). Commit-message trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

## File structure

| File | Change |
|---|---|
| `Arcane/Core/src/Arcane/Physics/Solver/Solver.hpp` | `SolverContext` gains `ITaskExecutor* executor` |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` | `SetExecutor` + `m_executor` + `m_serialExecutor`; include `<Arcane/Jobs/TaskExecutor.hpp>` |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` | resolve + fill `ctx.executor` at the `m_solver->Solve(ctx)` site (~1676) |
| `Arcane/Core/src/Arcane/Physics/Solver/ContactConstraintSimd.hpp` | range overloads of `SimdSolve::WarmStart/SolveNormalAndFriction/ApplyRestitution` |
| `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp` | per-color `ParallelFor` (Task 3) + per-awake-body `ParallelFor` (Task 4) |
| `Arcane/Sandbox/src/SandboxApp.cpp` | wire `world.SetExecutor(ctx->taskExecutor)` |
| `Arcane/Tests/src/SolverMtInvarianceTest.cpp` | NEW — thread-count invariance + SimdSolve partition-invariance |

---

## Task 1: Executor injection plumbing (serial default; no behavior change)

**Files:** `Solver.hpp`, `PhysicsWorld.hpp`, `PhysicsWorld.cpp`, `SandboxApp.cpp`, new `SolverMtInvarianceTest.cpp`.

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/SolverMtInvarianceTest.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>

using namespace Arcane::Physics;

TEST_CASE("PhysicsWorld accepts an executor and steps with it (serial default)", "[physics][solvermt]")
{
    PhysicsWorld w{};
    Arcane::SerialTaskExecutor serial;
    w.SetExecutor(&serial);                 // explicit serial
    w.Step(1.0f / 60.0f);                   // must not crash; serial path unchanged
    SUCCEED("stepped with an injected executor");

    PhysicsWorld w2{};
    w2.SetExecutor(nullptr);                // null -> falls back to the world's serial default
    w2.Step(1.0f / 60.0f);
    SUCCEED("stepped with null executor (serial fallback)");
}
```

- [ ] **Step 2: Regen + build to verify it fails**

REGEN, then BUILD-DEBUG. Expected: FAIL — `'SetExecutor': is not a member of 'Arcane::Physics::PhysicsWorld'`.

- [ ] **Step 3: Add `executor` to `SolverContext`**

In `Arcane/Core/src/Arcane/Physics/Solver/Solver.hpp`, add a forward decl near the other forward decls inside `namespace Arcane::Physics` (top, by `class PhysicsWorld;`):
```cpp
        // (file scope, namespace Arcane) -- ITaskExecutor lives in Core's Jobs module.
```
Add to `namespace Arcane { struct ITaskExecutor; }` — place this just ABOVE `namespace Arcane { namespace Physics {` (so it's `Arcane::ITaskExecutor`):
```cpp
namespace Arcane { struct ITaskExecutor; }
```
Then add the field to `struct SolverContext` (after `gravity`):
```cpp
            // Task-parallelism seam (Phase D1). Always non-null when the solver runs
            // (PhysicsWorld resolves it to its serial default if none was injected).
            ITaskExecutor* executor = nullptr;
```

- [ ] **Step 4: Add `SetExecutor` + members to `PhysicsWorld.hpp`**

In `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp`: add the include near the other Core includes:
```cpp
#include <Arcane/Jobs/TaskExecutor.hpp>
```
Add the public method (near `Step`):
```cpp
            // Phase D1: inject the task executor the solver parallelizes over.
            // nullptr -> the world's owned SerialTaskExecutor (deterministic default).
            void SetExecutor(ITaskExecutor* exec) noexcept { m_executor = exec; }
            [[nodiscard]] ITaskExecutor* Executor() noexcept
            {
                return m_executor ? m_executor : &m_serialExecutor;   // always valid; move-safe
            }
```
Add the members (private, near other members):
```cpp
            ITaskExecutor*     m_executor = nullptr;   // injected; null -> m_serialExecutor
            SerialTaskExecutor m_serialExecutor;       // owned deterministic fallback
```
(`m_executor` is a plain pointer, never a self-pointer to `m_serialExecutor`, so a move can't dangle it; `Executor()` resolves the fallback on read.)

- [ ] **Step 5: Fill `ctx.executor` at the Solve site in `PhysicsWorld.cpp`**

Find `m_solver->Solve(ctx);` (~line 1676). Immediately BEFORE it, add:
```cpp
                ctx.executor = Executor();   // Phase D1: always non-null (serial default)
```

- [ ] **Step 6: Wire the real executor in the Sandbox**

In `Arcane/Sandbox/src/SandboxApp.cpp`, where the `PhysicsWorld` is created/owned (search for the `PhysicsWorld` construction / where the `EngineContext`/`ctx` is available), add after the world exists:
```cpp
            // Phase D1: drive the solver over the engine's enki pool (the first
            // consumer of EngineContext::taskExecutor). Null in headless/odd hosts
            // -> SetExecutor(nullptr) keeps the serial default.
            world.SetExecutor(ctx ? ctx->taskExecutor : nullptr);
```
(Use the actual world variable + the `EngineContext*` name in scope. If the world is rebuilt on scene reset, set it there too. If `ctx`/world wiring isn't obvious, STOP and ask rather than guessing.)

- [ ] **Step 7: Build + run to verify pass**

BUILD-DEBUG, then TEST `"[solvermt]"`. Expected: PASS. Then TEST `"[physics],[determinism]"` — must stay green (serial default = zero behavior change).

- [ ] **Step 8: Commit**
```powershell
git add Arcane/Core/src/Arcane/Physics/Solver/Solver.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Sandbox/src/SandboxApp.cpp Arcane/Tests/src/SolverMtInvarianceTest.cpp
git commit -m @'
feat(arcane/physics): inject ITaskExecutor into the solver (serial default)

PhysicsWorld::SetExecutor + SolverContext::executor; the world resolves to an
owned SerialTaskExecutor when none injected (no hot-loop null-checks). Sandbox
wires EngineContext::taskExecutor (first consumer). No behavior change yet.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 2: Range-based `SimdSolve` overloads (the parallel building block)

**Files:** `ContactConstraintSimd.hpp`.

The driver currently calls `SimdSolve::WarmStart(batches, m_bodyState)` etc., each looping over ALL of a color's `batches`. Add overloads taking `[begin, end)` over `batches`, so the existing full call delegates to `(…, 0, batches.size())`. This is a **pure behavior-preserving refactor** — the new overloads are unused until Task 3, and the full versions delegate, so their gate is "existing suites stay byte-identical green" (the refactor's safety net) and Task 3's invariance test proves the split path end-to-end. No new standalone test.

- [ ] **Step 1: Add the range overloads in `ContactConstraintSimd.hpp`**

In `namespace SimdSolve` (around line 456), for EACH of `WarmStart`, `SolveNormalAndFriction`, `ApplyRestitution`: add a range overload whose loop runs `[begin,end)` instead of the whole `batches`, with the loop BODY unchanged, and make the existing full-arg function delegate to it. Pattern (shown for `WarmStart`; apply the same shape to the other two, preserving their extra params `h,useBias,maxBiasVel` and `threshold`):
```cpp
        // BEFORE (existing): loops the whole list.
        //   inline void WarmStart(std::vector<ContactConstraintSimd>& batches, BodyStateSoA& bs) {
        //       for (ContactConstraintSimd& cc : batches) { /* ...body... */ }
        //   }
        //
        // AFTER: a range overload does the work; the whole-list version delegates.
        inline void WarmStart(std::vector<ContactConstraintSimd>& batches, BodyStateSoA& bs,
                              std::size_t begin, std::size_t end)
        {
            for (std::size_t i = begin; i < end; ++i)
            {
                ContactConstraintSimd& cc = batches[i];
                /* ...the EXACT existing loop body, unchanged... */
            }
        }
        inline void WarmStart(std::vector<ContactConstraintSimd>& batches, BodyStateSoA& bs)
        {
            WarmStart(batches, bs, 0, batches.size());
        }
```
Do the same for `SolveNormalAndFriction(batches, bs, h, useBias, maxBiasVel)` and `ApplyRestitution(batches, bs, threshold)`: extract the loop body into a `(…, std::size_t begin, std::size_t end)` overload, delegate from the full version. `StoreImpulses` does NOT need a range overload (it stays serial, called once per color after the solve). Leave the per-batch body math byte-for-byte identical — only the loop header changes (`for (cc : batches)` → `for (i=begin; i<end; ++i) { auto& cc = batches[i]; … }`).

- [ ] **Step 2: Build + run the existing suites (refactor safety net)**

BUILD-DEBUG, then TEST `"[physics],[determinism],[simd]"`. Expected: ALL still byte-identical green — the delegating full-version is identical behavior, so nothing changes. (The new range overloads are unused until Task 3; this step proves the delegation introduced no drift.) If anything fails, the extracted loop body diverged from the original — STOP and diff it against the original loop.

- [ ] **Step 3: Commit**
```powershell
git add Arcane/Core/src/Arcane/Physics/Solver/ContactConstraintSimd.hpp
git commit -m @'
refactor(arcane/physics): SimdSolve range overloads (WarmStart/Solve/Restitution)

Add [begin,end) batch-range overloads; the whole-list versions delegate.
Pure behavior-preserving refactor -- the building block for within-color
ParallelFor. Gated by the existing [physics]/[determinism]/[simd] suites
staying byte-identical green; Task 3 proves the split path end-to-end.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 3: Parallelize the per-color passes (the main win) + thread-count invariance gate

**Files:** `SoftStep.cpp`, `SolverMtInvarianceTest.cpp`.

- [ ] **Step 1: Write the failing test (thread-count invariance on a pile)**

Append to `Arcane/Tests/src/SolverMtInvarianceTest.cpp`:
```cpp
#include <Arcane/Jobs/JobSystem.hpp>
#include <cstdint>

namespace
{
    // Deterministic pile: a static floor + N dynamic boxes dropped in a column.
    // Returns a flat snapshot (x,y,angle,vx,vy per body) after `steps`, driven by
    // the given executor. Identical inputs + within-color-parallel solve => the
    // snapshot must be byte-identical across executors/thread counts.
    std::vector<float> RunPile(Arcane::ITaskExecutor* exec, int steps)
    {
        using namespace Arcane::Physics;
        WorldDef wd; PhysicsWorld w(wd);
        w.SetExecutor(exec);
        std::vector<BodyHandle> bodies;
        // floor
        { BodyDef fd; fd.type = BodyType::Static; fd.position = Vec2(0, 0);
          fd.shape = MakeAabb(20.0f, 0.5f); w.AddBody(fd); }
        // 200 dynamic boxes in a 10-wide column (deterministic placement)
        for (int i = 0; i < 200; ++i)
        {
            BodyDef bd; bd.type = BodyType::Dynamic;
            bd.position = Vec2(static_cast<Real>((i % 10) - 5) * 0.6f,
                               1.0f + static_cast<Real>(i / 10) * 0.6f);
            bd.shape = MakeAabb(0.25f, 0.25f);
            bodies.push_back(w.AddBody(bd));
        }
        for (int s = 0; s < steps; ++s) { w.Step(1.0f / 60.0f); }
        std::vector<float> out; out.reserve(bodies.size() * 5);
        for (auto h : bodies)
        {
            const Vec2 p = w.Position(h); const Vec2 v = w.Velocity(h);
            out.push_back(static_cast<float>(p.x)); out.push_back(static_cast<float>(p.y));
            out.push_back(static_cast<float>(w.GetAngle(h)));
            out.push_back(static_cast<float>(v.x)); out.push_back(static_cast<float>(v.y));
        }
        return out;
    }
}

TEST_CASE("solver thread-count invariance: serial == enki(1) == enki(N)", "[physics][determinism][solvermt]")
{
    Arcane::SerialTaskExecutor serial;
    Arcane::JobSystem one(1), many(0);    // 1 thread vs hardware default

    const auto a = RunPile(&serial, 120);
    const auto b = RunPile(one.TaskExecutor(), 120);
    const auto c = RunPile(many.TaskExecutor(), 120);

    REQUIRE(a.size() == b.size());
    REQUIRE(a == b);    // byte-identical (within-color parallel == serial by construction)
    REQUIRE(a == c);
}
```
> Implementer note: confirm the `BodyDef`/`WorldDef`/`MakeAabb` field names against `PhysicsWorld.hpp` / `Shapes.hpp` (e.g. `MakeAabb(halfW, halfH)`); adjust the fixture to the real API (it must produce a multi-color pile — 200 stacked boxes generate plenty of inter-box contacts across colors). Keep the pile deterministic (no RNG). The test FAILS now because the solver ignores the executor (still serial), so all three are equal *trivially* — to make it a real RED, FIRST run it after Task 2 (it passes trivially), then it stays GREEN through Task 3; its teeth come from Task 3 onward (any non-deterministic parallel solve breaks `a == c`). If you want a hard RED before Task 3, temporarily assert `many.TaskExecutor()->WorkerCount() > 1` so the test is meaningful on this machine.

- [ ] **Step 2: Build + run (baseline green, pre-parallel)**

BUILD-DEBUG, TEST `"[solvermt]"`. Expected: PASS trivially (solver still serial). This pins the invariant before introducing parallelism.

- [ ] **Step 3: Parallelize the per-color passes in `SoftStep::Solve`**

In `Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp`, add a grain constant near the top of the anonymous namespace / file:
```cpp
        // Phase D1: min batches per ParallelFor chunk (small colors run inline).
        static constexpr std::size_t kSolverColorGrain = 8;
```
Add the include at the top:
```cpp
#include <Arcane/Jobs/TaskExecutor.hpp>
```
In `Solve()`, replace the FOUR per-color serial loops (warm-start ~791, biased-solve ~797-800, relax ~808-811, restitution ~816) — each currently `for (auto& batches : m_colorBatches) { SimdSolve::X(batches, …); }` — with a within-color `ParallelFor`. The OUTER color loop stays serial; only the per-color batch processing parallelizes. Warm-start example:
```cpp
                // Warm start (per sub-step): serial color loop, parallel within each color.
                for (auto& batches : m_colorBatches)
                {
                    ctx.executor->ParallelFor(batches.size(), kSolverColorGrain,
                        [&](std::size_t b, std::size_t e, std::uint32_t)
                        { SimdSolve::WarmStart(batches, m_bodyState, b, e); });
                }
                OverflowWarmStart(ctx);
```
Biased solve:
```cpp
                for (auto& batches : m_colorBatches)
                {
                    ctx.executor->ParallelFor(batches.size(), kSolverColorGrain,
                        [&](std::size_t b, std::size_t e, std::uint32_t)
                        { SimdSolve::SolveNormalAndFriction(batches, m_bodyState, static_cast<float>(h), /*useBias=*/true, maxBiasVel, b, e); });
                }
                OverflowSolve(ctx, h, /*useBias=*/true);
```
Relax (same as biased with `useBias=false`):
```cpp
                for (auto& batches : m_colorBatches)
                {
                    ctx.executor->ParallelFor(batches.size(), kSolverColorGrain,
                        [&](std::size_t b, std::size_t e, std::uint32_t)
                        { SimdSolve::SolveNormalAndFriction(batches, m_bodyState, static_cast<float>(h), /*useBias=*/false, maxBiasVel, b, e); });
                }
                OverflowSolve(ctx, h, /*useBias=*/false);
```
Restitution:
```cpp
            for (auto& batches : m_colorBatches)
            {
                ctx.executor->ParallelFor(batches.size(), kSolverColorGrain,
                    [&](std::size_t b, std::size_t e, std::uint32_t)
                    { SimdSolve::ApplyRestitution(batches, m_bodyState, threshold, b, e); });
            }
            OverflowRestitution(ctx);
```
Leave `OverflowWarmStart/OverflowSolve/OverflowRestitution`, the color loop sequencing, `StoreImpulses`, and the integrate calls UNCHANGED. (`ctx.executor` is guaranteed non-null by Task 1's `Executor()` resolution.)

- [ ] **Step 4: Build + run to verify invariance holds under real parallelism**

BUILD-DEBUG, TEST `"[physics],[determinism],[simd],[solvermt]"`. Expected: ALL PASS, byte-identical (`a == c` with N threads). If `a != c`, the within-color disjointness assumption is violated somewhere — STOP and report (do not loosen the assertion). Re-baseline NOTHING.

- [ ] **Step 5: Commit**
```powershell
git add Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp Arcane/Tests/src/SolverMtInvarianceTest.cpp
git commit -m @'
feat(arcane/physics): parallelize the per-color solver passes (within-color MT)

Serial color loop, ParallelFor over each color's disjoint-body batches for
warm-start/solve/relax/restitution; overflow + color sequencing stay serial.
[determinism] proves serial == enki(1) == enki(N) byte-identical.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 4: Parallelize the per-awake-body integrate loops

**Files:** `SoftStep.cpp`.

The per-body loops (`IntegrateVelocitiesSoA`, `IntegratePositionsSoA`, `FinalizePositionsSoA`) iterate `w.ForEachAwake([&](s){…})` over `AwakeBodies()` (per-body disjoint writes). Convert each to `ParallelFor` over the indexable `w.AwakeBodies()`.

- [ ] **Step 1: Parallelize the three SoA integrate loops**

Add a body-grain constant near `kSolverColorGrain`:
```cpp
        static constexpr std::size_t kSolverBodyGrain = 256;
```
For EACH of `IntegrateVelocitiesSoA`, `IntegratePositionsSoA`, `FinalizePositionsSoA` in `SoftStep.cpp`, replace the `w.ForEachAwake([&](std::uint32_t s){ … })` with a `ParallelFor` over the awake vector (the loop body is unchanged — same `s`, same per-body math). Pattern (shown for `IntegrateVelocitiesSoA`):
```cpp
            const std::vector<std::uint32_t>& aw = w.AwakeBodies();
            ctx.executor->ParallelFor(aw.size(), kSolverBodyGrain,
                [&](std::size_t bgn, std::size_t end, std::uint32_t)
                {
                    for (std::size_t j = bgn; j < end; ++j)
                    {
                        const std::uint32_t s = aw[j];
                        /* ...the EXACT existing per-body body, using `s`, unchanged... */
                    }
                });
```
Apply the identical wrapper to all three methods. Do NOT touch the world<->SoA `SyncInCompacted`/`SyncOutCompacted` (they live in `BodyStateSoA` without executor access — out of scope for D1; they're a small fraction). Per-body writes are disjoint (each awake slot is distinct), so this is byte-identical regardless of thread count.

- [ ] **Step 2: Build + run to verify invariance still holds**

BUILD-DEBUG, TEST `"[physics],[determinism],[solvermt]"`. Expected: ALL PASS, still byte-identical. If `a != c`, STOP and report.

- [ ] **Step 3: Commit**
```powershell
git add Arcane/Core/src/Arcane/Physics/Solver/SoftStep.cpp
git commit -m @'
feat(arcane/physics): parallelize the per-awake-body solver integrate loops

IntegrateVelocitiesSoA / IntegratePositionsSoA / FinalizePositionsSoA become
ParallelFor over AwakeBodies() (per-body disjoint writes). Byte-identical at
any thread count. Lifts the serial-fraction ceiling on solver scaling.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 5: Gates — full correctness suites + the scaling measurement (the deliverable)

**No code commits** (the scaling instrumentation is throwaway, reverted). This task DECIDES whether D1 succeeded.

- [ ] **Step 1: Full correctness gate (both configs, both backends)**
```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane\Arcane.slnx -t:Rebuild -p:Configuration=Release -m
Push-Location Arcane\bin\Release-windows-x86_64-md\ArcaneTests; .\ArcaneTests.exe; Pop-Location
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane\Arcane.slnx -p:Configuration=Debug -m
Push-Location Arcane\bin\Debug-windows-x86_64-md\ArcaneTests; .\ArcaneTests.exe; Pop-Location
```
Expected: all green incl. `[physics]`/`[determinism]`/`[simd]`/`[solvermt]`/`[gpu]` (D3D12+Vulkan). A clean `-t:Rebuild` Release is the stale-obj guard.

- [ ] **Step 2: ArcaneCore static-CRT (the solver compiles under /MT)**
```powershell
Push-Location Server; & "..\ThirdParty\premake5\premake5.exe" vs2026; Pop-Location
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Server\Aphelyon.slnx -p:Configuration=Release -m -t:ArcaneCore
```
Expected: clean (PhysicsWorld now holds a `SerialTaskExecutor` member + includes the Core Jobs header — must build static-CRT).

- [ ] **Step 3: The SCALING measurement (THROWAWAY instrumentation — the D1 deliverable)**

Measure the `solve` stage at serial vs N threads on scene 8 (reuse the STEPPROF approach):
1. Temporarily set `ARCANE_STEPPROF 1` in `Arcane/Core/src/Arcane/Physics/StepProf.hpp` and add the warmup-gated `Reset()@step180`/`Dump()@step480` in `PhysicsWorld::Step` (as in the profiling run).
2. To get the **serial baseline**, temporarily make the Sandbox wiring `world.SetExecutor(nullptr)` (forces the serial default); BUILD-RELEASE; run `ARCANE_SANDBOX_SCENE=8 Loom.exe --backend dx12 --no-vsync --frames 540`; record the `solve` ms.
3. For the **N-thread** number, restore `world.SetExecutor(ctx->taskExecutor)`; BUILD-RELEASE; run the same; record the `solve` ms (and note `WorkerCount`).
4. **REVERT all instrumentation** (`git restore` the StepProf.hpp + PhysicsWorld.cpp + SandboxApp.cpp edits); confirm `git status` clean.

Report: `solve` ms serial vs N-thread, the **speedup** (serial/N) and per-core efficiency, and the new total Step ms. **D1 PASSES iff `solve` shows a measurable speedup** (target: meaningfully > 1.0×; near-linear is the aspiration, but the SoftStep gather pattern may cap it). If it does NOT scale, the deliverable is the **diagnosis** (grain too coarse → raise/lower `kSolverColorGrain`; gather/scatter memory-bandwidth-bound; color sizes too small at 10k so most colors run inline; false sharing on `m_bodyState`), recorded for the follow-up — NOT a green check on an unscaled solver.

- [ ] **Step 4: Headless Loom smoke**
```powershell
Push-Location Arcane\bin\Debug-windows-x86_64-md\Loom; .\Loom.exe --backend dx12 --frames 30; "exit $LASTEXITCODE"; Pop-Location
```
Expected: exit 0, no validation errors (the parallel solver runs live under the real enki executor).

- [ ] **Step 5: Record results** (no commit) — the per-stage numbers + the scaling verdict feed the holistic review + the project memory.

---

## Notes for the executor

- **Determinism scope:** within-color parallel == serial *by construction* (the coloring invariant gives disjoint body writes per color). NO physics-invariant re-baseline — the byte-identical `a == c` assertion in Task 3/4 is the proof, and the existing `[determinism]`/`[simd]` suites must stay green untouched.
- **`ctx.executor` is always non-null** in the solver (Task 1's `Executor()` resolves the serial fallback) — no null-checks in the hot loop.
- **Grain tuning** (`kSolverColorGrain`=8, `kSolverBodyGrain`=256) is the one knob; Task 5's scaling measurement informs whether to adjust.
- **If Task 5 reveals a memory-bandwidth wall**, that is a documented finding (candidate follow-up: SoA/cache-blocking rework), per the spec §8 — not a D1 failure to hide.
