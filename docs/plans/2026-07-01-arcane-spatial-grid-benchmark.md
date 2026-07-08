# Arcane SpatialGrid Benchmark Spike Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a disposable, Core-only, CPU-only benchmark that measures the SpatialGrid-vs-DynamicAABBTree crossover (broadphase, statics, and query-index) so the SpatialGrid first-class feature (Cycle 2) is designed from data, not assumptions.

**Architecture:** A new standalone `Arcane/Bench/` `ConsoleApp` that links Core only (plus enkiTS) — no Arcane.dll / SDL / NVRHI / Astra. It builds scenes directly through the `PhysicsWorld` API, sweeps `WorldDef.broadphase` × scene × N × executor, and runs statics/query/residency micro-benches. Correctness gates (run-to-run determinism, ST==MT parity, query-oracle equivalence) run via a built-in `--selfcheck` mode; the full run emits CSV + a summary and drives a hand-authored findings doc.

**Tech Stack:** C++ (workspace dialect, /MD), premake5 (vs2026), Core `PhysicsWorld` / `SpatialGrid` / `DynamicTree` / `ITaskExecutor`, enkiTS (`enki::TaskScheduler`), chrono.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-01-arcane-spatial-grid-benchmark-design.md`. This plan implements that spec; every task inherits its constraints.
- **Disposable:** the whole thing is one deletable unit — the `Arcane/Bench/` directory + the `project "Bench"` block in `Arcane/premake5.lua`. No changes to permanent projects (ArcaneTests, Loom, Sandbox, Core, Arcane). No Tracy. No profiling seam added to Core.
- **Core-only:** `links { "Core", "enkiTS" }` (+ `ws2_32` only if the linker demands it). Never link `Arcane`, SDL, NVRHI, or Astra. No window, no GPU, no shader compile, no data copy.
- **/MD (dynamic CRT)** and workspace cppdialect — copy these from the `project "Loom"` block in `Arcane/premake5.lua` (Loom is the closest analog: a Core-linking ConsoleApp). No `/fp:fast` (determinism rule). UTF-8 without BOM, ASCII-only comments.
- **Additive / byte-identical:** the existing `[physics]` suite must stay green and byte-identical. This spike touches no sim code.
- **Build (PowerShell, not Git Bash):** `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /t:Bench /p:Configuration=Debug /m` (full-solution build without `/t:Bench` also works).
- **Regenerate projects** after editing premake: `ThirdParty\premake5\premake5.exe vs2026` run from `Arcane\` (NOT `GenerateProjects.bat` — it hangs on `pause`).
- **Bench exe path (Debug):** `Arcane\bin\Debug-windows-x86_64-md\Bench\Bench.exe`.
- **clangd diagnostics are FALSE POSITIVES** — MSVC via msbuild is the source of truth.
- **Staging:** stage ONLY bench files + `Arcane/premake5.lua` by explicit path. NEVER `git add -A` — the working tree has 10 unrelated parked changes (Client ui_screens, AGENTS.md, Arcane/.screenshots/, Server/cpp_coding_style.txt, two 2026-06-24 docs). Branch is `feature/arcane-spatial-grid-benchmark`.
- **Commit trailers** (every commit):
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux
  ```

## Determinism policy (applies to Tasks 3, 4, 6)

Three distinct notions — do not conflate:

1. **Run-to-run determinism (HARD ASSERT):** same broadphase + same seed, two constructions, K steps → identical state hash. Guards bench reproducibility.
2. **ST==MT parity (HARD ASSERT):** same broadphase, `SerialTaskExecutor` vs the bench enki executor → identical state hash after K steps. This is the engine's MT invariant.
3. **Cross-broadphase equivalence (MEASURE + REPORT, do NOT hard-assert):** Tree vs Hash vs Sap may legitimately diverge (different fat-AABB margins → different speculative pairs). Report the max per-body position delta after K steps as a finding; a large delta is data about broadphase equivalence, not a bench failure. If it is suspiciously large (bodies flying apart), STOP and investigate — it may be a real latent bug — but a small drift is expected and reported, not asserted away.

## File Structure

All under `Arcane/Bench/src/` unless noted. Reusable logic is header-only; `main.cpp` orchestrates.

| File | Responsibility |
|---|---|
| `Arcane/premake5.lua` (modify) | add `project "Bench"` |
| `BenchCheck.hpp` | tiny check/assert helper + registry for `--selfcheck` |
| `BenchTiming.hpp` | `Stopwatch`, `TimeAveraged` (warmup + mean/min ms) |
| `BenchMetrics.hpp` | `MetricRow` struct + `WriteCsv(ostream, rows)` |
| `BenchStateHash.hpp` | `HashWorldState(const PhysicsWorld&)` FNV-1a |
| `BenchEnkiExecutor.hpp` | bench-local `ITaskExecutor` over `enki::TaskScheduler` |
| `BenchScenes.hpp` | `SceneParams`, scene ids, `Populate(...)`, `SceneName(...)` |
| `BenchCells.hpp` | cell<->AABB helpers shared by the query micro-bench |
| `BenchDriver.hpp` / `.cpp` | the config-sweep + micro-benches (whole-Step, pair-find, statics, query, residency) |
| `main.cpp` | arg parse (`--selfcheck`, `--smoke`, `--out <dir>`), run selfcheck or full matrix, emit CSV + summary |

---

### Task 1: Scaffold the Bench project + selfcheck skeleton

**Files:**
- Modify: `Arcane/premake5.lua` (add `project "Bench"` near the `project "Loom"` block)
- Create: `Arcane/Bench/src/BenchCheck.hpp`
- Create: `Arcane/Bench/src/main.cpp`

**Interfaces:**
- Produces: `Bench::Check(bool ok, const char* name)` recording a pass/fail; `Bench::RunSelfCheck()` returning the failure count; `main` dispatching `--selfcheck`.

- [ ] **Step 1: Add the premake project.** Open `Arcane/premake5.lua`, copy the `project "Loom"` block as a template, and add this AFTER it. Match Loom's `language`/`cppdialect`/`staticruntime`/`systemversion`/`defines`/`filter` config blocks exactly (copy them verbatim from Loom); only the identity, kind, files, includedirs, and links differ:

```lua
project "Bench"
    location "Bench"
    kind "ConsoleApp"
    -- copy language / cppdialect / staticruntime "Off" (/MD) / systemversion
    -- and the per-config (Debug/Release/Dist) filter blocks verbatim from Loom
    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files { "%{prj.location}/src/**.cpp", "%{prj.location}/src/**.hpp" }

    includedirs {
        "%{prj.location}/src",
        "%{IncludeDir.Core}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.enkiTS}",
    }

    links { "Core", "enkiTS" }

    filter "system:windows"
        links { "ws2_32" }   -- Core TcpSocket TU may be pulled in transitively
    filter {}
```

- [ ] **Step 2: Create the check helper** `Arcane/Bench/src/BenchCheck.hpp`:

```cpp
#pragma once
// Minimal check harness for the disposable bench's --selfcheck mode.
#include <cstdio>
#include <vector>
#include <string>

namespace Bench
{
    struct CheckState { int passed = 0; int failed = 0; };
    inline CheckState& Checks() { static CheckState s; return s; }

    inline bool Check(bool ok, const std::string& name)
    {
        if (ok) { ++Checks().passed; std::printf("[ ok ] %s\n", name.c_str()); }
        else    { ++Checks().failed; std::printf("[FAIL] %s\n", name.c_str()); }
        return ok;
    }

    // Each Task appends its checks here (declared in headers, called from RunSelfCheck).
    int RunSelfCheck(); // defined in main.cpp; calls every task's check function.
}
```

- [ ] **Step 3: Create `Arcane/Bench/src/main.cpp`** with arg parse + a trivial first check (constructs a `PhysicsWorld`, steps it once):

```cpp
#include <cstring>
#include <memory>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include "BenchCheck.hpp"

namespace Bench
{
    // Task 1 smoke check: the world builds, adds a body, and steps without crashing.
    static void CheckWorldSmoke()
    {
        using namespace Arcane::Physics;
        PhysicsWorld w;
        BodyDef d; d.type = BodyType::Dynamic; d.shape = MakeCircle(Arcane::Physics::Real(4));
        d.position = Arcane::Physics::Vec2(0, 0);
        (void)w.AddBody(d);
        w.Step(Arcane::Physics::Real(1) / Arcane::Physics::Real(60));
        Check(w.Count() == 1, "world smoke: one body after AddBody+Step");
    }

    int RunSelfCheck()
    {
        CheckWorldSmoke();
        // (later tasks add their check calls here)
        std::printf("\nselfcheck: %d passed, %d failed\n", Checks().passed, Checks().failed);
        return Checks().failed;
    }
}

int main(int argc, char** argv)
{
    bool selfcheck = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--selfcheck") == 0) selfcheck = true;

    if (selfcheck)
        return Bench::RunSelfCheck();

    std::printf("Arcane physics bench. Use --selfcheck (matrix run lands in Task 10).\n");
    return 0;
}
```

- [ ] **Step 4: Regenerate + build.** In PowerShell from `Arcane\`:

Run: `ThirdParty\premake5\premake5.exe vs2026`
Then: `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /t:Bench /p:Configuration=Debug /m`
Expected: build succeeds; `Arcane\bin\Debug-windows-x86_64-md\Bench\Bench.exe` exists.

- [ ] **Step 5: Run the selfcheck**

Run: `Arcane\bin\Debug-windows-x86_64-md\Bench\Bench.exe --selfcheck`
Expected: `[ ok ] world smoke: one body after AddBody+Step` and `selfcheck: 1 passed, 0 failed`, exit code 0.

- [ ] **Step 6: Commit**

```bash
git add Arcane/premake5.lua Arcane/Bench/src/BenchCheck.hpp Arcane/Bench/src/main.cpp
git commit -m "feat(arcane/bench): scaffold Core-only physics bench + selfcheck skeleton

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux"
```

---

### Task 2: Timing + metric row + CSV writer

**Files:**
- Create: `Arcane/Bench/src/BenchTiming.hpp`
- Create: `Arcane/Bench/src/BenchMetrics.hpp`
- Modify: `Arcane/Bench/src/main.cpp` (register the check)

**Interfaces:**
- Consumes: `Bench::Check`.
- Produces:
  - `double Bench::TimeAveraged(int warmup, int iters, Fn&& fn)` — runs `fn` `warmup` times untimed, then `iters` timed; returns **mean ms per call**.
  - `struct Bench::MetricRow { std::string bench, config, scene; int n; std::string executor; double stepMs, pairMs; long long pairs; double queryNs; long long bytes; };`
  - `void Bench::WriteCsv(std::ostream&, const std::vector<MetricRow>&)` — header + one line per row.

- [ ] **Step 1: Write the failing checks.** Add to `main.cpp` a new `CheckTimingAndCsv()` and call it from `RunSelfCheck()`:

```cpp
#include <sstream>
#include "BenchTiming.hpp"
#include "BenchMetrics.hpp"

static void CheckTimingAndCsv()
{
    // TimeAveraged returns a positive mean for a non-trivial body.
    volatile double sink = 0.0;
    double ms = Bench::TimeAveraged(2, 50, [&]{ for (int i=0;i<10000;++i) sink += i; });
    Bench::Check(ms >= 0.0, "TimeAveraged returns non-negative mean ms");

    // CSV: header + N rows, deterministic column order.
    std::vector<Bench::MetricRow> rows{
        { "step", "Tree", "uniform-dense", 100, "serial", 1.5, 0.3, 240, 0.0, 4096 } };
    std::ostringstream os; Bench::WriteCsv(os, rows);
    const std::string s = os.str();
    Bench::Check(s.find("bench,config,scene,n,executor,step_ms,pair_ms,pairs,query_ns,bytes") == 0,
                 "CSV header is the canonical column order");
    Bench::Check(s.find("step,Tree,uniform-dense,100,serial,") != std::string::npos,
                 "CSV row renders in column order");
}
```

- [ ] **Step 2: Build + run to verify it fails**

Run: build (`/t:Bench`) then `Bench.exe --selfcheck`
Expected: compile error (headers missing) — that is the red state. Create the headers next.

- [ ] **Step 3: Implement `BenchTiming.hpp`:**

```cpp
#pragma once
#include <chrono>
#include <utility>

namespace Bench
{
    struct Stopwatch
    {
        using Clock = std::chrono::steady_clock;
        Clock::time_point t0 = Clock::now();
        void reset() { t0 = Clock::now(); }
        double ms() const
        {
            return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        }
    };

    // Run fn `warmup` times untimed, then `iters` times timed; mean ms per call.
    template <class Fn>
    double TimeAveraged(int warmup, int iters, Fn&& fn)
    {
        for (int i = 0; i < warmup; ++i) fn();
        if (iters <= 0) return 0.0;
        Stopwatch sw;
        for (int i = 0; i < iters; ++i) fn();
        return sw.ms() / static_cast<double>(iters);
    }
}
```

- [ ] **Step 4: Implement `BenchMetrics.hpp`:**

```cpp
#pragma once
#include <ostream>
#include <string>
#include <vector>

namespace Bench
{
    struct MetricRow
    {
        std::string bench;      // "step" | "pairs" | "statics" | "query" | "residency"
        std::string config;     // e.g. "Tree" / "Hash" / "grid" / "tree"
        std::string scene;      // scene name
        int         n = 0;      // body count
        std::string executor;   // "serial" | "enki"
        double      stepMs = 0.0;
        double      pairMs = 0.0;
        long long   pairs  = 0;
        double      queryNs = 0.0;
        long long   bytes  = 0;
    };

    inline void WriteCsv(std::ostream& os, const std::vector<MetricRow>& rows)
    {
        os << "bench,config,scene,n,executor,step_ms,pair_ms,pairs,query_ns,bytes\n";
        for (const auto& r : rows)
            os << r.bench << ',' << r.config << ',' << r.scene << ',' << r.n << ','
               << r.executor << ',' << r.stepMs << ',' << r.pairMs << ',' << r.pairs << ','
               << r.queryNs << ',' << r.bytes << '\n';
    }
}
```

- [ ] **Step 5: Build + run to verify PASS**

Run: build then `Bench.exe --selfcheck`
Expected: the 4 new checks report `[ ok ]`, total failures 0.

- [ ] **Step 6: Commit**

```bash
git add Arcane/Bench/src/BenchTiming.hpp Arcane/Bench/src/BenchMetrics.hpp Arcane/Bench/src/main.cpp
git commit -m "feat(arcane/bench): timing helper + metric row + CSV writer

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux"
```

---

### Task 3: Body-state hash + run-to-run determinism

**Files:**
- Create: `Arcane/Bench/src/BenchStateHash.hpp`
- Modify: `Arcane/Bench/src/main.cpp` (register the check)

**Interfaces:**
- Consumes: `PhysicsWorld::Count()`, `Alive(i)`, `PosSlot(i)`, `AngleSlot(i)`, `VelSlot(i)`, `AngVelSlot(i)`.
- Produces: `std::uint64_t Bench::HashWorldState(const Arcane::Physics::PhysicsWorld&)`.

- [ ] **Step 1: Write the failing check.** Add `CheckStateHash()` to `main.cpp` and call it from `RunSelfCheck()`:

```cpp
#include "BenchStateHash.hpp"

static void CheckStateHash()
{
    using namespace Arcane::Physics;
    auto build = []{
        auto w = std::make_unique<PhysicsWorld>();
        BodyDef d; d.type = BodyType::Dynamic; d.shape = MakeCircle(Real(4));
        d.fixedRotation = true;
        for (int i = 0; i < 8; ++i) { d.position = Vec2(Real(i*10), Real(0)); w->AddBody(d); }
        return w;
    };
    auto a = build(); auto b = build();
    for (int s = 0; s < 20; ++s) { a->Step(Real(1)/Real(60)); b->Step(Real(1)/Real(60)); }
    Bench::Check(Bench::HashWorldState(*a) == Bench::HashWorldState(*b),
                 "state hash: identical builds + steps -> equal hash (run-to-run determinism)");

    auto c = build();
    c->SetVelSlot(0, Vec2(Real(50), Real(0))); // perturb one body before stepping
    for (int s = 0; s < 20; ++s) c->Step(Real(1)/Real(60));
    Bench::Check(Bench::HashWorldState(*c) != Bench::HashWorldState(*a),
                 "state hash: perturbed run -> different hash");
}
```

> The two assertions are what matter: identical builds+steps -> equal hash; a one-body perturbation -> different hash. `SetVelSlot(i, v)` (public, `PhysicsWorld.hpp`) sets slot `i`'s velocity directly, so no `BodyHandle` plumbing is needed.

- [ ] **Step 2: Build + run to verify it fails**

Run: build then `Bench.exe --selfcheck`
Expected: compile error (`BenchStateHash.hpp` missing / `HashWorldState` undefined) — red.

- [ ] **Step 3: Implement `BenchStateHash.hpp`:**

```cpp
#pragma once
// FNV-1a over the SoA (pos + angle + linear/angular velocity) of every alive
// slot. Deterministic fingerprint of world state; used for run-to-run and
// ST==MT determinism gates and cross-broadphase divergence reporting.
#include <cstdint>
#include <cstring>
#include <Arcane/Physics/PhysicsWorld.hpp>

namespace Bench
{
    inline void FnvMix(std::uint64_t& h, double v)
    {
        std::uint64_t bits; std::memcpy(&bits, &v, sizeof(bits));
        for (int byte = 0; byte < 8; ++byte)
        {
            h ^= static_cast<std::uint64_t>((bits >> (byte * 8)) & 0xFF);
            h *= 1099511628211ull; // FNV prime
        }
    }

    inline std::uint64_t HashWorldState(const Arcane::Physics::PhysicsWorld& w)
    {
        std::uint64_t h = 1469598103934665603ull; // FNV offset basis
        const std::uint32_t n = w.Count();
        for (std::uint32_t i = 0; i < n; ++i)
        {
            if (!w.Alive(i)) continue;
            const auto p = w.PosSlot(i);
            const auto v = w.VelSlot(i);
            FnvMix(h, static_cast<double>(p.x));
            FnvMix(h, static_cast<double>(p.y));
            FnvMix(h, static_cast<double>(w.AngleSlot(i)));
            FnvMix(h, static_cast<double>(v.x));
            FnvMix(h, static_cast<double>(v.y));
            FnvMix(h, static_cast<double>(w.AngVelSlot(i)));
        }
        return h;
    }
}
```

- [ ] **Step 4: Build + run to verify PASS**

Run: build then `Bench.exe --selfcheck`
Expected: both state-hash checks `[ ok ]`.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Bench/src/BenchStateHash.hpp Arcane/Bench/src/main.cpp
git commit -m "feat(arcane/bench): body-state FNV hash + run-to-run determinism check

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux"
```

---

### Task 4: Bench-local enki executor + ST==MT parity

**Files:**
- Create: `Arcane/Bench/src/BenchEnkiExecutor.hpp`
- Modify: `Arcane/Bench/src/main.cpp` (register the check)

**Interfaces:**
- Consumes: `Arcane::ITaskExecutor`, `enki::TaskScheduler`, `PhysicsWorld::SetExecutor(ITaskExecutor*)`, `Bench::HashWorldState`.
- Produces: `class Bench::BenchEnkiExecutor : public Arcane::ITaskExecutor` (owns its scheduler; `WorkerCount() > 1` on a multi-core box).

- [ ] **Step 1: Write the failing check.** Add `CheckEnkiParity()` to `main.cpp`, called from `RunSelfCheck()`:

```cpp
#include "BenchEnkiExecutor.hpp"

static void CheckEnkiParity()
{
    using namespace Arcane::Physics;
    auto build = []{
        auto w = std::make_unique<PhysicsWorld>();
        BodyDef d; d.type = BodyType::Dynamic; d.shape = MakeCircle(Real(4));
        for (int i = 0; i < 64; ++i) { d.position = Vec2(Real((i%8)*10), Real((i/8)*10)); w->AddBody(d); }
        return w;
    };
    auto serial = build();
    auto mt     = build();
    Bench::BenchEnkiExecutor exec;
    Bench::Check(exec.WorkerCount() >= 1, "enki executor reports >= 1 worker");
    mt->SetExecutor(&exec);
    for (int s = 0; s < 30; ++s) { serial->Step(Real(1)/Real(60)); mt->Step(Real(1)/Real(60)); }
    Bench::Check(Bench::HashWorldState(*serial) == Bench::HashWorldState(*mt),
                 "ST==MT: serial vs enki executor -> identical state hash");
}
```

- [ ] **Step 2: Build + run to verify it fails**

Run: build then `Bench.exe --selfcheck`
Expected: compile error (`BenchEnkiExecutor` undefined) — red.

- [ ] **Step 3: Implement `BenchEnkiExecutor.hpp`** (mirrors `EnkiTaskExecutor` in `Arcane/Arcane/src/Arcane/Jobs/JobSystem.cpp`, but owns its scheduler so the bench needs no Arcane.dll):

```cpp
#pragma once
#include <cassert>
#include <cstdint>
#include <limits>
#include <TaskScheduler.h>
#include <Arcane/Jobs/TaskExecutor.hpp>

namespace Bench
{
    // Owns an enki scheduler; adapts it to Arcane::ITaskExecutor (worker-index aware).
    class BenchEnkiExecutor final : public Arcane::ITaskExecutor
    {
    public:
        explicit BenchEnkiExecutor(std::uint32_t threads = 0)
        {
            if (threads == 0) m_ts.Initialize();
            else              m_ts.Initialize(threads);
        }
        ~BenchEnkiExecutor() override { m_ts.WaitforAllAndShutdown(); }

        void ParallelFor(std::size_t count, std::size_t minBatch,
                         Arcane::FunctionRef<void(std::size_t, std::size_t, std::uint32_t)> fn) override
        {
            if (count == 0) return;
            assert(count <= static_cast<std::size_t>((std::numeric_limits<uint32_t>::max)()));
            enki::TaskSet task(
                static_cast<uint32_t>(count),
                [&fn](enki::TaskSetPartition range, uint32_t threadnum)
                {
                    fn(range.start, range.end, threadnum);
                });
            task.m_MinRange = static_cast<uint32_t>(minBatch == 0 ? 1 : minBatch);
            m_ts.AddTaskSetToPipe(&task);
            m_ts.WaitforTask(&task); // calling thread participates -> nested-safe
        }

        std::uint32_t WorkerCount() const noexcept override
        {
            return static_cast<std::uint32_t>(m_ts.GetNumTaskThreads());
        }

    private:
        enki::TaskScheduler m_ts;
    };
}
```

- [ ] **Step 4: Build + run to verify PASS**

Run: build then `Bench.exe --selfcheck`
Expected: `enki executor reports >= 1 worker` and `ST==MT ... identical state hash` both `[ ok ]`.

> If ST==MT fails: this is a real determinism finding, not a bench bug to paper over. STOP and report — the engine guarantees ST==MT byte-identity, so a mismatch means the bench is driving the executor wrong (e.g., a per-worker-scratch race) or a genuine engine regression. Investigate before proceeding.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Bench/src/BenchEnkiExecutor.hpp Arcane/Bench/src/main.cpp
git commit -m "feat(arcane/bench): bench-local enki ITaskExecutor + ST==MT parity check

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux"
```

---

### Task 5: Scene generators + invariants

**Files:**
- Create: `Arcane/Bench/src/BenchScenes.hpp`
- Modify: `Arcane/Bench/src/main.cpp` (register the check)

**Interfaces:**
- Consumes: `PhysicsWorld::AddBody`, `MakeAabb`, `MakeCircle`, `BodyDef`.
- Produces:
  - `enum class Bench::Scene { UniformDense, UniformSparse, ClusteredMixed };`
  - `struct Bench::SceneParams { Real cellSize; Real extent; Real staticRatio; Real velScale; int clusterCount; Real clusterSpread; std::uint32_t seed; };`
  - `Bench::SceneParams Bench::DefaultParams(Scene, int n);`
  - `void Bench::Populate(PhysicsWorld&, Scene, int n, const SceneParams&);`
  - `const char* Bench::SceneName(Scene);`

- [ ] **Step 1: Write the failing check.** Add `CheckScenes()` to `main.cpp`, called from `RunSelfCheck()`:

```cpp
#include "BenchScenes.hpp"

static void CheckScenes()
{
    using namespace Arcane::Physics;
    for (Bench::Scene sc : { Bench::Scene::UniformDense, Bench::Scene::UniformSparse, Bench::Scene::ClusteredMixed })
    {
        const int n = 256;
        PhysicsWorld w; Bench::Populate(w, sc, n, Bench::DefaultParams(sc, n));
        Bench::Check(static_cast<int>(w.Count()) == n,
                     std::string("scene populates exactly N: ") + Bench::SceneName(sc));
    }
    // Deterministic: same seed -> same first-body position.
    PhysicsWorld a, b;
    auto p = Bench::DefaultParams(Bench::Scene::ClusteredMixed, 128);
    Bench::Populate(a, Bench::Scene::ClusteredMixed, 128, p);
    Bench::Populate(b, Bench::Scene::ClusteredMixed, 128, p);
    Bench::Check(a.PosSlot(0).x == b.PosSlot(0).x && a.PosSlot(0).y == b.PosSlot(0).y,
                 "scene is deterministic given seed");
    // ClusteredMixed has some statics.
    PhysicsWorld c; Bench::Populate(c, Bench::Scene::ClusteredMixed, 256, Bench::DefaultParams(Bench::Scene::ClusteredMixed, 256));
    int statics = 0; for (std::uint32_t i=0;i<c.Count();++i) if (c.Alive(i) && c.TypeSlot(i)==BodyType::Static) ++statics;
    Bench::Check(statics > 0, "clustered-mixed has static bodies");
}
```

- [ ] **Step 2: Build + run to verify it fails**

Run: build then `Bench.exe --selfcheck`
Expected: compile error (`BenchScenes.hpp` missing) — red.

- [ ] **Step 3: Implement `BenchScenes.hpp`.** Use a local deterministic PRNG (seeded `std::mt19937` — allowed here; it drives *scene construction*, not the sim, and is fully seeded):

```cpp
#pragma once
// Scenes described by spatial-statistics, not game semantics (see spec 6.2).
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Shapes.hpp>

namespace Bench
{
    using namespace Arcane::Physics;

    enum class Scene { UniformDense, UniformSparse, ClusteredMixed };

    inline const char* SceneName(Scene s)
    {
        switch (s) {
            case Scene::UniformDense:   return "uniform-dense";
            case Scene::UniformSparse:  return "uniform-sparse";
            case Scene::ClusteredMixed: return "clustered-mixed";
        }
        return "?";
    }

    struct SceneParams
    {
        Real          cellSize      = Real(32);
        Real          extent        = Real(1000); // half-width of the field
        Real          staticRatio   = Real(0);    // fraction of bodies that are static
        Real          velScale      = Real(40);   // per-body speed magnitude
        int           clusterCount  = 1;
        Real          clusterSpread = Real(60);    // stddev of a cluster blob
        std::uint32_t seed          = 0xC0FFEEu;
    };

    inline SceneParams DefaultParams(Scene s, int n)
    {
        SceneParams p;
        const Real cell = Real(32);
        p.cellSize = cell;
        switch (s)
        {
            case Scene::UniformDense:
                // ~1 body/cell: extent chosen so n cells tile a square field.
                p.extent = cell * Real(0.5) * std::sqrt(Real(n));
                p.staticRatio = Real(0); p.velScale = Real(20);
                p.clusterCount = 1; p.clusterSpread = p.extent;
                break;
            case Scene::UniformSparse:
                // <<1 body/cell: 8x the linear extent of the dense case.
                p.extent = cell * Real(4) * std::sqrt(Real(n));
                p.staticRatio = Real(0); p.velScale = Real(60);
                p.clusterCount = 1; p.clusterSpread = p.extent;
                break;
            case Scene::ClusteredMixed:
                p.extent = cell * Real(2) * std::sqrt(Real(n));
                p.staticRatio = Real(0.25); p.velScale = Real(40);
                p.clusterCount = 4; p.clusterSpread = cell * Real(6);
                break;
        }
        return p;
    }

    inline void Populate(PhysicsWorld& w, Scene s, int n, const SceneParams& p)
    {
        std::mt19937 rng(p.seed);
        std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        std::normal_distribution<float>       gauss(0.0f, static_cast<float>(p.clusterSpread));
        std::uniform_real_distribution<float> ang(0.0f, 6.2831853f);

        // Cluster centers (ClusteredMixed); a single centered blob otherwise.
        std::vector<Vec2> centers;
        for (int k = 0; k < p.clusterCount; ++k)
            centers.push_back(Vec2(Real(u(rng)) * p.extent, Real(u(rng)) * p.extent));

        const int nStatic = static_cast<int>(Real(n) * p.staticRatio);
        for (int i = 0; i < n; ++i)
        {
            Vec2 pos;
            if (s == Scene::ClusteredMixed)
            {
                const Vec2 c = centers[static_cast<std::size_t>(i) % centers.size()];
                pos = Vec2(c.x + Real(gauss(rng)), c.y + Real(gauss(rng)));
            }
            else
            {
                pos = Vec2(Real(u(rng)) * p.extent, Real(u(rng)) * p.extent);
            }

            BodyDef d;
            const bool isStatic = (i < nStatic);
            d.type = isStatic ? BodyType::Static : BodyType::Dynamic;
            d.position = pos;
            d.shape = (i % 2 == 0) ? MakeCircle(Real(4)) : MakeAabb(Real(4), Real(4));
            d.fixedRotation = true; // keep motion translational + comparable
            BodyHandle h = w.AddBody(d);
            if (!isStatic)
            {
                const Real a = Real(ang(rng));
                w.SetVelocity(h, Vec2(std::cos(a) * p.velScale, std::sin(a) * p.velScale));
            }
        }
    }
}
```

> The exact `extent`/`spread`/`velScale` numbers are first-draft; Task 11 records the final values in the findings doc. The invariants the check enforces (exact N, determinism, some statics) are what must hold.
>
> **Dispersal caveat:** the dynamic bodies have velocity but the scenes have no boundary walls, so density drifts over a long timed run. Task 11 verifies the drift stays small over the timed window. If it does not (e.g. at 10k the in-`extent` count falls materially), add 4 static boundary walls sized to `extent` in `Populate` so density stays stable during timing — and update this task's count invariant from `n` to `n + 4`.

- [ ] **Step 4: Build + run to verify PASS**

Run: build then `Bench.exe --selfcheck`
Expected: all scene checks `[ ok ]`.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Bench/src/BenchScenes.hpp Arcane/Bench/src/main.cpp
git commit -m "feat(arcane/bench): spatial-statistics scene generators + invariants

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux"
```

---

### Task 6: Config-sweep driver — whole-Step, pair-find, cross-broadphase divergence

**Files:**
- Create: `Arcane/Bench/src/BenchDriver.hpp`
- Create: `Arcane/Bench/src/BenchDriver.cpp`
- Modify: `Arcane/Bench/src/main.cpp` (register the check)

**Interfaces:**
- Consumes: `WorldDef{ broadphase }`, `BroadphaseKind::{Tree,Hash,Sap}`, `Populate`, `HashWorldState`, `BenchEnkiExecutor`, `FixtureBroadphase()`, `IBroadphase::UpdatePairs`, `TimeAveraged`, `MetricRow`.
- Produces (in `BenchDriver.hpp`):
  - `std::vector<MetricRow> Bench::RunStepSweep(const std::vector<int>& counts, int steps, int warmup, int iters, bool withEnki);`
  - `double Bench::CrossBroadphaseMaxPosDelta(Scene, int n, int steps);` — max per-body |pos| delta between Tree and Hash after `steps`.

- [ ] **Step 1: Write the failing check.** Add `CheckStepSweep()` to `main.cpp`, called from `RunSelfCheck()`:

```cpp
#include "BenchDriver.hpp"

static void CheckStepSweep()
{
    auto rows = Bench::RunStepSweep(/*counts=*/{32}, /*steps=*/5, /*warmup=*/1, /*iters=*/2, /*withEnki=*/false);
    // 3 broadphases x 3 scenes x 1 count = 9 step rows + 9 pair rows.
    int stepRows = 0, pairRows = 0;
    for (auto& r : rows) { if (r.bench=="step") ++stepRows; if (r.bench=="pairs") ++pairRows; }
    Bench::Check(stepRows == 9 && pairRows == 9, "step sweep yields 9 step + 9 pair rows at N=32");
    for (auto& r : rows) if (r.bench=="step") Bench::Check(r.stepMs >= 0.0, "step ms non-negative");

    // Cross-broadphase divergence is measured, not asserted small; just sane + finite.
    double d = Bench::CrossBroadphaseMaxPosDelta(Bench::Scene::UniformDense, 64, 10);
    Bench::Check(d >= 0.0 && d < 1e6, "cross-broadphase pos delta is finite and reported");
}
```

- [ ] **Step 2: Build + run to verify it fails**

Run: build then `Bench.exe --selfcheck`
Expected: compile error (`BenchDriver.hpp` missing) — red.

- [ ] **Step 3: Implement `BenchDriver.hpp`** (declarations):

```cpp
#pragma once
#include <vector>
#include "BenchMetrics.hpp"
#include "BenchScenes.hpp"

namespace Bench
{
    std::vector<MetricRow> RunStepSweep(const std::vector<int>& counts, int steps,
                                        int warmup, int iters, bool withEnki);
    double CrossBroadphaseMaxPosDelta(Scene scene, int n, int steps);
}
```

- [ ] **Step 4: Implement `BenchDriver.cpp`:**

```cpp
#include "BenchDriver.hpp"
#include "BenchTiming.hpp"
#include "BenchStateHash.hpp"
#include "BenchEnkiExecutor.hpp"
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp>
#include <algorithm>
#include <cmath>
#include <memory>

namespace Bench
{
    using namespace Arcane::Physics;

    namespace
    {
        struct BpEntry { BroadphaseKind kind; const char* name; };
        const BpEntry kBroadphases[] = {
            { BroadphaseKind::Tree, "Tree" },
            { BroadphaseKind::Hash, "Hash" },
            { BroadphaseKind::Sap,  "Sap"  },
        };
        const Scene kScenes[] = { Scene::UniformDense, Scene::UniformSparse, Scene::ClusteredMixed };

        std::unique_ptr<PhysicsWorld> MakeWorld(BroadphaseKind k, Scene sc, int n)
        {
            WorldDef def; def.broadphase = k;
            auto w = std::make_unique<PhysicsWorld>(def);
            Populate(*w, sc, n, DefaultParams(sc, n));
            return w;
        }
    }

    std::vector<MetricRow> RunStepSweep(const std::vector<int>& counts, int steps,
                                        int warmup, int iters, bool withEnki)
    {
        std::vector<MetricRow> rows;
        const Real dt = Real(1) / Real(60);
        for (int n : counts)
            for (const auto& bp : kBroadphases)
                for (Scene sc : kScenes)
                {
                    // ---- whole-Step ms (serial) ----
                    {
                        auto w = MakeWorld(bp.kind, sc, n);
                        for (int i = 0; i < warmup; ++i) w->Step(dt);
                        Stopwatch sw;
                        for (int i = 0; i < iters * steps; ++i) w->Step(dt);
                        const double ms = sw.ms() / static_cast<double>(iters * steps);
                        rows.push_back({ "step", bp.name, SceneName(sc), n, "serial", ms, 0, 0, 0, 0 });
                    }
                    // ---- broadphase pair-find ms + #pairs (serial) ----
                    // Use Pairs() (full recompute), NOT UpdatePairs(): after the warmup
                    // Steps the move buffer is already drained (Step drives UpdatePairs
                    // internally), so UpdatePairs() would be a no-op re-emit for the Tree
                    // (incremental) while Hash/Sap fall back to a full Pairs() -- an
                    // apples-to-oranges comparison. Pairs() does the SAME full overlap
                    // detection for all three strategies, so the numbers are comparable.
                    // The Tree's incremental advantage is already captured in the whole-
                    // Step ms above (which drives UpdatePairs internally each step).
                    {
                        auto w = MakeWorld(bp.kind, sc, n);
                        for (int i = 0; i < warmup + 2; ++i) w->Step(dt); // let proxies populate
                        std::vector<BroadphasePair> pairs;
                        IBroadphase& bpx = w->FixtureBroadphase();
                        const double ms = TimeAveraged(2, iters * 4, [&]{ bpx.Pairs(pairs); });
                        rows.push_back({ "pairs", bp.name, SceneName(sc), n, "serial",
                                         0, ms, static_cast<long long>(pairs.size()), 0, 0 });
                    }
                    // ---- optional MT whole-Step (enki) at N >= 1000 ----
                    if (withEnki && n >= 1000)
                    {
                        auto w = MakeWorld(bp.kind, sc, n);
                        BenchEnkiExecutor exec; w->SetExecutor(&exec);
                        for (int i = 0; i < warmup; ++i) w->Step(dt);
                        Stopwatch sw;
                        for (int i = 0; i < iters * steps; ++i) w->Step(dt);
                        const double ms = sw.ms() / static_cast<double>(iters * steps);
                        rows.push_back({ "step", bp.name, SceneName(sc), n, "enki", ms, 0, 0, 0, 0 });
                    }
                }
        return rows;
    }

    double CrossBroadphaseMaxPosDelta(Scene scene, int n, int steps)
    {
        const Real dt = Real(1) / Real(60);
        auto tree = MakeWorld(BroadphaseKind::Tree, scene, n);
        auto hash = MakeWorld(BroadphaseKind::Hash, scene, n);
        for (int i = 0; i < steps; ++i) { tree->Step(dt); hash->Step(dt); }
        double maxd = 0.0;
        const std::uint32_t c = tree->Count();
        for (std::uint32_t i = 0; i < c; ++i)
        {
            if (!tree->Alive(i) || !hash->Alive(i)) continue;
            const auto a = tree->PosSlot(i); const auto b = hash->PosSlot(i);
            const double dx = double(a.x) - double(b.x), dy = double(a.y) - double(b.y);
            maxd = std::max(maxd, std::sqrt(dx*dx + dy*dy));
        }
        return maxd;
    }
}
```

- [ ] **Step 5: Build + run to verify PASS**

Run: build then `Bench.exe --selfcheck`
Expected: `step sweep yields 9 step + 9 pair rows`, `step ms non-negative`, and `cross-broadphase pos delta is finite` all `[ ok ]`.

- [ ] **Step 6: Commit**

```bash
git add Arcane/Bench/src/BenchDriver.hpp Arcane/Bench/src/BenchDriver.cpp Arcane/Bench/src/main.cpp
git commit -m "feat(arcane/bench): config-sweep driver (whole-Step + pair-find + cross-bp delta)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux"
```

---

### Task 7: Statics structure micro-bench (grid vs bench-local tree)

**Files:**
- Create: `Arcane/Bench/src/BenchStatics.hpp`
- Modify: `Arcane/Bench/src/main.cpp` (register the check)

**Interfaces:**
- Consumes: `SpatialGrid`, `DynamicTree`, `AabbOverlap`, `SlotAabb`, `TypeSlot`.
- Produces:
  - `std::vector<Bench::MetricRow> Bench::RunStaticsBench(const std::vector<int>& counts, int queries);`
  - internal oracle: grid and tree return the SAME tight-filtered candidate set.

- [ ] **Step 1: Write the failing check.** Add `CheckStatics()` to `main.cpp`:

```cpp
#include "BenchStatics.hpp"

static void CheckStatics()
{
    Bench::Check(Bench::StaticsOracleEquivalent(128, 40),
                 "statics: grid QueryAABB == tree QueryAABB (tight-filtered) on same set");
    auto rows = Bench::RunStaticsBench({128}, 40);
    bool grid=false, tree=false;
    for (auto& r : rows) { if (r.config=="grid") grid=true; if (r.config=="tree") tree=true; }
    Bench::Check(grid && tree, "statics bench emits grid + tree rows");
}
```

- [ ] **Step 2: Build + run to verify it fails** — compile error (`BenchStatics.hpp` missing), red.

- [ ] **Step 3: Implement `BenchStatics.hpp`.** Build a static AABB set from a `ClusteredMixed` scene's statics, then compare `SpatialGrid` vs a bench-local `DynamicTree`:

```cpp
#pragma once
#include <vector>
#include <random>
#include <algorithm>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Broadphase/SpatialGrid.hpp>
#include <Arcane/Physics/Broadphase/DynamicTree.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp> // AabbOverlap
#include "BenchScenes.hpp"
#include "BenchTiming.hpp"
#include "BenchMetrics.hpp"

namespace Bench
{
    using namespace Arcane::Physics;

    // Gather the world's static-body tight AABBs (id -> box).
    inline void GatherStatics(const PhysicsWorld& w, std::vector<std::uint32_t>& ids,
                              std::vector<Aabb2>& boxes)
    {
        ids.clear(); boxes.clear();
        for (std::uint32_t i = 0; i < w.Count(); ++i)
            if (w.Alive(i) && w.TypeSlot(i) == BodyType::Static)
            { ids.push_back(i); boxes.push_back(w.SlotAabb(i)); }
    }

    inline std::vector<Aabb2> QueryStream(const SceneParams& p, int queries)
    {
        std::mt19937 rng(p.seed ^ 0x51A71C5u);
        std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        std::vector<Aabb2> qs;
        for (int i = 0; i < queries; ++i)
        {
            const float cx = u(rng) * float(p.extent), cy = u(rng) * float(p.extent);
            const float h = float(p.cellSize) * 1.5f;
            Aabb2 q; q.min = Vec2(Real(cx-h), Real(cy-h)); q.max = Vec2(Real(cx+h), Real(cy+h));
            qs.push_back(q);
        }
        return qs;
    }

    inline std::vector<std::uint32_t> TightFilter(const std::vector<Aabb2>& boxes,
                                                  const std::vector<std::uint32_t>& cand, const Aabb2& q)
    {
        std::vector<std::uint32_t> out;
        for (std::uint32_t id : cand) if (AabbOverlap(boxes[id], q)) out.push_back(id);
        std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // Oracle: grid and tree return the identical tight-filtered set for every query.
    inline bool StaticsOracleEquivalent(int n, int queries)
    {
        PhysicsWorld w; Populate(w, Scene::ClusteredMixed, n, DefaultParams(Scene::ClusteredMixed, n));
        std::vector<std::uint32_t> ids; std::vector<Aabb2> boxes; GatherStatics(w, ids, boxes);
        // boxes indexed by slot id -> allocate a slot-indexed lookup for the filter.
        std::vector<Aabb2> byId(w.Count());
        for (std::size_t k = 0; k < ids.size(); ++k) byId[ids[k]] = boxes[k];

        SpatialGrid grid(DefaultParams(Scene::ClusteredMixed, n).cellSize);
        DynamicTree tree;
        for (std::size_t k = 0; k < ids.size(); ++k) { grid.Insert(ids[k], boxes[k]); tree.Update(ids[k], boxes[k]); }

        for (const Aabb2& q : QueryStream(DefaultParams(Scene::ClusteredMixed, n), queries))
        {
            std::vector<std::uint32_t> gc, tc;
            grid.QueryAABB(q, gc); tree.QueryAABB(q, tc);
            if (TightFilter(byId, gc, q) != TightFilter(byId, tc, q)) return false;
        }
        return true;
    }

    inline std::vector<MetricRow> RunStaticsBench(const std::vector<int>& counts, int queries)
    {
        std::vector<MetricRow> rows;
        for (int n : counts)
        {
            const auto pr = DefaultParams(Scene::ClusteredMixed, n);
            PhysicsWorld w; Populate(w, Scene::ClusteredMixed, n, pr);
            std::vector<std::uint32_t> ids; std::vector<Aabb2> boxes; GatherStatics(w, ids, boxes);
            const auto qs = QueryStream(pr, queries);

            SpatialGrid grid(pr.cellSize); DynamicTree tree;
            for (std::size_t k = 0; k < ids.size(); ++k) { grid.Insert(ids[k], boxes[k]); tree.Update(ids[k], boxes[k]); }

            std::vector<std::uint32_t> scratch;
            const double gms = TimeAveraged(2, 20, [&]{ for (auto& q : qs) grid.QueryAABB(q, scratch); })
                               / std::max<std::size_t>(1, qs.size()) * 1e6; // ns/query
            const double tms = TimeAveraged(2, 20, [&]{ for (auto& q : qs) tree.QueryAABB(q, scratch); })
                               / std::max<std::size_t>(1, qs.size()) * 1e6;
            rows.push_back({ "statics", "grid", SceneName(Scene::ClusteredMixed), n, "serial", 0,0,0, gms, 0 });
            rows.push_back({ "statics", "tree", SceneName(Scene::ClusteredMixed), n, "serial", 0,0,0, tms, 0 });
        }
        return rows;
    }
}
```

- [ ] **Step 4: Build + run to verify PASS** — statics oracle + rows checks `[ ok ]`.

> If `StaticsOracleEquivalent` fails: the grid and tree disagree on candidates — investigate (likely a cell-size vs box-size mismatch making the grid miss a cell), do not proceed with a broken oracle.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Bench/src/BenchStatics.hpp Arcane/Bench/src/main.cpp
git commit -m "feat(arcane/bench): statics micro-bench (grid vs bench-local tree) + oracle

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux"
```

---

### Task 8: Query micro-bench (tile / ring / region) + oracle

**Files:**
- Create: `Arcane/Bench/src/BenchCells.hpp`
- Create: `Arcane/Bench/src/BenchQueries.hpp`
- Modify: `Arcane/Bench/src/main.cpp` (register the check)

**Interfaces:**
- Consumes: `SpatialGrid` (residency-style, dynamic bodies), `DynamicTree`, `AabbOverlap`.
- Produces:
  - `Aabb2 Bench::CellBox(Real cell, Vec2 origin, int cx, int cy);`
  - `Aabb2 Bench::RingBox(Real cell, Vec2 origin, int cx, int cy, int k);`
  - `bool Bench::QueryOracleEquivalent(int n);` — grid vs tree+filter identical for tile/ring/region.
  - `std::vector<Bench::MetricRow> Bench::RunQueryBench(const std::vector<int>& counts);`

- [ ] **Step 1: Write the failing check.** Add `CheckQueries()` to `main.cpp`:

```cpp
#include "BenchQueries.hpp"

static void CheckQueries()
{
    Bench::Check(Bench::QueryOracleEquivalent(256),
                 "query: grid tile/ring/region == tree+filter (identical sets)");
    auto rows = Bench::RunQueryBench({256});
    bool tile=false, ring=false, region=false;
    for (auto& r : rows) { if (r.config=="grid-tile") tile=true; if (r.config=="grid-ring") ring=true; if (r.config=="grid-region") region=true; }
    Bench::Check(tile && ring && region, "query bench emits tile/ring/region rows");
}
```

- [ ] **Step 2: Build + run to verify it fails** — compile error, red.

- [ ] **Step 3: Implement `BenchCells.hpp`:**

```cpp
#pragma once
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp> // Aabb2

namespace Bench
{
    using namespace Arcane::Physics;
    // Cell (cx,cy) -> world AABB, matching SpatialGrid's floor((p-origin)/cell) convention.
    inline Aabb2 CellBox(Real cell, Vec2 origin, int cx, int cy)
    {
        Aabb2 b;
        b.min = Vec2(origin.x + Real(cx) * cell,       origin.y + Real(cy) * cell);
        b.max = Vec2(origin.x + Real(cx + 1) * cell,   origin.y + Real(cy + 1) * cell);
        return b;
    }
    // The (2k+1)x(2k+1) block of cells centered on (cx,cy) as one AABB.
    inline Aabb2 RingBox(Real cell, Vec2 origin, int cx, int cy, int k)
    {
        Aabb2 b;
        b.min = Vec2(origin.x + Real(cx - k) * cell,     origin.y + Real(cy - k) * cell);
        b.max = Vec2(origin.x + Real(cx + k + 1) * cell, origin.y + Real(cy + k + 1) * cell);
        return b;
    }
}
```

- [ ] **Step 4: Implement `BenchQueries.hpp`.** Build a residency-style `SpatialGrid` + bench-local `DynamicTree` over the scene's **dynamic** bodies; compare tile/ring/region:

```cpp
#pragma once
#include <vector>
#include <algorithm>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Broadphase/SpatialGrid.hpp>
#include <Arcane/Physics/Broadphase/DynamicTree.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp>
#include "BenchScenes.hpp"
#include "BenchCells.hpp"
#include "BenchTiming.hpp"
#include "BenchMetrics.hpp"

namespace Bench
{
    using namespace Arcane::Physics;

    struct QueryFixture
    {
        SpatialGrid grid; DynamicTree tree;
        std::vector<Aabb2> byId; Real cell; Vec2 origin{Real(0),Real(0)};
        std::vector<Vec2> pts; // sample query centers (body positions)
        explicit QueryFixture(Real c) : grid(c), cell(c) {}
    };

    inline QueryFixture BuildQueryFixture(int n)
    {
        const auto pr = DefaultParams(Scene::ClusteredMixed, n);
        PhysicsWorld w; Populate(w, Scene::ClusteredMixed, n, pr);
        QueryFixture f(pr.cellSize);
        f.byId.assign(w.Count(), Aabb2{});
        for (std::uint32_t i = 0; i < w.Count(); ++i)
        {
            if (!w.Alive(i) || w.TypeSlot(i) != BodyType::Dynamic) continue;
            const Aabb2 box = w.SlotAabb(i);
            f.grid.Insert(i, box); f.tree.Update(i, box); f.byId[i] = box;
            if (f.pts.size() < 64) f.pts.push_back(w.PosSlot(i));
        }
        return f;
    }

    inline std::vector<std::uint32_t> GridQuery(const QueryFixture& f, const Aabb2& box)
    {
        std::vector<std::uint32_t> c; f.grid.QueryAABB(box, c);
        std::vector<std::uint32_t> out;
        for (std::uint32_t id : c) if (AabbOverlap(f.byId[id], box)) out.push_back(id);
        std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
    inline std::vector<std::uint32_t> TreeQuery(const QueryFixture& f, const Aabb2& box)
    {
        std::vector<std::uint32_t> c; f.tree.QueryAABB(box, c);
        std::vector<std::uint32_t> out;
        for (std::uint32_t id : c) if (AabbOverlap(f.byId[id], box)) out.push_back(id);
        std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    inline bool QueryOracleEquivalent(int n)
    {
        QueryFixture f = BuildQueryFixture(n);
        for (const Vec2& p : f.pts)
        {
            int cx, cy; f.grid.CellCoord(p, cx, cy);
            const Aabb2 tile = CellBox(f.cell, f.origin, cx, cy);
            const Aabb2 ring = RingBox(f.cell, f.origin, cx, cy, 2);
            Aabb2 region; region.min = Vec2(p.x - f.cell*Real(4), p.y - f.cell*Real(4));
                          region.max = Vec2(p.x + f.cell*Real(4), p.y + f.cell*Real(4));
            if (GridQuery(f, tile)   != TreeQuery(f, tile))   return false;
            if (GridQuery(f, ring)   != TreeQuery(f, ring))   return false;
            if (GridQuery(f, region) != TreeQuery(f, region)) return false;
        }
        return true;
    }

    inline std::vector<MetricRow> RunQueryBench(const std::vector<int>& counts)
    {
        std::vector<MetricRow> rows;
        for (int n : counts)
        {
            QueryFixture f = BuildQueryFixture(n);
            std::vector<std::uint32_t> scratch;
            auto timeKind = [&](const char* label, auto boxOf)
            {
                const double gridNs = TimeAveraged(2, 50, [&]{
                    for (const Vec2& p : f.pts) { int cx,cy; f.grid.CellCoord(p,cx,cy); f.grid.QueryAABB(boxOf(cx,cy,p), scratch); }
                }) / std::max<std::size_t>(1, f.pts.size()) * 1e6;
                const double treeNs = TimeAveraged(2, 50, [&]{
                    for (const Vec2& p : f.pts) { int cx,cy; f.grid.CellCoord(p,cx,cy); f.tree.QueryAABB(boxOf(cx,cy,p), scratch); }
                }) / std::max<std::size_t>(1, f.pts.size()) * 1e6;
                rows.push_back({ "query", std::string("grid-")+label, "clustered-mixed", n, "serial", 0,0,0, gridNs, 0 });
                rows.push_back({ "query", std::string("tree-")+label, "clustered-mixed", n, "serial", 0,0,0, treeNs, 0 });
            };
            timeKind("tile",   [&](int cx,int cy,Vec2){ return CellBox(f.cell,f.origin,cx,cy); });
            timeKind("ring",   [&](int cx,int cy,Vec2){ return RingBox(f.cell,f.origin,cx,cy,2); });
            timeKind("region", [&](int,int,Vec2 p){ Aabb2 r; r.min=Vec2(p.x-f.cell*Real(4),p.y-f.cell*Real(4)); r.max=Vec2(p.x+f.cell*Real(4),p.y+f.cell*Real(4)); return r; });
        }
        return rows;
    }
}
```

- [ ] **Step 5: Build + run to verify PASS** — query oracle + rows checks `[ ok ]`.

- [ ] **Step 6: Commit**

```bash
git add Arcane/Bench/src/BenchCells.hpp Arcane/Bench/src/BenchQueries.hpp Arcane/Bench/src/main.cpp
git commit -m "feat(arcane/bench): query micro-bench (tile/ring/region) grid vs tree+filter + oracle

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux"
```

---

### Task 9: Residency-upkeep micro-bench

**Files:**
- Create: `Arcane/Bench/src/BenchResidency.hpp`
- Modify: `Arcane/Bench/src/main.cpp` (register the check)

**Interfaces:**
- Consumes: `SpatialGrid::Move`, `PhysicsWorld::Step`, `SlotAabb`, `TypeSlot`.
- Produces: `std::vector<Bench::MetricRow> Bench::RunResidencyBench(const std::vector<int>& counts, int steps);`

Rationale (spec 6.5 refinement): residency upkeep can't be A/B-toggled inside `Step` without a `WorldDef` knob we deliberately excluded, so measure it as a **standalone micro-bench** — replicate per-step residency maintenance on a standalone `SpatialGrid` (Move every mover to its new AABB each step) and time it. This isolates the marginal cost the residency index adds.

- [ ] **Step 1: Write the failing check.** Add `CheckResidency()` to `main.cpp`:

```cpp
#include "BenchResidency.hpp"

static void CheckResidency()
{
    auto rows = Bench::RunResidencyBench({256}, 10);
    Bench::Check(rows.size() == 1 && rows[0].bench == "residency" && rows[0].stepMs >= 0.0,
                 "residency upkeep bench emits one non-negative row");
}
```

- [ ] **Step 2: Build + run to verify it fails** — compile error, red.

- [ ] **Step 3: Implement `BenchResidency.hpp`:**

```cpp
#pragma once
#include <vector>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Broadphase/SpatialGrid.hpp>
#include "BenchScenes.hpp"
#include "BenchTiming.hpp"
#include "BenchMetrics.hpp"

namespace Bench
{
    using namespace Arcane::Physics;

    inline std::vector<MetricRow> RunResidencyBench(const std::vector<int>& counts, int steps)
    {
        std::vector<MetricRow> rows;
        const Real dt = Real(1) / Real(60);
        for (int n : counts)
        {
            const auto pr = DefaultParams(Scene::ClusteredMixed, n);
            PhysicsWorld w; Populate(w, Scene::ClusteredMixed, n, pr);
            SpatialGrid residency(pr.cellSize);
            // seed the grid with current mover boxes
            std::vector<std::uint32_t> movers;
            for (std::uint32_t i = 0; i < w.Count(); ++i)
                if (w.Alive(i) && w.TypeSlot(i) == BodyType::Dynamic)
                { movers.push_back(i); residency.Insert(i, w.SlotAabb(i)); }

            // Time the per-step Move upkeep across `steps` advances of the sim.
            Stopwatch sw; double upkeepMs = 0.0;
            for (int s = 0; s < steps; ++s)
            {
                w.Step(dt);
                Stopwatch mv;
                for (std::uint32_t id : movers) residency.Move(id, w.SlotAabb(id));
                upkeepMs += mv.ms();
            }
            rows.push_back({ "residency", "grid-move", SceneName(Scene::ClusteredMixed), n, "serial",
                             upkeepMs / double(steps), 0, 0, 0, 0 });
        }
        return rows;
    }
}
```

- [ ] **Step 4: Build + run to verify PASS** — residency check `[ ok ]`.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Bench/src/BenchResidency.hpp Arcane/Bench/src/main.cpp
git commit -m "feat(arcane/bench): residency-upkeep micro-bench (standalone grid Move)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux"
```

---

### Task 10: Full-run orchestration → CSV + summary

**Files:**
- Modify: `Arcane/Bench/src/main.cpp` (add the full-run path + `--out`, `--smoke`)

**Interfaces:**
- Consumes: `RunStepSweep`, `RunStaticsBench`, `RunQueryBench`, `RunResidencyBench`, `CrossBroadphaseMaxPosDelta`, `WriteCsv`.
- Produces: `Bench.exe [--out <dir>] [--smoke]` writing `<dir>/bench-results.csv` + `<dir>/bench-summary.md`.

- [ ] **Step 1: Add a smoke check.** Add `CheckFullRunSmoke()` to `main.cpp` and call it from `RunSelfCheck()`:

```cpp
#include <fstream>
#include <filesystem>

static void CheckFullRunSmoke()
{
    // The aggregate returns a non-empty row set at tiny N.
    auto rows = Bench::RunAll(/*counts=*/{32}, /*steps=*/3, /*withEnki=*/false);
    Bench::Check(!rows.empty(), "full run aggregates >0 rows at N=32");
}
```

- [ ] **Step 2: Build + run to verify it fails** — `RunAll` undefined, red.

- [ ] **Step 3: Implement `RunAll` + the CSV/summary emit in `main.cpp`:**

```cpp
namespace Bench
{
    inline std::vector<MetricRow> RunAll(const std::vector<int>& counts, int steps, bool withEnki)
    {
        std::vector<MetricRow> rows = RunStepSweep(counts, steps, /*warmup=*/2, /*iters=*/3, withEnki);
        auto s = RunStaticsBench(counts, /*queries=*/64);   rows.insert(rows.end(), s.begin(), s.end());
        auto q = RunQueryBench(counts);                     rows.insert(rows.end(), q.begin(), q.end());
        auto r = RunResidencyBench(counts, steps);          rows.insert(rows.end(), r.begin(), r.end());
        return rows;
    }

    inline void WriteSummary(std::ostream& os, const std::vector<MetricRow>& rows,
                             const std::vector<std::pair<std::string,double>>& crossDelta)
    {
        os << "# SpatialGrid benchmark summary\n\n";
        os << "Cross-broadphase max per-body position delta after K steps ";
        os << "(measured, not asserted -- see determinism policy):\n\n";
        for (auto& c : crossDelta) os << "- " << c.first << ": " << c.second << "\n";
        os << "\nSee bench-results.csv for the full matrix.\n";
        // (Optional: pivot tables per bench kind. Keep minimal; the findings doc holds the analysis.)
    }
}
```

And the full-run branch in `main` (replacing the placeholder printf):

```cpp
    std::string outDir = "."; bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0 && i+1 < argc) outDir = argv[++i];
        else if (std::strcmp(argv[i], "--smoke") == 0) smoke = true;
    }
    const std::vector<int> counts = smoke ? std::vector<int>{100} : std::vector<int>{100, 1000, 10000};
    const int steps = smoke ? 10 : 120;
    auto rows = Bench::RunAll(counts, steps, /*withEnki=*/!smoke);

    std::vector<std::pair<std::string,double>> cross;
    for (auto sc : { Bench::Scene::UniformDense, Bench::Scene::UniformSparse, Bench::Scene::ClusteredMixed })
        cross.emplace_back(Bench::SceneName(sc),
                           Bench::CrossBroadphaseMaxPosDelta(sc, counts.front(), 30));

    std::filesystem::create_directories(outDir);
    { std::ofstream f(outDir + "/bench-results.csv"); Bench::WriteCsv(f, rows); }
    { std::ofstream f(outDir + "/bench-summary.md");  Bench::WriteSummary(f, rows, cross); }
    std::printf("bench: wrote %zu rows to %s\n", rows.size(), outDir.c_str());
    return 0;
```

- [ ] **Step 4: Build + run to verify PASS.**

Run: build then `Bench.exe --selfcheck`
Expected: `full run aggregates >0 rows` `[ ok ]`.
Then smoke-run (PowerShell): `Arcane\bin\Debug-windows-x86_64-md\Bench\Bench.exe --smoke --out $env:TEMP\benchsmoke`
Expected: prints `bench: wrote N rows` and `bench-results.csv` + `bench-summary.md` exist in `$env:TEMP\benchsmoke`.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Bench/src/main.cpp
git commit -m "feat(arcane/bench): full-matrix run -> CSV + summary (+ --smoke/--out)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux"
```

---

### Task 11: Run the full benchmark + author the findings doc

**Files:**
- Create: `docs/superpowers/specs/2026-07-01-arcane-spatial-grid-benchmark-findings.md`
- Create (build artifact, committed for the record): `docs/superpowers/specs/bench-results.csv` (or keep alongside the findings)

**Interfaces:**
- Consumes: the built Release `Bench.exe`.
- Produces: the findings doc with the crossover map + Cycle-2 recommendation.

- [ ] **Step 1: Build Release.**

Run: `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /t:Bench /p:Configuration=Release /m`
Expected: `Arcane\bin\Release-windows-x86_64-md\Bench\Bench.exe` exists.

- [ ] **Step 2: Run the full matrix.**

Run: `Arcane\bin\Release-windows-x86_64-md\Bench\Bench.exe --out docs\superpowers\specs`
Expected: `bench: wrote N rows`, and `docs/superpowers/specs/bench-results.csv` + `bench-summary.md` written. (Runtime: the 10k configs dominate; expect minutes, not seconds. If 10k is impractically slow for iteration, run `--smoke` first to sanity-check output shape.)

- [ ] **Step 3: Confirm the regression suite is still green** (spike touched no sim code, but prove it):

Run: `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[physics]"` from its exe dir.
Expected: all `[physics]` cases pass (unchanged count).

- [ ] **Step 4: Author the findings doc.** Read `bench-results.csv` + `bench-summary.md` and write `docs/superpowers/specs/2026-07-01-arcane-spatial-grid-benchmark-findings.md` with these sections (fill with the ACTUAL numbers):
  - **Setup** — machine, config (Release), scene params (echo the `DefaultParams` values used), steps/warmup/iters. Verify body **dispersal**: for each scene, report the fraction of dynamic bodies still within `extent` after the timed window. If it falls materially (say < 90%), the density-dependent numbers are unreliable — add boundary walls (Task 5 dispersal caveat) and re-run before trusting the crossover map.
  - **Crossover map** — for each metric (whole-Step, pair-find, statics query, tile/ring/region query), which config wins in each N × scene regime. A table per metric.
  - **Statics verdict** — grid vs tree for static candidate lookup; recommendation.
  - **Query verdict** — grid O(1) vs tree+filter; the cost of the queries the grid exists to serve.
  - **Residency upkeep** — the per-step cost the index adds.
  - **Cross-broadphase divergence** — the measured max-pos-delta per scene, with interpretation (byte-identical vs expected drift).
  - **MT picture** — enki vs serial where run.
  - **Cycle-2 recommendation** — an explicit, data-backed answer to: does the grid earn its keep as a broadphase / statics structure / query index? unify or separate `m_staticGrid`/`m_residencyGrid`? where does the opt-in crossover sit? what query API surface does the data justify?

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/specs/2026-07-01-arcane-spatial-grid-benchmark-findings.md docs/superpowers/specs/bench-results.csv docs/superpowers/specs/bench-summary.md
git commit -m "docs(arcane/physics): SpatialGrid benchmark findings + crossover map (Cycle 1 complete)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_013rJhc57BPdoFe7MVUYzNux"
```

---

## Post-plan: Cycle 2 handoff

With the findings doc in hand, start a new brainstorm → spec → plan → implement cycle for the actual SpatialGrid first-class feature. Cycle 2 is out of scope for this plan (it is designed from this spike's data, not from assumptions). Do NOT begin Cycle 2 implementation as part of this plan.

## Self-Review (author checklist — completed)

- **Spec coverage:** §5 executable → Task 1; §6.1 broadphase sweep → Task 6; §6.2 scenes → Task 5; §6.3 statics → Task 7; §6.4 query → Task 8; §6.5 residency → Task 9 (measured as a micro-bench, a documented refinement of the spec's A/B-delta since we excluded the WorldDef off-switch); §7 metrics → Tasks 2/6/7/8/9; §8 determinism → Tasks 3/4/6 (refined: hard-assert run-to-run + ST==MT, measure-and-report cross-broadphase — a documented refinement of the spec's "assert identical across configs," which is too strong given different fat-AABB margins); §9 outputs → Tasks 10/11; §10 success criteria → Task 11 steps 2/3. MT open-ended axis → Task 4 + Task 6 enki path.
- **Placeholder scan:** no TBD/TODO; every code step has complete code. Scene constants are first-draft-but-concrete and recorded in the findings (Task 11) — not placeholders.
- **Type consistency:** `MetricRow` fields, `Scene` enum, `Populate`/`DefaultParams`/`SceneName`, `HashWorldState`, `BenchEnkiExecutor`, `RunStepSweep`/`RunStaticsBench`/`RunQueryBench`/`RunResidencyBench`/`RunAll` names/signatures match across tasks.
- **Two spec refinements** (residency micro-bench vs A/B; cross-broadphase measure vs assert) are reconciled back into the spec in a companion edit so spec and plan stay in lockstep.
