# PhysicsWorld Decomposition — Step 1: IslandManager Extraction — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract island *topology* ownership out of the `PhysicsWorld` god-object into a contained `IslandManager` collaborator, with zero behavior change (byte-identical `~[gpu]` suite) and no perf regression (`[perf]` tripwire).

**Architecture:** `IslandManager` owns the island registry + merge/split/sleep-decision + split-linkage adjacency, mirroring Box2D v3's `b2Island` (topology + sleep trigger). It holds NO back-pointer to the world; every method takes `PhysicsWorld& w` and reaches body-slot data + the awake-set *mechanism* through the world's existing per-slot accessors — a verbatim copy of the proven `ContactManager` beachhead delegation pattern. The hot awake-set / kinematic-set dense arrays STAY world-owned (they are read per-body every substep and their order is a determinism tripwire — "behavior-over-shared-hot-SoA → keep flat"). End-state is a hybrid: world keeps the flat entity SoA + `Step` orchestration; `IslandManager m_islandMgr` owns island topology.

**Tech Stack:** C++23, Arcane Core static lib (`/MD` in the Arcane workspace), Catch2 tests, msbuild/premake5, D3D12 + Vulkan via NVRHI for the `[gpu]`/Loom gates.

## Global Constraints

- **MODEL = OPUS ONLY** — no fable, ever (standing user directive). Substitute opus wherever tooling names fable.
- **Byte-identity is the gate.** Every task ends with the FULL relevant suite passing with ZERO test-value re-baselines. If any asserted float value would have to change, the move was NOT mechanical — STOP and diagnose. A mechanical move-don't-reorder is byte-safe; opportunistic cleanups are how a determinism tripwire silently breaks.
- **Move code, do NOT "improve" logic while in there.** No reordering, no renaming of locals, no "while I'm here" refactors. Field access changes ONLY as dictated by the ownership move (a moved member `m_foo` → `m_foo` on the manager; a stayed member `m_bar` → `w.Bar()` / `w.m_bar` via friend/accessor).
- **No `/fp:fast`; UTF-8 no BOM; ASCII comments.** Determinism rules of the engine.
- **Units are MKS.** Not touched by this refactor, but do not introduce px-scale anything.
- **`.vcxproj`/`.slnx` are gitignored** — a NEW `.cpp` needs a premake regen (`ThirdParty\premake5\premake5.exe vs2026`, run from `Arcane/`; NOT `GenerateProjects.bat` which hangs on `pause`). New `.cpp` is auto-globbed after regen.
- **Test hygiene:** run `[gpu]`/smoke/Loom FROM THE EXE'S OWN DIR (PluginHost resolves `Sandbox.dll` relative to CWD) and SERIALIZE them (DLL copy lock). Foreground each test command (backgrounding stalls kill sessions).
- **Commit messages** via `git commit -F` (a file or heredoc; pipe/here-string = BOM in PowerShell — use the Bash tool). House style = conventional-commit, no Claude trailers. Do NOT `git add -A` (parked `Client/data` + untracked noise); stage exact paths.

---

## Baseline (capture BEFORE any edit — this is the byte-identity oracle)

Branch off `main` at the frozen post-closeout baseline. Record these numbers; every task re-asserts them unchanged.

- `[physics]` = **30658 assertions / 281 cases**
- `[geometry]` = 118689 / 12 (untouched, sanity only)
- full `~[gpu]` = **176633 assertions / 502 cases**
- `[perf]` tripwire (`PhysicsPerfTripwireTest`, Release, 300-circle pile) = healthy ~1375 ms, ceiling 14000 ms (~10x). Excludable via `~[perf]`.
- `Loom --frames 180` both backends exit 0 (visual GPU-verify).

Build (from `Arcane/`):
- Debug: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m`
- Release (for `[perf]`): same with `/p:Configuration=Release`.

Run (from the exe dir, e.g. `Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/`):
- `ArcaneTests.exe "[physics]"` · `ArcaneTests.exe ~[gpu]` · Release `ArcaneTests.exe "[perf]"`

---

## File Structure

- **Create:** `Arcane/Core/src/Arcane/Physics/IslandManager.hpp` — the collaborator's state + method declarations (mirrors `ContactManager.hpp` shape: owns its invariant, takes `PhysicsWorld&` per method, no world back-pointer).
- **Create:** `Arcane/Core/src/Arcane/Physics/IslandManager.cpp` — the moved method bodies.
- **Modify:** `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` — remove the moved island members; add `IslandManager m_islandMgr;` value member; keep the awake/kinematic SoA + hot accessors; add/keep a friend decl or a curated accessor seam for the delegation surface.
- **Modify:** `Arcane/Core/src/Arcane/Physics/PhysicsWorld.cpp` — delete the moved method bodies; replace their call sites with `m_islandMgr.X(*this, ...)`; keep `Step` orchestration + the atomic UpdateContacts ordering.
- **Modify:** `Arcane/Core/src/Arcane/Physics/Island.cpp` / `Island.hpp` — fold `Island::UpdateSleep` into `IslandManager::UpdateSleep` (or have IslandManager call it); resolve who owns the sleep-pass decision.
- **Test:** no NEW test file for the mechanical move; the existing `~[gpu]` + `[perf]` suites ARE the gate. (A new `[physics]` unit test that asserts an `IslandManager` invariant directly MAY be added in the exit task only if it does not perturb existing numbers.)

### Ownership split (the crux — from the three surveys)

**MOVES into `IslandManager` (island topology — own-cohesive-state):**

| Member (PhysicsWorld.hpp) | Role |
|---|---|
| `m_islands` (1324) | island records (registry) |
| `m_islandFree` (1325) | island free-list |
| `m_islandId` (1323) | body-slot → island id map |
| `m_pendingMerges` (1352) | cross-awake merge queue |
| `m_splitCandidates` (1353) | split-candidate list |
| `m_splitLocalIndex` (1359) + `kSplitLocalNone` | split scratch |
| `m_bodyContacts` (1229) | split-linkage contact adjacency (body-indexed) |

Methods (PhysicsWorld.cpp): `AllocIsland`/`FreeIsland`/`MergeIslands`/`MarkSplitCandidate` (~3466–3531, pure), `SplitIsland` (~3533–3653, needs a delegate for contact/joint edges), `WakeIsland`, `IslandRootOf`/`IslandOf`, `DetachContactAdjacency`, `DebugValidateBodyContacts`, the split-candidate scan, and the sleep decision `Island::UpdateSleep` (Island.cpp:26–126).

**STAYS on `PhysicsWorld` (shared hot SoA — resist wrapping):**

- Awake-set dense arrays `m_awakeBodies` (1334) + `m_awakeIndex` (1335) and the hot accessors `AwakeCount()`/`AwakeBodies()`/`AwakeIndexOf()`/`AwakeIndexData()` — read per-body per-substep; `AwakeIndexData()` handed as a raw pointer into `ContactConstraintSimd.hpp:298,342`; element ORDER feeds `AssignContactColor` (determinism). `AddToAwakeSet`/`RemoveFromAwakeSet` stay as the MECHANISM the manager calls.
- Kinematic-set `m_kinematicBodies`/`m_kinematicIndex` (1345–1346) — solver working set; kinematics are never island members.
- `m_awake` flag (SoA) — distinct from the awake-SET; the ~10 dual-representation seams stay.
- The contact pool + solver contact-coloring block (`m_bodyColorMask`, `m_colorContacts`, `AssignContactColor`…) — a SEPARATE future collaborator (Step 2), not this one.

**Delegation seam:** `IslandManager` reaches stayed state through `PhysicsWorld& w` — either `friend class IslandManager;` on PhysicsWorld (simplest for a mechanical move; matches that IslandManager was internal world code) or the curated per-slot accessor block (rename the existing "P2.4 seam" at PhysicsWorld.hpp:940–994). Lifecycle hooks for create/remove seams mirror `ContactManager::DropBody`: `IslandManager::OnBodyAdded(slot)`, `OnBodyRemoved(w, slot)`, `OnContactCreated(w, ci)`, `OnContactDestroyed(w, ci)`.

---

## Task 1: Scaffold IslandManager + wire it as a member (no logic moved)

Establishes the class, the value member, and the premake regen — compiles, suite byte-identical (nothing has moved yet).

**Files:**
- Create: `Arcane/Core/src/Arcane/Physics/IslandManager.hpp`
- Create: `Arcane/Core/src/Arcane/Physics/IslandManager.cpp`
- Modify: `Arcane/Core/src/Arcane/Physics/PhysicsWorld.hpp` (add `friend class IslandManager;` + `IslandManager m_islandMgr;`; add include)

**Interfaces:**
- Produces: `class Arcane::IslandManager {};` (empty for now), `PhysicsWorld::m_islandMgr` value member, `friend class IslandManager;` on PhysicsWorld.

- [ ] **Step 1: Create the header skeleton**, mirroring `ContactManager.hpp`'s doc-comment + shape (no world back-pointer; methods will take `PhysicsWorld&`). Include-guard/pragma-once, `namespace Arcane`, forward-declare `class PhysicsWorld;`. Empty body + a `// Step 1: scaffold only` marker.
- [ ] **Step 2: Create the .cpp skeleton** — `#include <Arcane/Physics/IslandManager.hpp>` + `#include <Arcane/Physics/PhysicsWorld.hpp>`, `namespace Arcane {}`.
- [ ] **Step 3:** In `PhysicsWorld.hpp`, add `#include <Arcane/Physics/IslandManager.hpp>`, add `friend class IslandManager;` near the other friends, and add `IslandManager m_islandMgr;` next to `ContactManager m_contacts;` (PhysicsWorld.hpp:1421).
- [ ] **Step 4: Regen the project** (new .cpp): from `Arcane/`, `ThirdParty\premake5\premake5.exe vs2026`.
- [ ] **Step 5: Build Debug** — expect clean.
- [ ] **Step 6: Byte-identity gate** — `ArcaneTests.exe "[physics]"` → **30658/281**, unchanged. (Full `~[gpu]` not required yet — nothing behavioral moved; a `[physics]` pass suffices for a pure-scaffold commit.)
- [ ] **Step 7: Commit** — `refactor(arcane/physics): scaffold IslandManager collaborator (no logic moved)`.

## Task 2: Move the island registry + pure methods

Move `m_islands`/`m_islandFree`/`m_islandId`/`m_pendingMerges`/`m_splitCandidates`/`m_splitLocalIndex` and `AllocIsland`/`FreeIsland`/`MergeIslands`/`MarkSplitCandidate`/`IslandOf`/`WakeIsland` into `IslandManager`. PhysicsWorld delegates via `m_islandMgr.X(*this, ...)`.

**Files:** Modify all four (IslandManager.hpp/.cpp, PhysicsWorld.hpp/.cpp).

**Interfaces:**
- Produces (exact signatures to be lifted verbatim from the current world methods, `this->m_foo` → member, stayed state → `w.`): `IslandId IslandManager::Alloc(...)`, `void Free(...)`, `void Merge(PhysicsWorld& w, ...)`, `void MarkSplitCandidate(...)`, `IslandId IslandOf(uint32_t slot) const`, `void WakeIsland(PhysicsWorld& w, IslandId)`. (Preserve current names where callers are many; a thin rename is allowed ONLY if every call site is updated in the same commit.)

- [ ] **Step 1: Move the member fields** from PhysicsWorld.hpp into IslandManager.hpp (cut, not copy). Preserve types + init exactly.
- [ ] **Step 2: Move the method bodies** (cpp:3466–3531 registry block) into IslandManager.cpp verbatim; rewrite `m_islands`→member (unchanged token, now resolves to the manager's field), and any stayed-state access (e.g. body SoA, awake-set) → `w.`. Add `PhysicsWorld& w` params only where a stayed-state access requires it.
- [ ] **Step 3: Replace call sites** in PhysicsWorld.cpp with `m_islandMgr.X(*this, ...)` (survey-2 call-site map is the checklist; all are RARE/PER-PAIR/PER-STEP-ONCE — zero perf cost to indirect).
- [ ] **Step 4: Build Debug** — clean.
- [ ] **Step 5: Byte-identity gate** — `ArcaneTests.exe "[physics]"` → **30658/281** unchanged, then `ArcaneTests.exe ~[gpu]` → **176633/502** unchanged (registry move can affect island sleep/wake across the whole suite — full run required).
- [ ] **Step 6: Commit** — `refactor(arcane/physics): move island registry + alloc/free/merge into IslandManager (byte-identical)`.

## Task 3: Move split-linkage adjacency + SplitIsland + lifecycle hooks

Move `m_bodyContacts` + `DetachContactAdjacency` + `DebugValidateBodyContacts` + `SplitIsland` (with a narrow delegate for contact-touching-bodies + joint-edge iteration) and add the `OnContactCreated`/`OnContactDestroyed`/`OnBodyRemoved` lifecycle hooks (mirror `ContactManager::DropBody`) at the world's create/destroy seams.

**Files:** Modify all four.

**Interfaces:**
- Produces: `void IslandManager::OnContactCreated(PhysicsWorld& w, uint32_t ci)`, `OnContactDestroyed(PhysicsWorld& w, uint32_t ci)`, `OnBodyRemoved(PhysicsWorld& w, uint32_t slot)`, `void SplitIsland(PhysicsWorld& w, IslandId)`, `void DetachContactAdjacency(PhysicsWorld& w, uint32_t ci)`, `bool DebugValidateBodyContacts(const PhysicsWorld& w) const`.

- [ ] **Step 1: Move `m_bodyContacts`** into IslandManager; add the lifecycle hooks and call them at the exact world seams that today mutate `m_bodyContacts` (TryCreateContact / ReleaseAndDestroyContact / RemoveBody). Move-don't-reorder — preserve the update points 1:1.
- [ ] **Step 2: Move `SplitIsland`** (cpp:3533–3653) verbatim; the reads of `m_contactPool`/`m_joints`/`TypeSlot` become `w.` accesses (via friend). Preserve the ascending-slot-member + first-seen-root id-assignment order EXACTLY (survey risk C5).
- [ ] **Step 3: Move `DetachContactAdjacency` + `DebugValidateBodyContacts`**; update call sites.
- [ ] **Step 4: Build Debug** — clean. (Debug builds run the `DebugValidate*` asserts — a good tripwire.)
- [ ] **Step 5: Byte-identity gate** — `~[gpu]` → **176633/502** unchanged. Preserve the UpdateContacts sort→WakeIsland→MergeIslands atomic ordering (survey risk C3 — trips the no-sleeping-dynamic assert if broken; the B7 deterministic tripwire covers this).
- [ ] **Step 6: Commit** — `refactor(arcane/physics): move split-linkage + SplitIsland into IslandManager (byte-identical)`.

## Task 4: Move the sleep-pass decision (UpdateSleep)

Fold `Island::UpdateSleep` (Island.cpp:26–126) into `IslandManager::UpdateSleep(PhysicsWorld& w, Real dt)`; `PhysicsWorld::Step` stage 4 calls `m_islandMgr.UpdateSleep(*this, dt)` in place of `Island::UpdateSleep(*this, dt)` (PhysicsWorld.cpp:1913). The awake-set removal is still `w.RemoveFromAwakeSet(...)` (mechanism stays on world).

**Files:** Modify Island.cpp/.hpp (remove/redirect the free function), IslandManager.hpp/.cpp, PhysicsWorld.cpp (the stage-4 call).

**Interfaces:**
- Consumes: the awake-set mechanism `w.RemoveFromAwakeSet`, per-body velocity/sleep-timer accessors (unchanged).
- Produces: `void IslandManager::UpdateSleep(PhysicsWorld& w, Real dt)`.

- [ ] **Step 1: Move the `UpdateSleep` body** into IslandManager (it currently reads world state via public accessors — those stay `w.`; island timers/roots are now members). Preserve the combined `|v| + |w|*maxExtent` sleep predicate + per-island timer aging EXACTLY.
- [ ] **Step 2: Redirect the call** at PhysicsWorld.cpp:1913 to `m_islandMgr.UpdateSleep(*this, dt)`; remove the now-dead `Island::UpdateSleep` (or leave `Island.hpp` for the shared structs it still exports — verify what else Island.hpp provides before deleting).
- [ ] **Step 3: Build Debug** — clean.
- [ ] **Step 4: Byte-identity gate** — `~[gpu]` → **176633/502** unchanged (the settle/sleep asserts across `PhysicsSolverBudgetTest`, `PhysicsIslandWakeMergeTest`, the pile tests are the load-bearing checks here).
- [ ] **Step 5: Commit** — `refactor(arcane/physics): move island sleep-pass into IslandManager (byte-identical)`.

## Task 5: Exit — full regression, perf, GPU-verify, header hygiene, review

**Files:** possibly minor PhysicsWorld.hpp header regrouping (LEAVE the SoA + hot inline accessors inline — moving them to .cpp risks inlining regressions without LTO).

- [ ] **Step 1: Full Debug `~[gpu]`** → **176633/502**, zero re-baseline.
- [ ] **Step 2: Build Release**; run Release `ArcaneTests.exe "[perf]"` → `PhysicsPerfTripwireTest` within budget (~1375 ms healthy, well under the 14000 ms ceiling). The extraction must be perf-NEUTRAL — if the pile time moved materially, an inner read went indirect; find it.
- [ ] **Step 3: Loom GPU-verify** (from Loom dir, serialized): `Loom.exe --backend vulkan --frames 180` and `--backend dx12 --frames 180` → exit 0, `RenderErrorCount()==0`, camera tripwire green.
- [ ] **Step 4: Confirm the shrink** — `wc -l PhysicsWorld.cpp` down ~450–550 lines; `IslandManager.{hpp,cpp}` holds the moved topology; verify no member of the world's flat SoA was wrapped (anemic-manager check: IslandManager holds real state, not just forwards).
- [ ] **Step 5: Whole-branch review** (opus): byte-identity argument per task, move-don't-reorder discipline honored, delegation seam clean (no hot-path indirection), no stale comments citing moved code. Apply comment-only fixes if any.
- [ ] **Step 6: Push the branch**; USER merges (FF, per the proven cadence).

---

## Self-Review (against the design note + surveys)

- **Coverage:** design note Step 1 (IslandManager: island alloc/free/merge/split + split-linkage) = Tasks 2+3; the design note also listed "awake/kinematic sets" — INTENTIONALLY narrowed out per all three surveys (hot SoA, determinism tripwire) — documented in the ownership-split table + surfaced to the user. Sleep-pass = Task 4. Exit gates (byte-identity + perf + Loom) = Task 5. Covered.
- **Placeholder scan:** relocation instructions cite exact files/line-ranges + the field-access transformation rule; no "TBD"/"handle edge cases". The moved bodies are lifted verbatim (mechanical), so full code is NOT reproduced by design — the "code" is the precise move + `w.`-access rule.
- **Type consistency:** method names preserved from the current world methods where callers are many; any rename updates all call sites in the same commit (Task 2 note). `IslandManager` takes `PhysicsWorld& w` uniformly (ContactManager parity).
- **Risk register (survey C1–C5) mapped:** C1 (awake-set color order) + C2 (AwakeIndex bijection) SIDESTEPPED by keeping the awake-set on the world; C3 (UpdateContacts atomic order) → Task 3 Step 5; C4 (m_awake dual rep) → stays on world; C5 (SplitIsland id order) → Task 3 Step 2 move-verbatim.
