# Arcane MKS Units — Phase 1 (Flip + Pin) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the MKS constants flip in Core with PX-PIN scaffolding so the whole suite stays green: pin pass (no-op) -> runtime WorldDef default flips (identical counts) -> compile-time constant flips (green, justified re-baselines).

**Architecture:** Three sequential stages on one branch (spec §4 Phase 1). Stage i writes today's default values explicitly into every test/sandbox WorldDef site (greppable `PX-PIN` markers). Stage ii flips the five runtime WorldDef defaults to cite-checked Box2D v3 values behind a new defaults test. Stage iii flips the compile-time length constants (kSkin, DynamicTree::kMargin, residency tile, contact-pad floor) and runs a length-literal audit; px-scale tests may shift and each re-baseline carries a written justification.

**Tech Stack:** C++23 (Arcane Core/Physics), MSVC via msbuild, Catch2. Parity source: vendored `ThirdParty/box2d-3.1.1` — **any question or invariant during implementation is checked against it directly (user directive), never recalled.**

**Phases 2-6 are separate plan documents,** written when their turn comes (spec §4): P2 solver/dynamics cluster, P3 sleep/settle (acceptance risk), P4 broadphase/spatial + CharacterController retunes, P5 CCD/clamp/joints, P6 sandbox + camera PPM.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-02-arcane-physics-mks-units-design.md` (constants table §3 with vendored cites; Phase-1 three-stage acceptance §4).
- **Vendored cites (verified 2026-07-02):** `types.c:12-13` gravity (0,-10) y-up; `:15` restitutionThreshold 1.0; `:16` maxContactPushSpeed 3.0; `:17-18` hertz 30 / damping 10; `:21` maximumLinearSpeed 400; `:34` sleepThreshold 0.05 (BodyDef-level in b2); `constants.h:23` slop 0.005; `:33` maxRotation 0.25*pi; `:38` B2_SPECULATIVE_DISTANCE = 4*slop; `:44` B2_AABB_MARGIN **0.05** (v3.1.1 — NOT older 0.1); `:47` timeToSleep 0.5.
- **Do NOT flip in Phase 1:** `kLinearSlop` (already 0.005), `maxLinearVelocity` (already 400), `kMaxRotation`, `kSleepTime`, `tileCellSize` (stays 1), CharacterController constants (P4).
- **Build (PowerShell, NOT Git Bash):** `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m`. Pre-existing noise: the `Bench` project fails (gitignored slnx ghost, sources on an unmerged branch) — ignore; Core/Arcane/ArcaneTests/Sandbox/Loom must be clean.
- **Tests from exe dir** `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\`. Full-suite runs use `.\ArcaneTests.exe ~[gpu]` (the two SandboxSmoke `[gpu]` cases contend on the plugin-DLL copy under same-run execution; run them isolated, names need `\,` escapes) . New test .cpp => premake regen `ThirdParty\premake5\premake5.exe vs2026` from `Arcane\` (NOT GenerateProjects.bat — hangs on `pause`).
- **Baseline (capture before ANY edit, Task 1 Step 1):** record exact counts of `~[gpu]` full run and `[physics]` (last known: 276 cases / 28866 assertions) — the identical-counts yardstick for stages i-ii.
- **PX-PIN format (exact, greppable):** `wd.sleepThreshold = Real(8); // PX-PIN: remove when this file converts to MKS`
- **clangd/IDE diagnostics are FALSE POSITIVES** — msbuild is truth.
- **Staging:** stage ONLY per-task files by explicit path. NEVER `git add -A` (unrelated parked changes incl. untracked `ThirdParty/box2d-3.1.1/`). Branch: `feature/arcane-physics-mks-units`.
- **Commit trailers (every commit):**
  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466
  ```

## File Structure

| File | Change |
|---|---|
| `Arcane/Tests/src/Physics*.cpp`, `Arcane/Tests/src/Sandbox*.cpp` (~45 files) | Stage i pins at every WorldDef/PhysicsWorld construction site (T1) |
| `Arcane/Sandbox/src/Sandbox.cpp`, `Arcane/Sandbox/src/SandboxApp.cpp` | Stage i pins in app WorldDef setup (T1) |
| `Arcane/Tests/src/PhysicsMksDefaultsTest.cpp` | NEW — Box2D v3 defaults test (T2, extended T3) |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` | WorldDef default flips + backing members (T2); `m_residencyGrid` 64->1 (T3) |
| `Arcane/Core/src/Arcane/Physics/PhysicsTypes.hpp` | `kSkin` 0.01->0.02 (T3) |
| `Arcane/Core/src/Arcane/Physics/Broadphase/DynamicTree.hpp` | `kMargin` 8->0.05 (T3) |
| `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` | pad floor `Real(2)` -> `DynamicTree::kMargin` (T3) |
| (audit-driven) | length-literal audit fixes, if any (T4) |

---

### Task 1: Stage i — PX-PIN pass (no-op scaffolding)

**Files:**
- Modify: every file in `Arcane/Tests/src/` matching `Physics*.cpp` or `Sandbox*.cpp` that constructs a `PhysicsWorld` (grep inventory in Step 2)
- Modify: `Arcane/Sandbox/src/Sandbox.cpp` (~line 56, `wd.gravityY = kGravityY`), `Arcane/Sandbox/src/SandboxApp.cpp` (~line 48, `InstallFreshPhysicsResource`)

**Interfaces:**
- Produces: every world-construction site in Tests+Sandbox has ALL FIVE flip-set runtime fields explicit: `gravityX`, `gravityY`, `sleepThreshold`, `restitutionThreshold`, `contactPushMaxVelocity`, `hashCellSize` (six field names, five pin values — gravity is two fields). Marker contract: added lines end with `// PX-PIN: remove when this file converts to MKS`.

- [ ] **Step 1: Capture the baseline.** Build current HEAD, then from the exe dir run `.\ArcaneTests.exe ~[gpu]` and `.\ArcaneTests.exe "[physics]"`. Record BOTH exact "assertions in test cases" lines in your report. These are the identical-counts yardstick for T1 and T2.

- [ ] **Step 2: Build the site inventory.** `grep -n "PhysicsWorld w\|PhysicsWorld world\|WorldDef" Arcane/Tests/src/*.cpp Arcane/Sandbox/src/*.cpp` (adjust to catch helper factories — also grep `MakeWorld\|makeWorld\|TestWorld`). Account for EVERY construction site: direct `PhysicsWorld w;` (default ctor), `PhysicsWorld w(wd);`, and per-file helper functions. List the inventory (file -> sites) in your report.

- [ ] **Step 3: Apply the uniform pin rule at every site.** Rule: for each of the six fields, if the site (or its WorldDef) already sets the field, leave it untouched (no marker); otherwise add the OLD default explicitly with the marker. Old defaults: `gravityX = 0`, `gravityY = 0`, `sleepThreshold = 8`, `restitutionThreshold = 20`, `contactPushMaxVelocity = 300`, `hashCellSize = 64`.

Site transformation for bare default-ctor worlds:

```cpp
// BEFORE
PhysicsWorld w;

// AFTER
WorldDef wd;
wd.gravityX               = Real(0);   // PX-PIN: remove when this file converts to MKS
wd.gravityY               = Real(0);   // PX-PIN: remove when this file converts to MKS
wd.sleepThreshold         = Real(8);   // PX-PIN: remove when this file converts to MKS
wd.restitutionThreshold   = Real(20);  // PX-PIN: remove when this file converts to MKS
wd.contactPushMaxVelocity = Real(300); // PX-PIN: remove when this file converts to MKS
wd.hashCellSize           = Real(64);  // PX-PIN: remove when this file converts to MKS
PhysicsWorld w(wd);
```

Site transformation for existing WorldDef blocks: append only the missing fields (e.g. a test that already sets `wd.gravityY = 400` and `wd.hashCellSize = 32` gets pins for gravityX, sleepThreshold, restitutionThreshold, contactPushMaxVelocity only). Files with a shared helper get the pins ONCE in the helper. Do NOT alter any value a test already sets (e.g. `PhysicsNarrowphaseMtTest`'s `sleepThreshold = 0` stays).

Sandbox: `Sandbox.cpp` and `SandboxApp.cpp` WorldDef setup sites get the same missing-field pins (gravityY is already explicit there — leave).

- [ ] **Step 4: Build + run — verify IDENTICAL counts.** Build, then `.\ArcaneTests.exe ~[gpu]` and `"[physics]"` from the exe dir. Expected: exactly the Step-1 counts, all passing. Any deviation means a pin changed a value that differed from the default — find it (diff the failing test's WorldDef against `PhysicsWorld.hpp` defaults) and fix before proceeding.

- [ ] **Step 5: Record the burn-down zero-point.** `grep -rc "PX-PIN" Arcane/Tests/src Arcane/Sandbox/src | grep -v ":0"` — record the total in your report (this number only goes down in P2-P6).

- [ ] **Step 6: Commit** (stage the touched test/sandbox files by explicit path — listing them is fine via `git add Arcane/Tests/src/<each>.cpp Arcane/Sandbox/src/Sandbox.cpp Arcane/Sandbox/src/SandboxApp.cpp`):

```bash
git commit -m "test(arcane/physics): PX-PIN pass — pin px-era runtime defaults explicitly (MKS P1 stage i)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466"
```

---

### Task 2: Stage ii — runtime WorldDef default flips + defaults test

**Files:**
- Create: `Arcane/Tests/src/PhysicsMksDefaultsTest.cpp`
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` (WorldDef defaults ~lines 191-242; backing members ~1446-1448 — locate by content, lines have drifted)

**Interfaces:**
- Consumes: T1's pins (make this flip a no-op for every existing test).
- Produces: `WorldDef` defaults = Box2D v3: `gravityX 0 / gravityY 10`, `sleepThreshold 0.05`, `restitutionThreshold 1.0`, `contactPushMaxVelocity 3.0`, `hashCellSize 1.0`. Backing member initializers match. New tag `[mks]`.

- [ ] **Step 1: Write the failing test** — `Arcane/Tests/src/PhysicsMksDefaultsTest.cpp`:

```cpp
// Box2D v3 default parity for WorldDef (b2DefaultWorldDef) — every value cites
// the vendored source at ThirdParty/box2d-3.1.1. Spec:
// docs/superpowers/specs/2026-07-02-arcane-physics-mks-units-design.md §3.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>

using namespace Arcane::Physics;
using Catch::Approx;

TEST_CASE("WorldDef defaults are Box2D v3 MKS", "[physics][mks]")
{
    WorldDef def;
    CHECK(def.gravityX == Real(0));                        // types.c:12
    CHECK(def.gravityY == Real(10));                       // types.c:13 ((0,-10) y-up -> +10 y-down)
    CHECK(def.sleepThreshold == Approx(Real(0.05)));       // types.c:34 (BodyDef-level in b2)
    CHECK(def.restitutionThreshold == Approx(Real(1)));    // types.c:15
    CHECK(def.contactPushMaxVelocity == Approx(Real(3)));  // types.c:16 maxContactPushSpeed
    CHECK(def.maxLinearVelocity == Approx(Real(400)));     // types.c:21 (unchanged)
    CHECK(def.contactHertz == Approx(Real(30)));           // types.c:17 (unchanged)
    CHECK(def.contactDampingRatio == Approx(Real(10)));    // types.c:18 (unchanged)
    CHECK(def.hashCellSize == Approx(Real(1)));            // Arcane grid tuning (MKS)
}

TEST_CASE("BodyDef.sleepThreshold defaults to inherit", "[physics][mks]")
{
    BodyDef bd;
    CHECK(bd.sleepThreshold == Real(-1)); // inherit WorldDef (Arcane architecture; value carries)
}
```

- [ ] **Step 2: Regen + build + run to verify it FAILS.** `ThirdParty\premake5\premake5.exe vs2026` from `Arcane\` (new .cpp), build, then `.\ArcaneTests.exe "[mks]"` from the exe dir. Expected: FAIL on gravityY (0 != 10), sleepThreshold (8 != 0.05), restitutionThreshold (20 != 1), contactPushMaxVelocity (300 != 3), hashCellSize (64 != 1).

- [ ] **Step 3: Flip the defaults in `PhysicsWorld.hpp`.** Locate by content (WorldDef block; audit anchors: hashCellSize ~:191, gravity ~:202-203, restitutionThreshold ~:225, contactPushMaxVelocity ~:226, sleepThreshold ~:242):

```cpp
            Real hashCellSize = Real(1);   // MKS grid tuning: ~2-4x a typical 0.25-0.5 m body
...
            // Box2D v3 default gravity (types.c:12-13, (0,-10) y-up). Arcane is
            // y-down, so the default is +10. Zero-g worlds must say so explicitly.
            Real gravityX = Real(0);
            Real gravityY = Real(10);
...
            // Box2D v3 b2DefaultWorldDef.restitutionThreshold (types.c:15): 1 m/s.
            Real restitutionThreshold = Real(1);
            // Box2D v3 b2DefaultWorldDef.maxContactPushSpeed (types.c:16): 3 m/s.
            Real contactPushMaxVelocity = Real(3);
...
            // Box2D v3 sleep threshold (types.c:34, BodyDef-level in b2): 0.05 m/s.
            // Supersedes the px-era empirical 8 (2026-06-28 spec) — root cause was
            // scale, not threshold. Sleep test: |v| + |w|*maxExtent < threshold.
            Real sleepThreshold = Real(0.05);
```

Sync the backing member initializers (~:1446-1448): `m_restitutionThreshold = Real(1)`, `m_contactPushMaxVelocity = Real(3)` (m_maxLinearVelocity stays 400). Grep `= Real(20)` / `= Real(300)` / `= Real(8)` / `= Real(64)` in PhysicsWorld.hpp to catch every mirror (the sleep default may also appear in a member initializer — sync it too).

- [ ] **Step 4: Build + run — `[mks]` PASSES, suite counts = baseline + new cases.** Build, `.\ArcaneTests.exe "[mks]"` (pass), then `.\ArcaneTests.exe ~[gpu]` and `"[physics]"`. Expected: T1's recorded counts + exactly the 2 new `[mks]` cases (+11 assertions). ANY other change = an unpinned inheritance site T1 missed — add the missing pin (with marker) and re-run.

- [ ] **Step 5: Commit**

```bash
git add Arcane/Tests/src/PhysicsMksDefaultsTest.cpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp
git commit -m "feat(arcane/physics): WorldDef defaults -> Box2D v3 MKS (gravity +10, sleep 0.05, restitution 1, push 3) (MKS P1 stage ii)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466"
```

---

### Task 3: Stage iii — compile-time length constants + pad floor

**Files:**
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsTypes.hpp` (kSkin ~:154)
- Modify: `Arcane/Core/src/Arcane/Physics/Broadphase/DynamicTree.hpp` (kMargin ~:64)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` (m_residencyGrid ~:1433)
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` (pad floor ~:2717 + comment ~:2710-2712)
- Modify: `Arcane/Tests/src/PhysicsMksDefaultsTest.cpp` (append constants case)
- Modify: (re-baselines) any px-scale test whose assertions shift — each with justification

**Interfaces:**
- Consumes: `DynamicTree::kMargin` must be a usable constant expression from PhysicsWorld.cpp (it already is: `DynamicTree.hpp` is included and kMargin is public static — verify before writing).
- Produces: `kSkin = 0.02`, `DynamicTree::kMargin = 0.05`, residency tile 1.0, pad floor = kMargin.

- [ ] **Step 1: Append the failing constants test** to `PhysicsMksDefaultsTest.cpp` (add the DynamicTree include):

```cpp
#include <Arcane/Physics/Broadphase/DynamicTree.hpp>

TEST_CASE("Engine length constants are Box2D v3 MKS", "[physics][mks]")
{
    CHECK(kLinearSlop == Approx(Real(0.005)));          // constants.h:23 (unchanged)
    CHECK(kSkin == Approx(Real(0.02)));                 // constants.h:38 B2_SPECULATIVE_DISTANCE = 4*slop
    CHECK(kSkin == Approx(Real(4) * kLinearSlop));      // the RELATION, not just the value
    CHECK(DynamicTree::kMargin == Approx(Real(0.05))); // constants.h:44 B2_AABB_MARGIN (v3.1.1)
    CHECK(kMaxRotation == Approx(Real(0.25) * kPi));    // constants.h:33 (unchanged)
}
```

- [ ] **Step 2: Build + run `[mks]` — verify the new case FAILS** (kSkin 0.01, kMargin 8).

- [ ] **Step 3: Flip the constants.**

`PhysicsTypes.hpp` (~:154):
```cpp
        // Box2D v3 B2_SPECULATIVE_DISTANCE (constants.h:38): 4 * linear slop.
        // Collision skin / speculative contact distance, in meters.
        inline constexpr Real kSkin = Real(4) * kLinearSlop; // = 0.02
```

`DynamicTree.hpp` (~:64):
```cpp
            // Box2D v3 B2_AABB_MARGIN (constants.h:44): 0.05 m fat-AABB margin.
            // (v3.1.1 value — older Box2D used 0.1.)
            static constexpr Real kMargin = Real(0.05);
```

`PhysicsWorld.hpp` (~:1433):
```cpp
            SpatialGrid m_residencyGrid{ Real(1) }; // MKS tile; TODO(Phase 2): wire to the map's real tile size
```

`PhysicsWorld.cpp` (~:2710-2717) — the pad floor plays Box2D's pair-discovery fat-margin role:
```cpp
                        // Query pad: max(fat-AABB margin, velocity-scaled speculative
                        // margin). The floor is DynamicTree::kMargin — the same role
                        // Box2D's B2_AABB_MARGIN plays in pair discovery (constants.h:44).
                        const Real speedSqA   = m_velX[i] * m_velX[i] + m_velY[i] * m_velY[i];
                        const Real specMargin = (speedSqA > threshSq)
                                                    ? std::sqrt(speedSqA) * moveDt : kSkin;
                        const Aabb box = SlotAabb(i);
                        const Real pad = std::max(DynamicTree::kMargin, specMargin);
```

Then grep for OTHER hardcoded pad floors: `grep -n "Real(2)" Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Core/src/Arcane/Physics/*.cpp` — inspect each hit; any that is a length floor in a query/margin expression gets the same treatment (report the inventory; the audit expects exactly this one site plus possibly a legacy twin — the comment at 2710 references "the legacy GenerateContacts", which was deleted in the collision rebuild; if a twin survives anywhere, fix it identically).

- [ ] **Step 4: Build + FULL suite run + triage.** Build, `.\ArcaneTests.exe ~[gpu]` and `"[physics]"`. Expected: `[mks]` green. For the rest: PASS required, identical counts NOT expected. Triage protocol for every difference vs the T2 counts:
  - A test that still PASSES with the same assertions: fine, no action.
  - A test whose assertion FAILS: diagnose which constant moved it (kMargin pair-timing, kSkin speculative window, residency granularity). If the new behavior is correct-at-MKS-constants and the assertion was px-tuned: re-baseline the assertion WITH an inline comment naming the constant (`// re-baselined: kMargin 8->0.05 shifts pair creation by one step (MKS P1.iii)`), and add the file+reason to the justification table in your report. If the new behavior looks WRONG (bodies passing through, NaNs, sleep never happening): STOP — report as a blocker, do not paper over.
  - Expected-shift candidates (from the audit): none guaranteed — most tests assert with Approx margins; `PhysicsInvariantsTest` seam asserts and `PhysicsQueriesTest`/residency-region assertions are the most likely to notice. The pile/settle tests are pinned at sleepThreshold 8, so their sleep behavior should hold.
- [ ] **Step 5: Commit** (include the justification table in the commit body if any re-baselines happened):

```bash
git add Arcane/Core/src/Arcane/Physics/PhysicsTypes.hpp Arcane/Core/src/Arcane/Physics/Broadphase/DynamicTree.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp Arcane/Tests/src/PhysicsMksDefaultsTest.cpp <re-baselined test files>
git commit -m "feat(arcane/physics): compile-time length constants -> Box2D v3 MKS (skin 0.02, margin 0.05, pad floor) (MKS P1 stage iii)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466"
```

---

### Task 4: Length-literal audit over Core/Physics

**Files:**
- Modify: (audit-driven; expected small or empty) any `Arcane/Core/src/Arcane/Physics/**` file with a stray length literal
- Modify: `Arcane/Core/src/Arcane/Physics/Broadphase/SpatialGrid.cpp` (comment-only: guard-bound semantics at 1 m tiles)

**Interfaces:**
- Consumes: T3's constants.
- Produces: a written audit table (in the task report): every numeric literal in Core/Physics with length/velocity semantics -> verdict {unit-free | already-MKS | fixed | deferred-to-phase-N}.

- [ ] **Step 1: Sweep.** From repo root: `grep -rn "Real([0-9]" Arcane/Core/src/Arcane/Physics --include=*.cpp --include=*.hpp | grep -v -i "test"` plus targeted greps for bare literals in comparisons (`> 2`, `< 64`, `* 0.5`). For each hit decide: unit-free (ratios, iteration counts, hertz, Baumgarte betas, angles) | already-MKS (kBulletEpsilon 0.001, slop-derived) | length-suspect.
- [ ] **Step 2: Fix length-suspects** the T3 way (derive from kSkin/kMargin/slop with a cite comment) — each fix is TDD-able only where behavior is observable; otherwise the `[mks]` constants test + full-suite green is the gate. Known-checked (verify, then record): `kBulletEpsilon = 0.001` (meter-sane, keep); SpatialGrid `kMaxCellsPerAxis`/`kMaxCellsTotal` (cell-count semantics; at 1 m residency tiles the magnitude bound is ~65 km and the total budget ~2 km x 2 km — still far beyond content; update the comment near `kMaxCellsPerAxis` to state the MKS reading); `threshSq`/`moveDt` in the pad expression (derived from kSkin and dt — confirm derivation is unit-consistent and cite in the audit table).
- [ ] **Step 3: Build + full suite** — same triage protocol as T3 Step 4 (PASS required; justified re-baselines only).
- [ ] **Step 4: Commit** (audit table in the commit body):

```bash
git add <files the audit touched>
git commit -m "chore(arcane/physics): length-literal audit for MKS (P1 stage iii sweep)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01D2T3GD7rYdYXCiAZ5pX466"
```

---

## Post-plan: Phase 1 exit

- `grep -rc PX-PIN` total recorded (burn-down zero-point).
- Full `~[gpu]` suite + isolated SandboxSmoke `[gpu]` pair green; headless `Loom.exe --frames 180` GPU-verify clean (sandbox is pinned, so it must behave exactly as before).
- Whole-branch review, then merge + push = USER's call. Phase 2 (solver/dynamics cluster) gets its own plan document.

## Self-Review (author checklist — completed)

- **Spec coverage:** §3 runtime rows -> T2; §3 compile-time rows (minus CC -> P4 per §4) -> T3; §4 stage i -> T1; landmines (pad floor, SpatialGrid guard comment, literal sweep) -> T3/T4; §6 per-phase acceptance -> per-task verification steps.
- **Placeholder scan:** the pin pass and audit are procedures with exact rules, inventories, and verification gates — no TBDs; all code steps show code.
- **Type consistency:** `WorldDef` field names (gravityX/gravityY, sleepThreshold, restitutionThreshold, contactPushMaxVelocity, hashCellSize) consistent across T1 pins, T2 flips, and the `[mks]` test; `kSkin`/`DynamicTree::kMargin` consistent between T3 code and test.
- **Verified against vendored source while writing:** all cites in Global Constraints re-checked this session (types.c read in full; constants.h grepped); B2_AABB_MARGIN 0.05 correction caught by this check.
