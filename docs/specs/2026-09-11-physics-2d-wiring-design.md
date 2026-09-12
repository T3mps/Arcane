# 2D physics wiring — engine-owned world, editor integration, fixture authoring

**Date:** 2026-09-11 · **Status:** approved 2026-09-11; Plan 1 written (docs/plans/2026-09-11-physics-2d-wiring-plan1-runtime.md) · **Follows:** Astra adoption Plans 1–2 (ABI 27, `PhysicsInterpBuffer{prev, slotOf, captured}` the one interpolation history) · **Precedes:** F2c Plan 2 (rendering)

## 1. What this is

Every mechanism for 2D physics exists and is tested — `PhysicsSystem` (mint / capture / step / reconcile / write-back), `RigidBody2D` / `Collider2D` / `PhysicsBodyRef`, `PhysicsInterpBuffer`, `DrawPhysicsDebug`, the RunLoop's pause / single-step / time-scale, the editor's Play / Stop snapshot — and none of it is connected. The Astra adoption spec (§2, §8) recorded the trigger as "a game module schedules `PhysicsSystem`". That trigger is unreachable: `PhysicsSystem.hpp` includes four Manifold2D headers and the game-module SDK surface (`build/arcane.lua`) carries no Manifold2D row, so neither `ReferenceGame.cpp` nor `Aphelyon.cpp` can compile the call.

Four gaps, measured 2026-09-11:

1. **Compile barrier** — modules cannot name `PhysicsSystem`.
2. **No world** — nothing outside tests constructs `PhysicsResource` or `PhysicsInterpBuffer`; nothing runs `PhysicsSystem` in Edit mode, so Plan 1's paused reconcile (PASS 3.5) has no caller.
3. **No overlay** — `DrawPhysicsDebug` has no production caller.
4. **No authoring** — `Collider2D::fixtures` is `Serializable(false)` on the JSON path (the reflection→JSON bridge has no container branch) and the Inspector has no vector editor. Both game scenes have zero physics components.

This spec closes all four. Physics becomes engine-owned (the industry shape: UE's `UWorld` owns `FPhysScene` and game code orders itself by tick group; Source 2's Rubikon is engine-owned with content opting in), settings are project-level with a per-scene override (UE's `UPhysicsSettings` + `WorldSettings` split), and shapes are authored as a list per body (UE's `UBodySetup::AggGeom`, Source 2's physics shape list — and what `Collider2D` already is).

## 2. Scope and non-goals

**In scope:** engine-owned world + scheduling; project + scene settings; Edit-mode bodies; Play/Stop; the editor overlay (selected-body outline always, whole-world toggle); Astra `FieldInfo` element access; the JSON container branch; the Inspector vector editor; a demonstration scene with a witness; the ABI bump and both restamps.

**Non-goals (recorded, not built):** viewport handles for shapes (F4 owns gizmo work — the selected-body outline is the feedback); joints; physics materials; a layer/mask UI beyond the raw `categoryBits` / `maskBits` fields; sensor / contact events to gameplay; an overlay or cvars in ArcaneRuntime (cvars are last in the roadmap); per-scene `fixedHz`; polygon fixture authoring (`ShapeKind::Polygon` has no verts field in the data model yet).

## 3. Rulings ledger (user-decided 2026-09-11)

| # | Ruling | Alternative rejected |
|---|---|---|
| R1 | **Engine-owned physics, always on.** `Runtime` owns the world and the `fixedUpdate` slot; modules never see Manifold2D. | Widening the SDK surface with Manifold2D (every plugin inlines its layouts ⇒ every Manifold2D change is an ABI bump; boilerplate per module). A module-pulled trigger (`EnablePhysics` from `OnLoad`) — the editor needs the engine-side hooks regardless. |
| R2 | **Project settings as the base, per-scene override.** `.arcproj` `"physics"` block; `Arcane::PhysicsSettings` component on the scene-root entity overrides when present. | Project-only; per-scene resource (needs a new serialisation path and Inspector surface the component gets for free). |
| R3 | **Fixture list authored as an array** (UE / Source 2 model). The data model stays; the bridge and Inspector grow generic container support. | One shape per component / child entity (Unity / Godot) — unwinds the M6 fixture-list decision and forces a hierarchy for every compound body. |
| R4 | **No enable flag.** An empty world costs nothing per step; a flag adds a branch to every host and both modules. | `"physics": { "enabled": … }`. |
| R5 | **Overlay state is session-only.** | Persisting in the layout ini / editor settings. |
| R6 | **Two plans.** Plan 1 = everything but the Inspector editor (hand-written fixtures in `.arcscene` fall under Play with the overlay on); Plan 2 = the Inspector vector editor. | One plan. |

## 4. Engine-owned physics (`Runtime`, Arcane.dll)

All of this lives in Arcane.dll behind a Manifold2D-free public surface; the only headers a game module compiles are unchanged except where §9 says.

### 4.1 `PhysicsSystem` becomes schedulable

- `static constexpr bool RequiresExclusive = true;` — the owed fix from Plan 1 (`PhysicsSystem.hpp`'s tick-contract note): it advances the registry tick and mutates structurally (adds `PhysicsBodyRef`), so it must own its scheduler segment like `TransformPropagationSystem` does.
- Ordering edge: the trait list gains `Astra::Before<TransformPropagationSystem>` so the propagation pass reads the write-back PASS 4 produced this step, regardless of the order the module and the engine inserted their systems (Astra's plan honours `Before`/`After` edges and keeps insertion order only among unconstrained systems).
- The scheduled instance is `PhysicsSystem{ 1.0f / fixedHz, /*stepWorld*/ true }` with `fixedHz` from the RunLoop config.
- **PASS 4 (write-back) is gated on `m_stepWorld`.** Today it writes `Transform` and `RigidBody2D::velocity` for every body on every pass, paused included — the standing defect recorded at Plan 1's close. In a paused pass the author owns the pose and PASS 3.5 has already pushed edits body-ward, so there is nothing to reflect back; the unconditional write stamped every physics entity's `Transform` changed every Edit frame (propagation recomposed them all, defeating Plan 1's change detection for exactly the entities physics touches) and flattened any authored out-of-plane rotation to its Z turn. Gating closes both.

### 4.1a Edit-mode structural edits reach the body

PASS 3.5 reconciles pose and scale only (`Changed<Transform>`); a `Collider2D` or `RigidBody2D` edit never reached the body. Same mechanism Plan 1 used for `Transform`:

- `Collider2D` and `RigidBody2D` declare `static constexpr bool AstraChangeTracked = true` (8 B per entity each; physics entities are few).
- PASS 1 (destroy), **paused passes only**, gains two destroy criteria beside dead / no-`RigidBody2D`: no `Collider2D`, or `Changed<Collider2D>` / `Changed<RigidBody2D>` since `lastReconcile`. A destroyed body is re-minted by PASS 2 in the same pass from the current components, so an Inspector edit of a fixture, a body type, or a mass takes effect the next Edit frame, as does undo. Play passes skip the criteria: PASS 4 writes `velocity` every step, which would otherwise read as a change.
- The Inspector's existing post-commit `Modified(e, id)` is what stamps the edit (Plan 1 T6).
- **PASS 1.5 (plan-time addition):** an entity carrying `RigidBody2D` + `Collider2D` but no `PhysicsBodyRef` gets one added before PASS 2 — the Inspector can never add it (`ComponentCatalog` structure-locks it), so an editor-authored body would otherwise never match the mint view. Collected, then added (never inside a `ForEach`).

### 4.2 `Runtime::InstallEngineSystems()`

Adds the engine-owned systems — today exactly one, `PhysicsSystem` into `fixedUpdate`. Called by Runtime's constructor (module-less hosts and tests) and by `PluginHost` after every successful module load and reload, because the unload path's `ClearSystems()` wipes every scheduler. Idempotent: a second call is a no-op (Astra's `AlreadyRegistered`).

### 4.3 `Runtime::EnsurePhysics()`

Called by both hosts once per frame before `Loop().Advance`, beside `SetRenderContext`.

1. Resolve settings (§5).
2. If the current registry has no `PhysicsResource`: construct a `PhysicsWorld` from a `WorldDef` carrying the resolved gravity, `SetResource(PhysicsResource{world, {}})`, and `SetResource(PhysicsInterpBuffer{})`.
3. Else if the resolved gravity differs from the world's: **replace the world** (plan-time amendment: the vendored `PhysicsWorld` exposes `Gravity()` but no setter). Bodies re-mint from their current `Transform`s on the next pass; in Play this drops velocities — a settings edit is authoring, not gameplay. A `SetGravity` upstream in Manifold2D is the recorded follow-up that makes the edit live.

This one rule covers every registry replacement identically — scene open, `RestoreRegistry` (Play → Stop, a structural undo), hot reload's `ResetRegistry`: the old world dies with the old registry's resource storage; the next frame mints a fresh one; the next physics pass re-creates bodies from the authored `Transform`s through PASS 2. There is no explicit "reset the world on Stop" hook, and `RegistryStateCommand`'s deferred "post-restore reconcile hook" is discharged by construction.

### 4.4 `Runtime::PhysicsEditPass()`

Runs `PhysicsSystem{ 1.0f / fixedHz, /*stepWorld*/ false }(registry)` bare: PASS 1 destroy, PASS 2 mint, PASS 3.5 reconcile authored transforms, no capture, no step, no write-back. Edit mode's only physics (§6.1). The bare-invocation tick contract Plan 1 designed for this (advance after every pass, scheduled or bare) is unchanged.

### 4.5 Interpolation

Nothing new: the buffer exists (4.3), PASS 2.5 fills `prev` + `slotOf` on every stepping pass, `SetRenderContext` already carries `Alpha()`, and `RenderSubmissionSystem` blends through the map. Play-mode sprites interpolate between fixed steps from the first stepping frame; Edit mode's buffer stays `captured == false` (a fresh buffer after every restore), so it snaps.

## 5. Settings

**Project.** `ProjectManifest` gains `struct PhysicsConfig { glm::vec2 gravity{0.0f, 9.81f}; }` parsed leniently from an optional `"physics": { "gravity": [x, y] }` block, defaults when absent (the `SplashConfig` precedent). +Y is down: the world is screen-space (`screen = world * zoom + offset`), and Manifold2D's convention and every existing test (`wd.gravityY = 10`) already point that way. `Project::Create` writes the block with defaults.

**Scene override.** `Arcane::PhysicsSettings { glm::vec2 gravity{0.0f, 9.81f}; }` — a reflected component (`AngleFormat` not applicable; plain `Vec2` row), appended to the engine roster after `MeshRenderer` in both `RegisterSceneComponents` and Runtime's `Register<…>()` list (append, not insert: ids after it do not shift). It is read from the **scene-root entity** (`SceneRoot` resource) only — beside `Camera` / `PostProcess`, where scene-level facts already live. Presence is the override; the component is user-addable through the catalog and editable in the Inspector with the existing `Vec2` editor and `ComponentEditCommand` undo. Documented on the reflect block: "meaningful on the scene root; ignored elsewhere".

**Resolution** (`EnsurePhysics`, every frame): scene-root `PhysicsSettings` if present, else the project block. Cheap (one `GetComponent`), and it means an Inspector edit of gravity takes effect next frame in Edit mode's world and in Play's.

## 6. Editor integration

### 6.1 Edit mode

`EditModeSchedule::RunFrame` calls the edit pass before `m_schedule.Execute` (propagation). The pass is injected as a callable (the schedule is tested device-less and must not link Manifold2D), bound by `EditorApp` to `Runtime::PhysicsEditPass`. Bodies therefore exist in the editor world without simulating — UE's editor-world behaviour — and every authored change reaches them the frame after it lands:

- gizmo / Inspector `Transform` edits and undo → PASS 3.5 moves the body (Plan 1 T7);
- `Collider2D` / `RigidBody2D` edits and undo → PASS 1 destroys, PASS 2 re-mints in the same pass (§4.1a);
- add / remove `RigidBody2D` or `Collider2D` → PASS 2 mints / PASS 1 destroys.

### 6.2 Play / Stop / Pause / Step

Nothing new. `PlayMode::Play` snapshots and unpauses; `fixedUpdate` runs the scheduled `PhysicsSystem` (§4.1); the toolbar's Pause and Step drive the RunLoop; `PlayMode::Stop` restores the snapshot, which replaces the registry, which §4.3 turns into a fresh world and re-minted bodies at their authored poses. Play-paused frames run no physics pass at all (the RunLoop skips `fixedUpdate`); the gizmo is Edit-only, so nothing needs reconciling there.

### 6.3 Overlay

`DrawPhysicsDebug(world, batcher, opts)` is called from `EditorApp::SubmitSceneToBatcher` after `Loop().SubmitRender()` and before the gizmo, with `opts.cameraOffset` / `opts.zoom` from the same camera the sprites used, `opts.interp = &PhysicsInterpBuffer`, `opts.alpha = Loop().Alpha()` — so the overlay and the sprites agree to the bit (Plan 2's `Lerp` / `AngleLerp` contract).

Two visibilities:

- **Selected-body outline, always, Edit mode only.** `PhysicsDebugDrawOptions` gains an optional body filter (`std::optional<Phys::BodyHandle> onlyBody` — the header already forward-references Manifold2D types through pointers; the editor passes the selected entity's `PhysicsBodyRef::handle`). Outlines only; no contacts, AABBs or velocity rays. This is the authoring feedback (Unity's collider gizmo).
- **View → Physics Overlay** toggle: the whole world with outlines + contacts (the option struct's defaults), Edit and Play. Session state (R5).

ArcaneRuntime draws nothing.

## 7. Authoring

### 7.1 Astra — `FieldInfo` element access (Astra repo, `dev`, then vendor)

`MakeFieldInfo` already derives `isVector` from `ContainerTraits`. For vector fields it additionally records:

```cpp
uint64_t elementTypeHash;   // TypeID<ContainerTraits<T>::ValueType>::Hash()
size_t   elementSize;
std::function<size_t(const void* inst)>            vectorSize;
std::function<void(void* inst, size_t n)>          vectorResize;   // default-constructs growth
std::function<void*(void* inst, size_t i)>         vectorElement;  // nullptr when i >= size
std::function<void(void* inst, size_t i)>          vectorErase;
std::function<void(void* inst, size_t i)>          vectorInsert;   // default element at i (i == size appends)
```

Generic — nothing Fixture-specific; Astra's own `JsonSchema` may use `elementTypeHash` to stop emitting `"type": "object"` for every array, but that is not required here. Astra-side tests pin each accessor on `std::vector<int>` and `std::vector<Struct>`. Then `sync-astra.ps1`, `VENDORED.txt`, and the ABI bump (§9): reflect blocks are compiled into plugins and `FieldInfo` grew.

### 7.2 `ReflectionJson.hpp` — the container branch

Writer: a vector field whose element type is a **reflected struct** (has a `TypeMeta`, is not an enum) becomes a JSON array of objects, each walked by the existing sub-writer recursion (Fixture is a nested reflected struct; so is `MeshSlot`). Reader: classify by type first (the existing discipline), shape-check every element, `vectorResize` to the array's length, then read each element in place; a malformed element or sub-field latches for the whole field, never a partial list. Vectors of scalars / `glm` / enums stay "unsupported field type" — fail loud — until a roster field needs them (plan-time amendment: the scalar helpers key off a `FieldInfo`, an element has none, and no roster field is such a vector; recorded follow-up). `Collider2D::fixtures` loses `Serializable(false)` and the comment that prescribed this fix.

`kSceneJsonVersion` 4 → 5 (`Min` stays 3): a file carrying `fixtures` is one an older engine would refuse on the read side, so the number says so; every existing scene still loads (the `SceneAssetTest` "older version still loads" pin gains the v4 case).

### 7.3 Inspector — `FieldKind::Vector` (Plan 2)

`ClassifyField` returns `Vector` when `isVector` and the element type is one the editor can draw (arithmetic, `glm`, registered enum, or a reflected struct whose fields all classify); otherwise `ReadOnly`, as compound types are today. The editor draws:

- a header row — field name, element count, **+** (`vectorInsert` at end);
- per element — an indented, collapsible block that recurses the element type's reflected fields through the existing field editors (Fixture's `kind` enum, floats, `vec2`, `uint32`, `bool` all have editors), with **−** (`vectorErase`) and up / down (swap through `vectorElement`).

Every mutation — a scalar edit inside an element, add, remove, reorder — commits through the existing `ComponentEditCommand` (whole-component before / after through the descriptor serialize seam; `Collider2D::Serialize` carries the vector), so undo / redo is byte-identical in shape to today's field edits, and the existing post-commit `Modified(e, id)` mark is what makes the next `PhysicsEditPass` rebuild the fixtures. List operations are one-shot commands (snapshot, mutate, snapshot, push); in-element scalar drags use the same transaction bracket scalar fields use today.

## 8. Demonstration and tests

**Scene.** `ReferenceProject/Content/scenes/physics.arcscene` — a static `Aabb` ground, three dynamic bodies (circle, box, capsule) with matching `Circle` / `Rect` / `Capsule` sprites, and a `PhysicsSettings` override on its scene root so the override path is exercised by content. **Not** the boot scene: `main.arcscene` and its four blessed golden lanes are untouched — no re-bless. One `[witness][gpu]` scenario boots it in ArcaneRuntime (`--scene <guid> --frames 120`, absolute `--project`) and probes that a dynamic body's sprite ends below where it started; fixed `dt` makes the frame deterministic.

**Tests, per piece.**

| Piece | Pin |
|---|---|
| Astra `FieldInfo` | each accessor on `std::vector<int>` / `std::vector<Struct>`; `VendorSmokeTest` names them |
| §4.1 ordering | `RuntimeTest`: the `fixedUpdate` plan orders `PhysicsSystem` before `TransformPropagationSystem` whichever is added first |
| §4.2 install | present after ctor; present again after a `ClearSystems` + `InstallEngineSystems`; idempotent |
| §4.3 ensure | mints once; survives `RestoreRegistry` with a fresh world; gravity edit reaches the world next call |
| §5 resolution | project default; scene-root override wins; override on a non-root entity is ignored |
| §4.1 PASS 4 gate | a paused pass stamps no `Transform` and no `RigidBody2D` (propagation's second pass composes nothing); an authored out-of-plane rotation survives a paused pass |
| §4.1a | a paused-pass `Collider2D` edit (fixture radius) re-mints the body with the new shape; a `RigidBody2D::type` edit re-mints with the new type; removing `Collider2D` destroys the body; a stepping pass never re-mints on the velocity write |
| §4.4 edit pass | mints bodies, moves none, steps nothing (`captured` stays false) |
| §6.1 | `EditModeScheduleTest`: the injected pass runs before propagation, once per Edit frame, never in Play |
| §6.2 | `EditorPlayModeTest`: Play → bodies fall → Stop → authored poses, fresh world, `captured == false` |
| §6.3 | the overlay call is gated exactly as specified (selected-only in Edit; whole-world only when toggled); `onlyBody` draws one outline |
| §7.2 | `SceneJsonTest`: `Collider2D` round-trips its fixtures; v5 written; v4 and v3 still load; malformed element refused whole |
| §7.3 | the device-less ImGui drive (`AssetsGraphCanvasTest` pattern): add / remove / reorder / in-element edit each produce one undoable command; undo restores the list |
| Plan 2's owed case | real PASS 2.5 output through `RenderSubmissionSystem` at α = 0.25 pins the blend direction |
| witness | the falling-body scenario above |

## 9. ABI, build ritual, plans

- **ABI 27 → 28** in Plan 1's vendor task: `FieldInfo` grew (compiled into every plugin's reflect blocks), `PhysicsSettings` joined the roster, `PhysicsSystem` gained `RequiresExclusive` + an ordering edge + the paused-pass gates, and `Collider2D` / `RigidBody2D` became tracked types (a static member changes no bytes, and `PhysicsComponents.hpp` is **not** on the game-module include surface — only the editor and the tests compile it — but Transform's precedent in the v26 entry records tracked types in the ledger regardless). v28 ledger in the v26/v27 form with the grep evidence over both game modules; `ReferenceProject.arcproj` → 28; Gacha's restamp is Plan 1's own last task (as v26 was), not a follow-up.
- **Build order** (spec 2026-09-11-astra-adoption §9, unchanged): `sync-astra.ps1` → `GenerateProjects.bat` → `ReferenceProject.slnx` first per configuration (`/t:Rebuild` on every config flip of the single-slot `Binaries\`) → `Arcane.slnx` → unfiltered suite + `~[gpu]` + `check-baselines.ps1`. Absolute `--project` for every host launch. A Debug host launch happens while the Debug DLL is staged.
- **Baseline:** 56216 / 1632 (`~[gpu]`, Debug and Release, Astra adoption Plan 2 close). Rises attributed per task; the golden lanes are untouched by construction.
- **Plans:** `docs/plans/2026-09-11-physics-2d-wiring-plan1-runtime.md` — Astra `FieldInfo` → vendor + ABI 28 → JSON container branch → `PhysicsSettings` + `.arcproj` block → `PhysicsSystem` schedulable → `InstallEngineSystems` / `EnsurePhysics` / `PhysicsEditPass` + host calls → editor Edit / Play integration → overlay → `physics.arcscene` + witness → Gacha restamp → close. `…-plan2-inspector.md` — `FieldKind::Vector` editor + tests → close.

## 10. Hazards ledger

| Hazard | Where addressed |
|---|---|
| Engine system wiped on module reload | §4.2 — `PluginHost` re-installs after every load |
| Stale world after `RestoreRegistry` / scene open / hot reload | §4.3 — mint-if-absent every frame; old world dies with the old registry |
| Physics writes `Transform` after propagation read it | §4.1 — `Before<TransformPropagationSystem>` edge |
| Bare edit pass vs scheduler tick contract | §4.4 — Plan 1's advance-after-every-pass contract, unchanged |
| Edit-mode fixture / body-type edit not reaching the body | §4.1a — tracked `Collider2D` / `RigidBody2D`; paused PASS 1 destroys on change, PASS 2 re-mints |
| Paused PASS 4 write-back stamping every physics entity each Edit frame | §4.1 — PASS 4 gated on `m_stepWorld` |
| Play-mode velocity write-back read as a "change" by the re-mint criteria | §4.1a — criteria are paused-only; `lastReconcile` advances after PASS 4 |
| Golden lanes disturbed by the demo scene | §8 — separate scene, not the boot scene |
| Older engine reading a scene with fixtures | §7.2 — `kSceneJsonVersion` 5 |
| Debug host launched against the Release DLL | §9 — build ritual; the Astra adoption Plan 2 T3 lesson |
| `PhysicsSettings` on a non-root entity silently ignored | §5 — documented on the reflect block; pinned by a test |
