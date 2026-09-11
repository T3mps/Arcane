# Astra adoption — module residency + change detection — Design

**Date:** 2026-09-11
**Status:** Approved (brainstorm run same day; every ruling below user-decided in-session)
**Research:** `D:\dev\starworks\Astra\.superpowers\sdd\2026-09-10-astra-change-detection\arcane-adoption-map.md`
(the adoption map — every Arcane site named below is anchored there against Arcane `9b0ad0f5`
and Astra `feat/change-detection` @ `b664aa8`; re-locate by SYMBOL before editing). Two
exploration digests from the brainstorm are folded in where they changed a decision (§3, §6).
Astra side: `docs/superpowers/specs/2026-09-10-astra-change-detection-design.md` (rulings E-O,
deviations 1-5) and `…/2026-09-09-astra-meta-binder-scoping-design.md` (§3.5 release flows,
§3.6 residency).

---

## 1. What this is

Arcane adopts two Astra features that were built for it: **module residency** (a never-unmapping
module declares `Resident` so its reflection metas are pinned across every registry's lifetime)
and **change detection** (per-column chunk versions, an opt-in per-entity tick tier,
`Changed<T>`/`Added<T>` views, `Registry::Modified`). Adoption replaces the transform system's
hand-rolled shadow-copy change detector, makes editor writes visible to it, runs propagation
once per editor frame through a real scheduler, and — as a second plan — retires
`PreviousTransform` in favour of the physics interpolation buffer.

Two plans, one spec (§9). Plan 1 lands ABI 26 with the vendor; Plan 2 lands ABI 27 with the
component deletion.

## 2. Scope and non-goals

**In scope (Plan 1):** two small Astra primitives, merged to `dev` and vendored; the Arcane ABI
bump to 26; the engine roster on a Runtime-owned `ComponentModule` under `Resident`;
const-correct views and reads; `Transform` change-tracked; propagation on a `Changed<Transform>`
pre-pass with an early-out; Inspector/undo writes marked; the paused physics reconcile gated;
an editor-owned scheduler running propagation once per Edit-mode frame with a single pending
camera-frame request; Gacha's `Aphelyon.arcproj` restamped and `Aphelyon.dll` rebuilt.

**In scope (Plan 2):** sprite interpolation reads `PhysicsInterpBuffer`; `PreviousTransform`
and `LerpPose` deleted; ABI 27.

**Non-goals, each with its trigger:**

| Out | Trigger |
|---|---|
| Tracking `WorldTransform` | A retained submission path (the resident mesh cache is the first candidate); every reader today rebuilds per frame, so `Changed<WorldTransform>` would DROP unchanged sprites |
| Tracking any other component | A `Changed<>` reader for it |
| Scheduling `PhysicsSystem` / wiring `PhysicsInterpBuffer` in production | A game module that runs physics (neither `ReferenceGame` nor `Aphelyon` schedules `PhysicsSystem` today — both interpolation paths are test-only until then) |
| A `.meta`-recorded extraction map, `Registry::IsChanged` consumers beyond tests | Noted only; unrelated arcs |
| Resident for the host EXEs / test exe | They register no components; Resident there buys nothing and would mask a future plugin leak in tests |

## 3. Rulings ledger (user-decided 2026-09-11)

| # | Question | Ruling |
|---|---|---|
| R1 | Touch Astra before vendoring? | **Yes** — `Registry::Modified(Entity, ComponentID)`, `Registry::IsChanged<T>(Entity, Tick)`, `IsAdded<T>`; merge `feat/change-detection` → `dev` (fast-forward); vendor from `dev`. |
| R2 | Residency scope | **Stamp + owned module.** `SetTypeContext(ctx, Resident)` at `Runtime.cpp:114` AND a Runtime-owned `ComponentModule "Arcane"`. Re-opens the 2026-08-10 ratification: its blocking caveat ("one registry per context") is gone from the vendored headers. |
| R3 | `PreviousTransform` | **Delete it; move sprite lerp onto `PhysicsInterpBuffer`** (Plan 2). Finding that made it cheap: neither path is production-wired today (§6). `LerpPose` goes with it. |
| R4 | Sequencing | **One spec, two plans**: ABI 26 at the vendor, ABI 27 at the deletion. |
| R5 | Editor propagation | **Once per frame through an editor-owned scheduler**; the two event-driven calls collapse into one pending request serviced after it. |
| R6 | `WorldTransform` tracking | **Untracked** (trigger above). |
| R7 | Gacha debt | **Restamp `Aphelyon.arcproj` 21 → 26 and rebuild `Aphelyon.dll`** as Plan 1's last task, in the Gacha repo. |
| R8 | Propagation mechanism | **`Changed<Transform>` view pre-pass + early-out** — chosen for performance: chunk-reject skips untouched chunks with one compare, the tracked tier yields exactly the moved entities, and a static scene skips the whole linear pass. Per-row `IsChanged` stays O(rows) with scattered loads. |

## 4. Astra side (R1)

Both primitives are forwards to machinery that exists on `b664aa8`:

- `bool Registry::Modified(Entity e, ComponentID id)` — `AssertContextAffinity()`; `false` for an
  invalid entity; else `m_archetypeManager->MarkWritten(e, id)` (already type-erased: stamps the
  column, marks a tracked entity, `false` for a stale handle / absent component / tag).
- `template<Component T> bool Registry::IsChanged<T>(Entity e, Tick since) const` — record →
  `idToColumn[TypeID<T>]` → if `chunk->IsTracked(col)`, `IsNewer(chunk->GetTicks(col)[row].changed, since)`,
  else `IsNewer(chunk->GetColumnVersion(col), since)` (chunk-coarse for an untracked `T`, documented).
  `IsAdded<T>` identical over `.added`. **Never stamps** — `const` all the way down.
- Tests (GoogleTest, `tests/Registry/ChangeDetectionTest.cpp`): mirror
  `ChangeDetectionStamp.ModifiedStampsAndSetIfNeqStampsOnlyOnInequality` (:316) for the ID
  overload incl. the three `false` paths; tracked-exact vs untracked-coarse for `IsChanged`; and
  "IsChanged does not stamp" (column version unchanged across the call).
- README §"Change detection" + spec §3.7 API-surface lines gain the three entries.
- Suite green Debug + Release (`AstraTest.exe --gtest_brief=1`); commit on `feat/change-detection`;
  fast-forward `dev`; `scripts/sync-astra.ps1` from the `dev` checkout (records the commit in
  `ThirdParty/Astra/VENDORED.txt`).

## 5. Residency (R2)

- `Runtime.cpp:114`: `Astra::SetTypeContext(context, Astra::ModuleResidency::Resident);` —
  Arcane.dll never unmaps; this is the pinning drain in every host and, via the
  `test_main.cpp:32` throwaway pin, in the test process.
- `Runtime::Impl`: `std::optional<Astra::ComponentModule> engineModule;` declared after
  `components` (destructs before the registry it references). Ctor, after the registry is
  created: `engineModule.emplace(Astra::ComponentModule::Open(components, "Arcane"))` then
  `Register<Transform, WorldTransform, PreviousTransform, SpriteRenderer, PostProcess, Identity,
  Hidden, Camera, MeshRenderer, RigidBody2D, Collider2D, PhysicsBodyRef>()` — the exact current
  order of `SceneModule.hpp:21-34` + `PhysicsComponents.hpp:275-277`, so first-touch ids do not
  move (Plan 2 drops `PreviousTransform` from this list; the id shift is harmless in-process per
  `SceneModule.hpp:28-32`). `Open` succeeds because the slot is installed and the registry was
  born under it. `~Impl` resets via the optional; under `Resident` every release reports
  `Retained`, metas stay, registry-less `GetMeta` keeps resolving. No plugin-side static — `Impl`
  is pimpl-held and reset from `~Runtime`.
- `RegisterSceneComponents`/`RegisterPhysicsComponents` are kept for the 35 test files that
  register on bare registries; `Runtime` stops calling them.
- Comments: rewrite the ratification block `Runtime.cpp:116-165` (record the 2026-09-11
  reversal and why), `PluginHost.cpp:259-265`; the v26 ledger records the outcome. Plugins
  (`HotReloadPlugin`, `ReferenceGame`, `Aphelyon`), hosts (`EditorApp`, `RuntimeApp`) and
  `test_main.cpp` keep the one-arg call (Transient).
- Test: two `Runtime`s against `SharedTypeContext()`, both destroyed, then
  `Astra::GetMeta<Transform>()` still resolves — the shape that crashed in 2026-08-10.

## 6. Change detection (R6, R8) — Plan 1

### 6.1 Const-correctness (mechanical, the map's B4 / B10 #3-4 lists)
Every read-only `CreateView<T&>` becomes `const T` with `const T&` params; every read-only
non-const `GetComponent<T>` goes through `std::as_const(reg)`. `PhysicsSystem` PASS 2 keeps
`PhysicsBodyRef` mutable; PASS 3.5 keeps `PhysicsBodyRef` mutable and `RigidBody2D` becomes
`Astra::With<RigidBody2D>`; PASS 4 makes `PhysicsBodyRef` const. `RenderSystems.hpp:38`'s
`Reads<…>` trait becomes true; the "Astra has no component change tracking" comment
(`TransformSystems.hpp:84-85`) is deleted. `TransformSystems.hpp:301`'s `WorldTransform` fetch
is restructured so only the write path fetches non-const.

### 6.2 `Transform` opts in
`static constexpr bool AstraChangeTracked = true;` in `Components.hpp`'s `Transform` (8 B per
entity on 40 B). Nothing else. `Hidden` cannot (tag, `static_assert`).

### 6.3 Propagation
`TransformOrder` (a registry resource — so a swapped registry resets it, and both schedulers
share it) gains `Astra::Tick lastRun = 0`, `std::vector<std::uint8_t> moved`, and an
entity→row map filled in `Rebuild` (which already walks every entity). `shadow`, `shadowValid`
and `SamePose` are deleted; `dirty[]` (topological inheritance) and `world[]` stay.

`operator()(Astra::Registry&)`, the one overload, everywhere:
1. Rebuild if `structureVersion`/root changed; **a Rebuild sets `lastRun = 0`** — a reparent
   must recompose everything, and a rebuilt row order invalidates any per-row memory.
2. Pre-pass: `reg.CreateView<const Transform, Astra::Changed<Transform>>().Since(cache.lastRun)
   .ForEach([&](Entity e, const Transform&){ moved[row(e)] = 1; })` — chunk-reject first, exact
   per entity because `Transform` is tracked.
3. **Early-out**: if the pre-pass flipped nothing and no Rebuild ran this call, return (nothing
   moved ⇒ nothing inherited).
4. Otherwise the existing linear pass over `order[]`, with `moved[i]` where `SamePose` was;
   `needsWorld` handling unchanged.
5. `cache.lastRun = reg.CurrentTick(); reg.AdvanceTick();` — mirrors the scheduler's
   post-segment advance so a write made after the pass is strictly newer than `lastRun`; a
   double advance inside a scheduler is harmless. `TransformOrderTest`'s "a clean leaf is not
   rewritten" case is the canary (bare `propagate(reg)` calls with a write between them).

### 6.4 Editor writes mark
`InspectorView::ForEachTarget` after `fn(e, data)`, and `ComponentEditCommand::Restore` after
`deserialize`: `reg.Modified(e, descriptor->id)`. The gizmo path already stamps through
non-const `GetComponent<Transform>`.

### 6.5 Paused physics reconcile
PASS 3.5's view gains `Astra::Changed<Transform>` and `.Since(m_lastReconcile)` (tick stored on
`PhysicsResource`, advanced as in 6.3 step 5); the exact `appliedScale` compare and the
pos/rot divergence test stay inside — position-only edits change `Transform` but must not
trigger `RebuildScaledFixtures`. PASS 4's `PreviousTransform` stash is untouched in Plan 1.

### 6.6 Tests
Re-pinned: `TransformOrderTest` (clean-leaf canary; rebuild counts), `TransformPropagationTest`,
`AuthoredTransformSyncTest`, `PhysicsPauseTest`. New: descriptor-path Inspector edit seen by the
next propagation; undo restore seen; static scene → second pass early-outs (composition counter
unchanged); Rebuild forces a full recompose after a reparent under a clean parent.

## 7. Editor scheduling (R5) — Plan 1

Finding: Edit mode holds the `RunLoop` paused, so the game module's `fixedUpdate` (which owns
propagation) never runs there, while `update` and `render` run every frame unconditionally;
today three ad-hoc temporaries paper over this — `RefreshSceneResolution` per frame (phase 9),
`FrameSceneIfPending` on scene open (which then calls `FrameCamera`, propagating AGAIN), and
`FrameCamera` on the F/Home key at input time.

- `EditorApp` owns `Astra::SystemScheduler m_editSchedule` with one system,
  `TransformPropagationSystem`, added at boot. It executes once per frame at phase 9 when
  `!InPlayMode()`; in Play mode `fixedUpdate` owns propagation, so nothing runs twice. Both
  schedulers share `TransformOrder` and its `lastRun`; a Play→Stop switch costs one full
  recompose, which the registry restore forces anyway. Ruling H covers scene load.
- `FrameCamera(selectionOnly)` at input time and `FrameSceneIfPending` both record
  `m_pendingFrame{selectionOnly}`; `ServicePendingFrame()` runs right after the phase-9
  scheduler pass — bounds computed on fresh `WorldTransform`, camera moved, same frame, before
  `RenderSceneToViewport`. The input-time and scene-open temporaries are deleted.
- Tests (pure editor paths): a moved entity is framed at its new bounds on the next frame
  service; propagation runs exactly once per Edit-mode frame (a run counter on `TransformOrder`).

## 8. Interpolation (R3) — Plan 2

Finding (brainstorm digest): `PhysicsInterpBuffer` is never `SetResource`'d in production,
`PhysicsSystem` is never scheduled by either game module, `DrawPhysicsDebug` has no production
caller — and `PreviousTransform`'s only writer is `PhysicsSystem` PASS 4. Neither interpolation
path is live in production; this plan is mechanism + tests.

- `RenderSubmissionSystem`: the view gains `Astra::Optional<const PhysicsBodyRef>`; when the
  `PhysicsInterpBuffer` resource exists, is `captured`, `handle.index < prev.size()` and
  `prev[index].generation == handle.generation`, blend **position (XY) and angle** from
  `prev[index]` to the current world pose with `Lerp`/`AngleLerp` (the debug draw's helpers);
  any miss → the snap-to-current path. World-slot poses are world poses (more correct than the
  old local-as-world approximation).
- Delete `PreviousTransform`: struct + reflect block (`Components.hpp`), `RegisterSceneComponents`
  slot and the `ComponentModule` list (§5), `PhysicsSystem` trait + PASS 4 stash,
  `ComponentCatalog`'s hidden/structure-locked set and `EditorComponentCatalogTest`'s pins. The
  v16 ledger mention stays as history.
- Delete `LerpPose` and its `TransformSpineTest` cases (its only consumer was the old lerp
  block).
- `RenderInterpolationTest`'s two `PreviousTransform` cases rebuild the buffer by hand instead
  (position midpoint at α=0.5; shortest-arc 350°→10° lands at 0°; generation mismatch snaps).
- Reflect-block change ⇒ **ABI 27**, v27 ledger entry, `ReferenceProject.arcproj` restamped (Gacha
  restamp recorded as that repo's follow-up, as v26 did).

## 9. ABI, build ritual, testing, plans

- **ABI 26** lands in Plan 1's vendor task (headers move inlined layouts — same class as v10/v24):
  v26 ledger in the v24 style with grep evidence over both game modules, the residency outcome
  (§5), and the tracked-type note; `kGamePluginABIVersion = 26`; `ReferenceProject.arcproj` → 26.
- **Order after the sync** (map B9): `sync-astra.ps1` (from `dev`) → `GenerateProjects.bat` →
  **`ReferenceProject.slnx` first, for every configuration the run targets** (single-slot
  `Binaries\`) → `Arcane.slnx` → full unfiltered suite (only it runs `[witness]`) + `~[gpu]`
  baseline + `scripts/check-baselines.ps1`. Launch hosts with an ABSOLUTE `--project` path.
- **Gacha** (R7): Plan 1's last task, in `D:\dev\starworks\Gacha`: `Game/Aphelyon.arcproj` 21 → 26,
  `Aphelyon.dll` rebuilt against the synced SDK, one launch of the Gacha project in a v26 host.
- **Baseline**: 56118 assertions / 1620 cases (`~[gpu]`, Debug and Release). Per-task deltas
  attributed. No render change ⇒ the golden lanes are untouched, no re-bless.
- **Plans**: Plan 1 (`docs/plans/2026-09-11-astra-adoption-plan1-residency-change-detection.md`):
  Astra primitives → merge → vendor + ABI 26 → residency → const-ification → Transform tracked +
  propagation → editor marks → PASS 3.5 gate → editor scheduler → Gacha restamp → closeout.
  Plan 2 (`…-plan2-interpolation.md`): buffer-driven lerp → delete `PreviousTransform` +
  `LerpPose` → ABI 27 → closeout.

## 10. Hazards ledger

| Hazard | Where addressed |
|---|---|
| Rebuilt row order makes per-row change memory stale | §6.3 step 1 — Rebuild resets `lastRun` (full recompose) |
| Write after the pass at the same tick as `lastRun` is invisible | §6.3 step 5 — advance after every pass, scheduled or bare |
| Inspector/undo writes bypass stamping (`GetComponentByHash` stamps nothing) | §6.4 — `Modified(e, descriptor->id)` |
| Position-only edit rebuilding every fixture | §6.5 — exact compares stay inside the gate |
| Last `Runtime`'s `Reset` erasing shared metas (the 2026-08-10 bug) | §5 — `Resident` ⇒ `Retained`; test pins it |
| Stale `ReferenceGame.dll` under a new host (`plugin: initial load failed`) | §9 build order; absolute `--project` |
| Play↔Edit both driving propagation | §7 — Edit scheduler gated `!InPlayMode()`; shared `lastRun` |
| `Changed<WorldTransform>` dropping unchanged sprites | §2 non-goal, trigger recorded |
