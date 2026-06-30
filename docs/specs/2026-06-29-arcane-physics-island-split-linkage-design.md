# Arcane Physics — island contact linkage + `SplitIsland` rewrite (G1, revised)

Date: 2026-06-29
Workstream: Box2D-v3 MT-parity program, gap **G1** (finalize/sleep) — REDIRECTED by measure-first.
Branch: `feature/arcane-physics-island-split-linkage` (stacked on the G2 create-phase tip
`feature/arcane-physics-narrowphase-mt-create-phase`, which itself stacks on the update-phase
branch off `origin/main`).

## TL;DR

The planned G1 — parallelize the per-body finalize/sleep work the way Box2D's
`b2FinalizeBodiesTask` does — is a **non-starter in Arcane**: Arcane already integrates positions
inside the MT solver, so the only residual per-body work (the idle/sleep test) is ~0.1 ms, under
1% of the "sleep" Step stage. Measure-first instead pinned the stage's real cost on
`PhysicsWorld::SplitIsland`, which is accidentally **O(poolSize x islandSize)** and fires nearly
every step. This spec replaces that quadratic with a Box2D-faithful **per-body contact adjacency**
so the split walks only an island's own contacts: **O(islandBodies + islandEdges)**. Byte-identical
island result (the determinism tripwire stays live). MT-offload of split is explicitly deferred;
this design is its prerequisite.

## Measure-first findings (Dist, scene 8 with the kinematic whisk, settled window)

The whisk keeps ~99.7% of bodies awake (a genuine worst case for the stage). Sub-phase timing
inside Step stage 5 (`IslandSleep`):

| finalize/sleep sub-phase | 2000 bodies | 10000 bodies |
|---|---:|---:|
| **`SplitIsland`** (flood-fill, fires ~every step) | **2.07 ms** | **24.07 ms** |
| `UpdateSleep` per-body idle loop *(the G1-as-planned target)* | 0.011 ms | 0.12 ms |
| `UpdateSleep` per-island sleep decision | 0.004 ms | 0.10 ms |
| candidate scan | 0.006 ms | 0.05 ms |
| whole "sleep" Step stage (avg/step) | 1.54 ms | **13.97 ms** |

For scale at 10k: narrow 69 ms, solve 56 ms, **sleep 14 ms** (3rd-biggest stage, ~99% `SplitIsland`).
Absolute ms carry run-to-run system-load noise; the *structure* (SplitIsland dominates, the
per-body idle loop is negligible) is stable and corroborated by the code's complexity.

### Root cause

`PhysicsWorld::SplitIsland` (`PhysicsWorld.cpp:3416-3509`) re-derives the connected components of one
candidate island via a local union-find. Two compounding inefficiencies:

1. `localOf(slot)` (the member->local-index lookup) is a **linear scan** over the island's members
   — O(island) per call.
2. It unions over `m_contactPool.ForEach`, which walks the **entire contact pool** (not just this
   island's contacts), calling `localOf` twice per touching dyn-dyn contact.

Combined: **O(poolSize x islandSize)** — quadratic once a big settled pile forms (one island with
thousands of members; a pool with tens of thousands of contacts). It fires nearly every step
because the whisk's contact churn keeps re-marking the pile `splitCandidate` (`kMaxSplitsPerStep = 1`,
so one full re-derivation per step on the big island).

Box2D v3 does not have this: island management is incremental (per-body contact edges + per-island
contact lists), `b2SplitIsland` flood-fills using per-body contact edges scoped to the island, and
it is offloadable (`b2SplitIslandTask`). Arcane has **no** per-body or per-island contact adjacency
today — every "contacts of this island" query is a full pool scan. This spec adds that adjacency.

## Decision

Chosen approach (user-selected): the **full Box2D-faithful contact-linkage** fix (not the minimal
O(1)-membership-only patch). Realized as **per-body** contact adjacency rather than per-island lists
— see rationale below.

## Design

### Data structures

- `std::vector<std::vector<std::uint32_t>> m_bodyContacts;` — per-body (keyed by body SoA slot)
  list of the **pool ids of that body's dyn-dyn body contacts**. Mirrors the existing
  `m_bodyFixtures` (`PhysicsWorld.hpp:1226`) exactly: a vector-of-vectors that grows to `m_count`,
  inner vectors retain capacity across the create/destroy churn.
- `std::vector<std::uint32_t> m_splitLocalIndex;` — reused per-slot scratch (sentinel
  `0xFFFFFFFF`), the O(1) member->local-DSU-index map used inside `SplitIsland`. Mirrors the
  `m_awakeIndex` / `m_kinematicIndex` swap-remove back-index idiom (`PhysicsWorld.hpp:1332/1343`).
  Sized in `EnsureCapacity` alongside the other per-slot arrays.

### Why per-*body* adjacency, not per-island lists

A contact's home island changes on **every merge** (and on split). Per-island contact lists would
therefore need splicing on the hot merge path (`MergeIslands` runs on every begin-touch, many per
step) and re-homing on split — a fragile "a contact's island always equals its bodies' island"
invariant maintained across merge, split, and the begin-touch/merge ordering.

Per-body adjacency is keyed by the body **slot**, which never changes. So **merge, split, sleep, and
wake touch the adjacency not at all** — maintenance collapses to two sites (contact create + destroy).
This is also what Box2D's `b2SplitIsland` actually traverses (per-body `b2ContactEdge`), is reusable
beyond split (wake propagation, neighbour queries), and is the prerequisite for a future
`b2SplitIslandTask`-style MT offload.

### Maintenance points (the only changes besides `SplitIsland`)

- **Create** — `TryCreateContact` (`PhysicsWorld.cpp:2298-2403`), after `EnsurePair` returns
  `created == true`: if `bIsBody && bodyB != kInvalidSlot && TypeSlot(bodyA)==Dynamic &&
  TypeSlot(bodyB)==Dynamic` (i.e. a dyn-dyn body contact — the only kind that unions dynamic island
  members), push the new id into both `m_bodyContacts[bodyA]` and `m_bodyContacts[bodyB]`. `bodyA` is
  canonical-dynamic by construction, so the test reduces to "bodyB is Dynamic". This runs in the
  deterministic serial create tail (new pairs replayed in `awakeIndex`-sorted order), so adjacency
  insert order is deterministic (though split is order-insensitive — see byte-identity).

- **Destroy** — centralize the three existing destroy sites
  (`UpdateContacts` serial tail `kNpDestroy` `:2870`, `DestroyContactsForFixture` `:1944`,
  `DestroyContactsForBody` `:1972`) behind one helper `DestroyPooledContact(id)` that does, in order:
  `ReleaseContactColor(id)` (preserving the existing precondition), swap-remove `id` from both
  endpoints' `m_bodyContacts` (linear scan of the small inner vector; read `bodyA`/`bodyB` BEFORE
  `pool.Destroy` frees the slot), then `m_contactPool.Destroy(id)`. Only dyn-dyn body contacts were
  ever inserted, so the swap-remove is a no-op for others (guarded by the same type test, or simply
  "id not present").

- **Slot recycle / teardown** — reset `m_bodyContacts[slot]` on a recycled-slot `AddBody`
  (mirroring the awake/kinematic index reset at `AddBody`) and on `RemoveBody`; clear all of
  `m_bodyContacts` in any world `Clear()` path (the `ContactPool::Clear()` bypass that skips
  `ReleaseContactColor` must also drop the adjacency).

- **Merge / split / sleep / wake — unchanged.** This is the payoff of keying by body slot.

### `SplitIsland` rewrite

Keep the function's structure and the **component-id assignment loop byte-for-byte**; replace only
the edge-gathering step:

1. Snapshot `members` (unchanged); bail if `<= 1` (unchanged).
2. Set `m_splitLocalIndex[members[i]] = i` for `i in [0, n)` (O(island)).
3. DSU `parent[0..n)` with identical path-halving `find` (unchanged).
4. For each member `i`, walk `m_bodyContacts[members[i]]`; for each id, `c = pool.Get(id)`; if
   `c.touching`, compute `other = (c.bodyA == members[i]) ? c.bodyB : c.bodyA`, look up
   `j = m_splitLocalIndex[other]` (O(1)); if `j` is not the sentinel, `union(i, j)`. Same dyn-dyn +
   touching filter as today; same defensive guard when the other body is not a member. (Each edge is
   visited twice, once per endpoint — union is idempotent.)
5. The first-seen-root -> `islandId`/`AllocIsland()` assignment loop over `members` (O(island)) is
   **unchanged**, including the `m_islands` realloc-hazard discipline (`PhysicsWorld.cpp:3477-3479`:
   never hold an `Island&` across `AllocIsland`).
6. Reset only the touched `m_splitLocalIndex[members[i]]` entries to the sentinel (O(island)).

Complexity: **O(islandBodies + islandEdges)**. The whole-pool scan is gone.

### Byte-identity argument

The edge **set** is unchanged — exactly the touching dyn-dyn contacts incident to the island's
members, now reached via per-body adjacency instead of a filtered whole-pool scan. The DSU therefore
produces the **same partition** (connected components are independent of union order). The `members`
order and the assignment loop are untouched, so the first member of each component, the
`islandId`-reuse-vs-`AllocIsland` choice, and the `AllocIsland` call order are all identical ->
`m_islandId` assignment is bit-identical. (DSU root *element* identity may differ with a different
union order, but within one DSU state each set has a unique root, so component dedup in the
assignment loop is unaffected.) The determinism tripwire stays live; **no re-baseline**.

## Verification

- Full `[physics]` suite green with no re-baseline — especially `PhysicsIslandTest`,
  `PhysicsPersistentIslandTest`, `PhysicsDeterminismTest`, and the `[physics][mt]` byte-identity
  guards (`PhysicsNarrowphaseMtTest`, `SolverMtInvarianceTest`, `BroadphaseMtInvarianceTest`).
- Add a focused split-equivalence test if existing coverage does not exercise multi-component
  fracture of a large island hard enough (build a pile, sever it into N components, assert island
  membership matches a reference / pre-rewrite capture).
- Re-measure with throwaway `SPLITPROF` instrumentation (the same pattern used for the measure-first
  pass) to confirm `SplitIsland` 24 ms/step@10k -> sub-ms, then revert it (mirrors the update-phase
  Task-6 instrumentation revert `162374d1`).

## Scope boundaries

- **In:** `m_bodyContacts` + `m_splitLocalIndex`, their maintenance at contact create/destroy/recycle,
  the `SplitIsland` rewrite, the `DestroyPooledContact` centralization, the verification above.
- **Out:** the candidate-scan loop (O(island-pool), ~0.05 ms — negligible). MT-offload of split
  (deferred; this design enables it). The pre-existing tile-span sleep-invariant bug (dense dynamics
  resting purely on tile spans tripping the `EmitContactConstraints` no-sleeping-dynamic invariant)
  — serial, identical serial==MT, handled separately.
- The unrelated working-tree changes (Client `ui_screens`, `AGENTS.md`, `Arcane/.screenshots/`,
  `Server/cpp_coding_style.txt`, the two 2026-06-24 docs) stay untouched; commits stage only the
  per-task files by explicit path.

## Build / test commands

- Build: `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"`
  `"D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /p:Configuration=Debug /m` (Dist for the perf re-measure).
- Premake (only if files are added/removed): `ThirdParty\premake5\premake5.exe vs2026` from `Arcane\`.
- Tests from the exe dir:
  `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\ArcaneTests.exe "[physics]"`.
- clangd diagnostics are false positives in this workspace; MSVC + ArcaneTests are the truth.
