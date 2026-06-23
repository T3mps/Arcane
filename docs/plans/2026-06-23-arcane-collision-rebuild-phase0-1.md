# Collision Module Rebuild — Phase 0 + 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the perf-measurement backbone + an honest pause, then build the first-class `SpatialGrid` and route static collision queries through it (the static-scan fix).

**Architecture:** Phase 0 is two independent quick wins (a `stepWorld` flag so paused frames skip the solve; a gated `--perf` harness + env scene-select in Loom). Phase 1 adds a per-shape spatial-hash `SpatialGrid` (Core, presentation-free), registers static fixtures into it, and reroutes `PhysicsWorld::StaticCandidates`' static-body lookup from the O(dynamics×statics) linear scan to a grid query — validated against the old scan as a reference oracle.

**Tech Stack:** C++23, Core (static-CRT + /MD dual build), Catch2 (`Arcane/Tests`), premake5 (`vs2026`), MSBuild. Spec: `docs/superpowers/specs/2026-06-23-arcane-collision-module-rebuild-design.md`.

---

## Conventions (read once)

- **Build (Debug):** `"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo` (Release/Dist: swap `-p:Configuration`).
- **Run tests:** from the exe dir — `cd "Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[physics]"`. Full gate = no filter, Debug AND Release.
- **New source files** are picked up by premake's `src/**` glob but require a **project regen**: run `ThirdParty\premake5\premake5.exe vs2026` from `Arcane/` (NOT `GenerateProjects.bat` — it hangs on a `pause`).
- **clangd diagnostics are false positives** in this repo (include-path resolution); MSVC is the source of truth.
- Core stays **presentation-free** (glm + std + sibling Physics headers only) and must compile **both** /MD and static-CRT (`Server/ArcaneCore/ArcaneCore.vcxproj`).
- Commit trailer on every commit: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

## File Structure

- Modify `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp` — add a `stepWorld` flag (skip `world.Step` when false).
- Modify `Arcane/Sandbox/src/SandboxApp.cpp` — paused path uses the no-step `PhysicsSystem`; `BuildInitialScene` reads `ARCANE_SANDBOX_SCENE`.
- Modify `Arcane/Loom/src/main.cpp` — gated `--perf` per-phase timing.
- Create `Arcane/Tests/src/PhysicsPauseTest.cpp` — the no-step behavioral test.
- Create `Arcane/Core/src/Arcane/Physics/Broadphase/SpatialGrid.hpp` / `.cpp` — the spatial-hash grid.
- Create `Arcane/Tests/src/PhysicsSpatialGridTest.cpp` — brute-force cross-check + residency/region tests.
- Modify `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` / `.cpp` + `Queries.cpp` — own a `SpatialGrid` for statics; register statics; reroute `StaticCandidates`.

---

# PHASE 0 — perf harness + honest pause

### Task 1: `PhysicsSystem` no-step flag + Sandbox pause fix

**Files:**
- Modify: `Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp` (ctor + `operator()` ~line 164, 300)
- Modify: `Arcane/Sandbox/src/SandboxApp.cpp:206` (the paused `mintOnly` path)
- Test: `Arcane/Tests/src/PhysicsPauseTest.cpp`

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/PhysicsPauseTest.cpp`:

```cpp
// Pausing must SKIP the solve, not run Step(0): a no-step PhysicsSystem mints +
// writes back but generates zero contacts (the expensive narrowphase/solve is
// skipped). Guards the interactive "pause to inspect" path + the perf claim.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Astra/Registry/Registry.hpp>

using namespace Arcane;

namespace
{
    // Spawn one dynamic box resting on a wider static box so a real step would
    // generate >=1 contact constraint.
    void BuildOverlap(Astra::Registry& reg)
    {
        reg.SetResource(PhysicsResource{
            std::make_unique<Physics::PhysicsWorld>(Physics::WorldDef{}), {} });
        auto add = [&](glm::vec2 pos, glm::vec2 half, Physics::BodyType t) {
            Astra::Entity e = reg.CreateEntity();
            LocalTransform lt; lt.position = pos;
            reg.AddComponent<LocalTransform>(e, lt);
            reg.AddComponent<WorldTransform>(e, WorldTransform{});
            RigidBody2D rb; rb.type = t; rb.fixedRotation = true;
            reg.AddComponent<RigidBody2D>(e, rb);
            Collider2D col; Fixture fx;
            fx.kind = Physics::ShapeKind::Aabb; fx.halfW = half.x; fx.halfH = half.y;
            col.fixtures.push_back(fx);
            reg.AddComponent<Collider2D>(e, col);
            reg.AddComponent<PhysicsBodyRef>(e, PhysicsBodyRef{});
        };
        add({0.0f, 100.0f}, {200.0f, 20.0f}, Physics::BodyType::Static);
        add({0.0f,  79.0f}, { 20.0f, 20.0f}, Physics::BodyType::Dynamic); // resting/overlapping
    }
}

TEST_CASE("PhysicsSystem no-step skips contact generation", "[physics][pause]")
{
    Astra::Registry reg;
    BuildOverlap(reg);

    // No-step: mint + write-back, but DO NOT solve -> zero contacts generated.
    PhysicsSystem noStep(1.0f / 60.0f, /*stepWorld=*/false);
    noStep(reg);
    auto* res = reg.GetResource<PhysicsResource>();
    REQUIRE(res->world->ActiveContactCount() == 0);

    // A real step on the same overlap DOES generate at least one contact.
    PhysicsSystem real(1.0f / 60.0f, /*stepWorld=*/true);
    real(reg);
    CHECK(res->world->ActiveContactCount() >= 1);
}
```

- [ ] **Step 2: Regenerate projects + run the test to verify it fails to compile**

Run: `cd Arcane && ThirdParty\premake5\premake5.exe vs2026` then build Debug, then `./ArcaneTests.exe "[pause]"`
Expected: COMPILE FAIL — `PhysicsSystem` has no two-arg constructor.

- [ ] **Step 3: Add the `stepWorld` flag to `PhysicsSystem`**

In `PhysicsSystem.hpp`, replace the constructor:

```cpp
        // fixedDt: the fixed timestep forwarded to PhysicsWorld::Step.
        // stepWorld: when false, run the DESTROY/CREATE/WRITE-BACK passes but
        // SKIP world.Step -- a paused frame mints spawned bodies + reflects poses
        // without paying the (dt-independent) narrowphase + solve. Default true.
        explicit PhysicsSystem(float fixedDt, bool stepWorld = true) noexcept
            : m_fixedDt(fixedDt), m_stepWorld(stepWorld) {}
```

Guard the step (PASS 3, ~line 300):

```cpp
            // PASS 3: STEP -- advance the world (skipped when paused).
            if (m_stepWorld)
                world.Step(m_fixedDt);
```

Add the member next to `m_fixedDt`:

```cpp
        bool  m_stepWorld;   // false on paused frames -> skip the solve
```

- [ ] **Step 4: Point the Sandbox paused path at the no-step system**

In `SandboxApp.cpp`, the `if (!runThisStep)` block — replace `PhysicsSystem mintOnly(0.0f);` with:

```cpp
            // Frozen: mint a paused-spawned body + write back its pose, but DO
            // NOT step (skip the narrowphase + solve entirely -> pausing the dense
            // stress scene instantly recovers FPS).
            PhysicsSystem mintOnly(kFixedDt, /*stepWorld=*/false);
            mintOnly(reg);
            return;
```

- [ ] **Step 5: Build Debug + run the test to verify it passes**

Run: build Debug, then `cd "Arcane/bin/Debug-windows-x86_64-md/ArcaneTests" && ./ArcaneTests.exe "[pause]"`
Expected: PASS (1 test case).

- [ ] **Step 6: Commit**

```bash
git add Arcane/Arcane/src/Arcane/Scene/PhysicsSystem.hpp Arcane/Sandbox/src/SandboxApp.cpp Arcane/Tests/src/PhysicsPauseTest.cpp
git commit -m "feat(arcane/physics): no-step PhysicsSystem so pause skips the solve

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Gated `--perf` harness + env scene-select in Loom

**Files:**
- Modify: `Arcane/Loom/src/main.cpp` (arg parse ~line 57; frame loop ~line 175)
- Modify: `Arcane/Sandbox/src/SandboxApp.cpp` (`BuildInitialScene` ~line 117)

- [ ] **Step 1: Env scene-select (replaces the m_sceneIndex hack permanently)**

In `SandboxApp.cpp`, `BuildInitialScene`:

```cpp
    void SandboxApp::BuildInitialScene(Astra::Registry& reg)
    {
        // ARCANE_SANDBOX_SCENE selects the initial scene headlessly (default 0).
        m_sceneIndex = 0;
        if (const char* s = std::getenv("ARCANE_SANDBOX_SCENE"))
        {
            const long v = std::strtol(s, nullptr, 10);
            if (v >= 0) m_sceneIndex = static_cast<std::size_t>(v);
        }
        RebuildScene(reg, m_sceneIndex);
    }
```

Add `#include <cstdlib>` near the top of `SandboxApp.cpp` if absent.

- [ ] **Step 2: Add a `--perf` flag**

In `main.cpp` arg parsing, add a `bool perf = false;` near the other flags and a branch:

```cpp
        else if (std::strcmp(argv[i], "--perf") == 0)                   { perf = true; }
```

Add `"  --perf                  log per-phase ms every 60 frames\n"` to `PrintUsage`.

- [ ] **Step 3: Add the gated per-phase timer block**

In `main.cpp`, just before `while (running)`, add (this is the previously-validated harness, now permanent + gated):

```cpp
        using PClock = std::chrono::steady_clock;
        auto perfT = [](PClock::time_point a, PClock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count(); };
        double accFrame=0, accSim=0, accRec=0, accEnd=0, accTone=0, accImgui=0, accPresent=0, accPoll=0;
        uint64_t perfFrames = 0;
```

Wrap the existing phases with timestamps (only the math is added; the calls are unchanged):
`perfFrameStart` at loop top; `accSim` around `runtime.Loop().Advance(...)`; `accRec` around `SetRenderContext + SubmitRender`; `accEnd` around `batcher->End()`; `accTone` around `tonemap->Run`; `accImgui` around `imgui->Render`; `accPresent` around `executeCommandList + Present`; `accPoll` around `plugin.Poll()`. After `plugin.Poll()`:

```cpp
            if (perf && ++perfFrames >= 60)
            {
                const Arcane::Batch2DStats bs = batcher->Stats();
                ARC_INFO("[PERF] {:.2f} ms ({:.1f} FPS) | sim {:.2f} rec {:.2f} end {:.2f} "
                         "tone {:.2f} imgui {:.2f} present {:.2f} poll {:.2f} | quads {} draws {}",
                         accFrame/perfFrames, 1000.0*perfFrames/accFrame, accSim/perfFrames,
                         accRec/perfFrames, accEnd/perfFrames, accTone/perfFrames, accImgui/perfFrames,
                         accPresent/perfFrames, accPoll/perfFrames, bs.quads, bs.drawCalls);
                accFrame=accSim=accRec=accEnd=accTone=accImgui=accPresent=accPoll=0; perfFrames=0;
            }
```

(Always accumulate; only log when `perf`. Guard `accFrame` accumulation behind `if (perf)` to avoid the cost when off.)

- [ ] **Step 4: Build Debug + smoke the harness**

Run: build Debug; `cd "Arcane/bin/Debug-windows-x86_64-md/Loom"` then
`ARCANE_SANDBOX_SCENE=8 ./Loom.exe --frames 120 --no-vsync --perf 2>&1 | grep PERF`
Expected: at least one `[PERF] ... | sim ...` line; scene 8 (stress) running.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Loom/src/main.cpp Arcane/Sandbox/src/SandboxApp.cpp
git commit -m "feat(arcane/loom): gated --perf per-phase timing + ARCANE_SANDBOX_SCENE select

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

# PHASE 1 — `SpatialGrid` (statics first-class on the grid)

### Task 3: `SpatialGrid` core + brute-force cross-check

**Files:**
- Create: `Arcane/Core/src/Arcane/Physics/Broadphase/SpatialGrid.hpp`
- Create: `Arcane/Core/src/Arcane/Physics/Broadphase/SpatialGrid.cpp`
- Test: `Arcane/Tests/src/PhysicsSpatialGridTest.cpp`

- [ ] **Step 1: Write the failing test**

Create `Arcane/Tests/src/PhysicsSpatialGridTest.cpp`:

```cpp
// SpatialGrid: a per-shape fixed-tile spatial hash. Contract: after the caller's
// tight AABB filter, QueryAABB returns EXACTLY the brute-force overlap set,
// sorted + unique. The grid only narrows; correctness is gated by the oracle.
#include <algorithm>
#include <random>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Physics/Broadphase/SpatialGrid.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp> // AabbOverlap

using namespace Arcane::Physics;

namespace
{
    Aabb2 Box(Real x0, Real y0, Real x1, Real y1)
    { Aabb2 a; a.min = Vec2(x0,y0); a.max = Vec2(x1,y1); return a; }
}

TEST_CASE("SpatialGrid query == brute-force after tight filter", "[physics][grid]")
{
    std::mt19937 rng(0xC0FFEE);
    std::uniform_real_distribution<float> pos(-500.0f, 500.0f);
    std::uniform_real_distribution<float> ext(2.0f, 60.0f);

    SpatialGrid grid(/*tileSize=*/32.0f);
    std::vector<Aabb2> boxes;
    for (std::uint32_t i = 0; i < 400; ++i)
    {
        const float x = pos(rng), y = pos(rng), w = ext(rng), h = ext(rng);
        Aabb2 b = Box(x, y, x + w, y + h);
        boxes.push_back(b);
        grid.Insert(i, b);
    }

    auto tightFilter = [&](const Aabb2& q, std::vector<std::uint32_t> cand) {
        std::vector<std::uint32_t> out;
        for (std::uint32_t id : cand) if (AabbOverlap(boxes[id], q)) out.push_back(id);
        std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    };
    auto brute = [&](const Aabb2& q) {
        std::vector<std::uint32_t> out;
        for (std::uint32_t i = 0; i < boxes.size(); ++i) if (AabbOverlap(boxes[i], q)) out.push_back(i);
        return out; // already sorted ascending
    };

    for (int t = 0; t < 200; ++t)
    {
        const float x = pos(rng), y = pos(rng), w = ext(rng), h = ext(rng);
        Aabb2 q = Box(x, y, x + w, y + h);
        std::vector<std::uint32_t> cand;
        grid.QueryAABB(q, cand);
        REQUIRE(tightFilter(q, cand) == brute(q));
    }

    SECTION("Move + Remove keep the invariant")
    {
        grid.Move(0, Box(900.0f, 900.0f, 920.0f, 920.0f)); boxes[0] = Box(900,900,920,920);
        grid.Remove(1); boxes[1] = Box(1e9f, 1e9f, 1e9f, 1e9f); // unreachable
        Aabb2 q = Box(890.0f, 890.0f, 930.0f, 930.0f);
        std::vector<std::uint32_t> cand; grid.QueryAABB(q, cand);
        std::vector<std::uint32_t> got = tightFilter(q, cand);
        REQUIRE(std::find(got.begin(), got.end(), 0u) != got.end());
        REQUIRE(std::find(got.begin(), got.end(), 1u) == got.end());
    }
}
```

- [ ] **Step 2: Regen + build + verify it fails to compile**

Run: `cd Arcane && ThirdParty\premake5\premake5.exe vs2026`; build Debug; `./ArcaneTests.exe "[grid]"`
Expected: COMPILE FAIL — no `SpatialGrid.hpp`.

- [ ] **Step 3: Write `SpatialGrid.hpp`**

```cpp
#pragma once
// SpatialGrid: a per-shape fixed-tile spatial hash (Physics v2 collision rebuild).
// A FIRST-CLASS grid index: ids (fixtures or bodies) register into every cell
// their tight AABB overlaps; QueryAABB returns the union of those cells' ids,
// SORTED + de-duped (the caller applies the tight AABB test). Bounded memory
// (hash, not a dense array); cell coord = floor((p - origin)/tileSize).
// DETERMINISM: cells visited row-major; output sorted+unique; no fp in the cell
// index math beyond the floor. PRESENTATION-FREE + C++20 (glm+std+Physics only).
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp> // Aabb2

namespace Arcane { namespace Physics {

class SpatialGrid
{
public:
    explicit SpatialGrid(Real tileSize, Vec2 origin = Vec2(Real(0), Real(0)));

    void Insert(std::uint32_t id, const Aabb2& box); // register into overlapping cells
    void Remove(std::uint32_t id);                   // drop from all its cells
    void Move(std::uint32_t id, const Aabb2& box);   // remove + insert (re-register)

    // Ids whose cells overlap `box`, SORTED ascending + de-duped. out cleared
    // then filled (clear()+push_back -> capacity preserved). Returns out.size().
    int QueryAABB(const Aabb2& box, std::vector<std::uint32_t>& out) const;

    [[nodiscard]] Real TileSize() const { return m_tileSize; }
    [[nodiscard]] Vec2 Origin()   const { return m_origin; }

    // Integer cell coord of a world point (floor). Public for tests / residency.
    void CellCoord(Vec2 p, int& cx, int& cy) const;

private:
    static std::uint64_t Key(int cx, int cy)
    { return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32)
           |  static_cast<std::uint64_t>(static_cast<std::uint32_t>(cy)); }
    void CellRange(const Aabb2& box, int& x0, int& y0, int& x1, int& y1) const;

    Real m_tileSize;
    Vec2 m_origin;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> m_cells;   // cell -> ids
    std::unordered_map<std::uint32_t, std::vector<std::uint64_t>> m_idCells; // id -> its cells
    mutable std::vector<std::uint32_t> m_scratch; // QueryAABB collect buffer
};

}} // namespace Arcane::Physics
```

- [ ] **Step 4: Write `SpatialGrid.cpp`**

```cpp
#include <Arcane/Physics/Broadphase/SpatialGrid.hpp>
#include <algorithm>
#include <cmath>

namespace Arcane { namespace Physics {

SpatialGrid::SpatialGrid(Real tileSize, Vec2 origin)
    : m_tileSize(tileSize > Real(0) ? tileSize : Real(1)), m_origin(origin) {}

void SpatialGrid::CellCoord(Vec2 p, int& cx, int& cy) const
{
    cx = static_cast<int>(std::floor((p.x - m_origin.x) / m_tileSize));
    cy = static_cast<int>(std::floor((p.y - m_origin.y) / m_tileSize));
}

void SpatialGrid::CellRange(const Aabb2& box, int& x0, int& y0, int& x1, int& y1) const
{
    CellCoord(box.min, x0, y0);
    CellCoord(box.max, x1, y1);
}

void SpatialGrid::Insert(std::uint32_t id, const Aabb2& box)
{
    int x0, y0, x1, y1; CellRange(box, x0, y0, x1, y1);
    std::vector<std::uint64_t>& owned = m_idCells[id];
    owned.clear();
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx)
        {
            const std::uint64_t k = Key(cx, cy);
            m_cells[k].push_back(id);
            owned.push_back(k);
        }
}

void SpatialGrid::Remove(std::uint32_t id)
{
    auto it = m_idCells.find(id);
    if (it == m_idCells.end()) return;
    for (std::uint64_t k : it->second)
    {
        auto cit = m_cells.find(k);
        if (cit == m_cells.end()) continue;
        std::vector<std::uint32_t>& ids = cit->second;
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        if (ids.empty()) m_cells.erase(cit);
    }
    m_idCells.erase(it);
}

void SpatialGrid::Move(std::uint32_t id, const Aabb2& box)
{
    Remove(id);
    Insert(id, box);
}

int SpatialGrid::QueryAABB(const Aabb2& box, std::vector<std::uint32_t>& out) const
{
    out.clear();
    int x0, y0, x1, y1; CellRange(box, x0, y0, x1, y1);
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx)
        {
            auto cit = m_cells.find(Key(cx, cy));
            if (cit == m_cells.end()) continue;
            out.insert(out.end(), cit->second.begin(), cit->second.end());
        }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return static_cast<int>(out.size());
}

}} // namespace Arcane::Physics
```

- [ ] **Step 5: Build Debug + run the test to verify it passes**

Run: build Debug; `./ArcaneTests.exe "[grid]"`
Expected: PASS (1 case, both sections).

- [ ] **Step 6: Confirm static-CRT (ArcaneCore) still builds**

Run: `cd Server && "<msbuild>" ArcaneCore/ArcaneCore.vcxproj -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo`
Expected: `ArcaneCore.lib` produced, no errors.

- [ ] **Step 7: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/Broadphase/SpatialGrid.hpp Arcane/Core/src/Arcane/Physics/Broadphase/SpatialGrid.cpp Arcane/Tests/src/PhysicsSpatialGridTest.cpp
git commit -m "feat(arcane/physics): SpatialGrid (per-shape fixed-tile spatial hash)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Register statics in the world grid + reroute `StaticCandidates`

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` (member + include)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (`AddBody`/`AddFixture`/`RemoveBody` static registration)
- Modify: `Arcane/Core/src/Arcane/Physics/Queries.cpp` (`StaticCandidates` static-body lookup ~line 549)
- Test: extend `Arcane/Tests/src/PhysicsSpatialGridTest.cpp`

- [ ] **Step 1: Write the failing test (grid static set == linear-scan set)**

Append to `PhysicsSpatialGridTest.cpp`:

```cpp
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Body.hpp>

TEST_CASE("StaticCandidates static-body set unchanged by grid reroute", "[physics][grid]")
{
    PhysicsWorld w;
    auto addStatic = [&](Real cx, Real cy, Real hw, Real hh) {
        BodyDef d; d.type = BodyType::Static; d.position = Vec2(cx, cy);
        d.shape = MakeAabb(hw, hh); return w.AddBody(d);
    };
    addStatic(0,   0,   50, 10);
    addStatic(120, 0,   50, 10);
    addStatic(0,   200, 10, 80);

    // Reference: brute-force overlap of the query box against every static slot.
    Aabb2 q; q.min = Vec2(-60, -20); q.max = Vec2(60, 20);
    std::vector<Aabb2> spans; std::vector<std::uint32_t> statics;
    w.StaticCandidates(q, spans, statics);
    // Only the first static (centered at 0,0, half 50x10) overlaps q.
    REQUIRE(statics.size() == 1);
}
```

- [ ] **Step 2: Build + verify it passes against the CURRENT linear scan**

Run: build Debug; `./ArcaneTests.exe "[grid]"`
Expected: PASS — this pins current behavior before the reroute (a characterization test).

- [ ] **Step 3: Add a static `SpatialGrid` member to `PhysicsWorld`**

In `PhysicsWorld.hpp`, add the include `#include <Arcane/Physics/Broadphase/SpatialGrid.hpp>` and, near `m_staticList`:

```cpp
            // Per-shape static index (collision-rebuild Phase 1). Static BODIES
            // register their slot id here (keyed by body slot; statics are single
            // proxies today -- per-fixture proxies arrive in Phase 2). StaticCandidates
            // queries this instead of the O(dynamics*statics) m_staticList scan.
            // Tile size = a coarse default until the map's tile size is wired in.
            SpatialGrid m_staticGrid{ Real(64) };
            // Dedicated query scratch for the grid lookup inside StaticCandidates.
            // MUST NOT reuse m_scratchStatics: ShapeCast calls
            // StaticCandidates(..., m_scratchStatics) as the OUTPUT, so reusing it
            // for the grid query would alias and corrupt the result.
            mutable std::vector<std::uint32_t> m_staticGridScratch;
```

- [ ] **Step 4: Register / unregister statics in the lifecycle**

In `PhysicsWorld.cpp` `AddBody`, where a Static body is pushed to `m_staticList`, also:

```cpp
                m_staticGrid.Insert(idx, SlotAabb(idx));
```

In `RemoveBody`, where a static is removed from `m_staticList`, also `m_staticGrid.Remove(idx);`. (Statics never move, so no `Move` is needed; `AddFixture` on a static re-inserts: call `m_staticGrid.Move(bodySlot, SlotAabb(bodySlot))` at the end of `AddFixture` when the owning body is Static.)

- [ ] **Step 5: Reroute `StaticCandidates`' static-body lookup**

In `Queries.cpp` `StaticCandidates`, replace the `for (i in m_staticList) if (AabbOverlap(box, SlotAabb)) push` loop with a grid query (tile spans from `TileGrid` are unchanged):

```cpp
            staticsOut.clear();
            m_staticGrid.QueryAABB(box, m_staticGridScratch);
            for (std::uint32_t idx : m_staticGridScratch)
                if (m_alive[idx] && AabbOverlap(box, SlotAabb(idx)))
                    staticsOut.push_back(idx);
```

Uses the dedicated `m_staticGridScratch` (NOT `m_scratchStatics`, which `ShapeCast`
passes as `staticsOut`). `QueryAABB` returns sorted ids, so `staticsOut` is
slot-ascending — a **deliberate, deterministic** order that may differ from the old
`m_staticList` insertion order. This is allowed (byte-identity to the Lua is retired);
it changes contact *order*, not correctness.

- [ ] **Step 6: Build + run the full physics suite to verify behavior unchanged**

Run: build Debug; `./ArcaneTests.exe "[physics]"`
Expected: PASS (all physics cases — the reroute is behavior-preserving; `PhysicsCharacterTest`, `PhysicsCcdTest`, scene tests all exercise `StaticCandidates`). If a *position-golden* snapshot trips because the static-candidate order changed (sorted vs insertion), **re-baseline it deliberately** (spec §8: goldens are a re-baselineable tripwire, not a freeze) — but a behavioral/invariant test failing (a stack no longer settles, a body tunnels) is a real regression, not a re-baseline.

- [ ] **Step 7: Confirm ArcaneCore static-CRT builds, then commit**

Run the ArcaneCore Debug build (Task 3 Step 6). Then:

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Core/src/Arcane/Physics/Queries.cpp Arcane/Tests/src/PhysicsSpatialGridTest.cpp
git commit -m "perf(arcane/physics): route StaticCandidates through the static SpatialGrid

Replaces the O(dynamics*statics) m_staticList linear scan (which also recomputed
each static AABB per query) with a per-shape grid query.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Dynamic residency + `Residents` queries (first-class grid)

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/Broadphase/SpatialGrid.hpp` / `.cpp` (region/resident query helpers)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` / `.cpp` (a residency `SpatialGrid` updated on body move; `Residents` accessors)
- Test: extend `Arcane/Tests/src/PhysicsSpatialGridTest.cpp`

- [ ] **Step 1: Write the failing test**

Append:

```cpp
TEST_CASE("Residents(region) returns bodies in a tile region", "[physics][grid][residency]")
{
    PhysicsWorld w;
    BodyDef d; d.type = BodyType::Dynamic; d.shape = MakeCircle(Real(8));
    d.position = Vec2(10, 10);   BodyHandle a = w.AddBody(d);
    d.position = Vec2(300, 300); BodyHandle b = w.AddBody(d);
    w.Step(Real(1)/Real(60));    // commit positions -> residency updated

    // Region covering ~(0,0)..(64,64) in world space should contain `a`, not `b`.
    Aabb2 region; region.min = Vec2(0, 0); region.max = Vec2(64, 64);
    std::vector<std::uint32_t> residents;
    w.Residents(region, residents);
    REQUIRE(std::find(residents.begin(), residents.end(), a.index) != residents.end());
    REQUIRE(std::find(residents.begin(), residents.end(), b.index) == residents.end());
}
```

- [ ] **Step 2: Build + verify compile fail** (`Residents` undefined). Run `./ArcaneTests.exe "[residency]"`.

- [ ] **Step 3: Add a residency grid + update it on commit**

In `PhysicsWorld.hpp` add `SpatialGrid m_residencyGrid{ Real(64) };` and a public accessor:

```cpp
            // First-class tile residency: dynamic/kinematic body slots register
            // their world AABB cell-occupancy, refreshed when their position
            // commits (CommitSlotPosition + kinematic stage-1). Region/tile
            // queries serve gameplay (combat-sphere extraction).
            int Residents(const Aabb2& region, std::vector<std::uint32_t>& out) const
            { return m_residencyGrid.QueryAABB(region, out); }
```

In `PhysicsWorld.cpp` `AddBody`, for a mover (Dynamic/Kinematic) add an initial `m_residencyGrid.Insert(idx, SlotAabb(idx));` (so a resting body whose position never re-commits is still resident). In `CommitSlotPosition` (and the stage-1 kinematic integrate where `m_moverBroadphase->Update` is called), add `m_residencyGrid.Move(i, SlotAabb(i));`. In `RemoveBody` for a mover add `m_residencyGrid.Remove(idx);`.

- [ ] **Step 4: Build + run the residency test to verify it passes.** Run `./ArcaneTests.exe "[residency]"`.

- [ ] **Step 5: ArcaneCore static-CRT build, then commit**

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Tests/src/PhysicsSpatialGridTest.cpp
git commit -m "feat(arcane/physics): first-class dynamic tile residency + Residents(region) query

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Phase 0+1 full gate + perf re-measure

**Files:** none (verification only).

- [ ] **Step 1: Full ArcaneTests Debug + Release**

Run (both configs, from each exe dir): `./ArcaneTests.exe`
Expected: `All tests passed` both configs (assertion count ≥ the pre-phase baseline + the new pause/grid/residency cases).

- [ ] **Step 2: ArcaneCore static-CRT Debug + Release**

Run the ArcaneCore vcxproj build for both configs.
Expected: `ArcaneCore.lib` both, no errors.

- [ ] **Step 3: Perf re-measure (static-scan fix sanity)**

Build Release; `cd "Arcane/bin/Release-windows-x86_64-md/Loom"` then
`ARCANE_SANDBOX_SCENE=8 ./Loom.exe --frames 2000 --no-vsync --perf 2>&1 | grep PERF | tail -5`
Expected: steady-state `[PERF]` lines; `sim` no worse than the pre-grid baseline (the stress scene has few statics, so the win here is correctness/architecture, not a big number — the headline win lands in Phase 2). Record the numbers in the Phase-2 plan's baseline.

- [ ] **Step 4: Update the memory pointer**

Note in `project_arcane_physics_perf_stress` memory: Phase 0+1 landed (pause-skips-solve, gated `--perf` harness, `SpatialGrid` static index + residency), gate green; next = Phase 2 (per-fixture `DynamicTree`).

---

## Self-Review Notes (addressed)

- **Spec coverage:** Phase 0 (harness + pause) and Phase 1 (`SpatialGrid` statics + residency + query API, `StaticCandidates` reroute) map to the spec §10 sequencing items 0–1. Passability fold-in (§6) and the brute-force *broadphase-pair* cross-check (§8, the move-buffer oracle) belong to Phase 2 and are deferred there — noted, not dropped. Region query (`Residents`) covers the combat-sphere seam (§6).
- **Type consistency:** `SpatialGrid::QueryAABB`, `Insert`, `Remove`, `Move`, `CellCoord` are referenced consistently across Tasks 3–5; `Aabb2`/`Vec2`/`Real`/`BodyHandle.index`/`AabbOverlap`/`SlotAabb`/`MakeAabb`/`MakeCircle` are existing Core symbols. `PhysicsSystem(float, bool)` matches Task 1 usage in `SandboxApp.cpp`.
- **Tunables:** `m_staticGrid`/`m_residencyGrid` tile size defaults to `64` (coarse) until the map's real tile size is wired in (a Phase-2/map-integration follow-up; flagged, not a placeholder).
