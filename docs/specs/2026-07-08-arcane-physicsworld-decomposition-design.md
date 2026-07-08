# Arcane PhysicsWorld Decomposition — Design Note (future foundations task)

**Status:** DESIGN NOTE / deferred foundations task. NOT to execute now. Captured 2026-07-08
during the physics/geometry closeout as the agreed next architecture investment.

**Sequencing (agreed priority):** this runs **AFTER** the physics/geometry closeout, as the first
foundations task. Full closeout priority order lives in the `project-physics-closeout-focus`
memory; in short: **B parity fixes → E geometry robust predicates (+ the A6 escape-crash guard) →
C declare-done + D2 docs sweep → THEN this decomposition.**

**Why after, not before (the load-bearing reason):** this is a byte-identity-gated,
no-behavior-change refactor, so it needs a *frozen* behavioral baseline to verify against. Several
closeout items are behavior-*changing* (B1 kinematic clamp, likely B7 tile-span invariant, the
escape-crash robustness guard). Landing those small behavioral fixes FIRST locks the baseline; then
the refactor is a clean "prove it's byte-identical" exercise instead of chasing a moving target.
Waiting costs ~nothing: the remaining closeout work adds almost no new code to PhysicsWorld (B = tiny
fixes, E lives in the geometry kernel, C/D are docs).

**Governing principle (holds for future items too):** correctness/behavior-changing work before
pure-refactor work; close the near-done arc before opening a large new one.

---

## The problem

`Arcane/Core/src/Arcane/Physics/PhysicsWorld.{hpp,cpp}` is a genuine god-object:
- `PhysicsWorld.cpp` ~3865 lines, ~87 method definitions.
- `PhysicsWorld.hpp` ~1735 lines, ~60 `std::vector` SoA member arrays + many hot inline accessors.

The rest of the `Physics/` tree is already well-modularized (Broadphase/, Narrowphase/, Solver/,
Joints/, and an existing `ContactManager`). PhysicsWorld is the sole outlier.

## The constraint that governs the design (READ THIS FIRST)

The engine is **data-oriented**. PhysicsWorld owns ~60 flat SoA arrays indexed by body/fixture/
contact slot; the solver, contact, and island code all gather from them directly. This is
deliberate — cache layout, SIMD (ContactConstraintSimd), and determinism (byte-identical ST==MT,
no /fp:fast) all lean on those arrays being flat and world-owned. The existing `ContactManager`
header states the tell verbatim: *"the fixture SoA is private to the world, so the manager delegates
rather than reaching in."*

**Do NOT wholesale-OO this.** A "proper" object graph that wraps the flat entity SoA behind
accessors either (a) re-exposes the raw arrays (porous encapsulation, no coupling reduced) or (b)
adds indirection to the hottest loop (perf + determinism risk). Box2D v3 keeps bodies in a flat
world-owned pool for exactly this reason.

### The litmus test for "can this be a real contained object"

Not file size — **does the responsibility own cohesive state of its own, or is it just behavior
over the shared entity SoA?**

**Own-state → extract as a real contained collaborator (the goal):**
- **Islands / constraint-graph** — island roots, awake-set, kinematic-set, split-linkage adjacency.
  Self-contained membership bookkeeping that *reads* body slots but *owns* its own state. **Best
  first candidate.** Currently ~400 lines loose in PhysicsWorld.cpp (`AllocIsland`, `FreeIsland`,
  `MergeIslands`, `MarkSplitCandidate`, `SplitIsland`, `AddToAwakeSet`/`RemoveFromAwakeSet`,
  `AddToKinematicSet`/`RemoveFromKinematicSet`, `WakeIsland`, `IslandRootOf`, `DetachContactAdjacency`,
  `DebugValidateBodyContacts`). Box2D models this as `b2ConstraintGraph` + island sets.
- **Contact lifecycle** — the contact pool + color allocation + persistent-color validation +
  `UpdateContacts`/`UpdateOneContact`/`EmitContactConstraints` driving (~1000 lines, the single
  biggest cohesive block, currently spanning ~1999–3234 in PhysicsWorld.cpp). `ContactManager`
  already exists as the **beachhead** (it owns the begin/stay/end event queue + pair-key
  bookkeeping); grow it (or a sibling `ConstraintGraph`) to own the pool + coloring + narrowphase
  driving that today live in the world.

**Shared-hot-SoA → keep flat, owned by the world (resist wrapping):**
- The body/fixture SoA store itself. Data-oriented by design, not by neglect.
- The solver inner loop — already factored (SoftStep + ContactConstraintSimd), must stay flat/SIMD.

## Target end-state: a HYBRID, not a dissolved world

PhysicsWorld stays the owner of the flat entity SoA and the `Step` orchestrator, and **contains**
the cohesive-state subsystems as real collaborators:
- `Broadphase` (already a real object — DynamicTree behind IBroadphase).
- `Solver` (already factored — SoftStep + ContactConstraintSimd).
- `ContactManager` / `ConstraintGraph` (grow it: contact pool + coloring + UpdateContacts driving).
- `IslandManager` (new: island membership + awake/kinematic sets + split-linkage).

The god-object shrinks because contact and island **state + logic** move into objects that own them
— NOT because methods get scattered across `PhysicsWorld.Contacts.cpp`-style partial TUs (that's a
cosmetic boundary that reduces no coupling; explicitly rejected in favor of real composition).

## Caveats to hold the work to

1. **Higher-risk than a pure TU split.** Moving state ownership changes access patterns, which can
   perturb the hot loop or evaluation order → determinism AND perf both need verifying, per
   extraction. Guardrails: the `[mks]` + MT-byte-identity tests + full `~[gpu]` suite (behavioral
   identity) and the `[perf]` wall-time tripwire (`PhysicsPerfTripwireTest`, added in MKS P5) for
   the hot path. Discipline: **move code, don't "improve" logic while in there** — a mechanical
   move-don't-reorder is byte-safe; opportunistic cleanups are how a determinism tripwire silently
   breaks.
2. **The anemic-manager trap.** If a collaborator ends up delegating everything back to the world
   for data, it adds indirection without buying encapsulation. Extraction only pays off where the
   object holds enough of its own state to carry real invariants — islands and the contact pool/
   coloring clear that bar; a body-array wrapper would not.

## Rough incremental sequence (one collaborator per commit, each verified byte-identical + perf-neutral)

1. **IslandManager first** — cleanest, most self-contained state, lowest cross-cutting surface.
   Extract island alloc/free/merge/split + awake/kinematic sets + split-linkage adjacency; PhysicsWorld
   holds one `IslandManager m_islands` and delegates. Verify `~[gpu]` byte-identical + `[perf]`.
2. **Contact lifecycle into ContactManager/ConstraintGraph** — move the contact pool + color pool +
   UpdateContacts/EmitContactConstraints driving out of the world. Larger + more hot-path-adjacent;
   watch the solver-feed byte-identity (EmitContactConstraints output must be bit-stable) and the
   `[perf]` tripwire.
3. (Optional, surgical) header hygiene — regroup non-hot declaration blocks (debug accessors,
   config) but LEAVE the SoA + hot inline accessors inline (moving them to a .cpp risks inlining
   regressions without LTO).

Not a weekend. Each step is a scoped, reviewed, byte-identity-gated change.
