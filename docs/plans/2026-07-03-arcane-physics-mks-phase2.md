# Arcane MKS Units — Phase 2 (Solver/Dynamics Cluster Conversion) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert the solver/dynamics test cluster to MKS content (bodies 0.1-10 m, g=10, m/s velocities), delete its 276 PX-PINs, **retire the Baumgarte oracle solver** (USER decision 2026-07-03 — supersedes the P1 "flip the locals" carry-forward), and land the `MKS-DEFER(Pn)` marker convention — suite green at every commit.

**Architecture:** Spec §4 Phase 2 (`docs/superpowers/specs/2026-07-02-arcane-physics-mks-units-design.md`). One branch, ten tasks: markers first (comment-only), then the Baumgarte retirement (engine + tests in one byte-identical-for-SoftStep commit, followed by the MKS conversion of the surviving SoftStep budget file), then seven mechanical per-file conversion tasks, then exit verification. Content is **re-authored in round meters, not mechanically divided** (spec §2); assertions that are analytic or relational survive by construction, the handful of magic-px sites re-derive with written justification.

**Tech Stack:** C++23 (Arcane Core/Physics), MSVC via msbuild, Catch2. Parity source: vendored `ThirdParty/box2d-3.1.1` — **any question or invariant during implementation is checked against it directly (user directive), never recalled.**

**Phases 3-6 are separate plan documents** (spec §4): P3 sleep/settle (acceptance risk — probe battery 2026-07-03 retired it: pile sleeps at 0.05 by step 338, zero re-wake), P4 broadphase/spatial + CharacterController retunes + kShapeCastTol re-couple, P5 CCD/clamp/joints + MouseJoint maxForce, P6 sandbox + camera PPM + wall-time restore assert.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-02-arcane-physics-mks-units-design.md` (§2 authoring rules, §4 P2 cluster list, §6 acceptance + parity contingency).
- **Branch:** `feature/arcane-physics-mks-phase2` off `main` (@15d824cc or later). Push when green; **merge to main = USER's call** (honor-system branch workflow).
- **Build (PowerShell, NOT Git Bash):** `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m`. Pre-existing noise: the `Bench` project fails (gitignored slnx ghost) — ignore; Core/Arcane/ArcaneTests/Sandbox/Loom must be clean.
- **Tests from exe dir** `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\`. Full-suite = `.\ArcaneTests.exe ~[gpu]` (**~18 min at px scale — budget subagent timeouts ≥25 min**); cluster runs = `"[physics]"` (minutes); per-file runs use the file's tags (fast). No new .cpp files in this phase → no premake regen needed.
- **clangd/IDE diagnostics are FALSE POSITIVES** — msbuild is truth.
- **Staging:** stage ONLY per-task files by explicit path. NEVER `git add -A` (unrelated parked changes incl. untracked `ThirdParty/box2d-3.1.1/`).
- **Commit trailers (every commit):**
  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466
  ```

### Conversion Protocol (applies to every per-file conversion task)

Every task's requirements implicitly include these rules:

1. **Authoring rules (spec §2):** round meter numbers; bodies 0.1-10 m; g = 10 (y-down `gravityY = Real(10)` — now the DEFAULT, so delete the pin and the line unless the case needs zero-g or custom g); velocities in m/s well under the 400 cap. Per-scene scale choice is free (÷10 or ÷100 mappings below are recommendations, not mandates) as long as bodies land in the happy range and numbers are round.
2. **PX-PIN deletion:** delete every `// PX-PIN` line in the file. Two survivors change form, not value:
   - Zero-g cases keep `wd.gravityX = Real(0); wd.gravityY = Real(0);` **without** the marker — the default is now +10, so zero-g is a deliberate scene statement.
   - A case that genuinely needs a non-default knob (e.g. `sleepThreshold = 0` to hold bodies awake) keeps it as an intentional, commented scene choice.
   Everything else (sleepThreshold 8, restitutionThreshold 20, contactPushMaxVelocity 300, hashCellSize 64) is deleted outright — converted files inherit the MKS defaults (0.05 / 1.0 / 3.0 / 1.0).
3. **Impulses are authored as mass × Δv**, computed in the test (analytic mass from density × area, e.g. circle `density * kPi * r * r`, box `density * 4 * hw * hh`) with the target Δv in m/s named in a comment — no magic impulse literals. Files that already build analytic references keep their own convention.
4. **Slop-scale seatings re-derive from constants, not scale division:** any authored overlap/seating offset that encoded a px-era tolerance (e.g. stack seatings 0.2 px deep) is re-expressed from `kLinearSlop` (0.005) / `kSkin` (0.02), with the constant named in a comment.
5. **Step counts re-derive from physics:** falls take longer in meters (t = sqrt(2h/g): 2 m under g=10 ≈ 0.63 s ≈ 38 steps vs px 0.32 s). Check every "step N times then assert settled/landed" loop against the new fall/settle time; extend with headroom where needed.
6. **Triage protocol (per file):** build, run the file's tags.
   - **Behavioral/relational asserts (sleep flags, island roots, contact counts, orderings, invariant validators, no-tunnel gates) must pass UNMODIFIED.** A behavioral failure at MKS content is a potential parity bug: STOP the task, diff the scenario against vendored Box2D v3 behavior, report — do NOT weaken the assert and do NOT touch engine constants (the px-era hack ban, spec §6).
   - **Numeric baselines** (position windows, penetration/KE/drift budgets) re-derive empirically: run, record the observed value, set the bound with ~1.5× headroom, and justify inline naming the driving constant (`// re-baselined for MKS: penetration budget ~ kSkin (MKS P2)`).
   - **Run-twice / cross-broadphase / cross-path identity asserts** are scale-independent and must pass as-is; only self-derived hash VALUES change (never asserted literally).
7. **Rotation authoring rule:** dynamic `MakeAabb` hard-asserts `fixedRotation` — any converted content that spins a rectangle uses `MakePolygon` (survey confirms both existing spinners — PhysicsRotationTest, PhysicsDeterminismTest — already comply; keep it that way).
8. **dt stays `1/60`;** `substepCount` default 4, `velIters` defaults/overrides (iteration counts) are unit-free and unchanged.
9. **Acceptance per task:** file tags green + `"[physics]"` green + file's PX-PIN grep count == 0. Commit body carries the justification table for every re-baselined assertion (the tables live in commit bodies — `.superpowers/` is gitignored).

### Survey facts the recipes rely on (verified 2026-07-03)

- No shared test header exists; every helper and px constant is file-local. PX-PIN counts per file: Solver 56, Baumgarte 11, Dynamics 36, Rotation 11, CompoundCom 28, CompoundSlide 15, SimdSolver 11, CompactedSolve 20, BodyContacts 5, PersistentContact 40, PersistentIsland 30, Determinism 4, Phase1Harness 5, Phase2Harness 4 — **total 276; repo total 946 → 670 after P2**.
- The determinism/harness files also carry **untagged** px scene constants (`gravityY = Real(300)`, `hashCellSize = Real(64)`/`Real(32)`, `tileCellSize = Real(16)`) that convert even though no pin marks them.
- Baumgarte/solverKind consumers (complete, grep-verified 2026-07-03 — NOTHING in Sandbox/Loom/production selects it): Core plumbing (`PhysicsWorld.hpp` SolverKind enum :116-120 + `solverKind` :252 + `velIters` :256 + `VelIters()` :1153 + `m_velIters` :1454; `PhysicsWorld.cpp` include :25 + ctor switch :112-119 + `m_velIters` init :137; `Solver/Baumgarte.{cpp,hpp}`) and 6 test files: PhysicsBaumgarteTest (A/B loops), PhysicsDeterminismTest (`RunScene` param, Baumgarte case :517), PhysicsPhase2HarnessTest (`RunScene` param, Baumgarte case :242), PhysicsJointsTest (P5 file — `GravityWorld(kind)` :80, `run(SolverKind::Baumgarte)` :120/:152), PhysicsSimdSolverTest (`wd.solverKind = SolverKind::SoftStep` :1028), PhysicsInvariantsTest (comments :120/:168).
- **"Baumgarte" the TECHNIQUE name stays.** Joint.hpp/Joints.cpp/SoftStep.cpp comments describing the Baumgarte positional-bias math (e.g. Joint.hpp:7,19,40,121,130; SoftStep.cpp:276,280) refer to the standard stabilization technique, not the retired solver class — do not touch them in T2.

## File Structure

| File | Change |
|---|---|
| `Arcane/Core/src/Arcane/Physics/CharacterController.hpp` | MKS-DEFER(P4) markers on kMaxSubstep/kDepenetrationSkin (T1) |
| `Arcane/Core/src/Arcane/Physics/Narrowphase/Gjk.hpp` | MKS-DEFER(P4) marker on kShapeCastTol (T1) |
| `Arcane/Core/src/Arcane/Physics/Joints/Joint.hpp`, `Joints.hpp`, `Joints.cpp` | MKS-DEFER(P5) markers on MouseJoint maxForce 1e9 (T1) |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` | residency-grid TODO relabel (T1, comment-only) |
| `Arcane/Core/src/Arcane/Physics/Solver/Baumgarte.cpp`, `Baumgarte.hpp` | DELETED (T2) |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp`, `PhysicsWorld.cpp` | SolverKind/solverKind/velIters plumbing removed (T2) |
| `Arcane/Core/src/Arcane/Physics/Solver/Solver.hpp`, `SoftStep.hpp` | comment updates only (T2) |
| `Arcane/Tests/src/PhysicsBaumgarteTest.cpp` | → RENAMED `PhysicsSolverBudgetTest.cpp`, SoftStep-only + MKS conversion (T2) |
| `Arcane/Tests/src/PhysicsJointsTest.cpp`, `PhysicsSimdSolverTest.cpp`, `PhysicsInvariantsTest.cpp` | Baumgarte selection/reference removal only (T2; JointsTest stays pinned px) |
| `Arcane/Tests/src/PhysicsDynamicsTest.cpp` | MKS conversion (T3) |
| `Arcane/Tests/src/PhysicsSolverTest.cpp` | MKS conversion (T4) |
| `Arcane/Tests/src/PhysicsRotationTest.cpp` | MKS conversion (T5) |
| `Arcane/Tests/src/PhysicsCompoundComTest.cpp`, `PhysicsCompoundSlideTest.cpp` | MKS conversion (T6) |
| `Arcane/Tests/src/PhysicsSimdSolverTest.cpp`, `PhysicsCompactedSolveTest.cpp`, `PhysicsBodyContactsTest.cpp` | MKS conversion (T7) |
| `Arcane/Tests/src/PhysicsPersistentContactTest.cpp`, `PhysicsPersistentIslandTest.cpp` | MKS conversion (T8) |
| `Arcane/Tests/src/PhysicsDeterminismTest.cpp`, `PhysicsPhase1HarnessTest.cpp`, `PhysicsPhase2HarnessTest.cpp` | MKS conversion (T9) |

---

### Task 1: Branch, baseline, and MKS-DEFER markers

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/CharacterController.hpp` (~:78-89, comments only)
- Modify: `Arcane/Core/src/Arcane/Physics/Narrowphase/Gjk.hpp` (~:156-159, comment only)
- Modify: `Arcane/Core/src/Arcane/Physics/Joints/Joint.hpp` (~:98-100), `Joints.hpp` (~:166), `Joints.cpp` (~:458) (comments only)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` (~:1435, comment only)

**Interfaces:**
- Produces: the greppable deferred-constant convention `MKS-DEFER(Pn)` — one marker per Core constant that is still px-era by design, naming its target value and phase. Later phases burn these down (`grep -rn "MKS-DEFER" Arcane/Core/src`).

- [ ] **Step 1: Branch + capture the baseline.** `git checkout -b feature/arcane-physics-mks-phase2`. Build current HEAD, then from the exe dir run `.\ArcaneTests.exe ~[gpu]` and `.\ArcaneTests.exe "[physics]"`, and record BOTH exact "assertions in test cases" lines plus `grep -rc "PX-PIN" Arcane/Tests/src Arcane/Sandbox/src | grep -v ":0"` totals (expect 946) in your report. Counts will legitimately drift during P2 (re-baselines + new cases); the baseline is the reference for what changed, not an identity gate.

- [ ] **Step 2: Add the markers.** Comment-only edits; values do NOT change in this task.

`CharacterController.hpp` — replace the two constants' comments (locate by content near :78-89):
```cpp
            // MKS-DEFER(P4): 8 px -> 0.1 m when the CC test cluster converts
            // (spec §3; it IS a length — kMaxPasses is the iteration count).
            // The capsule march step. Discrete substeps <= this length stand in
            // for the true swept test (Lua MAX_SUBSTEP).
            static constexpr Real kMaxSubstep = Real(8);
```
```cpp
            // MKS-DEFER(P4): 0.05 px-era -> kSkin (0.02) when the CC test
            // cluster converts (spec §3). Resolve to a hair outside the surface
            // (Lua SKIN). Distinct from Arcane::Physics::kSkin (speculative
            // skin) in purpose; P4 aligns the value.
            static constexpr Real kDepenetrationSkin = Real(0.05);
```

`Gjk.hpp` (~:156-159) — prepend to the conservative-advancement comment:
```cpp
        // MKS-DEFER(P4): re-couple to kLinearSlop (Box2D distance.c:611-614
        // uses linearSlop-scale termination) when the Queries cluster converts.
        // Conservative-advancement constants (Cast.lua:11-12, verbatim).
        inline constexpr Real kShapeCastTol     = Real(0.05);
```

`Joint.hpp` (~:98-100), `Joints.hpp` (~:166), `Joints.cpp` (~:458) — tag all three `1e9` sites:
```cpp
            Real maxForce = Real(1e9); // MKS-DEFER(P5): rescale — at MKS mass*g is ~1-1e3 N, 1e9 is a px-era "infinite" clamp
```
(same trailing comment on the `m_maxForce` member default and the `Joints.cpp` factory fallback).

`PhysicsWorld.hpp` (~:1435) — the existing `TODO(Phase 2)` collides with MKS phase numbering and is NOT an MKS obligation (the value is already 1 m); relabel:
```cpp
            SpatialGrid m_residencyGrid{ Real(1) }; // MKS tile; TODO(map-integration): wire to the map's real tile size
```

- [ ] **Step 3: Build + verify no behavior change.** Build; run `.\ArcaneTests.exe "[physics]"` — identical counts to Step 1 (comments cannot shift anything; this catches accidental token edits). `grep -rn "MKS-DEFER" Arcane/Core/src` must list exactly 5 sites (2 CC, 1 Gjk, 3 MouseJoint — Joint.hpp + Joints.hpp + Joints.cpp).

- [ ] **Step 4: Commit**

```bash
git add Arcane/Core/src/Arcane/Physics/CharacterController.hpp Arcane/Core/src/Arcane/Physics/Narrowphase/Gjk.hpp Arcane/Core/src/Arcane/Physics/Joints/Joint.hpp Arcane/Core/src/Arcane/Physics/Joints/Joints.hpp Arcane/Core/src/Arcane/Physics/Joints/Joints.cpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp
git commit -m "docs(arcane/physics): adopt MKS-DEFER(Pn) markers for deferred px-era Core constants (MKS P2)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466"
```

---

### Task 2: Retire the Baumgarte oracle solver + convert the surviving budget file

**USER decision 2026-07-03 (supersedes the P1 "flip the locals" carry-forward):** Baumgarte is a test-only A/B oracle from the M6 port era — nothing in production selects it, no test asserts cross-solver equality, and SoftStep's own parity/determinism/MT coverage now exceeds what the diverged PGS port adds. Retire it instead of dragging it through MKS and every future change. Two commits: (1) retirement, **byte-identical for every surviving SoftStep assertion**; (2) MKS conversion of the renamed budget file. The old plan's restitution-plumbing test is MOOT — SoftStep already reads `WorldDef::restitutionThreshold` at runtime (`SoftStep.cpp:689/874`); the foot-gun dies with the solver that had it.

**Files:**
- Delete: `Arcane/Core/src/Arcane/Physics/Solver/Baumgarte.cpp`, `Baumgarte.hpp` (git rm)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp`, `PhysicsWorld.cpp` (plumbing removal), `Solver/Solver.hpp` + `Solver/SoftStep.hpp` (comments only)
- Rename: `Arcane/Tests/src/PhysicsBaumgarteTest.cpp` → `PhysicsSolverBudgetTest.cpp` (git mv; SoftStep-only restructure, then MKS conversion; 11 PX-PINs deleted)
- Modify: `Arcane/Tests/src/PhysicsDeterminismTest.cpp`, `PhysicsPhase2HarnessTest.cpp`, `PhysicsJointsTest.cpp`, `PhysicsSimdSolverTest.cpp`, `PhysicsInvariantsTest.cpp` (Baumgarte selection/reference removal ONLY — JointsTest and the others STAY PINNED px; their conversions belong to later tasks/phases)

**Interfaces:**
- Consumes: the complete consumer inventory in Global Constraints (grep-verified 2026-07-03).
- Produces: a single-solver engine — `PhysicsWorld` constructs SoftStep unconditionally; `SolverKind`/`solverKind`/`velIters`/`VelIters()`/`m_velIters` no longer exist; `ISolver` STAYS (it is the seam SoftStep implements and the future-solver hook). `PhysicsSolverBudgetTest.cpp` fully MKS with SoftStep-only budgets, tags `[physics][solver][budget]`.

**COMMIT 1 — retirement (byte-identical for SoftStep):**

- [ ] **Step 1: Capture pre-retire counts.** From the exe dir: `.\ArcaneTests.exe "[physics]"` — record the exact result line, plus per-file case counts for the 6 touched test files (run each file's cases via its tag or name filter and record). This is the reconciliation base.

- [ ] **Step 2: Remove the Core plumbing.**
  - `PhysicsWorld.cpp`: delete the Baumgarte include (:25); replace the ctor solver switch (:112-119) with an unconditional `return std::make_unique<SoftStep>();` (keep a one-line comment: SoftStep is THE solver — the P2.3 A/B oracle was retired 2026-07-03, MKS P2); delete the `m_velIters` ctor init (:137); update the :1856 warm-start comment (drop the Baumgarte clause).
  - `PhysicsWorld.hpp`: delete the `SolverKind` enum + its comment block (:107-120), the `solverKind` field + comment (:246-252), the `velIters` field + comment (:254-256), the `VelIters()` accessor + comment (:1151-1153), the `m_velIters` member + comment (:1453-1454); update the solver-seam comments (:1042, :1053, :1459-1462) to single-solver phrasing.
  - `git rm Arcane/Core/src/Arcane/Physics/Solver/Baumgarte.cpp Arcane/Core/src/Arcane/Physics/Solver/Baumgarte.hpp`.
  - `Solver/Solver.hpp`: comment updates only (:12-14, :31, :141-142, :234-241, :277) — describe the single-solver reality; `ISolver` and `SolverContext` STAY unchanged.
  - `Solver/SoftStep.hpp`: delete the ":116 (for Baumgarte, which still keeps its own m_cache)" clause; the :6 historical Lua-origin comment may stay (it describes the port lineage, not the retired class).
  - **Do NOT touch "Baumgarte" technique-name comments** (Joint.hpp, Joints.cpp, SoftStep.cpp bias math — see Global Constraints).

- [ ] **Step 3: Remove the test-side selection.**
  - `git mv Arcane/Tests/src/PhysicsBaumgarteTest.cpp Arcane/Tests/src/PhysicsSolverBudgetTest.cpp`, then restructure to SoftStep-only: drop every `for (SolverKind kind : {...})` loop (straight-line SoftStep cases); `MakeWorldDef(kind, gravityY)` → `MakeWorldDef(gravityY)`; delete the `wd.velIters = 32u` line (:262 — the knob is gone; the high-mass-ratio case KEEPS running under SoftStep's fixed regime); `BudgetFor(SolverKind)` collapses to the SoftStep budget constants; delete the `SolverWarmStartCacheSize() < 100u` assert (:232 — it bounded the retired solver's private cache; SoftStep warm-starts via the contact pool); the per-solver run-twice determinism case keeps its SoftStep half only; update the file header comment; retag `[physics][solver][baumgarte]` → `[physics][solver][budget]`. Content stays px-pinned in this commit — conversion is Commit 2.
  - `PhysicsDeterminismTest.cpp`: `RunScene(SolverKind solverKind, ...)` → drop the parameter (SoftStep implicit; delete the `wdef.solverKind` line :173); delete the Baumgarte TEST_CASE (:517-523); update the cross-solver comments (:33, :63-64, :512-515).
  - `PhysicsPhase2HarnessTest.cpp`: same — drop the `RunScene` solver parameter (delete :135), delete the Baumgarte TEST_CASE (:242+), update comments (:80-82, :238-239).
  - `PhysicsJointsTest.cpp` (P5 file — STAYS PINNED px, nothing else changes): `GravityWorld(SolverKind kind)` → `GravityWorld()` (delete the :80 assignment); delete the two `run(SolverKind::Baumgarte);` calls (:120, :152); if `run` only parameterized the kind, collapse it to direct calls.
  - `PhysicsSimdSolverTest.cpp`: delete `wd.solverKind = SolverKind::SoftStep;` (:1028) — the default is now unconditional.
  - `PhysicsInvariantsTest.cpp`: comment references (:120, :168) → `PhysicsSolverBudgetTest`.

- [ ] **Step 4: Regen + build + BYTE-IDENTICAL gate.** File set changed → `ThirdParty\premake5\premake5.exe vs2026` from `Arcane\` (NOT GenerateProjects.bat — hangs on `pause`). Build, then `.\ArcaneTests.exe "[physics]"`. Expected: Step-1 counts MINUS exactly the deleted Baumgarte-side assertions (Baumgarte halves of the A/B loops + the cache assert + the 2 deleted TEST_CASEs + JointsTest's 2 Baumgarte runs). **Reconcile the arithmetic explicitly in your report (P1-style): every surviving SoftStep assertion count must be IDENTICAL.** Any other delta = the retirement shifted SoftStep behavior somewhere — STOP and find it.

- [ ] **Step 5: Commit 1** (explicit paths; note the rename + deletions):

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Core/src/Arcane/Physics/Solver/Solver.hpp Arcane/Core/src/Arcane/Physics/Solver/SoftStep.hpp Arcane/Tests/src/PhysicsSolverBudgetTest.cpp Arcane/Tests/src/PhysicsDeterminismTest.cpp Arcane/Tests/src/PhysicsPhase2HarnessTest.cpp Arcane/Tests/src/PhysicsJointsTest.cpp Arcane/Tests/src/PhysicsSimdSolverTest.cpp Arcane/Tests/src/PhysicsInvariantsTest.cpp
# (git rm/git mv already staged the deletions + rename)
git commit -m "refactor(arcane/physics): retire Baumgarte oracle solver -- SoftStep is THE solver (MKS P2, user decision 2026-07-03)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466"
```

**COMMIT 2 — convert PhysicsSolverBudgetTest to MKS:**

- [ ] **Step 6: Convert the file.** Recipe (content mirrors PhysicsSolverTest's family):
  - `MakeWorldDef(gravityY)`: delete its 5 PX-PIN lines; callers pass `Real(10)` (or `Real(0)` where they passed 0).
  - Kinematic case's inline WorldDef: delete its 6 pins; keep explicit `gravityX = gravityY = Real(0)` as a zero-g scene statement.
  - Geometry: floor `(0, 5), hw 50, hh 5` → `(0, 0.5), hw 5, hh 0.5` (top stays y=0); ball/box half-extent & radius `2` → `0.2`; drop heights `20-40` → `2-4`; kick `120` → `12` m/s; kinematic `60` → `6` m/s; densities 1 and 100 unchanged (the mass-ratio scenario is the point).
  - Budgets: re-derive the SoftStep `{ball, stack, massRatio}` penetration budgets empirically at MKS (run, record maxPen, ×1.5 headroom, justify vs kLinearSlop/kSkin in a comment). Expect kSkin-scale numbers (~0.01-0.05 m).
  - Step counts: falls of 2-4 m under g=10 need ~0.6-0.9 s (38-54 steps) to land — re-check every fixed step loop per Conversion Protocol rule 5.
- [ ] **Step 7: Build + run + triage.** `.\ArcaneTests.exe "[budget]"` green; `grep -c "PX-PIN" Arcane/Tests/src/PhysicsSolverBudgetTest.cpp` == 0; then `.\ArcaneTests.exe "[physics]"` — triage per protocol (this file's changes are self-contained).
- [ ] **Step 8: Commit 2**

```bash
git add Arcane/Tests/src/PhysicsSolverBudgetTest.cpp
git commit -m "test(arcane/physics): PhysicsSolverBudgetTest -> MKS content, pins deleted (MKS P2)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466"
```

---

### Task 3: Convert PhysicsDynamicsTest (the analytic idiom-setter)

**Files:**
- Modify: `Arcane/Tests/src/PhysicsDynamicsTest.cpp` (376 lines, 7 cases `[physics][dynamics]`, 36 pins scattered across all 7 cases)

**Interfaces:**
- Consumes: MKS WorldDef defaults (P1) — this file's asserts are closed-form analytic (`expV = g*dt*k`, damped recurrence, terminal `g/damp`, kinematic `y0 + v*t`), so they recompute from the authored constants with no re-measurement.
- Produces: the conversion idiom later tasks imitate — analytic targets expressed from authored constants, impulses as mass × Δv.

- [ ] **Step 1: Convert content.** Recipe:
  - `g = Real(400)` → `Real(10)` in every gravity case; the determinism case's `gravityX=50, gravityY=400` → `gravityX = Real(2.5), gravityY = Real(10)` (both axes stay nonzero — that's the case's point).
  - Circle radius 1 → `0.5`; positions `(10,-5)`/`(1,2)` → round meters (e.g. `(1,-0.5)`, `(1,2)` is already fine); linear damping 2 / 0.3 (1/s, unit-free) unchanged; densities unchanged.
  - ApplyImpulse cases: rewrite as mass × Δv per protocol rule 3 (e.g. `const Real mass = ddef.density * kPi * r * r; ApplyImpulse(h, mass * Vec2(Real(2), Real(-1)));`) and let the existing analytic reference (`expDV = J * invMass`) recompute.
  - Kinematic case velocity 20 → `2` m/s; its closed-form `5 + 3*kStep*20` recomputes from the authored numbers.
  - Sub-stepped references reading `WorldDef{}.substepCount` are unit-free — untouched.
  - Delete all 36 PX-PIN lines; zero-g cases (:221, :307) keep explicit zero gravity sans marker.
  - Step counts: pure-integration cases are step-exact (no settle waits) — formulas track whatever count is used; leave counts unless a case waits for terminal velocity (terminal `g/damp = 10/2 = 5` m/s is approached with time-constant 1/damp = 0.5 s — verify the existing loop length still converges within the assert's epsilon).
- [ ] **Step 2: Build + run + triage.** `.\ArcaneTests.exe "[dynamics]"` green; `grep -c "PX-PIN" Arcane/Tests/src/PhysicsDynamicsTest.cpp` == 0; then `"[physics]"` green (triage per protocol — this file's changes are self-contained, cross-file shifts are unexpected).
- [ ] **Step 3: Commit**

```bash
git add Arcane/Tests/src/PhysicsDynamicsTest.cpp
git commit -m "test(arcane/physics): PhysicsDynamicsTest -> MKS content, pins deleted (MKS P2)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466"
```

---

### Task 4: Convert PhysicsSolverTest

**Files:**
- Modify: `Arcane/Tests/src/PhysicsSolverTest.cpp` (717 lines, 11 cases `[physics][solver]`, 56 pins scattered across ~11 inline WorldDef blocks)

**Interfaces:**
- Consumes: T3's idiom. Body helpers `AddBox`/`AddCircle`/`AddFloor`/`Speed` (:61-102) take caller-passed sizes — helpers themselves need no edit beyond callers' meter arguments.
- Produces: the solver-behavior cluster at MKS; its px-magnitude tolerance family re-derived against named constants.

- [ ] **Step 1: Convert content.** Recipe (÷10 family):
  - Every WorldDef block: `gravityY = 400` → delete the line (default is now 10) or set `Real(10)` explicitly where the case comments on gravity; delete all pins; the kinematic zero-g case (:416-421) keeps explicit zero gravity sans marker.
  - Floor `(0,5), hw 50, hh 5` → `(0,0.5), hw 5, hh 0.5` (top y=0 preserved); friction-slide floor hw 200 → 20; bodies r/half 2 → `0.2`; drop heights 20-40 → 2-4; kick velocity 120 → `12` m/s; kinematic velocity 60 → `6` m/s (its position formula `-10 + 60*t` → `-1 + 6*t`).
  - Tolerance re-derivation (per protocol rule 6, justify each): `penetration < 0.1` → expect kSkin-scale (`~0.02-0.03`); `maxPen < 0.21` → likewise; settle checks `Speed(...) < 1.0` → sleep-threshold-scale (`< 0.1`); `maxDrift < 0.5` → `0.05`; `peakKE < 50` → recompute from the new masses/velocities empirically (mass scales ~×1/100 at ÷10 sizes, v² ~×1/100); rebound-fraction and `maxX > 2.0`-style relational bounds rescale with their driving lengths.
  - Warm-start/contact-count/`std::isfinite`/run-twice cases: assert-stable, content rescale only.
  - Step counts per protocol rule 5 (drops now take ~2× the steps).
- [ ] **Step 2: Build + run + triage.** `.\ArcaneTests.exe "[solver]"` green (includes the T2-converted `PhysicsSolverBudgetTest` — both share `[solver]`); file PX-PIN grep == 0; `"[physics]"` green.
- [ ] **Step 3: Commit**

```bash
git add Arcane/Tests/src/PhysicsSolverTest.cpp
git commit -m "test(arcane/physics): PhysicsSolverTest -> MKS content, pins deleted (MKS P2)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466"
```

---

### Task 5: Convert PhysicsRotationTest

**Files:**
- Modify: `Arcane/Tests/src/PhysicsRotationTest.cpp` (425 lines, 5 cases `[physics]`, 11 pins — centralized in `MakeGravityWorld` (:67-77) + one inline zero-g case (:245-250))

**Interfaces:**
- Consumes: `MakePolygon` spinning-rect rule (file already complies via `MakeBoxPolygon` :55, `AddRotatingBox` :91 — keep).
- Produces: rotation/tipping behavior at MKS with the y-window baselines re-derived from authored constants instead of magic numbers.

- [ ] **Step 1: Convert content.** Recipe (÷10 family):
  - `MakeGravityWorld`: delete 5 pins; gravity default (delete the 400 line or write `Real(10)`).
  - Zero-g case (:241-250): delete 6 pins, keep explicit zero gravity.
  - Geometry: floor hw 200 → 20, hh 5 → 0.5, floorTop 100/200 → 10/20; boxes half 10×10 → `1×1`; drop positions 150/92/165 → 15/9.2/16.5 (round where the scenario allows); circle fixture r 6 @ (18,4) → 0.6 @ (1.8, 0.4); impulse 1000 → mass × Δv per protocol rule 3.
  - Re-express the y-window bounds from authored constants where feasible instead of re-hardcoding (e.g. `finalPos.y >= dropY`, `<= floorTop + hh*something`) — the survey flags :171-172's `[150, 230]` window family as the file's only magic-px sites; each re-derived bound carries its justification comment.
  - Angle asserts (`angleResidual < 0.05`, `AngleToNearestHalfPi`, `fabs(finalAngle) < 1e-4`) are radian/unit-free — unchanged.
  - `ActiveContactCount() >= 2` behavioral — must pass unmodified.
  - Step counts per protocol rule 5 (tip-and-settle sequences slow down in meters; 150-step loops likely need ~2× — verify empirically).
- [ ] **Step 2: Build + run + triage.** File tag `"[physics]"` is broad — run the file by name: `.\ArcaneTests.exe -f` is not set up, so run the named cases or just `"[physics]"` (fast enough); file PX-PIN grep == 0.
- [ ] **Step 3: Commit**

```bash
git add Arcane/Tests/src/PhysicsRotationTest.cpp
git commit -m "test(arcane/physics): PhysicsRotationTest -> MKS content, pins deleted (MKS P2)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466"
```

---

### Task 6: Convert PhysicsCompoundComTest + PhysicsCompoundSlideTest

**Files:**
- Modify: `Arcane/Tests/src/PhysicsCompoundComTest.cpp` (462 lines, 5 cases, 28 pins — inline per-case + `RunHeavyTip` helper :154-204)
- Modify: `Arcane/Tests/src/PhysicsCompoundSlideTest.cpp` (307 lines, 3 cases, 15 pins — inline per-case; `MakeLopsided` :84-104 is the sandbox-derived body)

**Interfaces:**
- Consumes: T3's mass × Δv impulse idiom (CompoundCom's analytic asserts — `expectedAngVel = -lc.x*J/I`, `J/mass` — recompute from it directly).
- Produces: compound-COM behavior at MKS; the CompoundSlide scene lands at the same scale the P6 sandbox re-author will use (its content came FROM the sandbox at ~100 px/m — use ÷100 here).

- [ ] **Step 1: Convert PhysicsCompoundComTest.** Recipe (÷10 family):
  - Delete all 28 pins (three zero-g cases keep explicit zero gravity; `RunHeavyTip` and case (d) drop the 400 line for the default 10).
  - Barbell: r 3 → 0.3, heavy offset 20 → 2, densities 1/9 unchanged → COM x = 0.9 × offset = 1.8; **rewrite the `Approx(18)` COM assert as `Approx(Real(0.9) * offset)`** (analytic form, not a new magic number). Pivot floor `MakeAabb(8,5)` → `(0.8, 0.5)` with position keeping top-at-y=0. Positions y=-3 → -0.3, (7,-4) → (0.7,-0.4). Impulse 1000 → mass × Δv. omega 0.5 rad/s unchanged.
  - The measured-px-trajectory table in comments (:229-240) is documentation of the old scale — replace its header with one line noting values predate MKS (directional signs asserted below it are scale-free).
  - Closed-form free-fall reference (case d) recomputes from g=10 + substepCount — formula untouched.
  - Step counts per protocol rule 5.
- [ ] **Step 2: Convert PhysicsCompoundSlideTest.** Recipe (÷100 — sandbox provenance):
  - `kGravity 400` → `Real(10)`; delete all 15 pins.
  - Floor top 620 → 6.2, hw 560 → 5.6, hh 20 → 0.2; walls x ±90 → ±0.9, hh 140 → 1.4; lopsided core r 18 → 0.18 / heavy r 22 → 0.22 at localPos ±40 → ±0.4, densities 0.5/4.0 unchanged; PROBE r 12 → 0.12, offset 40 → 0.4; drops y 360-420 → 3.6-4.2, spacing 55 → 0.55.
  - Ratio asserts (`lateMax <= impactPeak*1.05`, `finalKE < impactPeak*0.02`) are scale-free — unchanged. The two px bounds re-derive: `Position(b).y < 700` → `< 7`; `lateMax < 50` (KE) → re-measure empirically (KE scales ~×1e-6 at ÷100 sizes: mass ×1/1e4, v² ×1/1e2 — expect a tiny budget; justify).
  - `ActiveContactCount() >= 2`, distinct-id, `warmStarted >= 2` behavioral — unmodified.
  - Step counts: 540-step settle runs cover ~1 s falls comfortably — verify per rule 5.
- [ ] **Step 3: Build + run + triage.** `"[physics]"` green; both files' PX-PIN grep == 0.
- [ ] **Step 4: Commit**

```bash
git add Arcane/Tests/src/PhysicsCompoundComTest.cpp Arcane/Tests/src/PhysicsCompoundSlideTest.cpp
git commit -m "test(arcane/physics): compound COM/slide tests -> MKS content, pins deleted (MKS P2)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466"
```

---

### Task 7: Convert PhysicsSimdSolverTest + PhysicsCompactedSolveTest + PhysicsBodyContactsTest

**Files:**
- Modify: `Arcane/Tests/src/PhysicsSimdSolverTest.cpp` (1123 lines but only 2 of 13 cases build worlds; 11 pins in those 2 cases)
- Modify: `Arcane/Tests/src/PhysicsCompactedSolveTest.cpp` (157 lines, 4 cases `[physics][phasec]`, 20 pins)
- Modify: `Arcane/Tests/src/PhysicsBodyContactsTest.cpp` (66 lines, 1 case `[physics][island]`, 5 pins)

**Interfaces:**
- Consumes: nothing new — the SIMD packer/solver math fixtures (11 of 13 SimdSolver cases) are px-agnostic hand-built data and are NOT touched.
- Produces: the remaining `[phasec]`/adjacency coverage at MKS.

- [ ] **Step 1: Convert PhysicsSimdSolverTest (2 world cases ONLY).** SyncIn/SyncOut (:69-76): delete 6 pins; g default; circle r 1 → keep `1` (1 m is in the happy range) or `0.5`; static box `(20,1)` → keep (meters already sane) — round per judgment. Overflow-hub (:1026-1051): delete 5 pins (the `solverKind = SoftStep` line was already removed in T2 — the field is retired); floorTop 300 → 3, static `(200,5)` → `(20,0.5)`, hub r 12 → 1.2, ring r 3 → 0.3; `ke < keBound` re-derives empirically; `Position(hub).y + 12 <= floorTop + 1` rewrites from the authored radius/floorTop (`+ hubR <= floorTop + 0.1` — justify). Direct-solve fixtures (`substeps = 4`, `h = (1/60)/4`, `maxBiasVel=4.0`, `threshold=1.0`, seeds, `MakeCC` et al.) — DO NOT TOUCH (solver-math contracts, not world content).
- [ ] **Step 2: Convert PhysicsCompactedSolveTest.** Delete all 20 pins; g default 10. Floors `(200,5)`/`(400,5)` → `(20,0.5)`/`(40,0.5)`; boxes half 4/5 → 0.4/0.5; kinematic plate `(60,2)` → `(6,0.2)`, its `SetVelocity (3,0)` → `(0.3, 0)` (keeps the plate-crawl character); drop positions y −8..−50 → −0.8..−5.0, spacing 9 → 0.9. Behavioral membership/coloring validators and both run-twice bitwise cases must pass unmodified. Note: at MKS the settled boxes will genuinely SLEEP (threshold 0.05) during the 600-step runs — that is valid for run-twice identity and exercises the sleep path; do not pin sleepThreshold to prevent it.
- [ ] **Step 3: Convert PhysicsBodyContactsTest.** Delete 5 pins; g default. Floor `(0,200) (300,20)` → `(0,20) (30,2)`; spawn bounds ±200/±150 → ±20/±15; circle r 6-12 → 0.6-1.2; `MakeAabb(8,8)` → `(0.8,0.8)` (fixedRotation stays true); capsule `(10,5)` → `(1,0.5)`. The LCG `rnd` seeds/sequence unchanged (bounds are arguments). `DebugValidateBodyContacts()` before/after 200 steps — behavioral, unmodified.
- [ ] **Step 4: Build + run + triage.** `.\ArcaneTests.exe "[simd]"`, `"[phasec]"`, `"[island]"` green; all three files' PX-PIN grep == 0; `"[physics]"` green.
- [ ] **Step 5: Commit**

```bash
git add Arcane/Tests/src/PhysicsSimdSolverTest.cpp Arcane/Tests/src/PhysicsCompactedSolveTest.cpp Arcane/Tests/src/PhysicsBodyContactsTest.cpp
git commit -m "test(arcane/physics): SIMD/compacted/body-contacts tests -> MKS content, pins deleted (MKS P2)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466"
```

---

### Task 8: Convert PhysicsPersistentContactTest + PhysicsPersistentIslandTest

**Files:**
- Modify: `Arcane/Tests/src/PhysicsPersistentContactTest.cpp` (464 lines, 7 cases, ~40 pins — 6-pin zero-g form in 5 cases, 5-pin form in 2)
- Modify: `Arcane/Tests/src/PhysicsPersistentIslandTest.cpp` (306 lines, 6 cases `[physics][island]`, 30 pins)

**Interfaces:**
- Consumes: protocol rule 4 — this task is its main customer (slop-scale stack seatings).
- Produces: contact/island lifecycle coverage at MKS.

- [ ] **Step 1: Convert PhysicsPersistentContactTest.** Recipe (÷10 family):
  - Delete all pins; the 5 zero-g cases keep explicit zero gravity sans markers; cases 3-4 use default g=10.
  - Halves 10 → 1, 5 → 0.5, circles r 5 → 0.5, floor `(50,5)` → `(5,0.5)`.
  - **Stack seatings re-derive from slop (rule 4):** the px stack `-9.8/-19.6/-29.4` encoded 0.2 px-deep seatings (floor top −5, half 5 → rest −10, authored −9.8). At MKS with floor top −0.5 and half 0.5: rest −1.0, author `−0.99` etc. — a `2*kLinearSlop` seat, stated in a comment. Same treatment for every deliberate overlap in the file.
  - Teleport target `(10000,10000)` → `(1000,1000)` (still "far"). Oracle tolerance `kEps = 1e-4` (:141): keep — it is ABSOLUTE meters now, i.e. physically tighter, which is the right direction; if the oracle comparison fails at 1e-4, that is a finding to report, not a tolerance to loosen.
  - Tile-span case (:255-335): `kCellSize 20` → `Real(1)` (the MKS residency-tile scale), box half 5 → 0.5 at `(130,150)` → `(6.5, 7.5)`, `spanTopY = 10*kCellSize` → recomputes to 10; the rest assert margins `0.5` → `0.05`; `p.y < spanTopY` unchanged in form (no-tunnel gate — behavioral, unmodified).
- [ ] **Step 2: Convert PhysicsPersistentIslandTest.** Delete all 30 pins (case 1 zero-g explicit). Floors `(200,5)`/`(400,5)`/`(50,5)` → `(20,0.5)`/`(40,0.5)`/`(5,0.5)`; boxes half 4/5 → 0.4/0.5; chain gap 0.5 → 0.05; far body x 500 → 50. Fling velocities `(400,-2000)`/`(±600,-2500)` → `(4,-20)`/`(±6,-25)` m/s (fast separation, far under the 400 cap); impulses `(0,-8000)`/`(150,-3000)` → mass × Δv per rule 3. All island-root/awake asserts behavioral — unmodified. Case 5 run-twice identity (positions + awake + roots) scale-independent — unmodified. Sleep-wait loops (700/500 steps): at MKS threshold 0.05 the settle-then-sleep still completes well within — verify per rule 5.
- [ ] **Step 3: Build + run + triage.** `"[island]"` + `"[physics]"` green; both files' PX-PIN grep == 0.
- [ ] **Step 4: Commit**

```bash
git add Arcane/Tests/src/PhysicsPersistentContactTest.cpp Arcane/Tests/src/PhysicsPersistentIslandTest.cpp
git commit -m "test(arcane/physics): persistent contact/island tests -> MKS content, pins deleted (MKS P2)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466"
```

---

### Task 9: Convert PhysicsDeterminismTest + Phase1/Phase2 harnesses

**Files:**
- Modify: `Arcane/Tests/src/PhysicsDeterminismTest.cpp` (2 cases after T2's Baumgarte-case deletion, `[physics][determinism]`; 4 pins centralized in `RunScene` + UNTAGGED `gravityY=300` and `hashCellSize=64`; pre-T2 line refs — locate by content)
- Modify: `Arcane/Tests/src/PhysicsPhase1HarnessTest.cpp` (293 lines, 4 cases, 5 pins centralized in `MakeDef` :125 + untagged `hashCellSize=32` / `tileCellSize=16`)
- Modify: `Arcane/Tests/src/PhysicsPhase2HarnessTest.cpp` (4 cases after T2's Baumgarte-case deletion; 4 pins centralized in `RunScene` + untagged `gravityY=300` / `hashCellSize=64`; pre-T2 line refs — locate by content)

**Interfaces:**
- Consumes: probe-battery CCD result (150 m/s validated under the 400 cap, 2026-07-03) for the bullet gate.
- Produces: the determinism/harness coverage at MKS. Hash formulas (`floor(x*1000)` — now mm resolution) are UNCHANGED; hash values are self-derived (never asserted literally), so identity asserts carry.

- [ ] **Step 1: Convert PhysicsDeterminismTest.** All in `RunScene` + scene constants (:87-120):
  - Delete 4 pins; `gravityY 300` → `Real(10)` (untagged — convert anyway); `hashCellSize 64` → delete (default 1.0); broadphase pinned to Tree stays (deliberate).
  - Geometry ÷10: floor `(300,12)@(200,250)` → `(30,1.2)@(20,25)`; thin wall `(1,80)@(350,100)` → `(0.1,8)@(35,10)` (`kWallNearX` derives from the authored constants); circles r 8 → 0.8 (restitution 0.3 unchanged); anchors r 3 → 0.3; stirrer r 10 → 1.0, vel `(60,0)` → `(6,0)`; rotating `BoxPolygon(10,10)@(200,180)` → `(1,1)@(20,18)`, init angle 0.6 rad unchanged (MakePolygon rule already satisfied); deep-overlap ball r 5 → 0.5 buried in static `(20,20)` → `(2,2)`.
  - **Bullets:** r 2 → 0.2; `kBulletSpeed = 300/kDt = 18000` → `Real(150)` m/s flat (probe-validated; per-step travel 2.5 m >> 0.2 m wall thickness — genuinely tunnels without CCD, under the 400 cap). The anti-tunnel gate `pos.x <= kWallNearX` is behavioral — unmodified in form.
  - Scripted impulses `(0,-300)`/`(2000,0)`/`(80,-120)` → mass × Δv per rule 3 at steps 60/120 (unchanged steps — they keep the scene live past MKS sleep onset).
  - Run-twice hash cases: assert-stable (h1==h2, h1!=0).
- [ ] **Step 2: Convert PhysicsPhase1HarnessTest.** All in `MakeGrid`/`MakeDef`: delete 5 pins (zero-g kinematic scene keeps explicit zero gravity); `kCell 16` → `Real(1)`; `tileCellSize 16` → `Real(1)`; `hashCellSize 32` → `Real(2)` (preserves the 2-cells relation); obstacle `(12,12)@(136,136)` → `(0.75,0.75)@(8.5,8.5)`; movers r 6 → `0.4` (preserves ~r/cell overlap character); scripted phase velocities ±40 → ±2.5 (40/16 cells-per-second preserved). Cross-broadphase + run-twice hash asserts unmodified.
- [ ] **Step 3: Convert PhysicsPhase2HarnessTest.** All in `RunScene`: delete 4 pins; `gravityY 300` → `Real(10)`; `hashCellSize 64` → delete (default 1.0). Floor `(220,12)@(150,220)` → `(22,1.2)@(15,22)`; balls r 8 → 0.8 at `(60+40i, 40+10i)` → `(6+4i, 4+1i)`; stirrer r 10 → 1.0 vel `(50,0)` → `(5,0)`; DistanceJoint length 40 → 4; impulses → mass × Δv at steps 60/120. Run-twice + cross-broadphase hash asserts unmodified.
- [ ] **Step 4: Build + run + triage.** `.\ArcaneTests.exe "[determinism]"` green ×3 files; all three PX-PIN greps == 0; `"[physics]"` green. Sanity guard: the determinism scenes must stay LIVE at MKS (stirrer + scheduled impulses re-wake sleepers); if a scene goes fully asleep early, the hash still passes but coverage weakens — check the final-step positions differ from initial (report if a scene died early; fix by nudging scripted impulse timing, not by touching sleep).
- [ ] **Step 5: Commit**

```bash
git add Arcane/Tests/src/PhysicsDeterminismTest.cpp Arcane/Tests/src/PhysicsPhase1HarnessTest.cpp Arcane/Tests/src/PhysicsPhase2HarnessTest.cpp
git commit -m "test(arcane/physics): determinism + phase harnesses -> MKS content, pins deleted (MKS P2)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466"
```

---

### Task 10: Phase 2 exit verification

**Files:** none (verification + report only; fixes loop back into the owning task's file).

**Interfaces:**
- Consumes: everything above.
- Produces: the Phase-2 exit evidence for the whole-branch review.

- [ ] **Step 1: Burn-down check.** `grep -rc "PX-PIN" Arcane/Tests/src Arcane/Sandbox/src | grep -v ":0"` — total must be **670** (946 − 276), with ZERO hits in the 14 converted files. `grep -rn "MKS-DEFER" Arcane/Core/src` still lists exactly the 5 T1 sites.
- [ ] **Step 2: Full suite.** Build, then `.\ArcaneTests.exe ~[gpu]` (budget ≥25 min) — all green; record exact counts vs the T1 baseline with a one-line delta explanation (new plumbing test case + any count shifts from re-baselines). Run the isolated SandboxSmoke `[gpu]` pair separately (names need `\,` escapes) — green.
- [ ] **Step 3: Headless GPU-verify.** `Arcane\bin\Debug-windows-x86_64-md\Loom\Loom.exe --frames 180` — clean exit, `RenderErrorCount() == 0` path green (sandbox is still pinned px content in P2 — it must behave exactly as before).
- [ ] **Step 4: Assemble the exit report.** Consolidated justification table (every re-baselined assertion across T2-T9 with its named driving constant), the pin burn-down number, suite counts, and the carry-forward reminders for the next plans: P3 = sleep/settle cluster (probe report numbers to copy in), P4 = CC retunes + kShapeCastTol + `[mks]` blind-spot note (residency tile / SpatialHash default have no regression tripwire), P5 = MouseJoint maxForce, P6 = wall-time restore assert (~1087 s px-era `~[gpu]` should drop once sandbox content stops churning the 0.05 margin).
- [ ] **Step 5: Push the branch** (`git push -u origin feature/arcane-physics-mks-phase2`), then STOP — whole-branch review + merge to main = USER's call.

---

## Self-Review (author checklist — completed; re-run after the 2026-07-03 retire amendment)

- **Spec coverage:** §4 P2 cluster list (14 files) → T2-T9 one-for-one (PhysicsBaumgarteTest survives as the renamed SoftStep budget file); §2 authoring rules → Conversion Protocol rules 1-5; §6 per-phase acceptance + parity contingency → protocol rule 6 + per-task triage; P1 carry-forwards → T1 (MKS-DEFER convention), T2 (the Baumgarte-locals carry-forward is SUPERSEDED by the user's retire decision — deletion satisfies it, foot-gun comment included).
- **Placeholder scan:** every conversion task carries its concrete old→new recipe table and named magic-assert sites from the 2026-07-03 surveys; the only execution-time-derived numbers are empirical re-baselines, which the protocol defines procedurally (run → record → 1.5× headroom → justify) — that is the designed mechanism, not a TBD.
- **Type consistency:** `kLinearSlop`/`kSkin` names match PhysicsTypes.hpp (P1-verified); WorldDef field names match the P1 pins being deleted; the T2 removal inventory (SolverKind :116-120, solverKind :252, velIters :256, VelIters() :1153, m_velIters :1454, ctor switch :112-119) grep-verified 2026-07-03.
- **Ordering:** T2 retires the solver BEFORE any conversion task meets an A/B loop (T3-T9 never see SolverKind); its Commit 1 is gated byte-identical for every surviving SoftStep assertion, so the retirement cannot be confused with conversion shifts; T3 sets the analytic idiom before the heavier files consume it.
