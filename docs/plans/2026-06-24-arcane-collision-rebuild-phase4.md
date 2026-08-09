# Physics Collision Rebuild — Phase 4: Events-as-Byproduct — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use `- [ ]` checkboxes.

**Goal:** Derive contact Begin/Stay/End events from the persistent contacts' touch-state transitions and **delete `ContactManager::Step`'s redundant second broadphase+narrowphase (`SlotsOverlap`) pass** — collapsing the ~37% events pass, with byte-identical event sequences.

**Architecture:** Phase 3 left a persistent fixture-level `ContactPool` that computes each manifold once/step (`UpdateContacts`). Phase 4 (a) **expands** the pool to also create contacts for the **event-relevant** pairs Phase 3 excluded (sensors, kinematic-kinematic, kinematic-vs-static-body) — tiles stay out — so the single narrowphase pass covers both solver + events; (b) keeps a **body-pair event-state map** (deduped fixture→body) fed by the contacts' `touching` flag, deriving Begin/Stay/End from per-body-pair touch-state transitions with the existing gating + sorted-End determinism; (c) **deletes `ContactManager::Step`'s** broadphase `UpdatePairs` + `SlotsOverlap` pass, keeping its event delivery + per-body gating + Disarm/Rearm/DropBody (now fed by the new touch-state). The solver feed (`EmitContactConstraints`) gains an explicit solver-relevance filter (since creation no longer pre-filters sensors/kinematic).

**Tech Stack:** C++23, Core (presentation-free, /MD + static-CRT), Catch2 (`[physics]`), premake5/MSBuild. SPEC: `docs/superpowers/specs/2026-06-23-arcane-collision-module-rebuild-design.md` §4, §7, §10 Phase 4. Branch `feature/arcane-collision-rebuild-phase4` (off the Phase-3 HEAD).

---

## Conventions
Same as Phase 3 (see `docs/superpowers/plans/2026-06-24-arcane-collision-rebuild-phase3.md` "Conventions"): MSBuild Debug via `Arcane.slnx`; tests from the exe dir `./ArcaneTests.exe "[physics]"`; ArcaneCore static-CRT via `Server/ArcaneCore/ArcaneCore.vcxproj`; new files → regen BOTH workspaces (`premake5 vs2026`, NOT GenerateProjects.bat); clangd diagnostics are FALSE POSITIVES (MSVC is truth); kill stray Loom before building; commit per task with the `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` trailer; do NOT push. **DETERMINISM IS THE CONTRACT** — run-twice-identical + the existing event-sequence tests are the gate.

## THE EVENT SEMANTICS (verified — preserve EXACTLY; `PhysicsEventsTest` is the gate)

Old `ContactManager::Step` (`Arcane/Core/src/Arcane/Physics/ContactManager.cpp`) fires events for these BODY-pairs:
1. **Mover-mover** (broadphase `UpdatePairs` → body-pairs, sorted+deduped): dynamic-dynamic, dynamic-kinematic, **kinematic-kinematic**. **Sensors INCLUDED** (`SlotsOverlap`/`ShapesOverlap` does NOT skip sensor fixtures).
2. **Kinematic-vs-static-BODY** (explicit `StaticList()` loop, guard `TypeSlot==Kinematic`, AABB pre-reject then `Touch`).
3. **Dynamic-vs-static-BODY: NO events** (deliberate — `ContactManager.cpp:161-165`: "the solver owns dynamic-vs-static response; events are for gameplay triggers on kinematic movers").
4. **Mover-vs-TILE: NO events** (cells aren't bodies).

Per body-pair: `touching` (== `SlotsOverlap` pointCount>0) → get/create pair, stamp; **gating** = `!EventsEnabled() || !EvtOn(a) || !EvtOn(b)` → drop + reset `begun`; else `!begun` → `begun=true` + **Begin**, else **Stay**. Separation (untouched this step) → if `begun && !gated` → **End** (collected, **sorted by (a,b)**, then emitted), erase the pair. Events are BODY-level `ContactEvent{type, BodyHandle a(lower slot)/b, bool sensor = SensorSlot(a)||SensorSlot(b)}`, **deferred** (queued during, delivered after all state settles, only if a listener is set). `Disarm`/`Rearm` (level-triggered re-arm: currently-overlapping !begun non-gated pairs emit a fresh Begin immediately) / `DropBody(idx)` (erase pairs involving idx) / `ForEachBegunPair` (debug, used by `world.ForEachContact`).

**The fixture→body dedup:** a compound body's N² fixture-pairs for one body-pair = ONE event pair. (Old code sorts+uniques body-pairs before `Touch`.)

## Phase-3 pool vs the event set (the gap Phase 4 closes)

Phase-3 `TryCreateContact` creates contacts only for **solver-relevant** pairs: skips same-body, dead, **sensors** (body + fixture), and **`!da && !db`** (neither dynamic). So the pool HAS dynamic-dynamic, dynamic-kinematic, dynamic-static-body; LACKS kinematic-kinematic, kinematic-static-body, all sensor pairs. Events need the opposite tail (kinematic + sensors) and must EXCLUDE dynamic-static-body. → Phase 4 expands creation to the union (minus tiles) and filters per-purpose.

## File Structure
- Modify `PhysicsWorld.hpp/.cpp` — expand `TryCreateContact` (+ a kinematic-static create path); tag contacts solver-relevant vs event-only; tighten `EmitContactConstraints`' solver filter; add the body-pair event-state + derivation (or feed `ContactManager`); wire `Step`.
- Modify `ContactManager.hpp/.cpp` — delete the `Step` overlap pass; keep delivery/gating/Disarm/Rearm/DropBody, fed by the persistent touch-state.
- Tests: extend `PhysicsPersistentContactTest.cpp` (pool-includes-sensor/kinematic; solver feed unchanged) + keep `PhysicsEventsTest.cpp` green (the event-sequence gate).

---

### Task 1: Expand the contact set to the event union; keep the solver feed identical

**Files:** `PhysicsWorld.hpp/.cpp`; extend `PhysicsPersistentContactTest.cpp`.

- [ ] **Step 1: failing test** — append to `PhysicsPersistentContactTest.cpp`: a `[physics]` test that overlaps (a) a sensor fixture on a dynamic body with another body, and (b) a kinematic body with a static body, Steps, and asserts BOTH now appear in the pool (`DebugContactCount` rises / a new `DebugHasContactForBodies(a,b)` helper finds them), while the **Phase-3 oracle test STILL passes** (the solver feed is unchanged — sensors/kinematic produce contacts but NOT constraints).
- [ ] **Step 2: build + verify fail** (the pool currently excludes them).
- [ ] **Step 3: expand creation + tighten the solver filter.**
  - In `TryCreateContact` (mover-mover) + the static-create path: REMOVE the sensor skip and the `!da && !db` skip from CREATION; ADD a kinematic-vs-static-body create path (mirror the mover-vs-static path but for `TypeSlot==Kinematic` bodies — statics aren't in the mover broadphase, so iterate `StaticList()` like ContactManager does, AABB-reject then create). KEEP excluding same-body, dead, and TILES (tiles stay transient/no-contact-pool). Each created contact still computes `touching` in the update pass.
  - Tag each `Contact` with **solver-relevance**: add `bool solverRelevant` (true iff `(da||db) && !sensorA && !sensorB` — i.e. the OLD create filter). Set it at create.
  - In `EmitContactConstraints`: emit a `ContactConstraint` ONLY for `c.solverRelevant` touching contacts (plus the existing awake-gate). This keeps the solver feed byte-identical to Phase 3 (the oracle proves it) even though the pool now holds more contacts.
- [ ] **Step 4: build + the new test passes + the Phase-3 oracle STILL passes + full `[physics]` green** (solver behavior unchanged; pool is a superset now). ArcaneCore static-CRT clean.
- [ ] **Step 5: commit** `feat(arcane/physics): expand persistent contacts to the event union (sensors + kinematic), solver feed unchanged`.

### Task 2: Body-pair touch-state + event derivation from the contacts

**Files:** `PhysicsWorld.hpp/.cpp` (+ maybe a small `ContactEvents` helper); keep `PhysicsEventsTest.cpp` as the gate (do NOT weaken it).

- [ ] **Step 1:** Read `PhysicsEventsTest.cpp` to learn the exact event-sequence assertions (begin/stay/end ordering, sensor flag, gating, kinematic-static, dynamic-static-EXCLUSION). These are the contract.
- [ ] **Step 2:** Add a **body-pair event-state** map (port `ContactManager`'s `Pair{a,b,begun,stamp}` keyed by `PairKey`) — but FEED it from the persistent contacts each step instead of a fresh overlap pass: walk the pool (ascending id, deterministic); for each contact that is **event-relevant** (NOT a tile, NOT dynamic-static-body — i.e. mover-mover OR kinematic-static; sensors included), map to its body-pair `(min,max)` and mark that body-pair "touched this step" if `c.touching`. Dedup is automatic (the map key). Then run the SAME transition logic as `Touch`/the separation sweep: gating → Begin/Stay; untouched → sorted End; deferred delivery. Set `solverRelevant`/event-relevant tags so the derivation can filter (dynamic-static excluded, tiles never in pool).
  - **CRITICAL determinism:** Begin/Stay must be emitted in a deterministic body-pair order (the old code used the sorted broadphase body-pairs; here, collect touched body-pairs, SORT by (a,b), then emit Begin/Stay in that order). End already collected+sorted. Match the old emission order so `PhysicsEventsTest` sequences hold (or re-baseline ONLY if a deliberate, documented order improvement — but prefer matching).
- [ ] **Step 3:** build + `PhysicsEventsTest` + full `[physics]` green (event sequences preserved). If a sequence differs, diagnose (ordering, dedup, the dynamic-static exclusion, sensor flag, gating) — do NOT weaken the test.
- [ ] **Step 4: commit** `feat(arcane/physics): derive contact events from persistent touch-state (body-pair Begin/Stay/End)`.

### Task 3: Delete `ContactManager::Step`'s overlap pass; wire delivery to the new state

**Files:** `ContactManager.hpp/.cpp`, `PhysicsWorld.cpp` (the Step Stage-6 call).

- [ ] **Step 1:** Remove `ContactManager::Step`'s candidate walk (the `UpdatePairs` body-pair loop + the kinematic-static `SlotsOverlap` loop + the separation sweep) — that work is now Task 2's derivation. Decide the cleanest seam: EITHER (a) move the body-pair event-state + derivation INTO `PhysicsWorld` (Task 2) and reduce `ContactManager` to just the listener + gating helpers + Disarm/Rearm/DropBody/ForEachBegunPair reading the world's state; OR (b) keep `ContactManager` owning the body-pair map but feed it via a new `ContactManager::DeriveEvents(world, <the per-step touched body-pairs from the pool>)` instead of `Step`'s own overlap pass. Pick the one that minimizes churn to `Disarm`/`Rearm`/`DropBody`/`ForEachBegunPair` + the `OnContact` listener API (those are PUBLIC and have consumers — keep their signatures).
- [ ] **Step 2:** Ensure `SlotsOverlap` (the now-removed pass's overlap test) has no other live caller (grep); if it's now dead, leave it for Phase-5 cleanup or remove it (note in the report). Keep `ForEachBegunPair` working (`world.ForEachContact` + the Sandbox debug overlay consume it).
- [ ] **Step 3:** build + `PhysicsEventsTest` + full `[physics]` + `[sandbox]` (debug overlay reads begun pairs) green. Run-twice determinism green. ArcaneCore static-CRT clean.
- [ ] **Step 4: commit** `perf(arcane/physics): events as a byproduct -- delete ContactManager's redundant overlap pass`.

### Task 4: Full gate + perf smoke + memory

- [ ] **Step 1:** Full ArcaneTests Debug + Release (no filter, `[gpu]` both backends) green. ArcaneCore static-CRT Debug+Release clean.
- [ ] **Step 2:** Perf smoke (Dist Loom stress scene — the controller runs it, PAUSE for the user): the events-pass removal should show on the dense scene (the ~37% the design predicts). Report sim ms vs Phase 3.
- [ ] **Step 3:** Determinism run-twice + event sequences hold. Re-baseline a golden ONLY if a deliberate documented improvement (none expected).
- [ ] **Step 4: memory** — `project_arcane_collision_rebuild_phase4` + MEMORY.md line. Next = Phase 5 (cleanup/re-baseline: remove interim AABB cull + SweepAndPrune + Baumgarte + the equivalence invariant + dead SlotsOverlap; physically relocate warm-start onto the Contact; document tunables).

---

## Self-Review Notes
- **Spec coverage (§7 events-as-byproduct):** expand-to-event-union = T1; body-pair Begin/Stay/End from touch-state = T2; delete the second pass = T3; gate = T4. The dynamic-static-EXCLUSION + kinematic-static-INCLUSION + sensor-INCLUSION + tile-EXCLUSION are the exact event rules (preserved). Body-pair dedup + sorted Begin/Stay/End + deferred delivery + gating + Disarm/Rearm/DropBody preserved.
- **Determinism:** the derivation walks the pool ascending-id, collects touched body-pairs, sorts before emit; End already sorted. Run-twice + `PhysicsEventsTest` are the gate.
- **Solver unchanged:** T1's `solverRelevant` tag + `EmitContactConstraints` filter keep the solver feed byte-identical (the Phase-3 oracle stays green as a tripwire).
- **Soft spots for the executor:** (1) the kinematic-vs-static create path (statics aren't in the mover broadphase — iterate `StaticList()`, AABB-reject, like the old ContactManager). (2) matching the old Begin/Stay EMISSION ORDER (sort touched body-pairs). (3) the `ContactManager` refactor must keep the PUBLIC `OnContact`/`Disarm`/`Rearm`/`DropBody`/`ForEachBegunPair` API + semantics (consumers exist). (4) Confirm `EvtOn`/`EventsEnabled`/`SensorSlot`/`StaticList`/`SlotAabb`/`AabbOverlap` are the real accessors.
